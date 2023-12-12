# PASTA

PASTA is built upon TAPA and adds *buffer* channel support. If you're not familiar with the TAPA project please refer to [TAPA](https://github.com/UCLA-VAST/tapa) and its documentation [here](https://tapa.rtfd.io/).


## Installation
PASTA can be installed using the installer script already provided.
We recommend using a Conda environment for the PASTA project. If a different environment is being used, please follow the detailed installation steps to build from source.

### Download the repository

```
git clone https://github.com/SFU-HiAccel/pasta.git
### OR ###
git clone git@github.com:SFU-HiAccel/pasta.git
```

### Setup a Conda environment
[Install Miniconda](https://docs.conda.io/projects/miniconda/en/latest/miniconda-install.html)  

```
conda create -y --name "pasta"
conda activate pasta
```

### Build PASTA
Once you have a custom conda environment loaded, navigate to the `installer` folder and run the `install` script.

```
cd <repo_root>/installer
./install
```

Once the installation is complete, a `setup` file will be created in the project directory.  
Please use `source setup` to setup the current shell with the required PATHs.

---

## Summary of Usage
TAPA supports FIFO stream based communication channels between tasks. With PASTA, a producer task can send a chunk of data (under the hood stored in BRAMs or URAMs) to a consumer task in a ping-pong fashion.
<details>
<summary>Expand this to see example usage.</summary>

### Buffer channel configuration
A buffer channel declaration looks like the following:
```cpp
tapa::buffer<float[NX][NY][NZ],      // the type, followed by the dimensions and their sizes
             2,                      // the no of sections (e.g. 2 for ping-pong)
             tapa::array_partition<  // a list of partition strategy for each dimension
                tapa::normal,        // normal partitioning (no partitioning)
                tapa::cyclic<2>,     // cyclic partition with factor of 2, block is also supported
                tapa::complete       // complete partitioning
              >,
             tapa::memcore<tapa::bram> // the memcore to use, can be BRAM and URAM
             >
```

### Usage of buffers in tasks
Note that in the example below `...` show truncated text to keep the example clean.
```cpp
// renaming types for easier writing
using buf_type_t = tapa::buffer<int[10][20], 2, tapa::array_partition<tapa::normal>, tapa::memcore<tapa::BRAM>>;
using ibuf_type_t = tapa::ibuffer<int[10][20], 2, tapa::array_partition<tapa::normal>, tapa::memcore<tapa::BRAM>>;
using obuf_type_t = tapa::obuffer<int[10][20], 2, tapa::array_partition<tapa::normal>, tapa::memcore<tapa::BRAM>>;

// the producer task which writes a chunk of data
void producer(obuf_type_t& buf_out, ...) {
    for (int t = 0; t < 10; t++) {
        // block till we get enough space on channel to write a 2D array of [10][20] integers.
        auto section = buf_out.acquire();
        // get a reference to the underlying memory region
        auto& memref = section();
        // write to memref in any order possible.
        // - random dynamically accessed writes/reads are allowed. its equivalent of interacting with an array of shape [10][20] in HLS.
        for (int i = 0; i < 10; i++) {
            for (int j = 0; j < 20; j++) {
                memref[i][j] = some_integer_value();
            }
        }
        // once `section` goes out of scope, it automatically tells the consumer task that it can start reading
    }
}

// the consumer task which reads a chunk of data
void consumer(ibuf_type_t& buf_in, ...) {
    for (int t = 0; t < 10; t++) {
        // block till we get a valid chunk to read. the chunk is  a 2D array of [10][20] integers.
        auto section = buf_out.acquire();
        // get a reference to the underlying memory region
        auto& memref = section();
        // read from the memref as desired
        // - random dynamically accessed reads/writes are allowed. its equivalent of interacting with an array of shape [10][20] in HLS.
        for (int i = 0; i < 10; i++) {
            for (int j = 0; j < 20; j++) {
                memref[i][j] = some_integer_value();
            }
        }
        // once `section` goes out of scope, it automatically tells the producer task that the consumer is done reading and
        // the memory region can be used to write new data again
    }
}

void top(...) {
    buf_type_t buf_name;
    tapa::task()
      .invoke(producer, buf_name, ...)
      .invoke(consumer, buf_name, ...);
}
```
</details>

### Help Needed?
Please feel free to reach out to us if you need any help at [moazinkhatri@gmail.com](mailto:moazinkhatri@gmail.com) or [moazin_khatti@sfu.ca](mailto:moazin_khatti@sfu.ca).
You can also feel free to file issues in the repository.

### Publications
The PASTA work has been published at FCCM 2023.
> M. Khatti, X. Tian, Y. Chi, L. Guo, J. Cong and Z. Fang, "PASTA: Programming and Automation Support for Scalable Task-Parallel HLS Programs on Modern Multi-Die FPGAs," 2023 IEEE 31st Annual International Symposium on Field-Programmable Custom Computing Machines (FCCM), Marina Del Rey, CA, USA, 2023, pp. 12-22, doi: 10.1109/FCCM57271.2023.00011.

The TAPA and Autobridge work that this framework is built upon has been published at [FCCM 2021](https://ieeexplore.ieee.org/document/9444053), [FPGA 2021](https://dl.acm.org/doi/10.1145/3431920.3439289) and [TRETS 2023](https://dl.acm.org/doi/10.1145/3609335).
> Y. Chi, L. Guo, J. Lau, Y. -k. Choi, J. Wang and J. Cong, "Extending High-Level Synthesis for Task-Parallel Programs," 2021 IEEE 29th Annual International Symposium on Field-Programmable Custom Computing Machines (FCCM), Orlando, FL, USA, 2021, pp. 204-213, doi: 10.1109/FCCM51124.2021.00032.

> Licheng Guo, Yuze Chi, Jie Wang, Jason Lau, Weikang Qiao, Ecenur Ustun, Zhiru Zhang, and Jason Cong. 2021. AutoBridge: Coupling Coarse-Grained Floorplanning and Pipelining for High-Frequency HLS Design on Multi-Die FPGAs. In The 2021 ACM/SIGDA International Symposium on Field-Programmable Gate Arrays (FPGA '21). Association for Computing Machinery, New York, NY, USA, 81–92. https://doi.org/10.1145/3431920.3439289

> Licheng Guo, Yuze Chi, Jason Lau, Linghao Song, Xingyu Tian, Moazin Khatti, Weikang Qiao, Jie Wang, Ecenur Ustun, Zhenman Fang, Zhiru Zhang, and Jason Cong. 2023. TAPA: A Scalable Task-parallel Dataflow Programming Framework for Modern FPGAs with Co-optimization of HLS and Physical Design. ACM Trans. Reconfigurable Technol. Syst. 16, 4, Article 63 (December 2023), 31 pages. https://doi.org/10.1145/3609335
