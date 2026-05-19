  
# Buddy: communication offloading engine on DPU

### Citation
* Wahlgren, J., Hu, A., Pearce, R., Gokhale, M. and Peng, I., 2026. Communication Offloading on SmartNIC DPUs: A Quantitative Approach. In European Conference on Parallel Processing 2026 (EuroPar'26).

### Main components:
- `libbuddy.so`: The client library that interacts with the application. Its APIs are defined in `src/buddy.h`.
- `buddy-proxy`: The DPU routing agent. It runs on BlueField smartNIC DPU or x86 platforms.

## Dependency: 
- CMake version 3.9 and above
- MPI
- OpenMP

## Build

First time, you may need to initialize git submodules:

    git submodule update --init --recursive

On the host, build the host component:

    mkdir build-host && cd build-host
    cmake .. -DCMAKE_BUILD_TYPE=[Release|Debug]
    make

Then, on the DPU, build the DPU agent:

    mkdir build-dpu && cd build-dpu
    cmake .. -DCMAKE_BUILD_TYPE=[Release|Debug]
    make

## Run
First, on the DPU, start the DPU agent:

    ./build-dpu/src/buddy-proxy 

Then, on the host, start the program. For example,

    /build-host$ BUDDY_DPU=bf01 mpirun -np 16 ./test/histo 10000 1000000 1

## Configuration

This section lists environment variables that control Buddy.

### Client

- `BUDDY_DPU`: Network host of the local DPU. The DPU address is configured in the contents of `/usr/local/etc/buddy_dpu`.
- `BUDDY_TRACE`: Verbosity value from 1-3 to log API calls.

### DPU Agent

- `BUDDY_TRACE`: Set it 1 to log API calls.
- `BUDDY_TIMEOUT`: Seconds to wait for blocked requests to progress before
  failing.
- `BUDDY_QUIET_TIME`: Seconds to wait for additional messages to arrive before
  flushing buffers.

