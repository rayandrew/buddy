#pragma once

#include <cstddef>

namespace buddy {

struct request_head {
  size_t size;
  int src;
  int dst;
  int tag;
};

class ReqBufWrite {
  public:
    ReqBufWrite(char *buf, size_t len)
      : buf(buf)
      , len(len)
      , pos(0)
    {}

    bool append(request_head head, const char *data);

  private:
    const char *buf;
    size_t len;
    size_t pos;
};

class ReqBufRead {
  public:
    ReqBufRead()
      : buf(NULL)
      , len(0)
      , pos(0)
      , dst(0)
    {}

    ReqBufRead(char *buf, size_t len, int dst)
      : buf(buf)
      , len(len)
      , pos(0)
      , dst(dst)
    {}

    inline bool empty()
    {
      return len == 0;
    }

    inline void reset_pos()
    {
      pos = 0;
    }

    inline void reset_len(size_t new_len)
    {
      // No overwriting
      assert(pos == len);
      assert(len == 0 || new_len == 0);

      pos = 0;
      len = new_len;
    }

    bool next(request_head **head, char **data);

  private:
    char *buf;
    size_t len;
    size_t pos;
    int dst;
};

} // namespace buddy
