from typing import Optional

from tapa.verilog import ast

__all__ = [
    'RTL_SUFFIX', 'ISTREAM_SUFFIXES', 'OSTREAM_SUFFIXES',
    'STREAM_PORT_DIRECTION', 'STREAM_PORT_OPPOSITE', 'STREAM_PORT_WIDTH',
    'FIFO_READ_PORTS', 'FIFO_WRITE_PORTS', 'HANDSHAKE_CLK', 'HANDSHAKE_RST',
    'HANDSHAKE_RST_N', 'HANDSHAKE_START', 'HANDSHAKE_DONE', 'HANDSHAKE_IDLE',
    'HANDSHAKE_READY', 'HANDSHAKE_INPUT_PORTS', 'HANDSHAKE_OUTPUT_PORTS',
    'START', 'DONE', 'IDLE', 'READY', 'TRUE', 'FALSE', 'SENS_TYPE', 'CLK',
    'RST', 'RST_N', 'CLK_SENS_LIST', 'ALL_SENS_LIST', 'STATE',
    'BUILTIN_INSTANCES', 'get_stream_width', 'PRODUCER_BUFFER_FIFO_PORTS',
    'CONSUMER_BUFFER_FIFO_PORTS', 'BUFFER_AP_MEMORY_PORT_TEMPLATE'
]

# const strings

RTL_SUFFIX = '.v'

ISTREAM_SUFFIXES = (
    '_dout',
    '_empty_n',
    '_read',
)

OSTREAM_SUFFIXES = (
    '_din',
    '_full_n',
    '_write',
)

# {port_suffix: direction}
STREAM_PORT_DIRECTION = {
    '_dout': 'input',
    '_empty_n': 'input',
    '_read': 'output',
    '_din': 'output',
    '_full_n': 'input',
    '_write': 'output',
}

# {port_suffix: opposite_suffix}
# used when connecting two FIFOs head to tail
STREAM_PORT_OPPOSITE = {
    '_dout': '_din',
    '_empty_n': '_write',
    '_read': '_full_n',
    '_din': '_dout',
    '_full_n': '_read',
    '_write': '_empty_n',
}

# {port_suffix: width}, 0 is variable
STREAM_PORT_WIDTH = {
    '_dout': 0,
    '_empty_n': 1,
    '_read': 1,
    '_din': 0,
    '_full_n': 1,
    '_write': 1,
}

FIFO_READ_PORTS = (
    'if_dout',
    'if_empty_n',
    'if_read',
    'if_read_ce',
)

FIFO_WRITE_PORTS = (
    'if_din',
    'if_full_n',
    'if_write',
    'if_write_ce',
)

HANDSHAKE_CLK = 'ap_clk'
HANDSHAKE_RST = 'ap_rst_n_inv'
HANDSHAKE_RST_N = 'ap_rst_n'
HANDSHAKE_START = 'ap_start'
HANDSHAKE_DONE = 'ap_done'
HANDSHAKE_IDLE = 'ap_idle'
HANDSHAKE_READY = 'ap_ready'

HANDSHAKE_INPUT_PORTS = (
    HANDSHAKE_CLK,
    HANDSHAKE_RST_N,
    HANDSHAKE_START,
)
HANDSHAKE_OUTPUT_PORTS = (
    HANDSHAKE_DONE,
    HANDSHAKE_IDLE,
    HANDSHAKE_READY,
)

# const ast nodes

START = ast.Identifier(HANDSHAKE_START)
DONE = ast.Identifier(HANDSHAKE_DONE)
IDLE = ast.Identifier(HANDSHAKE_IDLE)
READY = ast.Identifier(HANDSHAKE_READY)
TRUE = ast.IntConst("1'b1")
FALSE = ast.IntConst("1'b0")
SENS_TYPE = 'posedge'
CLK = ast.Identifier(HANDSHAKE_CLK)
RST = ast.Identifier(HANDSHAKE_RST)
RST_N = ast.Identifier(HANDSHAKE_RST_N)
CLK_SENS_LIST = ast.SensList((ast.Sens(CLK, type=SENS_TYPE),))
ALL_SENS_LIST = ast.SensList((ast.Sens(None, type='all'),))
STATE = ast.Identifier('tapa_state')

BUILTIN_INSTANCES = {'hmss_0'}


def get_stream_width(port: str, data_width: int) -> Optional[ast.Width]:
  width = STREAM_PORT_WIDTH[port]
  if width == 0:
    width = data_width + 1  # for eot
  if width == 1:
    return None
  else:
    return ast.Width(msb=ast.Constant(width - 1), lsb=ast.Constant(0))


PRODUCER_BUFFER_FIFO_PORTS = (
    'fifo_free_buffers_empty_n',
    'fifo_free_buffers_read',
    'fifo_free_buffers_dout',
    'fifo_occupied_buffers_full_n',
    'fifo_occupied_buffers_write',
    'fifo_occupied_buffers_din',
    'fifo_free_buffers_read_ce',
    'fifo_occupied_buffers_write_ce',
)

CONSUMER_BUFFER_FIFO_PORTS = (
    'fifo_occupied_buffers_empty_n',
    'fifo_occupied_buffers_read',
    'fifo_occupied_buffers_dout',
    'fifo_free_buffers_full_n',
    'fifo_free_buffers_write',
    'fifo_free_buffers_din',
    'fifo_occupied_buffers_read_ce',
    'fifo_free_buffers_write_ce',
)

OBUFFER_FIFO_SUFFIXES = [
    '_fifo_free_buffers_empty_n',
    '_fifo_free_buffers_read',
    '_fifo_free_buffers_dout',
    '_fifo_occupied_buffers_full_n',
    '_fifo_occupied_buffers_write',
    '_fifo_occupied_buffers_din',
]

IBUFFER_FIFO_SUFFIXES = [
    '_fifo_occupied_buffers_empty_n',
    '_fifo_occupied_buffers_read',
    '_fifo_occupied_buffers_dout',
    '_fifo_free_buffers_full_n',
    '_fifo_free_buffers_write',
    '_fifo_free_buffers_din',
]

IBUFFER_MEMORY_SUFFIXES = [
    '_mem_{}consumer_address',
    '_mem_{}consumer_ce',
    '_mem_{}consumer_d',
    '_mem_{}consumer_we',
    '_mem_{}consumer_q',
]

OBUFFER_MEMORY_SUFFIXES = [
    '_mem_{}producer_address',
    '_mem_{}producer_ce',
    '_mem_{}producer_d',
    '_mem_{}producer_we',
    '_mem_{}producer_q',
]

BUFFER_AP_MEMORY_PORT_TEMPLATE = (
    'mem_{}address',
    'mem_{}ce',
    'mem_{}d',
    'mem_{}we',
    'mem_{}q',
)
