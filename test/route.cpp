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

#if defined(SIMD) && defined(FIXED)
void route_simd4(char *out_buf, size_t real_out_idx[], size_t out_len, char *inbuf, size_t incount, unsigned num_out)
{
  size_t out_els = out_len/(4+out_header_size);
  CHECK(out_els*num_out <= INT32_MAX);
  __m512i vout_els = _mm512_set1_epi32(out_els);

  const size_t elsize = in_header_size + 4;
  const size_t chunksize = 16;

  alignas(64) int32_t out_idx[num_out] = {};

  for (size_t chunk_ind = 0; chunk_ind < incount+1-chunksize; chunk_ind += chunksize) {
    alignas(64) int32_t dst_chunk[chunksize];
    alignas(64) int32_t data_chunk[chunksize];

    char *chunk = inbuf + chunk_ind*elsize;

    // Maybe this can be smarter
    for (size_t i = 0; i < chunksize; i++)
      dst_chunk[i] = *((int32_t *)(chunk + i*elsize));
    for (size_t i = 0; i < chunksize; i++)
      data_chunk[i] = *((int32_t *)(chunk + i*elsize + in_header_size));

    __m512i vdst = _mm512_load_epi32(dst_chunk);
    __m512i vdata = _mm512_load_epi32(data_chunk);

    __m512i vout_idx = _mm512_i32gather_epi32(vdst, out_idx, 4);

    __m512i vconflict = _mm512_conflict_epi32(vdst);
    __m512i vcount = _mm512_popcnt_epi32(vconflict);

    __m512i voff_base = _mm512_mullo_epi32(vdst, vout_els);
    __m512i voff_idx = _mm512_add_epi32(voff_base, vout_idx);
    __m512i voff_adj = _mm512_add_epi32(voff_idx, vcount);

#ifdef TERM
    _mm512_i32scatter_epi32(out_buf, voff_adj, vdata, 4);
#else
    // correct order?
    __m256i voff_adj_lo = _mm512_extracti64x4_epi64(voff_adj, 0);
    __m256i voff_adj_hi = _mm512_extracti64x4_epi64(voff_adj, 1);

    __m512i vpack_lo = _mm512_unpacklo_epi32(vdst, vdata);
    __m512i vpack_hi = _mm512_unpackhi_epi32(vdst, vdata);

    _mm512_i32scatter_epi64(out_buf, voff_adj_lo, vpack_lo, 8);
    _mm512_i32scatter_epi64(out_buf, voff_adj_hi, vpack_hi, 8);
#endif

    // Potentially we could reorder a bit and scatter vout_idx+vcount back into
    // out_idx instead of this loop?
    // "writes to overlapping vector indices are guaranteed to be ordered with respect to each other (from LSB to MSB of the source registers)"
    // "If two or more destination indices completely overlap, the “earlier” write(s) may be skipped."
    // https://www.felixcloutier.com/x86/vpscatterdd:vpscatterdq:vpscatterqd:vpscatterqq
    for (size_t i = 0; i < chunksize; i++)
      out_idx[dst_chunk[i]] += 1;
  }

  size_t remain = incount % chunksize;
  if (remain) {
    CHECK(!"TODO");
  }

  for (unsigned i = 0; i < num_out; i++)
    real_out_idx[i] = out_idx[i];
}

void route_simd8(char *out_buf, size_t out_idx[], size_t out_len, char *inbuf, size_t incount)
{
#ifndef TERM
  CHECK(!"TODO");
#endif

  CHECK(out_len < INT32_MAX);
  __m512i vout_len_data = _mm512_set1_epi64(out_len/8);

  size_t elsize = in_header_size + 8;

  for (size_t chunk_ind = 0; chunk_ind < incount-7; chunk_ind += 8) {
    alignas(64) uint64_t dst_chunk[8];
    alignas(64) uint64_t data_chunk[8];

    char *chunk = inbuf + chunk_ind*elsize;

    for (int i = 0; i < 8; i++)
      dst_chunk[i] = *((int32_t *)(chunk + i*elsize));
    for (int i = 0; i < 8; i++)
      data_chunk[i] = *((uint64_t *)(chunk + i*elsize + in_header_size));

    __m512i vdst = _mm512_load_epi64(dst_chunk);
    __m512i vdata = _mm512_load_epi64(data_chunk);

    __m512i vout_idx = _mm512_i64gather_epi64(vdst, out_idx, 8);

    __m512i vconflict = _mm512_conflict_epi64(vdst);
    __m512i vcount = _mm512_popcnt_epi64(vconflict);

    __m512i voff_base = _mm512_mul_epi32(vdst, vout_len_data);
    __m512i voff_idx = _mm512_add_epi64(voff_base, vout_idx);
    __m512i voff_adj = _mm512_add_epi64(voff_idx, vcount);

    _mm512_i64scatter_epi64(out_buf, voff_adj, vdata, 8);

    for (int i = 0; i < 8; i++)
      out_idx[dst_chunk[i]] += 1;
  }

  size_t remain = incount % 8;
  if (remain) {
    CHECK(!"TODO");
  }

}
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

