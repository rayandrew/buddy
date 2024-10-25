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
    char *data = reinterpret_cast<char*>(pos_head+1);

    pos += sizeof(request_head) + pos_head->size;
    assert(pos <= len);

    if (dst >= 0 && pos_head->dst != dst)
      continue;

    *out_head = pos_head;
    *out_data = data;

    return true;
  }

  assert(pos == len);

  return false;
}

char *ReqBufWrite::append_head(request_head head)
{
  assert(sizeof(head) + head.size <= len);

  if (pos + sizeof(head) + head.size > len)
    return NULL;

  memcpy(buf + pos, &head, sizeof(head));
  pos += sizeof(head);

  char *data = buf + pos;
  pos += head.size;

  return data;
}

bool ReqBufWrite::append(request_head head, const char *data)
{
  char *bufptr = append_head(head);
  if (!bufptr)
    return false;

  memcpy(bufptr, data, head.size);
  return true;
}

} // namespace buddy
