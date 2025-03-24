#include <cassert>
#include <cstring>
#include "request.h"

namespace buddy {

bool ReqBufRead::next(request_head **out_head, char **out_data)
{
  if (peek(out_head, out_data)) {
      advance(*out_head);
      return true;
  }

  return false;
}

bool ReqBufRead::peek(request_head **out_head, char **out_data)
{
  if (pos + sizeof(request_head) > len) {
    assert(pos == len);
    return false;
  }

  request_head *pos_head = reinterpret_cast<request_head*>(buf+pos);
  char *data = reinterpret_cast<char*>(pos_head+1);

  *out_head = pos_head;
  *out_data = data;

  return true;
}

void ReqBufRead::advance(request_head *head)
{
  pos += sizeof(request_head) + head->size;
  assert(pos <= len);
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

char *AtomicReqBufWrite::append_head(request_head head)
{
  size_t bump = sizeof(head) + head.size;
  assert(bump <= len);

  size_t start = pos.fetch_add(bump);

  if (start + bump > len)
    return NULL;

  memcpy(buf + start, &head, sizeof(head));

  char *data = buf + start + sizeof(head);
  return data;
}

bool AtomicReqBufWrite::append(request_head head, const char *data)
{
  char *bufptr = append_head(head);
  if (!bufptr)
    return false;

  memcpy(bufptr, data, head.size);
  return true;
}

bool SepReqBufRead::get(size_t idx, request_head *out_head, char **out_data)
{
  if (idx >= count) {
    return false;
  }

  // Headers are stored in reverse order from the end of buf
  auto head = reinterpret_cast<request_head*>(buf+len-(idx+1)*sizeof(request_head));

  // size is cumulative
  char *data_start;
  if (idx == 0)
    data_start = buf;
  else
    data_start = buf + head[1].size;

  char *data_end = buf + head[0].size;

  *out_head = *head;
  out_head->size = data_end - data_start;

  *out_data = data_start;

  return true;
}

char *SepReqBufWrite::append_head(request_head head)
{
  assert(sizeof(head) + head.size <= len);

  if (left + head.size > right - sizeof(head))
    return NULL;

  auto buf_head = reinterpret_cast<request_head*>(buf + right - sizeof(head));
  *buf_head = head;

  // size is cumulative
  if (right != len)
    buf_head->size += buf_head[1].size;

  right -= sizeof(head);

  char *data = buf + left;
  left += head.size;

  return data;
}

bool SepReqBufWrite::append(request_head head, const char *data)
{
  char *bufptr = append_head(head);
  if (!bufptr)
    return false;

  memcpy(bufptr, data, head.size);
  return true;
}

} // namespace buddy
