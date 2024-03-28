#pragma once

namespace buddy::host {

class DpuConn {

  public:
    DpuConn();
    DpuConn(int world_rank, int world_size);
    ~DpuConn();

  private:
    bool initialized;
};

} // namespace buddy::host
