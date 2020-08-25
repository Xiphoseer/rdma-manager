
# RDMA Manager
API for easy RDMA/GPUDirect programming based on the InfiniBand Verbs (IBV) of NVIDIA Mellanox. Abstracts connection types (reliable/unreliable RDMA) and memory types (main memory, CUDA memory). No IBV programming needed as the RDMA manager offers clean and simple functions that abstract away from IBV.

## Environment Variables
The following environment variables must be set (Linux: add to '.bashrc' file):
```
export CUDACXX="/usr/local/cuda-10.1/bin/nvcc"
export CPATH=/usr/local/cuda-10.1/include:$CPATH
export LD_LIBRARY_PATH=/usr/local/cuda-10.1/targets/x86_64-linux/lib/stubs/lib:$LD_LIBRARY_PATH
export PATH=/usr/local/cuda-10.1/bin:$PATH
```

## Build (with and without CUDA)
1. Clone this repository (GPUDirect is currently only supported in 'gpudirect' branch)
2. Go into cloned repository
3. Define build options in 'CMakeLists.txt' file
4. Execute: mkdir build && cd build
5. Execute: cmake ..
6. Execute: make -j

Binaries can the be found in ./build/bin/

## Setup
Default settings should be defined in './build/bin/conf/RDMA.conf' for the RDMA-Manager to properly work.
RDMA_NUMAREGION:  ID of the NUMA node at which the RDMA-enabled NIC is connected to
RDMA_INTERFACE:  Name of the RDMA interface
RDMA_IBPORT:  InfiniBand port that should be used
RDMA_SERVER_ADDRESSES:  IP of the RDMA-enabled NIC where the server should run
LOGGING_LEVEL:  Number of the logging level that should be used
NODE_SEQUENCER_IP:  IP of the normal NIC where the server should run
NODE_SEQUENCER_PORT:  Port that should be used for the node sequencer protocol

## Benchmarking
### Measuring
This project offers a benchmarking tool called 'perf_test' for measuring performance of the RDMA operations. The general concept is to run the tool twice at the same time. Onces as server by specifying the '--server' flag and once as client without this flag.
It is important to first start the server and afterwards the client!
To run multiple clients the server has to run as many threads as all connecting clients together (can be defined with '--threads' flag).
In addition the benchmarking tool allows to run predefined test suites '--fulltest', '--halftest', '--quicktest' or even custom ones by passing a list to a flag instead of a single value. All flags can be seen by entering:
```perf_test --help```

### Exporting & Plotting
The results of the benchmarking tool can be written into a CSV file with by simply adding the '--csv' flag. Plots can then be generated by calling 'PlotResults.py' which is a Python script that reads the CSV file and automatically generates plots and stores them by default in a PDF file. Other formats like JPEG or SVG are also possible.
How to use:
```python PlotResults.py (<csv-file>) (<format: pdf, jpg, png, svg, ...>)```