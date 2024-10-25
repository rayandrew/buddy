// Copyright (c) 2020, Institute for Defense Analyses
// 4850 Mark Center Drive, Alexandria, VA 22311-1882; 703-845-2500
//
// All rights reserved.
//
// This file is part of the conveyor package. For license information,
// see the LICENSE file in the top level directory of the distribution.


#include <inttypes.h>
#include <math.h>
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "bolite.h"

#include <mpi.h>
extern long xmpi_n_procs, xmpi_my_proc;
extern int xmpi_init(int argc, char* argv[]);
#define PROCS xmpi_n_procs
#define MY_PROC xmpi_my_proc
#define example_start() xmpi_init(argc,argv)
#define example_end() MPI_Finalize()

#include <buddy.h>
#include "request.h"
#include "util_mpi.h"

const size_t MAXLEN = 1*1024*1024;

long xmpi_my_proc = 0;
long xmpi_n_procs = 0;

int
xmpi_init(int argc, char* argv[])
{
  MPI_Init(&argc, &argv);

  int size, rank;
  MPI_Comm_size(MPI_COMM_WORLD, &size);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  xmpi_my_proc = rank;
  xmpi_n_procs = size;
  MPI_Barrier(MPI_COMM_WORLD);

  return argc;
}


int
main(int argc, char* argv[])
{
  example_start();

  // Parse command line and environment
  long bins = 10000;
  long load = 100000L;
  uint64_t seed = 1;
  if (argc > 1)
    bins = strtol(argv[1], NULL, 0);
  if (argc > 2)
    load = strtol(argv[2], NULL, 0);
  if (argc > 3)
    seed = strtoull(argv[3], NULL, 0);
  if (MY_PROC == 0)
    printf("command: %s %ld %ld %" PRIu64 "\n"
           "(parameters are: bins, load, seed)\n",
           argv[0], bins, load, seed);

  // Initialize local data
  brand_t _prng;
  brand_init(&_prng, (seed << 32) + MY_PROC);
  long* counts = (long*)calloc(bins, sizeof(long));
  long area = PROCS * bins;

  buddy_init(MPI_COMM_WORLD, MAXLEN, MAXLEN);

  int num_sendbuf = 1;
  int num_recvbuf = 1;
  char *env;

  env = getenv("BUDDY_SENDBUF");
  if (env && *env)
    num_sendbuf = atoi(env);

  // Bandwidth-delay product?
  env = getenv("BUDDY_RECVBUF");
  if (env && *env)
    num_recvbuf = atoi(env);

  buddy_buf *send_buf = buddy_alloc(num_sendbuf*MAXLEN);
  int curr_send = 0;
  bool send_busy[num_sendbuf] = {};

  buddy::ReqBufWrite writer((char *)send_buf->addr, MAXLEN);

  buddy_buf *recv_buf = buddy_alloc(num_recvbuf*MAXLEN);

  for (int i = 0; i < num_recvbuf; i++)
    buddy_recv(recv_buf, MAXLEN, i*MAXLEN, num_sendbuf+i);

  int status = EXIT_FAILURE;

  if (counts) {
    uint64_t send_count = 0;
    uint64_t recv_count = 0;

    MPI_Barrier(MPI_COMM_WORLD);
    double t0 = MPI_Wtime();
    /*** START OF CONVEYOR LOOP ***/
    long n = 0;

    long index = brand(&_prng) % area;
    for (;;) {
      bool send_block = false;

      for (; n < load; n++) {
        long payload = index / PROCS;
        long pe = index % PROCS;

        buddy::request_head head = {.size = sizeof(index), .dst = pe};
        char *data;
        while (!(data = writer.append_head(head))) {
          assert(!send_busy[curr_send]);
          buddy_send(send_buf, writer.get_pos(), curr_send*MAXLEN, curr_send);
          send_busy[curr_send] = true;

          send_block = true;
          for (int i = 1; i < num_sendbuf; i++) {
            int j = (curr_send + i) % num_sendbuf;
            if (!send_busy[j]) {
              curr_send = j;
              writer = buddy::ReqBufWrite((char *)send_buf->addr + curr_send*MAXLEN, MAXLEN);
              send_block = false;
              break;
            }
          }
          if (send_block)
            goto poll;
        }

        *(long *)data = payload;
        send_count++;

        index = brand(&_prng) % area;
      }

      if (n == load) {
        // Final send
        buddy_send(send_buf, writer.get_pos(), curr_send*MAXLEN, curr_send);
        send_busy[curr_send] = true;
      }

poll:
      MPI_Request count_req = MPI_REQUEST_NULL;
      uint64_t local_counts[2] = {};
      uint64_t global_counts[2] = {};

      while (send_block || n == load) {
        int num_buf = num_sendbuf + num_recvbuf;
        uint64_t ids[num_buf];
        size_t sizes[num_buf];
        int npoll = buddy_poll(ids, sizes, num_buf);
        for (int i = 0; i < npoll; i++) {
          if (ids[i] < num_sendbuf) {
            curr_send = ids[i];
            writer = buddy::ReqBufWrite((char *)send_buf->addr + curr_send*MAXLEN, MAXLEN);
            send_busy[curr_send] = false;
            send_block = false;
          } else {
            size_t offset = (ids[i] - num_sendbuf)*MAXLEN;
            buddy::ReqBufRead reader((char *)recv_buf->addr + offset, sizes[i]);

            buddy::request_head *head;
            char *data;
            while (reader.next(&head, &data, -1)) {
              assert(head->size == sizeof(long));
              long* local = (long *)data;
              counts[*local] += 1;
              recv_count++;
            }

            buddy_recv(recv_buf, MAXLEN, offset, ids[i]);
          }
        }

        if (n == load) {
          if (count_req == MPI_REQUEST_NULL) {
            local_counts[0] = send_count;
            local_counts[1] = recv_count;
            global_counts[0] = 0;
            global_counts[1] = 0;

            CHECK_MPI(MPI_Iallreduce(local_counts, global_counts, 2, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD, &count_req));
          }

          int flag;
          CHECK_MPI(MPI_Test(&count_req, &flag, MPI_STATUS_IGNORE));
          if (flag && global_counts[0] == global_counts[1])
            goto end;
        }
      }
    }
end:
    /*** END OF CONVEYOR LOOP ***/
    MPI_Barrier(MPI_COMM_WORLD);
    double t1 = MPI_Wtime();
    if (MY_PROC == 0) {
      printf("time: %lf\n", t1-t0);
    }

    status = EXIT_SUCCESS;

    // Produce a modest amount of output without further communication
    long peak = 0, where = 0;;
    for (long i = 0; i < bins; i++)
      if (counts[i] > peak) {
        peak = counts[i];
        where = i;
      }
    double lambda = load * 1.0 / bins;
    double tail = 0.0;
    for (long i = 0; i < 10; i++)
      tail += exp(-lambda + (peak + i) * log(lambda) - lgamma(peak + i + 1));
    if (tail * area < 4.0) {
      printf("RESULT: %ld[%ld] = %ld\n", (long)(MY_PROC), where, peak);
      fflush(stdout);
    }
  }
  buddy_free(send_buf);
  buddy_free(recv_buf);
  buddy_finalize();

  free(counts);

  example_end();
  exit(status);
}
