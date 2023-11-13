import collections
import decimal
import itertools
import json
import logging
import os
import os.path
import random
import re
import shutil
import string
import sys
import tarfile
import tempfile
import xml.etree.ElementTree as ET
from concurrent import futures
from typing import BinaryIO, Dict, List, Optional, TextIO, Tuple, Union

import toposort
import yaml
from haoda.backend import xilinx as hls

from tapa import util
from tapa.codegen.axi_pipeline import get_axi_pipeline_wrapper
from tapa.codegen.buffer import BufferConfig
from tapa.codegen.buffergen import generate_buffer_from_config, index_generator
from tapa.codegen.duplicate_s_axi_control import duplicate_s_axi_ctrl
from tapa.floorplan import (
    checkpoint_floorplan,
    generate_floorplan,
    generate_new_connectivity_ini,
    get_floorplan_result,
    get_post_synth_area,
)
from tapa.hardware import (
    DEFAULT_REGISTER_LEVEL,
    get_slr_count,
    is_part_num_supported,
)
from tapa.instance import Instance, Port
from tapa.safety_check import check_mmap_arg_name
from tapa.task import Task
from tapa.verilog import ast
from tapa.verilog import xilinx as rtl

_logger = logging.getLogger().getChild(__name__)

STATE00 = ast.IntConst("2'b00")
STATE01 = ast.IntConst("2'b01")
STATE11 = ast.IntConst("2'b11")
STATE10 = ast.IntConst("2'b10")


class InputError(Exception):
  pass


