#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <cstring>
#include "request.h"
#include "util.h"

#ifdef _OPENMP
#include <omp.h>
#endif

int main(int argc, char **argv)
{
  srand(1);

  size_t insize = 100*1024*1024;
  if (argc > 1)
    insize = std::strtoul(argv[1], NULL, 0);

  uint32_t datasize = 8;
  if (argc > 2)
    datasize = std::strtoul(argv[2], NULL, 0);

  unsigned num_out = 32;

  size_t count_per_out[num_out] = {};

  [[maybe_unused]] int num_threads = 1;
#ifdef _OPENMP
#pragma omp parallel
#pragma omp master
  num_threads = omp_get_num_threads();
#endif

  char *inbuf = (char *)malloc(insize);
  memset(inbuf, 123, insize);

#ifdef INLINE
  buddy::ReqBufWrite inwriter(inbuf, insize);
#else
  buddy::SepReqBufWrite inwriter(inbuf, insize);
#endif
  size_t incount = insize / (sizeof(buddy::request_head) + datasize);

  for (size_t i = 0; i < incount; i++) {
    buddy::request_head head = {
      .size = datasize,
      .dst = (int32_t)(rand() % num_out),
    };
    count_per_out[head.dst]++;

    inwriter.append_head(head);
  }

  size_t max_out = count_per_out[0];
  for (unsigned i = 1; i < num_out; i++)
    if (count_per_out[i] > max_out)
      max_out = count_per_out[i];

#ifdef SEPARATE_OUT
  num_out *= num_threads;
#endif

#ifdef ATOMIC_OUT
  buddy::AtomicReqBufWrite outwriters[num_out];
#else
  buddy::ReqBufWrite outwriters[num_out];
#endif

  char *outbufs[num_out];
  size_t out_len = max_out * (sizeof(buddy::request_head) + datasize);
  for (unsigned i = 0; i < num_out; i++) {
    outbufs[i] = (char *)malloc(out_len);
    memset(outbufs[i], 0, out_len);
    new (&outwriters[i]) buddy::ReqBufWrite(outbufs[i], out_len);
  }

#ifdef INLINE
  buddy::ReqBufRead inreader(inbuf, insize);
#else
  buddy::SepReqBufRead inreader(inbuf, insize, incount);
#endif

  auto t0 = std::chrono::high_resolution_clock::now();

#pragma omp parallel
  {
    int off = 0;

#ifdef SEPARATE_OUT
    off = num_threads*omp_get_thread_num();
#endif


#pragma omp for
    for (unsigned i = 0; i < incount; i++) {
      buddy::request_head *head;
      char *data;
      [[maybe_unused]] bool ok;

#ifdef INLINE
      ok = inreader.next(&head, &data);
      assert(ok);
#else
      buddy::request_head the_head;
      ok = inreader.get(i, &the_head, &data);
      assert(ok);
      head = &the_head;
#endif

      ok = outwriters[head->dst + off].append(*head, data);
      assert(ok);
    }
  }

  auto t1 = std::chrono::high_resolution_clock::now();
  auto t = std::chrono::duration_cast<std::chrono::duration<double>>(t1-t0);
  double bw = insize / t.count();

  std::cout << "time: " << t.count() << " s" << std::endl;
  std::cout << "bw: " << bw/1e9 << " GB/s" << std::endl;

#if 0
  size_t total = 0;
#pragma omp parallel for reduction(+:total)
  for (unsigned i = 0; i < num_out; i++) {

#ifdef SEPARATE_OUT
    unsigned out_dst = i % num_threads;
#else
    unsigned out_dst = i;
#endif

    buddy::ReqBufRead reader(outbufs[i], out_len);

    unsigned filled = 0;
    buddy::request_head *head;
    char *data;
    for (unsigned j = 0; j < max_out; j++) {
      if (!reader.next(&head, &data))
        break;
      filled++;
      CHECK(head->dst == out_dst);
      for (unsigned i = 0; i < head->size; i++)
        CHECK(data[i] == 123);
    }
    CHECK(!reader.next(&head, &data));
    total += filled;
  }

  CHECK(total == incount);
#endif
}
