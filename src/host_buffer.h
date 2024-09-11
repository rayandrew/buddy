#pragma once

#include <cstddef>
#include <list>
#include "request.h"

namespace buddy::host {

struct pending_recv {
  void *buf;
  size_t size;
  int src;
  int tag;
  size_t real_size = 0;
  int real_src = -1;
  int real_tag = -1;

  pending_recv(request_head head, void *buf)
    : buf(buf)
    , size(head.size)
    , src(head.src)
    , tag(head.tag)
  {}

  inline bool completed() { return real_src >= 0; }
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

typedef std::list<pending_recv>::iterator recv_handle;

void init();
void finalize();
void flush();
void put_send(request_head head, const char *data);
recv_handle put_recv(request_head head, void *buf);
bool poll_recv();
void complete_recv(recv_handle req_it);

} // namespace buddy::host