class Program:
  """Describes a TAPA program.

  Attributes:
    top: Name of the top-level module.
    work_dir: Working directory.
    is_temp: Whether to delete the working directory after done.
    toplevel_ports: Tuple of Port objects.
    _tasks: Dict mapping names of tasks to Task objects.
    frt_interface: Optional string of FRT interface code.
    files: Dict mapping file names to contents that appear in the HDL directory.
  """

  def __init__(self, obj: Dict, work_dir: Optional[str] = None):
    """Construct Program object from a json file.

    Args:
      obj: json object.
      work_dir: Specifiy a working directory as a string. If None, a temporary
          one will be created.
    """
    self.top: str = obj['top']
    self.cflags = ' '.join(obj.get('cflags', []))
    self.headers: Dict[str, str] = obj.get('headers', {})
    if work_dir is None:
      self.work_dir = tempfile.mkdtemp(prefix='tapa-')
      self.is_temp = True
    else:
      self.work_dir = os.path.abspath(work_dir)
      os.makedirs(self.work_dir, exist_ok=True)
      self.is_temp = False
    self.toplevel_ports = tuple(map(Port, obj['tasks'][self.top]['ports']))
    self._tasks: Dict[str, Task] = collections.OrderedDict()

    # create a buffer_configs dictionary which is a mapping from the hash
    # of a buffer_config to the actual buffer_config object
    self.buffer_configs_hashes: Dict[int, BufferConfig] = {}

    # go through a topological sorting of tasks
    for name in toposort.toposort_flatten(
        {k: set(v.get('tasks', ())) for k, v in obj['tasks'].items()}):
      task = Task(name=name, **obj['tasks'][name])
      # go through all buffer_configs in the task and update our dict
      for _, buffer_config in task.buffer_configs.items():
        hash_config = hash(buffer_config)
        if hash_config not in self.buffer_configs_hashes:
          self.buffer_configs_hashes[hash_config] = buffer_config
      if not task.is_upper or task.tasks:
        self._tasks[name] = task

    # generate unique names for each hash of buffer
    self.buffer_configs_names_to_hashes: Dict[str, int] = {}
    self.buffer_configs_hashes_to_names: Dict[str, int] = {}
    for buffer_hash, buffer_config in self.buffer_configs_hashes.items():
      buffer_name = self.buffer_name_generator()
      # keep generating another name if there's one existing w/ same
      # name
      while buffer_name in self.buffer_configs_names_to_hashes:
        buffer_name = self.buffer_name_generator()
      self.buffer_configs_names_to_hashes[buffer_name] = buffer_hash
      self.buffer_configs_hashes_to_names[buffer_hash] = buffer_name
    self.tasks_to_recompile = {}

    self.frt_interface = obj['tasks'][self.top].get('frt_interface')
    self.files: Dict[str, str] = {}
    self._hls_report_xmls: Dict[str, ET.ElementTree] = {}

  def __del__(self):
    if self.is_temp:
      shutil.rmtree(self.work_dir)

  def buffer_name_generator(self,
                            size=6,
                            chars=string.ascii_uppercase + string.digits):
    return ''.join(random.choice(chars) for _ in range(size))

  @property
  def tasks(self) -> Tuple[Task, ...]:
    return tuple(self._tasks.values())

  @property
  def top_task(self) -> Task:
    return self._tasks[self.top]

  @property
  def ctrl_instance_name(self) -> str:
    return rtl.ctrl_instance_name(self.top)

  @property
  def register_level(self) -> int:
    return self.top_task.module.register_level

  @property
  def start_q(self) -> rtl.Pipeline:
    return rtl.Pipeline(rtl.START.name, level=self.register_level)

  @property
  def done_q(self) -> rtl.Pipeline:
    return rtl.Pipeline(rtl.DONE.name, level=self.register_level)

  @property
  def rtl_dir(self) -> str:
    return os.path.join(self.work_dir, 'hdl')

  @property
  def autobridge_dir(self) -> str:
    return os.path.join(self.work_dir, 'autobridge')

  @property
  def cpp_dir(self) -> str:
    cpp_dir = os.path.join(self.work_dir, 'cpp')
    os.makedirs(cpp_dir, exist_ok=True)
    return cpp_dir

  def get_task(self, name: str) -> Task:
    return self._tasks[name]

  def get_cpp(self, name: str) -> str:
    return os.path.join(self.cpp_dir, name + '.cpp')

  def get_tar(self, name: str) -> str:
    os.makedirs(os.path.join(self.work_dir, 'tar'), exist_ok=True)
    return os.path.join(self.work_dir, 'tar', name + '.tar')

  def get_rtl(self, name: str, prefix: bool = True) -> str:
    return os.path.join(self.rtl_dir,
                        (util.get_module_name(name) if prefix else name) +
                        rtl.RTL_SUFFIX)

  def get_post_syn_rpt(self, module_name: str) -> str:
    return f'{self.work_dir}/report/{module_name}.hier.util.rpt'

  def _get_hls_report_xml(self, name: str) -> ET.ElementTree:
    tree = self._hls_report_xmls.get(name)
    if tree is None:
      filename = os.path.join(self.work_dir, 'report', f'{name}_csynth.xml')
      self._hls_report_xmls[name] = tree = ET.parse(filename)
    return tree

  def get_area(self, name: str) -> Dict[str, int]:
    node = self._get_hls_report_xml(name).find('./AreaEstimates/Resources')
    return {x.tag: int(x.text) for x in sorted(node, key=lambda x: x.tag)}

  def get_clock_period(self, name: str) -> decimal.Decimal:
    return decimal.Decimal(
        self._get_hls_report_xml(name).find('./PerformanceEstimates'
                                            '/SummaryOfTimingAnalysis'
                                            '/EstimatedClockPeriod').text)

  def extract_cpp(self) -> 'Program':
    """Extract HLS C++ files."""
    _logger.info('extracting HLS C++ files')
    check_mmap_arg_name(self._tasks.values())

    for task in self._tasks.values():
      with open(self.get_cpp(task.name), 'w') as src_code:
        src_code.write(util.clang_format(task.code))
    for name, content in self.headers.items():
      header_path = os.path.join(self.cpp_dir, name)
      os.makedirs(os.path.dirname(header_path), exist_ok=True)
      with open(header_path, 'w') as header_fp:
        header_fp.write(content)
    return self

  def run_hls(
      self,
      clock_period: Union[int, float, str],
      part_num: str,
      other_configs: str = '',
  ) -> 'Program':
    """Run HLS with extracted HLS C++ files and generate tarballs."""
    self.extract_cpp()

    _logger.info('running HLS')

    def worker(task: Task, idx: int) -> None:
      os.nice(idx % 19)
      hls_cflags = ' '.join((
          self.cflags,
          *(f'-isystem {x}/../tps/lnx64/gcc-6.2.0/include/c++/6.2.0'
            for x in util.get_vendor_include_paths()),
          '-DTAPA_TARGET_=XILINX_HLS',
      ))
      with open(self.get_tar(task.name), 'wb') as tarfileobj:
        with hls.RunHls(
            tarfileobj,
            kernel_files=[(self.get_cpp(task.name), hls_cflags)],
            top_name=task.name,
            clock_period=clock_period,
            part_num=part_num,
            auto_prefix=True,
            hls='vitis_hls',
            std='c++17',
            other_configs=other_configs,
        ) as proc:
          stdout, stderr = proc.communicate()
      if proc.returncode != 0:
        if b'Pre-synthesis failed.' in stdout and b'\nERROR:' not in stdout:
          _logger.error(
              'HLS failed for %s, but the failure may be flaky; retrying',
              task.name,
          )
          worker(task, 0)
          return
        sys.stdout.write(stdout.decode('utf-8'))
        sys.stderr.write(stderr.decode('utf-8'))
        raise RuntimeError('HLS failed for {}'.format(task.name))

    worker_num = util.nproc()
    _logger.info(
        'spawn %d workers for parallel HLS synthesis of the tasks',
        worker_num,
    )
    with futures.ThreadPoolExecutor(max_workers=worker_num) as executor:
      any(executor.map(worker, self._tasks.values(), itertools.count(0)))

    return self

  def find_buffer_user(self, task: Task, buffer_name: str,
                       direction: str) -> Tuple[Task, str]:
    if task.level == Task.Level.LOWER:
      return task, buffer_name
    user_task_name, user_task_index = task.buffers[buffer_name][direction]
    args = task.tasks[user_task_name][user_task_index]["args"]
    for port_name, arg_obj in args.items():
      if arg_obj["arg"] == buffer_name:
        return self.find_buffer_user(self._tasks[user_task_name], port_name,
                                     direction)
    raise ValueError("Buffer never used")

  def generate_task_rtl(
      self,
      additional_fifo_pipelining: bool = False,
      part_num: str = '',
  ) -> 'Program':
    """Extract HDL files from tarballs generated from HLS."""
    _logger.info('extracting RTL files')
    for task in self._tasks.values():
      with tarfile.open(self.get_tar(task.name), 'r') as tarfileobj:
        tarfileobj.extractall(path=self.work_dir)

    for file_name in (
        'arbiter.v',
        'async_mmap.v',
        'axi_pipeline.v',
        'axi_crossbar_addr.v',
        'axi_crossbar_rd.v',
        'axi_crossbar_wr.v',
        'axi_crossbar.v',
        'axi_register_rd.v',
        'axi_register_wr.v',
        'detect_burst.v',
        'fifo.v',
        'fifo_bram.v',
        'fifo_fwd.v',
        'fifo_srl.v',
        'initialized_fifo.v',
        'initialized_relay_station.v',
        'memcore_bram_simple.v',
        'memcore_uram_simple.v',
        'memcore_bram_true.v',
        'memcore_uram_true.v',
        'memcore_bram.v',
        'memcore_uram.v',
        'generate_last.v',
        'priority_encoder.v',
        'relay_station.v',
        'a_axi_write_broadcastor_1_to_3.v',
        'a_axi_write_broadcastor_1_to_4.v',
        'a_axi_write_broadcastor_1_to_2.v',
    ):
      shutil.copy(
          os.path.join(os.path.dirname(util.__file__), 'assets', 'verilog',
                       file_name), self.rtl_dir)

    # generate all buffer modules that we need
    for buffer_name, buffer_hash in self.buffer_configs_names_to_hashes.items():
      buffer_config = self.buffer_configs_hashes[buffer_hash]
      generate_buffer_from_config(buffer_name, buffer_config, self.rtl_dir)

    # extract and parse RTL and populate tasks
    _logger.info('parsing RTL files and populating tasks')
    for task, module in zip(
        self._tasks.values(),
        futures.ProcessPoolExecutor().map(
            rtl.Module,
            ([self.get_rtl(x.name)] for x in self._tasks.values()),
            (not x.is_upper for x in self._tasks.values()),
        ),
    ):
      _logger.debug('parsing %s', task.name)
      task.module = module
      task.self_area = self.get_area(task.name)
      task.clock_period = self.get_clock_period(task.name)
      _logger.debug('populating %s', task.name)
      self._populate_task(task)

    # scan through tasks to find simple buffer channels
    _logger.info('scanning tasks and analyzing buffer channels')
    for task_name, task in self._tasks.items():
      _logger.info('  analyzing buffer channels of %s', task_name)
      instantiated_buffers = {
          name: buffer
          for name, buffer in task.buffers.items()
          if 'is_instantiated' in buffer
      }
      for buffer_name, buffer_obj in instantiated_buffers.items():
        buffer_config = task.buffer_configs[buffer_name]
        dims_patterns = buffer_config.get_dim_patterns()
        producer_task, producer_buffer_name = self.find_buffer_user(
            task, buffer_name, "produced_by")
        consumer_task, consumer_buffer_name = self.find_buffer_user(
            task, buffer_name, "consumed_by")
        producer_reads = False
        consumer_writes = False
        for index in index_generator(dims_patterns):
          read_port_suffix = f'_data_{index}q0'
          write_port_suffix = f'_data_{index}we0'
          read_port = producer_task.module.get_port_of_buffer(
              producer_buffer_name, read_port_suffix)
          write_port = consumer_task.module.get_port_of_buffer(
              consumer_buffer_name, write_port_suffix)
          if read_port:
            producer_reads = True
          if write_port:
            consumer_writes = True
        make_simple = False
        if not producer_reads and not consumer_writes:
          make_simple = True
        task.buffer_simplicities[buffer_name] = {
            "buffer_nature": make_simple,
            "producer_reads": producer_reads,
            "actual_producer": producer_task,
            "relative_producer": self._tasks[buffer_obj['produced_by'][0]]
        }
        if make_simple:
          _logger.info('  buffer channel %s is simple', buffer_name)
        else:
          _logger.info(
              '  buffer channel %s is complex, requires resource-heavy '
              'T2P memories to be used', buffer_name)

    # instrument the upper-level RTL except the top-level
    _logger.info('instrumenting upper-level RTL')
    for task in self._tasks.values():
      if task.is_upper and task.name != self.top:
        self._instrument_non_top_upper_task(
            task,
            part_num,
            additional_fifo_pipelining,
        )

    return self

  def generate_post_synth_task_area(
      self,
      part_num: str,
      max_parallel_synth_jobs: int = 8,
  ):
    get_post_synth_area(
        self.rtl_dir,
        part_num,
        self.top_task,
        self.get_post_syn_rpt,
        self.get_task,
        self.get_cpp,
        max_parallel_synth_jobs,
    )

  def run_floorplanning(
      self,
      part_num,
      connectivity: TextIO,
      floorplan_pre_assignments: TextIO = None,
      read_only_args: List[str] = [],
      write_only_args: List[str] = [],
      separate_complex_buffer_tasks: bool = False,
      **kwargs,
  ) -> 'Program':
    _logger.info('Running floorplanning')

    os.makedirs(self.autobridge_dir, exist_ok=True)

    # generate partitioning constraints if partitioning directive is given
    config, config_with_floorplan = generate_floorplan(
        part_num,
        connectivity,
        read_only_args,
        write_only_args,
        user_floorplan_pre_assignments=floorplan_pre_assignments,
        autobridge_dir=self.autobridge_dir,
        top_task=self.top_task,
        fifo_width_getter=self._get_fifo_width,
        separate_complex_buffer_tasks=separate_complex_buffer_tasks,
        **kwargs,
    )

    open(f'{self.autobridge_dir}/pre-floorplan-config.json',
         'w').write(json.dumps(config, indent=2))
    open(f'{self.autobridge_dir}/post-floorplan-config.json',
         'w').write(json.dumps(config_with_floorplan, indent=2))
    checkpoint_floorplan(config_with_floorplan, self.autobridge_dir)
    generate_new_connectivity_ini(
        config_with_floorplan,
        self.work_dir,
        self.top,
    )

    return self

  def generate_top_rtl(self, constraint: TextIO, register_level: int,
                       additional_fifo_pipelining: bool, device_info: Dict,
                       other_hls_config: str) -> 'Program':
    """Instrument HDL files generated from HLS.

    Args:
        constraint: where to save the floorplan constraints
        register_level: Non-zero value overrides self.register_level.
        part_num: optinally provide the part_num to enable board-specific optimization
        additional_fifo_pipelining: replace every FIFO by a relay_station of LEVEL 2

    Returns:
        Program: Return self.
    """
    part_num = device_info['part_num']
    task_inst_to_slr = {}

    # extract the floorplan result
    if constraint:
      (
          fifo_pipeline_level,
          axi_pipeline_level,
          buffer_pipeline_level,
          task_inst_to_slr,
          fifo_to_depth,
      ) = get_floorplan_result(self.autobridge_dir, constraint)

      if not task_inst_to_slr:
        _logger.warning('generate top rtl without floorplanning')

      self.top_task.module.fifo_partition_count = fifo_pipeline_level
      self.top_task.module.axi_pipeline_level = axi_pipeline_level
      self.top_task.module.buffer_pipeline_level = buffer_pipeline_level

      # recompile buffer producer tasks if latency needs to change
      self.recompile_buffer_producers(device_info, other_hls_config)

      # adjust fifo depth for rebalancing
      for fifo_name, depth in fifo_to_depth.items():
        if fifo_name in self.top_task.fifos:
          self.top_task.fifos[fifo_name]['depth'] = depth
        else:
          # streams has different naming convention
          # change fifo_name_0 to fifo_name[0]
          streams_fifo_name = re.sub(r'_(\d+)$', r'[\1]', fifo_name)
          if streams_fifo_name in self.top_task.fifos:
            self.top_task.fifos[streams_fifo_name]['depth'] = depth
          else:
            _logger.critical('unrecognized FIFO %s, skip depth adjustment',
                             fifo_name)

    if register_level:
      assert register_level > 0
      self.top_task.module.register_level = register_level
    else:
      if is_part_num_supported(part_num):
        self.top_task.module.register_level = get_slr_count(part_num)
      else:
        _logger.info(
            'the part-num is not included in the hardware library, '
            'using the default register level %d.', DEFAULT_REGISTER_LEVEL)
        self.top_task.module.register_level = DEFAULT_REGISTER_LEVEL

    _logger.info('top task register level set to %d',
                 self.top_task.module.register_level)

    # instrument the top-level RTL
    _logger.info('instrumenting top-level RTL')
    self._instrument_top_task(self.top_task, part_num, task_inst_to_slr,
                              additional_fifo_pipelining)

    _logger.info('generating report')
    task_report = self.top_task.report
    with open(os.path.join(self.work_dir, 'report.yaml'), 'w') as fp:
      yaml.dump(task_report, fp, default_flow_style=False, sort_keys=False)
    with open(os.path.join(self.work_dir, 'report.json'), 'w') as fp:
      json.dump(task_report, fp, indent=2)

    # self.files won't be populated until all tasks are instrumented
    _logger.info('writing generated auxiliary RTL files')
    for name, content in self.files.items():
      with open(os.path.join(self.rtl_dir, name), 'w') as fp:
        fp.write(content)

    return self

  def pack_rtl(self, output_file: BinaryIO) -> 'Program':
    _logger.info('packaging RTL code')
    rtl.pack(top_name=self.top,
             ports=self.toplevel_ports,
             rtl_dir=self.rtl_dir,
             output_file=output_file)
    return self

  def _populate_task(self, task: Task) -> None:
    task.instances = tuple(
        Instance(self.get_task(name), verilog=rtl, instance_id=idx, **obj)
        for name, objs in task.tasks.items()
        for idx, obj in enumerate(objs))

  def _connect_fifos(self, task: Task) -> None:
    _logger.debug("  connecting %s's children tasks with FIFOs", task.name)
    for fifo_name in task.fifos:
      for direction in task.get_fifo_directions(fifo_name):
        task_name, _, fifo_port = task.get_connection_to_fifo(
            fifo_name, direction)

        for suffix in task.get_fifo_suffixes(direction):
          # declare wires for FIFOs
          wire_name = rtl.wire_name(fifo_name, suffix)
          wire_width = self.get_task(task_name).module.get_port_of(
              fifo_port, suffix).width
          wire = ast.Wire(name=wire_name, width=wire_width)
          task.module.add_signals([wire])

      if task.is_fifo_external(fifo_name):
        task.connect_fifo_externally(fifo_name, task.name == self.top)

  def _connect_buffers(self, task: Task) -> None:
    _logger.debug("  connecting %s's children tasks with buffers", task.name)
    for buffer_name in task.buffers:
      buffer_config = task.buffer_configs[buffer_name]
      for direction in task.get_buffer_directions(buffer_name):
        task_name, _, buffer_port = task.get_connection_to_buffer(
            buffer_name, direction)

        for suffix, width, wire_dir, port_suffix, _ in buffer_config.get_fifo_suffixes(
            direction):
          wire_name = rtl.wire_name(buffer_name, suffix)
          wire_width = ast.Width(
              ast.Minus(ast.IntConst(width), ast.IntConst('1')),
              ast.IntConst('0'))
          wire = ast.Wire(name=wire_name, width=wire_width)
          task.module.add_signals([wire])

        for index in index_generator(buffer_config.get_dim_patterns()):
          for suffix, width, wire_dir, port_suffix, _ in buffer_config.get_memory_suffixes(
              direction):
            wire_name = rtl.wire_name(buffer_name, suffix.format(index))
            wire_width = ast.Width(
                ast.Minus(ast.IntConst(width), ast.IntConst('1')),
                ast.IntConst('0'))
            wire = ast.Wire(name=wire_name, width=wire_width)
            task.module.add_signals([wire])

      if task.is_buffer_external(buffer_name):
        task.connect_buffer_externally(buffer_name, buffer_config)

  def _instantiate_fifos(
      self,
      task: Task,
      additional_fifo_pipelining: bool,
  ) -> None:
    _logger.debug('  instantiating FIFOs in %s', task.name)

    # skip instantiating if the fifo is not declared in this task
    fifos = {name: fifo for name, fifo in task.fifos.items() if 'depth' in fifo}
    if not fifos:
      return

    col_width = max(
        max(len(name), len(util.get_instance_name(fifo['consumed_by'])),
            len(util.get_instance_name(fifo['produced_by'])))
        for name, fifo in fifos.items())

    for fifo_name, fifo in fifos.items():
      _logger.debug('    instantiating %s.%s', task.name, fifo_name)

      # add FIFO instances
      task.module.add_fifo_instance(
          name=fifo_name,
          width=self._get_fifo_width(task, fifo_name),
          depth=fifo['depth'],
          additional_fifo_pipelining=additional_fifo_pipelining,
      )

      # print debugging info
      debugging_blocks = []
      fmtargs = {
          'fifo_prefix': '\\033[97m',
          'fifo_suffix': '\\033[0m',
          'task_prefix': '\\033[90m',
          'task_suffix': '\\033[0m',
      }
      for suffixes, fmt, fifo_tag in zip(
          (rtl.ISTREAM_SUFFIXES, rtl.OSTREAM_SUFFIXES),
          ('DEBUG: R: {fifo_prefix}{fifo:>{width}}{fifo_suffix} -> '
           '{task_prefix}{task:<{width}}{task_suffix} %h',
           'DEBUG: W: {task_prefix}{task:>{width}}{task_suffix} -> '
           '{fifo_prefix}{fifo:<{width}}{fifo_suffix} %h'),
          ('consumed_by', 'produced_by')):
        display = ast.SingleStatement(statement=ast.SystemCall(
            syscall='display',
            args=(ast.StringConst(
                value=fmt.format(width=col_width,
                                 fifo=fifo_name,
                                 task=(util.get_instance_name(fifo[fifo_tag])),
                                 **fmtargs)),
                  ast.Identifier(name=rtl.wire_name(fifo_name, suffixes[0])))))
        debugging_blocks.append(
            ast.Always(
                sens_list=rtl.CLK_SENS_LIST,
                statement=ast.make_block(
                    ast.IfStatement(cond=ast.Eq(
                        left=ast.Identifier(
                            name=rtl.wire_name(fifo_name, suffixes[-1])),
                        right=rtl.TRUE,
                    ),
                                    true_statement=ast.make_block(display),
                                    false_statement=None))))
      task.module.add_logics(debugging_blocks)

  def _instantiate_buffers(
      self,
      task: Task,
      additional_buffer_pipelining: bool,
  ) -> None:
    _logger.debug('  instantiating buffers in %s', task.name)

    # skip instantiating if the buffer is not declared in this task
    buffers = {
        name: buffer
        for name, buffer in task.buffers.items()
        if 'is_instantiated' in buffer
    }
    if not buffers:
      return

    for buffer_name, buffer_config in buffers.items():
      _logger.debug('    instantiating %s.%s', task.name, buffer_name)

      buffer_config = task.buffer_configs[buffer_name]
      buffer_hash = hash(buffer_config)
      buffer_module_name = self.buffer_configs_hashes_to_names[buffer_hash]

      buffer_obj = task.buffers[buffer_name]
      buffer_config = task.buffer_configs[buffer_name]
      make_simple = task.buffer_simplicities[buffer_name]['buffer_nature']

      # get the buffer config object
      task.module.add_buffer_instance(name=buffer_name,
                                      buffer_config=buffer_config,
                                      buffer_module_name=buffer_module_name,
                                      make_simple=make_simple)

  def _instantiate_children_tasks(
      self,
      task: Task,
      width_table: Dict[str, int],
      part_num: str,
      instance_name_to_slr: Dict[str, int],
  ) -> List[ast.Identifier]:
    is_done_signals: List[rtl.Pipeline] = []
    arg_table: Dict[str, rtl.Pipeline] = {}
    async_mmap_args: Dict[Instance.Arg, List[str]] = collections.OrderedDict()

    task.add_m_axi(width_table, self.files)

    # now that each SLR has an control_s_axi, slightly reduce the
    # pipeline level of the scalars
    if instance_name_to_slr:
      scalar_register_level = 2
    else:
      scalar_register_level = self.register_level

    for instance in task.instances:
      # connect to the control_s_axi in the corresponding SLR
      if instance.name in instance_name_to_slr:
        argname_suffix = f'_slr_{instance_name_to_slr[instance.name]}'
      else:
        argname_suffix = ''

      child_port_set = set(instance.task.module.ports)

      # add signal delcarations
      for arg in instance.args:
        if not arg.cat.is_stream and not arg.cat.is_buffer:
          width = 64  # 64-bit address
          if arg.cat.is_scalar:
            width = width_table.get(arg.name, 0)
            if width == 0:
              width = int(arg.name.split("'d")[0])
          q = rtl.Pipeline(
              name=instance.get_instance_arg(arg.name),
              level=scalar_register_level,
              width=width,
          )
          arg_table[arg.name] = q

          # arg.name may be a constant
          if arg.name in width_table:
            id_name = arg.name + argname_suffix
          else:
            id_name = arg.name
          task.module.add_pipeline(q, init=ast.Identifier(id_name))

        # arg.name is the upper-level name
        # arg.port is the lower-level name

        # check which ports are used for async_mmap
        if arg.cat.is_async_mmap:
          for tag in rtl.ASYNC_MMAP_SUFFIXES:
            if set(x.portname for x in rtl.generate_async_mmap_ports(
                tag=tag,
                port=arg.port,
                arg=arg.name,
                instance=instance,
            )) & child_port_set:
              async_mmap_args.setdefault(arg, []).append(tag)

        # declare wires or forward async_mmap ports
        for tag in async_mmap_args.get(arg, []):
          if task.is_upper and instance.task.is_lower:
            task.module.add_signals(
                rtl.generate_async_mmap_signals(
                    tag=tag,
                    arg=arg.mmap_name,
                    data_width=width_table[arg.name],
                ))
          else:
            task.module.add_ports(
                rtl.generate_async_mmap_ioports(
                    tag=tag,
                    arg=arg.name,
                    data_width=width_table[arg.name],
                ))

      # add reset registers
      rst_q = rtl.Pipeline(instance.rst_n, level=self.register_level)
      task.module.add_pipeline(rst_q, init=rtl.RST_N)

      # add start registers
      start_q = rtl.Pipeline(
          f'{instance.start.name}_global',
          level=self.register_level,
      )
      task.module.add_pipeline(start_q, self.start_q[0])

      if instance.is_autorun:
        # autorun modules start when the global start signal is asserted
        task.module.add_logics([
            ast.Always(
                sens_list=rtl.CLK_SENS_LIST,
                statement=ast.make_block(
                    ast.make_if_with_block(
                        cond=ast.Unot(rst_q[-1]),
                        true=ast.NonblockingSubstitution(
                            left=instance.start,
                            right=rtl.FALSE,
                        ),
                        false=ast.make_if_with_block(
                            cond=start_q[-1],
                            true=ast.NonblockingSubstitution(
                                left=instance.start,
                                right=rtl.TRUE,
                            ),
                        ),
                    )),
            ),
        ])
      else:
        # set up state
        is_done_q = rtl.Pipeline(
            f'{instance.is_done.name}',
            level=self.register_level,
        )
        done_q = rtl.Pipeline(
            f'{instance.done.name}_global',
            level=self.register_level,
        )
        task.module.add_pipeline(is_done_q, instance.is_state(STATE10))
        task.module.add_pipeline(done_q, self.done_q[0])

        if_branch = (instance.set_state(STATE00))
        else_branch = ((
            ast.make_if_with_block(
                cond=instance.is_state(STATE00),
                true=ast.make_if_with_block(
                    cond=start_q[-1],
                    true=instance.set_state(STATE01),
                ),
            ),
            ast.make_if_with_block(
                cond=instance.is_state(STATE01),
                true=ast.make_if_with_block(
                    cond=instance.ready,
                    true=ast.make_if_with_block(
                        cond=instance.done,
                        true=instance.set_state(STATE10),
                        false=instance.set_state(STATE11),
                    )),
            ),
            ast.make_if_with_block(
                cond=instance.is_state(STATE11),
                true=ast.make_if_with_block(
                    cond=instance.done,
                    true=instance.set_state(STATE10),
                ),
            ),
            ast.make_if_with_block(
                cond=instance.is_state(STATE10),
                true=ast.make_if_with_block(
                    cond=done_q[-1],
                    true=instance.set_state(STATE00),
                ),
            ),
        ))
        task.module.add_logics([
            ast.Always(
                sens_list=rtl.CLK_SENS_LIST,
                statement=ast.make_block(
                    ast.make_if_with_block(
                        cond=ast.Unot(rst_q[-1]),
                        true=if_branch,
                        false=else_branch,
                    )),
            ),
            ast.Assign(
                left=instance.start,
                right=instance.is_state(STATE01),
            ),
        ])

        is_done_signals.append(is_done_q)

      # insert handshake signals
      task.module.add_signals(instance.handshake_signals)

      # add task module instances
      portargs = list(rtl.generate_handshake_ports(instance, rst_q))
      for arg in instance.args:
        if arg.cat.is_scalar:
          portargs.append(
              ast.PortArg(portname=arg.port, argname=arg_table[arg.name][-1]))
        elif arg.cat.is_istream:
          portargs.extend(
              instance.task.module.generate_istream_ports(
                  port=arg.port,
                  arg=arg.name,
              ))
        elif arg.cat.is_ostream:
          portargs.extend(
              instance.task.module.generate_ostream_ports(
                  port=arg.port,
                  arg=arg.name,
              ))
        elif arg.cat.is_ibuffer:
          buffer_name = arg.unsanitize_name
          buffer_config = task.buffer_configs[buffer_name]
          portargs.extend(
              instance.task.module.generate_ibuffer_ports(
                  port=arg.port, arg=arg.name, buffer_config=buffer_config))
        elif arg.cat.is_obuffer:
          buffer_name = arg.unsanitize_name
          buffer_config = task.buffer_configs[buffer_name]
          portargs.extend(
              instance.task.module.generate_obuffer_ports(
                  port=arg.port, arg=arg.name, buffer_config=buffer_config))
        elif arg.cat.is_sync_mmap:
          portargs.extend(
              rtl.generate_m_axi_ports(
                  module=instance.task.module,
                  port=arg.port,
                  arg=arg.mmap_name,
                  arg_reg=arg_table[arg.name][-1].name,
              ))
        elif arg.cat.is_async_mmap:
          for tag in async_mmap_args[arg]:
            portargs.extend(
                rtl.generate_async_mmap_ports(
                    tag=tag,
                    port=arg.port,
                    arg=arg.mmap_name,
                    instance=instance,
                ))
      module_name = util.get_module_name(instance.task.name)
      if task == self.top_task:
        tup = (instance.task.name, instance.instance_id)
        if tup in self.tasks_to_recompile:
          module_name = f'{instance.task.name}_{instance.instance_id}'

      task.module.add_instance(
          module_name=module_name,
          instance_name=instance.name,
          ports=portargs,
      )

    # instantiate async_mmap modules at the upper levels
    # the base address may not be 0, so must use full 64 bit
    addr_width = 64
    _logger.debug('Set the address width of async_mmap to %d', addr_width)

    if task.is_upper:
      for arg in async_mmap_args:
        task.module.add_async_mmap_instance(
            name=arg.mmap_name,
            offset_name=arg_table[arg.name][-1],
            tags=async_mmap_args[arg],
            data_width=width_table[arg.name],
            addr_width=addr_width,
        )

    return is_done_signals

  def _instantiate_global_fsm(
      self,
      task: Task,
      is_done_signals: List[rtl.Pipeline],
  ) -> None:
    # global state machine

    def is_state(state: ast.IntConst) -> ast.Eq:
      return ast.Eq(left=rtl.STATE, right=state)

    def set_state(state: ast.IntConst) -> ast.NonblockingSubstitution:
      return ast.NonblockingSubstitution(left=rtl.STATE, right=state)

    countdown = ast.Identifier('countdown')
    countdown_width = (self.register_level - 1).bit_length()

    task.module.add_signals([
        ast.Reg(rtl.STATE.name, width=ast.make_width(2)),
        ast.Reg(countdown.name, width=ast.make_width(countdown_width)),
    ])

    state01_action = set_state(STATE10)
    if is_done_signals:
      state01_action = ast.make_if_with_block(
          cond=ast.make_operation(
              operator=ast.Land,
              nodes=(x[-1] for x in reversed(is_done_signals)),
          ),
          true=state01_action,
      )

    global_fsm = ast.make_case_with_block(
        comp=rtl.STATE,
        cases=[
            (
                STATE00,
                ast.make_if_with_block(
                    cond=self.start_q[-1],
                    true=set_state(STATE01),
                ),
            ),
            (
                STATE01,
                state01_action,
            ),
            (
                STATE10,
                [
                    set_state(STATE11 if self.register_level else STATE00),
                    ast.NonblockingSubstitution(
                        left=countdown,
                        right=ast.make_int(max(0, self.register_level - 1)),
                    ),
                ],
            ),
            (
                STATE11,
                ast.make_if_with_block(
                    cond=ast.Eq(
                        left=countdown,
                        right=ast.make_int(0, width=countdown_width),
                    ),
                    true=set_state(STATE00),
                    false=ast.NonblockingSubstitution(
                        left=countdown,
                        right=ast.Minus(
                            left=countdown,
                            right=ast.make_int(1, width=countdown_width),
                        ),
                    ),
                ),
            ),
        ],
    )

    task.module.add_logics([
        ast.Always(
            sens_list=rtl.CLK_SENS_LIST,
            statement=ast.make_block(
                ast.make_if_with_block(
                    cond=rtl.RST,
                    true=set_state(STATE00),
                    false=global_fsm,
                )),
        ),
        ast.Assign(left=rtl.IDLE, right=is_state(STATE00)),
        ast.Assign(left=rtl.DONE, right=self.done_q[-1]),
        ast.Assign(left=rtl.READY, right=self.done_q[0]),
    ])

    task.module.add_pipeline(self.start_q, init=rtl.START)
    task.module.add_pipeline(self.done_q, init=is_state(STATE10))

  def _instrument_non_top_upper_task(
      self,
      task: Task,
      part_num: str,
      additional_fifo_pipelining: bool = False,
  ) -> None:
    """ codegen for upper but non-top tasks """
    assert task.is_upper
    task.module.cleanup()

    # instantiate fifos
    self._instantiate_fifos(task, additional_fifo_pipelining)
    self._connect_fifos(task)

    # instantiate and connect buffers
    self._instantiate_buffers(task, False)
    self._connect_buffers(task)

    width_table = {port.name: port.width for port in task.ports.values()}
    is_done_signals = self._instantiate_children_tasks(
        task,
        width_table,
        part_num,
        {},
    )
    self._instantiate_global_fsm(task, is_done_signals)

    with open(self.get_rtl(task.name), 'w') as rtl_code:
      rtl_code.write(task.module.code)

  def _instrument_top_task(
      self,
      task: Task,
      part_num: str,
      instance_name_to_slr: Dict[str, int],
      additional_fifo_pipelining: bool = False,
  ) -> None:
    """ codegen for the top task """
    assert task.is_upper
    task.module.cleanup()

    # if floorplan is enabled, add a control_s_axi instance in each SLR
    if instance_name_to_slr:
      num_slr = get_slr_count(part_num)
      duplicate_s_axi_ctrl(task, num_slr)

    # instantiate and connect FIFOs
    self._instantiate_fifos(task, additional_fifo_pipelining)
    self._connect_fifos(task)

    #instantiate and connect Buffers
    self._instantiate_buffers(task, False)
    self._connect_buffers(task)

    width_table = {port.name: port.width for port in task.ports.values()}
    is_done_signals = self._instantiate_children_tasks(
        task,
        width_table,
        part_num,
        instance_name_to_slr,
    )
    self._instantiate_global_fsm(task, is_done_signals)

    self._pipeline_top_task(task)

  def _pipeline_top_task(self, task: Task) -> None:
    """
    add axi pipelines to the top task
    """
    # generate the original top module. Append a suffix to it
    top_suffix = '_inner'
    task.module.name += top_suffix
    with open(self.get_rtl(task.name + top_suffix), 'w') as rtl_code:
      rtl_code.write(task.module.code)

    # generate the wrapper that becomes the final top module
    with open(self.get_rtl(task.name), 'w') as rtl_code:
      rtl_code.write(get_axi_pipeline_wrapper(task.name, top_suffix, task))

  def _get_fifo_width(self, task: Task, fifo: str) -> int:
    producer_task, _, fifo_port = task.get_connection_to_fifo(
        fifo, 'produced_by')
    port = self.get_task(producer_task).module.get_port_of(
        fifo_port, rtl.OSTREAM_SUFFIXES[0])
    # TODO: err properly if not integer literals
    return int(port.width.msb.value) - int(port.width.lsb.value) + 1

  def recompile_buffer_producers(self, device_config,
                                 other_hls_configs) -> None:
    top_task = self.top_task
    tasks_to_recompile = {}
    for task_name, instance_list in top_task.tasks.items():
      for i in range(len(instance_list)):
        task_invocation = (task_name, i)
        produced_buffers = {}
        for buffer_name, buffer_json in top_task.buffers.items():
          producer_task_name, invocation_index = buffer_json['produced_by']
          if producer_task_name == task_name and invocation_index == i:
            # find the inside name of the buffer
            args = instance_list[i]['args']
            inside_name = None
            for _inside_name, obj in args.items():
              if obj['arg'] == buffer_name:
                inside_name = _inside_name
            produced_buffers[inside_name] = {
                'pipeline_level':
                    top_task.module.get_buffer_pipeline_level(
                        rtl.sanitize_array_name(buffer_name))
            }
        needs_recompilation = False
        for buffer_name in produced_buffers:
          if produced_buffers[buffer_name]['pipeline_level'] > 1:
            needs_recompilation = True
        if needs_recompilation:
          tasks_to_recompile[task_invocation] = produced_buffers

    for (task_name,
         invocation_index), produced_bufs in tasks_to_recompile.items():
      dest_task_name = f'{task_name}_{invocation_index}'
      source_path = self.get_cpp(task_name)
      dest_path = self.get_cpp(dest_task_name)
      shutil.copy(source_path, dest_path)
      # replace the task name in the function itself
      file_contents = None
      with open(dest_path, "r") as fh:
        file_contents = fh.read()
      re_string = f'void(\s*?){task_name}\('
      re_sub_string = f'void\\1{task_name}_{invocation_index}('
      file_contents = re.sub(re_string,
                             re_sub_string,
                             file_contents,
                             flags=re.MULTILINE | re.DOTALL)
      for buf_name, obj in produced_bufs.items():
        re_string = f'void(\s*?){dest_task_name}(.*?)\{{(.*?)ap_memory latency = 1 port =([^#]*?){buf_name}\.data(.*?)\}}'
        re_sub_string = f'void\\1{dest_task_name}\\2{{\\3ap_memory latency = {1 + 2*(obj["pipeline_level"] - 1)} port =\\4{buf_name}.data\\5}}'
        file_contents = re.sub(re_string,
                               re_sub_string,
                               file_contents,
                               flags=re.MULTILINE | re.DOTALL)
      with open(dest_path, "w") as fh:
        fh.write(file_contents)

    def worker(
        task_name: str,
        inside_task_name: str,
        idx: int,
    ) -> None:
      os.nice(idx % 19)
      hls_cflags = ' '.join((
          self.cflags,
          *(f'-isystem {x}/../tps/lnx64/gcc-6.2.0/include/c++/6.2.0'
            for x in util.get_vendor_include_paths()),
          '-DTAPA_TARGET_=XILINX_HLS',
      ))
      with open(self.get_tar(task_name), 'wb') as tarfileobj:
        with hls.RunHls(
            tarfileobj,
            kernel_files=[(self.get_cpp(task_name), hls_cflags)],
            top_name=task_name,
            clock_period=device_config['clock_period'],
            part_num=device_config['part_num'],
            auto_prefix=True,
            hls='vitis_hls',
            std='c++17',
            other_configs=other_hls_configs,
        ) as proc:
          stdout, stderr = proc.communicate()
      if proc.returncode != 0:
        if b'Pre-synthesis failed.' in stdout and b'\nERROR:' not in stdout:
          _logger.error(
              'HLS failed for %s, but the failure may be flaky; retrying',
              task_name,
          )
          worker(task_name, 0, inside_task_name)
          return
        sys.stdout.write(stdout.decode('utf-8'))
        sys.stderr.write(stderr.decode('utf-8'))
        raise RuntimeError('HLS failed for {}'.format(task_name))

    worker_num = util.nproc()
    _logger.info(
        'spawn %d workers for parallel HLS recompilation of the tasks',
        worker_num,
    )

    task_names = [task_name for task_name, _ in tasks_to_recompile]
    task_changed_names = [
        f'{task_name}_{index}' for task_name, index in tasks_to_recompile
    ]
    with futures.ThreadPoolExecutor(max_workers=worker_num) as executor:
      any(
          executor.map(worker, task_changed_names, task_names,
                       itertools.count(0)))

    # extract the tar files
    for task_name in task_changed_names:
      with tarfile.open(self.get_tar(task_name), 'r') as tarfileobj:
        tarfileobj.extractall(path=self.work_dir)

    self.tasks_to_recompile = tasks_to_recompile

    # get resource consumption reports
    _logger.info('Recompiled tasks may differ in area, too much of a difference'
                 ' can cause problems with floorplanning results')
    _logger.info('The differences are: (positive indicates increase)')
    for original_task, new_task in zip(task_names, task_changed_names):
      original_area = self.get_area(original_task)
      new_area = self.get_area(new_task)
      _logger.info('Buffer %s:', new_task)
      keys = ['BRAM_18K', 'DSP', 'FF', 'LUT', 'URAM']
      for key in keys:
        _logger.info(
            f'  {key}: {original_area[key]} -> {new_area[key]} = {new_area[key] - original_area[key]}'
        )
