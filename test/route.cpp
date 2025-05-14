#include <chrono>
#include <random>
#include <cstdlib>
#include <iostream>
#include <string>
#include <cstring>
#include <omp.h>
#include "request.h"
#include "util.h"

int main(int argc, char **argv)
{
  size_t insize = 100*1024*1024;
  if (argc > 1)
    insize = std::strtoul(argv[1], NULL, 0);

  uint32_t datasize = 8;
  if (argc > 2)
    datasize = std::strtoul(argv[2], NULL, 0);

#ifdef FIXED
    size_t header_size = sizeof(int32_t);
#else
    size_t header_size = sizeof(buddy::request_head);
#endif
  insize -= insize % (header_size + datasize);
  CHECK(insize % (header_size + datasize) == 0);

  unsigned num_out = 32;

#pragma omp parallel
  {
    char *inbuf = (char *)malloc(insize);
    memset(inbuf, 123, insize);

    std::default_random_engine rand_engine(omp_get_thread_num());
    std::uniform_int_distribution<int32_t> dst_dist(0, num_out-1);
    size_t count_per_out[num_out] = {0};

#ifdef FIXED
    size_t header_size = sizeof(int32_t);
    char *bufptr = inbuf;
#else
    buddy::ReqBufWrite inwriter(inbuf, insize);
#endif
    size_t incount = insize / (header_size + datasize);

    for (size_t i = 0; i < incount; i++) {
      int32_t dst = dst_dist(rand_engine);
      count_per_out[dst]++;

#ifdef FIXED
      *((int32_t *)bufptr) = dst;
      bufptr += sizeof(int32_t);
      bufptr += datasize;
#else
      buddy::request_head head = {
        .size = datasize,
        .dst = dst,
      };
      inwriter.append_head(head);
#endif
    }

    size_t max_out = count_per_out[0];
    for (unsigned i = 1; i < num_out; i++)
      if (count_per_out[i] > max_out)
        max_out = count_per_out[i];

#ifdef FIXED
    size_t out_idx[num_out] = {0};
#else
    buddy::ReqBufWrite outwriters[num_out];
#endif

    char *outbufs[num_out];
    size_t out_len = max_out * (header_size + datasize);
    for (unsigned i = 0; i < num_out; i++) {
      outbufs[i] = (char *)malloc(out_len);
      memset(outbufs[i], 0, out_len);
#ifndef FIXED
      new (&outwriters[i]) buddy::ReqBufWrite(outbufs[i], out_len);
#endif
    }

#pragma omp barrier
    auto t0 = std::chrono::high_resolution_clock::now();

#ifdef FIXED
    size_t in_idx = 0;
    while (in_idx < insize) {
      int32_t dst = *((int32_t *)(inbuf+in_idx));

      *((int32_t *)(outbufs[dst] + out_idx[dst])) = dst;
      in_idx += header_size;
      out_idx[dst] += header_size;

      memcpy(outbufs[dst] + out_idx[dst], inbuf + in_idx, datasize);
      in_idx += datasize;
      out_idx[dst] += datasize;
    }
#else
    buddy::ReqBufRead inreader(inbuf, insize);
    buddy::request_head *head;
    char *data;
    while (inreader.peek(&head, &data)) {
      [[maybe_unused]] bool ok = outwriters[head->dst].append(*head, data);
      assert(ok);
      inreader.advance(head);
    }
#endif

#pragma omp barrier
    auto t1 = std::chrono::high_resolution_clock::now();

#pragma omp master
    {
      int num_threads = omp_get_num_threads();

      double t = std::chrono::duration_cast<std::chrono::duration<double>>(t1-t0).count();
      double bw = num_threads * insize / t;
      double goodput = num_threads * (insize - incount * header_size) / t;
      
      std::cout << insize << std::endl;
      std::cout << incount << std::endl;
      std::cout << header_size << std::endl;
      std::cout << std::endl;

      std::cout << "time: " << t << " s" << std::endl;
      std::cout << "bw: " << bw/1e9 << " GB/s" << std::endl;
      std::cout << "goodput: " << goodput/1e9 << " GB/s" << std::endl;
    }
  }
}
