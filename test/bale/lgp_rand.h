#pragma once

#include <assert.h>

#define USE_KNUTH   /*!< Default define to set whether we use the Knuth random number generator or rand48 */
#ifdef USE_KNUTH
#define LGP_RAND_MAX 2251799813685248  /*!< max random number depends on which rng we use */
#include "knuth_rng_double_2019.h"
#else
#define LGP_RAND_MAX 281474976710656
#endif

/*! 
 * \brief seed for the random number generator
 * \param seed the seed
 * Note: if all thread call this with the same seed they actually get different seeds.
 */
inline void lgp_rand_seed(int64_t seed){
#ifdef USE_KNUTH
  ranf_start(seed + 1 + MYTHREAD);
#else
  srand48(seed + 1 + MYTHREAD);
#endif
}

/*! 
 * \brief return a random integer mod N.
 * \param N the modulus
 */
inline int64_t lgp_rand_int64(int64_t N){
  assert(N < LGP_RAND_MAX);
#ifdef USE_KNUTH
  return((int64_t)(ranf_arr_next()*N));
#else
  return((int64_t)(drand48()*N));
#endif
}

/*! 
 * \brief return a random double in the interval (0,1]
 */
inline double lgp_rand_double(){
#ifdef USE_KNUTH
  return(ranf_arr_next());
#else
  return(drand48());
#endif
}

