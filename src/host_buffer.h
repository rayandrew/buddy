#pragma once

#include <cstddef>

namespace buddy {

struct request_head {
  size_t size;
  int rank;
  int tag;
};

} // namespace buddy

namespace buddy::host {

const size_t SEND_BUFFER_SIZE = 4096;
const size_t RECV_BUFFER_SIZE = 4096;

struct request {
  request_head head;
  void *buf;
};

void init();
void flush();
void put_send(request_head head, const void *buf);
bool try_recv(request_head head, void *buf);

} // namespace buddy::host
