#include <cassert>
#include "request.h"

namespace buddy {

/*
bool ReqBufWrite::append(reqest_head head, const char *data)
{
}
*/

// Possible optimization: "merge" contiguous processed requests to reduce
// number of hops in subsequent parses of the same buffer by increasing the
// size of the first one.
bool ReqBufRead::next(request_head **out_head, char **out_data)
{
  while (pos + sizeof(request_head) <= len) {
    request_head *pos_head = reinterpret_cast<request_head*>(buf+pos);

    // Already completed
    if (pos_head->dst != dst)
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

} // namespace buddy
