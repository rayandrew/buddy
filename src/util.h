#pragma once

#include <iostream>
#include <cstdlib>
#include <time.h>
#include <string.h>

#define FAIL(a) do { std::cerr << "FAIL: " << a << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; abort(); } while (0)
#define WARN(a) do { std::cerr << "WARN: " << a << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; } while (0)
#define CHECK(a) do { if (!(a)) FAIL("check " #a); } while (0)
#define CHECK_ERRNO(a) do { if (!(a)) FAIL("check " #a " [errno=" << strerror(errno) << "]"); } while (0)
#define CHECK_ERR(a) do { int err = (a); if (err != 0) FAIL("check " #a " [error=" << strerror(err) << "]"); } while (0)
#define CHECK_DOCA(a) do { doca_error_t _err = a; if (_err != DOCA_SUCCESS) FAIL("doca " << doca_error_get_descr(_err)); } while (0)
#define CHECK_EQ(a, b) do { if ((a) != (b)) FAIL("check equal (" #a "; " #b ") i.e. (" << a << "; " << b << ")"); } while (0)
#define WARN_EQ(a, b) do { if ((a) != (b)) WARN("check equal (" #a "; " #b ") i.e. (" << a << "; " << b << ")"); } while (0)

namespace buddy {

inline double clock()
{
  timespec t;
  CHECK(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
  return t.tv_sec + 1e-9*t.tv_nsec;
}

// Depth of every RDMA queue: ibverbs SRQ/CQ/QP work-request caps, and the DOCA send queue. Both
// legs read this one value so a transport comparison is not a comparison of two defaults.
//
// It bounds bufcount_remote, which is a *count of posted receives*: d2d_depth is
// bufcount_remote * peers * threads, and exceeding this makes ibv_post_srq_recv return ENOMEM.
// Progress engines the DOCA fabric runs, independent of the routing thread count. DOCA has no
// shared completion queue, so every lane a thread owns is another engine to poll: at low traffic
// the empty polls cost more than the parallelism buys. 1 keeps routing multi-threaded on one engine.
inline unsigned fabric_lanes()
{
  static const unsigned n = [] {
    const char *e = getenv("BUDDY_FABRIC_LANES");
    const int v = (e && *e) ? atoi(e) : 0;
    return v > 0 ? (unsigned)v : 1u;
  }();
  return n;
}

inline unsigned queue_depth()
{
  static const unsigned n = [] {
    const char *e = getenv("BUDDY_QUEUE_DEPTH");
    const int v = (e && *e) ? atoi(e) : 0;
    return v > 0 ? (unsigned)v : 1000u;
  }();
  return n;
}

} // namespace buddy
