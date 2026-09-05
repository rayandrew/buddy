#include <chrono>
#include <mutex>
#include <utility>
#include <vector>

#ifdef MPI_VERSION
#include "util_mpi.h"
#endif

#ifdef TICTOC
// Per thread. These were plain statics, so every proxy timer above one thread was a data race:
// tt_calls undercounted and tt_duration mixed threads' intervals. That made the phase breakdown
// unusable at exactly the thread counts worth explaining. tt_total_* sums them at print time.
static thread_local std::chrono::time_point<std::chrono::high_resolution_clock> tt_start[TT_COUNT+1];
static thread_local std::chrono::high_resolution_clock::duration tt_duration[TT_COUNT+1];
static thread_local long long tt_calls[TT_COUNT+1];

// Every thread that has recorded anything, so the totals can be summed without knowing the count up
// front. Threads register once, on their first toc.
static std::mutex tt_reg_lock;
static std::vector<std::pair<std::chrono::high_resolution_clock::duration *, long long *>> tt_reg;
static thread_local bool tt_registered = false;

static inline void tt_register()
{
  if (tt_registered) return;
  tt_registered = true;
  std::lock_guard<std::mutex> lk(tt_reg_lock);
  tt_reg.emplace_back(tt_duration, tt_calls);
}

// Wall-clock sum across threads, which is what a phase costs the proxy as a whole. It can exceed
// the run time: four threads polling for a second spend four thread-seconds in poll.
static inline void tt_totals(double *secs, double *calls)
{
  std::lock_guard<std::mutex> lk(tt_reg_lock);
  for (int i = 0; i < TT_COUNT; i++) {
    std::chrono::high_resolution_clock::duration d{};
    double n = 0;
    for (auto &r : tt_reg) { d += r.first[i]; n += (double)r.second[i]; }
    secs[i] = std::chrono::duration_cast<std::chrono::duration<double>>(d).count();
    calls[i] = n;
  }
}
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
  tt_register();
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

  double all_secs[TT_COUNT], all_calls[TT_COUNT];
  tt_totals(all_secs, all_calls);
  double total_count = 0;
  for (int i = 0; i < TT_COUNT; i++)
    total_count += all_calls[i];

  auto total_cost = total_count * cost;
  auto t = std::chrono::duration_cast<std::chrono::duration<double>>(total_cost);
  return t.count();
}
#endif

static inline void tt_print(const char *title)
{
#ifdef TICTOC
  std::cout << "--- " << title << " ---" << std::endl;

  double secs[TT_COUNT], calls[TT_COUNT];
  tt_totals(secs, calls);
  for (int i = 0; i < TT_COUNT; i++)
    std::cout << tt_label[i] << '\t' << secs[i] << '\t' << (long long)calls[i] << std::endl;

  std::cout << "overhead" << '\t' << tt_overhead() << std::endl;
#endif
}

#ifdef MPI_VERSION
static inline void tt_print_mpi(const char *title)
{
#ifdef TICTOC
  double ts[TT_COUNT+1], tt_call_totals[TT_COUNT];
  tt_totals(ts, tt_call_totals);
  ts[TT_COUNT] = tt_overhead();

  double ts_sum[TT_COUNT+1];
  double ts_min[TT_COUNT+1];
  double ts_max[TT_COUNT+1];

  MPI_Reduce(ts, ts_sum, TT_COUNT+1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
  MPI_Reduce(ts, ts_min, TT_COUNT+1, MPI_DOUBLE, MPI_MIN, 0, MPI_COMM_WORLD);
  MPI_Reduce(ts, ts_max, TT_COUNT+1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);

  double fp_calls[TT_COUNT+1];
  for (int i = 0; i < TT_COUNT; i++)
    fp_calls[i] = tt_call_totals[i];

  double calls_sum[TT_COUNT];
  long long calls_min[TT_COUNT];
  long long calls_max[TT_COUNT];

  MPI_Reduce(fp_calls, calls_sum, TT_COUNT, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
  long long ll_calls[TT_COUNT];
  for (int i = 0; i < TT_COUNT; i++)
    ll_calls[i] = (long long)tt_call_totals[i];
  MPI_Reduce(ll_calls, calls_min, TT_COUNT, MPI_LONG_LONG, MPI_MIN, 0, MPI_COMM_WORLD);
  MPI_Reduce(ll_calls, calls_max, TT_COUNT, MPI_LONG_LONG, MPI_MAX, 0, MPI_COMM_WORLD);

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
