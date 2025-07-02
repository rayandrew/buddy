# buddy

## Build

Builds two components:
- `libbuddy.so`: Client library with the interface in `src/buddy.h`.
- `buddy-proxy`: The DPU routing agent.

Debug build:

    mkdir build-dbg
    cd build-dbg
    cmake .. -DCMAKE_BUILD_TYPE=Debug
    make

Release build:

    mkdir build-rel
    cd build-rel
    cmake .. -DCMAKE_BUILD_TYPE=Release
    make

## Configuration

This section lists environment variables that control Buddy.

### Client

- `BUDDY_DPU`: Network host of the local DPU.
- `BUDDY_TRACE`: Verbosity value from 1-3 to log API calls.

### DPU Agent

- `BUDDY_TRACE`: Set it 1 to log API calls.
- `BUDDY_TIMEOUT`: Seconds to wait for blocked requests to progress before
  failing.
- `BUDDY_QUIET_TIME`: Seconds to wait for additional messages to arrive before
  flushing buffers.
