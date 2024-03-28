#include <iostream>
#include "dpu_conn.h"
#include "sockets.h"
#include "local_proto.h"
#include "util.h"

namespace buddy::host {

DpuConn::DpuConn()
  : initialized(false)
{}

DpuConn::DpuConn(int world_rank, int world_size)
  : initialized(true)
{
  char *host = getenv("BUDDY_DPU");
  CHECK(host && *host);

  int sock = tcp_connect(host, LOCAL_PORT);

  local_init msg = {
    .world_rank = world_rank,
    .world_size = world_size
  };
  full_write(sock, (char *)&msg, sizeof(msg));
}

DpuConn::~DpuConn()
{
  if (!initialized)
    return;
  initialized = false;
}

} // namespace buddy::host
