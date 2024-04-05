#include <cassert>
#include <cstring>
#include "request.h"

namespace buddy {

// Possible optimization: "merge" contiguous processed requests to reduce
// number of hops in subsequent parses of the same buffer by increasing the
// size of the first one.
bool ReqBufRead::next(request_head **out_head, char **out_data, int dst)
{
  while (pos + sizeof(request_head) <= len) {
    request_head *pos_head = reinterpret_cast<request_head*>(buf+pos);

    if (dst >= 0 && pos_head->dst != dst)
      continue;

    char *data = reinterpret_cast<char*>(pos_head+1);

    pos += sizeof(request_head) + pos_head->size;
    assert(pos <= len);

    *out_head = pos_head;
    *out_data = data;

    return true;
  }

  assert(pos == len);
  return false;
}

bool ReqBufWrite::append(request_head head, const char *data)
{
  assert(sizeof(head) + head.size <= len);

  if (pos + sizeof(head) + head.size > len)
    return false;

  memcpy(buf + pos, &head, sizeof(head));
  pos += sizeof(head);

  memcpy(buf + pos, data, head.size);
  pos += head.size;

  return true;
}

} // namespace buddy
