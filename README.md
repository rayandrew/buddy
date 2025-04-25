# buddy

Offloading and batching of MPI non-blocking operations to BlueField SmartNIC. It consists of two components -- a library that overloads specific MPI calls to use buddy (libbuddy_mpi), and a DPU agent to handle offloading and batching (buddy-proxy).

## Build

    mkdir build-dbg
    cd build-dbg
    cmake .. -DCMAKE_BUILD_TYPE=Debug
    make

## Design

1. The application issues non-blocking send operations such as MPI_Isend().
2. The buddy runtime buffers send requests locally. They are transferred to the local DPU using either RDMA or DMA.
3. The DPU aggregates messages from all local ranks and forwards them to the DPU of remote nodes.
4. The receiving DPU divides the messages by destination rank and sends them to the local ranks using either RDMA or DMA.
5. The application receives the data using operations such as MPI_Irecv() and MPI_Wait().
