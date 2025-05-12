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

  CHECK(insize % datasize == 0);

  unsigned num_out = 32;

#pragma omp parallel
  {
    char *inbuf = (char *)malloc(insize);
    memset(inbuf, 123, insize);

    buddy::ReqBufWrite inwriter(inbuf, insize);
    size_t incount = insize / (sizeof(buddy::request_head) + datasize);

    std::default_random_engine rand_engine(omp_get_thread_num());
    std::uniform_int_distribution<int32_t> dst_dist(0, num_out-1);

    size_t count_per_out[num_out] = {0};

    for (size_t i = 0; i < incount; i++) {
      buddy::request_head head = {
        .size = datasize,
        .dst = dst_dist(rand_engine),
      };
      count_per_out[head.dst]++;

      inwriter.append_head(head);
    }

    size_t max_out = count_per_out[0];
    for (unsigned i = 1; i < num_out; i++)
      if (count_per_out[i] > max_out)
        max_out = count_per_out[i];

    buddy::ReqBufWrite outwriters[num_out];

    char *outbufs[num_out];
    size_t out_len = max_out * (sizeof(buddy::request_head) + datasize);
    for (unsigned i = 0; i < num_out; i++) {
      outbufs[i] = (char *)malloc(out_len);
      memset(outbufs[i], 0, out_len);
      new (&outwriters[i]) buddy::ReqBufWrite(outbufs[i], out_len);
    }

    buddy::ReqBufRead inreader(inbuf, insize);

#pragma omp barrier
    auto t0 = std::chrono::high_resolution_clock::now();

    buddy::request_head *head;
    char *data;
    while (inreader.peek(&head, &data)) {
      [[maybe_unused]] bool ok = outwriters[head->dst].append(*head, data);
      assert(ok);
      inreader.advance(head);
    }

#pragma omp barrier
    auto t1 = std::chrono::high_resolution_clock::now();

#pragma omp master
    {
      int num_threads = omp_get_num_threads();

      double t = std::chrono::duration_cast<std::chrono::duration<double>>(t1-t0).count();
      double bw = num_threads * insize / t;
      double goodput = num_threads * (insize - incount * sizeof(buddy::request_head)) / t;

      std::cout << "time: " << t << " s" << std::endl;
      std::cout << "bw: " << bw/1e9 << " GB/s" << std::endl;
      std::cout << "goodput: " << goodput/1e9 << " GB/s" << std::endl;
    }
  }
}
