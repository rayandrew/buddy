#pragma once

#include <cstddef>
#include <cassert>
#include <stdint.h>
#include <atomic>
#include <algorithm>

namespace buddy {

struct request_head {
  uint32_t size;
  int32_t dst;
};

class ReqBufRead {
  public:
    ReqBufRead()
      : buf(NULL)
      , len(0)
      , pos(0)
    {}

    ReqBufRead(char *buf, size_t len)
      : buf(buf)
      , len(len)
      , pos(0)
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
};

class ReqBufWrite {
  public:
    ReqBufWrite() = default;

    ReqBufWrite(char *buf, size_t len)
      : buf(buf)
      , len(len)
      , pos(0)
    {}

    inline bool empty()
    {
      return pos == 0;
    }

    inline void reset_pos()
    {
      pos = 0;
    }

    inline size_t get_pos()
    {
      return pos;
    }

    char *append_head(request_head head);
    bool append(request_head head, const char *data);

  private:
    char *buf;
    size_t len;
    size_t pos;
};

class AtomicReqBufWrite {
  public:
    AtomicReqBufWrite() = default;

    AtomicReqBufWrite(char *buf, size_t len)
      : buf(buf)
      , len(len)
      , pos(0)
    {}

    inline bool empty()
    {
      return pos == 0;
    }

    inline void reset_pos()
    {
      pos = 0;
    }

    inline size_t get_pos()
    {
      return std::min(pos.load(), len);
    }

    char *append_head(request_head head);
    bool append(request_head head, const char *data);

  private:
    char *buf;
    size_t len;
    std::atomic_size_t pos;
};

class SepReqBufRead {
  public:
    SepReqBufRead()
      : buf(NULL)
      , len(0)
    {}

    SepReqBufRead(char *buf, size_t len, size_t count)
      : buf(buf)
      , len(len)
      , count(count)
    {}

    inline bool empty()
    {
      return count == 0;
    }

    bool get(size_t idx, request_head *head, char **data);

  private:
    char *buf;
    size_t len;
    size_t count;
};

class SepReqBufWrite {
  public:
    SepReqBufWrite() = default;

    SepReqBufWrite(char *buf, size_t len)
      : buf(buf)
      , len(len)
      , left(0)
      , right(len)
    {}

    inline bool empty()
    {
      return left == 0;
    }

    inline void reset()
    {
      left = 0;
      right = len;
    }

    char *append_head(request_head head);
    bool append(request_head head, const char *data);

  private:
    char *buf;
    size_t len;
    size_t left;
    size_t right;
};


} // namespace buddy
