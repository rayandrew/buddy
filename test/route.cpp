#include <chrono>
#include <random>
#include <cstdlib>
#include <iostream>
#include <string>
#include <cstring>
#include <omp.h>
#include "request.h"
#include "util.h"

#ifdef SIMD
#include <immintrin.h>
#endif

const size_t in_header_size =
#ifdef FIXED
  sizeof(int32_t);
#else
  sizeof(buddy::request_head);
#endif

const size_t out_header_size =
#ifdef TERM
  0;
#else
  in_header_size;
#endif

size_t init_buf(char *inbuf, size_t incount, uint32_t datasize, unsigned num_out)
{
    std::default_random_engine rand_engine(omp_get_thread_num());
    std::uniform_int_distribution<int32_t> dst_dist(0, num_out-1);
    std::uniform_int_distribution<uint8_t> byte_dist(0, 255);
    size_t count_per_out[num_out] = {0};

#ifdef FIXED
    char *bufptr = inbuf;
#else
    size_t insize = incount * (in_header_size + datasize);
    buddy::ReqBufWrite inwriter(inbuf, insize);
#endif

    uint8_t *data = (uint8_t*)malloc(datasize);

    for (size_t i = 0; i < incount; i++) {
      int32_t dst = dst_dist(rand_engine);
      count_per_out[dst]++;

      for (uint32_t j = 0; j < datasize; j++)
        data[j] = byte_dist(rand_engine);

      /*
      for (uint32_t j = 0; j < datasize; j++)
        printf("%.2X ", data[j]);
      printf("\n");
      */

#ifdef FIXED
      *((int32_t *)bufptr) = dst;
      bufptr += sizeof(int32_t);
      memcpy(bufptr, data, datasize);
      bufptr += datasize;
#else
      buddy::request_head head = {
        .size = datasize,
        .dst = dst,
      };
      inwriter.append(head, (char *)data);
#endif
    }

    free(data);

    size_t max_out = count_per_out[0];
    for (unsigned i = 1; i < num_out; i++)
      if (count_per_out[i] > max_out)
        max_out = count_per_out[i];

    return max_out;
}

int main(int argc, char **argv)
{
  size_t incount = 10000000;

  if (argc > 1)
    incount = std::strtoul(argv[1], NULL, 0);

  uint32_t datasize = 8;
  if (argc > 2)
    datasize = std::strtoul(argv[2], NULL, 0);

  size_t insize = incount * (in_header_size + datasize);
  unsigned num_out = 32;

  FILE *proc = NULL;

#pragma omp parallel
  {
    char *inbuf = (char *)malloc(insize);
    size_t max_out = init_buf(inbuf, incount, datasize, num_out);

#ifdef FIXED
    size_t out_idx[num_out] = {0};
#else
    buddy::ReqBufWrite outwriters[num_out];
#endif

    char *outbufs[num_out];
    size_t out_len = max_out * (out_header_size + datasize);
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
      in_idx += sizeof(int32_t);

#ifndef TERM
      *((int32_t *)(outbufs[dst] + out_idx[dst])) = dst;
      out_idx[dst] += out_header_size;
#endif

      memcpy(outbufs[dst] + out_idx[dst], inbuf + in_idx, datasize);
      in_idx += datasize;
      out_idx[dst] += datasize;
    }
#else
    buddy::ReqBufRead inreader(inbuf, insize);
    buddy::request_head *head;
    char *datap;
    while (inreader.peek(&head, &datap)) {
      [[maybe_unused]] bool ok = outwriters[head->dst].append(*head, datap);
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
      double goodput = num_threads * (insize - incount * in_header_size) / t;
      double rate = num_threads * incount / t;
      
      std::cout << "time: " << t << " s" << std::endl;
      std::cout << "bw: " << bw/1e9 << " GB/s" << std::endl;
      std::cout << "goodput: " << goodput/1e9 << " GB/s" << std::endl;
      std::cout << "rate: " << rate/1e6 << " Mmsgs/s" << std::endl;
      std::cout << std::endl;
    }

    /* checksum for validation */
#pragma omp master
    {
      proc = popen("sha1sum", "w");
      CHECK(proc);
    }

    for (int thread = 0; thread < omp_get_num_threads(); thread++) {
#pragma omp barrier
      if (thread == omp_get_thread_num()) {
        for (unsigned i = 0; i < num_out; i++) {
          size_t n;
#ifdef FIXED
          n = out_idx[i];
#else
          n = outwriters[i].get_pos();
#endif
          size_t m = n / (out_header_size + datasize);
          // printf("%zu = %zu\n", n, m);
          CHECK(fwrite(&m, sizeof(m), 1, proc));

          buddy::request_head *head;
          char *data;
#ifdef FIXED
          buddy::request_head stack_head;
          head = &stack_head;
          for (size_t idx = 0; idx < out_idx[i];) {
            head->size = datasize;

#ifdef TERM
            head->dst = i;
#else
            head->dst = *((int32_t *)(outbufs[i]+idx));
            idx += sizeof(int32_t);
#endif

            data = outbufs[i] + idx;
            idx += datasize;
#else
          buddy::ReqBufRead reader(outbufs[i], n);
          while (reader.next(&head, &data)) {
#endif

            /*
            for (uint32_t j = 0; j < datasize; j++)
              printf("%.2X ", (uint8_t)data[j]);
            printf("\n");
            */

            CHECK(fwrite(&head->size, sizeof(head->size), 1, proc));
            CHECK(fwrite(&head->dst, sizeof(head->dst), 1, proc));
            CHECK(fwrite(data, datasize, 1, proc));
          }
        }
      }
    }

#pragma omp barrier
#pragma omp master
    CHECK(pclose(proc) == 0);

    for (unsigned i = 0; i < num_out; i++)
      free(outbufs[i]);

    free(inbuf);
  }
}
