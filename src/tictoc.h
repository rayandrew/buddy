#include <chrono>

#ifdef MPI_VERSION
#include "util_mpi.h"
#endif

#ifdef TICTOC
static std::chrono::time_point<std::chrono::high_resolution_clock> tt_start[TT_COUNT+1];
static std::chrono::high_resolution_clock::duration tt_duration[TT_COUNT+1];
static long long tt_calls[TT_COUNT+1];
#endif

static inline void tic(tt_clock clock)
{
#ifdef TICTOC
  tt_start[clock] = std::chrono::high_resolution_clock::now();
#endif
}

static inline void toc(tt_clock clock)
{
#ifdef TICTOC
  auto end = std::chrono::high_resolution_clock::now();
  tt_duration[clock] += end - tt_start[clock];
  tt_calls[clock] += 1;
#endif
}

/*
class TicToc {
  public:
    TicToc(tt_clock clock)
      : clock(clock)
    {
      tic(clock);
    }

    ~TicToc()
    {
      toc(clock);
    }

  private:
    tt_clock clock;
};
*/

#ifdef TICTOC
static double tt_overhead()
{
  for (int i = 0; i < 1000000; i++) {
    tic(TT_COUNT);
    toc(TT_COUNT);
  }

  auto cost = tt_duration[TT_COUNT] / tt_calls[TT_COUNT];

  long long total_count = 0;
  for (int i = 0; i < TT_COUNT; i++)
    total_count += tt_calls[i];

  auto total_cost = total_count * cost;
  auto t = std::chrono::duration_cast<std::chrono::duration<double>>(total_cost);
  return t.count();
}
#endif

static inline void tt_print()
{
#ifdef TICTOC
  for (int i = 0; i < TT_COUNT; i++) {
    auto t = std::chrono::duration_cast<std::chrono::duration<double>>(tt_duration[i]);
    std::cout << tt_label[i] << '\t' << t.count() << '\t' << tt_calls[i] << std::endl;
  }

  std::cout << "overhead" << '\t' << tt_overhead() << std::endl;
#endif
}

#ifdef MPI_VERSION
static inline void tt_print_mpi(const char *title)
{
#ifdef TICTOC
  double ts[TT_COUNT+1];
  for (int i = 0; i < TT_COUNT; i++) {
    auto t = std::chrono::duration_cast<std::chrono::duration<double>>(tt_duration[i]);
    ts[i] = t.count();
  }
  ts[TT_COUNT] = tt_overhead();

  double ts_sum[TT_COUNT+1];
  double ts_min[TT_COUNT+1];
  double ts_max[TT_COUNT+1];

  MPI_Reduce(ts, ts_sum, TT_COUNT+1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
  MPI_Reduce(ts, ts_min, TT_COUNT+1, MPI_DOUBLE, MPI_MIN, 0, MPI_COMM_WORLD);
  MPI_Reduce(ts, ts_max, TT_COUNT+1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);

  double fp_calls[TT_COUNT+1];
  for (int i = 0; i < TT_COUNT; i++)
    fp_calls[i] = tt_calls[i];

  double calls_sum[TT_COUNT];
  long long calls_min[TT_COUNT];
  long long calls_max[TT_COUNT];

  MPI_Reduce(fp_calls, calls_sum, TT_COUNT, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
  MPI_Reduce(tt_calls, calls_min, TT_COUNT, MPI_LONG_LONG, MPI_MIN, 0, MPI_COMM_WORLD);
  MPI_Reduce(tt_calls, calls_max, TT_COUNT, MPI_LONG_LONG, MPI_MAX, 0, MPI_COMM_WORLD);

  int rank, size;
  CHECK_MPI(MPI_Comm_rank(MPI_COMM_WORLD, &rank));
  CHECK_MPI(MPI_Comm_size(MPI_COMM_WORLD, &size));

  if (rank == 0) {
    if (title)
      std::cout << "--- " << title << " ---" << std::endl;

    for (int i = 0; i < TT_COUNT; i++)
      std::cout
        << tt_label[i] << '\t'
        << ts_sum[i]/size << '\t'
        << ts_min[i] << '\t'
        << ts_max[i] << '\t'
        << calls_sum[i]/size << '\t'
        << calls_min[i] << '\t'
        << calls_max[i] << std::endl;

    std::cout << "overhead" << '\t'
      << ts_sum[TT_COUNT]/size << '\t'
      << ts_min[TT_COUNT] << '\t'
      << ts_max[TT_COUNT] << std::endl;
  }

#endif
}
#endif