#ifndef SIMD
// noinline: gives the timed loop a symbol to attach per-function PMU to. One call per loop.
#ifdef FIXED
__attribute__((noinline)) static void route_kernel(char **outbufs, size_t out_idx[], char *inbuf,
                                                   size_t insize, uint32_t datasize)
{
#ifdef DATASIZE_CONST
  // compile-time size lets gcc fold memcpy into a single load/store
  const uint32_t ds = DATASIZE_CONST;
  (void)datasize;
#else
  const uint32_t ds = datasize;
#endif
  size_t in_idx = 0;
  while (in_idx < insize) {
    int32_t dst = *((int32_t *)(inbuf+in_idx));
    in_idx += sizeof(int32_t);

#ifndef TERM
    *((int32_t *)(outbufs[dst] + out_idx[dst])) = dst;
    out_idx[dst] += out_header_size;
#endif

    memcpy(outbufs[dst] + out_idx[dst], inbuf + in_idx, ds);
    in_idx += ds;
    out_idx[dst] += ds;
  }
}
#else
__attribute__((noinline)) static void route_kernel(buddy::ReqBufWrite outwriters[], char *inbuf,
                                                   size_t insize)
{
  buddy::ReqBufRead inreader(inbuf, insize);
  buddy::request_head *head;
  char *datap;
  while (inreader.peek(&head, &datap)) {
    [[maybe_unused]] bool ok = outwriters[head->dst].append(*head, datap);
    assert(ok);
    inreader.advance(head);
  }
}
#endif
#endif

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
    init_buf(inbuf, incount, datasize, num_out);
    size_t max_out = incount;

#ifdef FIXED
    size_t out_idx[num_out] = {0};
#else
    buddy::ReqBufWrite outwriters[num_out];
#endif

    size_t out_len = max_out * (out_header_size + datasize);
    char *buf = (char *)malloc(out_len * num_out);
    memset(buf, 0, out_len * num_out);

    char *outbufs[num_out];
    for (unsigned i = 0; i < num_out; i++) {
      outbufs[i] = buf + out_len*i;
#ifndef FIXED
      new (&outwriters[i]) buddy::ReqBufWrite(outbufs[i], out_len);
#endif
    }

#pragma omp barrier
    auto t0 = std::chrono::high_resolution_clock::now();

#ifdef SIMD
    switch (datasize) {
      case 4:
        route_simd4(outbufs[0], out_idx, out_len, inbuf, incount, num_out);
        break;
      case 8:
        route_simd8(outbufs[0], out_idx, out_len, inbuf, incount);
        break;
      default:
        CHECK(!"not implemented");
    }

    for (unsigned i = 0; i < num_out; i++)
      out_idx[i] *= out_header_size + datasize;

#else
#ifdef FIXED
    route_kernel(outbufs, out_idx, inbuf, insize, datasize);
#else
    route_kernel(outwriters, inbuf, insize);
#endif
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
      if (thread != omp_get_thread_num())
        continue;

      for (unsigned i = 0; i < num_out; i++) {
//printf("%u\t", i);
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
//printf("%d:%d\t", head->dst, *(int*)data);
        }
//printf("\n");
      }
    }

#pragma omp barrier
#pragma omp master
    CHECK(pclose(proc) == 0);

    free(outbufs[0]);
    free(inbuf);
  }
}
