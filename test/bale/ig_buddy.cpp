/******************************************************************
//
//
//  Copyright(C) 2019-2020, Institute for Defense Analyses
//  4850 Mark Center Drive, Alexandria, VA; 703-845-2500
// 
//
//  All rights reserved.
//  
//   This file is a part of Bale.  For license information see the
//   LICENSE file in the top level directory of the distribution.
//  
// 
 *****************************************************************/ 

/*! \file ig_conveyor.upc
 * \brief A conveyor implementation of indexgather.
 */
#include "ig.h"
#include "buddy_convey.h"
#include "compat.h"
#include <buddy.h>
#include <assert.h>
#include <inttypes.h>

/*!
 * \brief This routine implements the buddy variant of indexgather.
 * \param *tgt array of target locations for the gathered values
 * \param *pckindx array of packed indices for the distributed version of the global array of counts.
 * \param l_num_req the length of the pcindx array
 * \param *ltable localized pointer to the count array.
 * \return average run time
 *
 */
double ig_buddy(int64_t *tgt, int64_t *pckindx, int64_t l_num_req,  int64_t *ltable) {
  double tm;
  int64_t pe;
  int64_t i = 0;
  //minavgmaxD_t stat[1];

  // TODO: omit pe in reply pkg?
  typedef struct pkg_t {
    int64_t idx;
    int64_t val;
    int64_t pe;
  } pkg_t;
  pkg_t pkg;
  pkg_t *ptr = (pkg_t*)calloc(1, sizeof(pkg_t));
  
  convey_t* c = convey_new(SIZE_MAX, 0, NULL, 0);
  assert( c != NULL );

  convey_begin(c, sizeof(pkg_t), 0);
  lgp_barrier();
  
  tm = wall_seconds();

  i = 0;
  int64_t outstanding = 0;
  //int64_t max_outstanding = INT64_MAX;

  // We need to ensure to have space for replies in the send buffer.
  // A better way to do it might be to track the number of requests in the current send buffer instead?
  const size_t BUDDY_MAXLEN = 1*1024*1024;
  size_t BUDDY_HEADER_SIZE = 8;
  size_t msgsize = BUDDY_HEADER_SIZE + sizeof(pkg);
  int64_t max_outstanding = BUDDY_MAXLEN/msgsize/2;

  while (convey_advance(c, (i == l_num_req || outstanding == max_outstanding))) {
    for (; i < l_num_req && outstanding < max_outstanding; i++) {
      pkg.idx = i;
      pkg.val = pckindx[i] >> 16;
      pkg.pe = MYTHREAD;
      pe = pckindx[i] & 0xffff;
      if (! convey_push(c, &pkg, pe))
        break;
      //printf("request to %" PRId64 " (return to %" PRId64 ")\n", pe, pkg.pe);
      outstanding++;
    }

    while (convey_pull(c, ptr, NULL) == convey_OK) {
      if (ptr->pe == -MYTHREAD-1) {
        tgt[ptr->idx] = ptr->val;
        outstanding--;
        //printf("got reply\n");
      } else {
        pkg.idx = ptr->idx;
        pkg.val = ltable[ptr->val];
        pkg.pe = -ptr->pe-1;
        if (! convey_push(c, &pkg, ptr->pe)) {
          convey_unpull(c);
          break;
        }
        //printf("reply to %" PRId64 "\n", ptr->pe);
      }
    }
  }

  tm = wall_seconds() - tm;
  free(ptr);
  lgp_barrier();

  double sum;
  CHECK_MPI(MPI_Allreduce(&tm, &sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD));
  //lgp_min_avg_max_d( stat, tm, THREADS );
  convey_free(c);
  return(sum / THREADS);
}
