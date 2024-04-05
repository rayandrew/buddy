#pragma once

#include <cstddef>
#include <list>
#include "request.h"

namespace buddy::host {

struct request {
  request_head head;
  void *buf;
};

struct recv_key {
  int src;
  int tag;

  friend bool operator==(const recv_key& lhs, const recv_key& rhs)
  {
    return lhs.src == rhs.src && lhs.tag == rhs.tag;
  }

  template <typename H>
  friend H AbslHashValue(H h, const recv_key& key)
  {
    return H::combine(std::move(h), key.src, key.tag);
  }
};

typedef std::list<request>::iterator recv_handle;

void init();
void finalize();
void flush();
void put_send(request_head head, const void *buf);
recv_handle put_recv(request_head head, void *buf);
bool poll_recv();
void delete_recv(recv_handle req_it);

} // namespace buddy::host
