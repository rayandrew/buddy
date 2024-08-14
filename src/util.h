#pragma once

#include <iostream>
#include <cstdlib>
#include <time.h>

#define FAIL(a) do { std::cerr << "FAIL: " << a << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; abort(); } while (0)
#define CHECK(a) do { if (!(a)) FAIL("check " #a); } while (0)
#define CHECK_DOCA(a) do { doca_error_t _err = a; if (_err != DOCA_SUCCESS) FAIL("doca " << doca_get_error_string(_err)); } while (0)

namespace buddy {

inline double clock()
{
  timespec t;
  CHECK(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
  return t.tv_sec + 1e-9*t.tv_nsec;
}

} // namespace buddy
