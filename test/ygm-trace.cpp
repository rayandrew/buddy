#include <iostream>
#include <fstream>
#include <cstring>
#include <mpi.h>
#include <absl/container/flat_hash_map.h>
#include "json.hpp"
#include "buddy.h"
#include "util.h"
#include "util_mpi.h"
#include "request.h"

using json = nlohmann::json;

struct recv_meta {
  int count;
  int source;

  bool operator<(const recv_meta& x) const
  {
    if (source < x.source)
      return true;
    if (source > x.source)
      return false;
    if (count < x.count)
      return true;
    return false;
  }
};

const size_t MAXLEN = 1*1024*1024;
enum {
  ID_SEND,
  ID_RECV,
};

class TracePlayer {
  public:
    TracePlayer(int rank, bool validate, bool check_count)
      : rank(rank)
      , validate(validate)
      , check_count(check_count)
    {
      send_buf = buddy_alloc(MAXLEN);
      recv_buf = buddy_alloc(MAXLEN);

      writer = buddy::ReqBufWrite((char *)send_buf->addr, MAXLEN);

      //memset(send_buf, 0, MAXLEN);
      //memset(recv_buf, 0, MAXLEN);

      buddy_recv(recv_buf, MAXLEN, 0, ID_RECV);
    }

    ~TracePlayer()
    {
      buddy_free(send_buf);
      buddy_free(recv_buf);
    }

    bool poll()
    {
      uint64_t ids[2];
      size_t sizes[2];

      int n = buddy_poll(ids, sizes, 2);
      for (int i = 0; i < n; i++) {
        if (ids[i] == ID_SEND) {
          CHECK(send_block);
          writer.reset_pos();
          send_block = false;
        } else if (ids[i] == ID_RECV) {
          process_recv_buf(sizes[i]);
          buddy_recv(recv_buf, MAXLEN, 0, ID_RECV);
        } else {
          CHECK(false);
        }
      }

      return n > 0;
    }

    void process_recv_buf(size_t len)
    {
      buddy::ReqBufRead reader((char *)recv_buf->addr, len);
      buddy::request_head *head;
      char *data;

      while (reader.next(&head, &data)) {
        CHECK(head->dst == rank);
        actual_recv_count++;

        if (validate)
          actual_recv.push_back({.count = check_count ? head->size : 0});
      }
    }

    void flush()
    {
      if (writer.empty())
        return;

      send_block = true;
      buddy_send(send_buf, writer.get_pos(), 0, ID_SEND);
    }

    void play_send(int source, int dest, int count)
    {
      CHECK(source == rank);

      buddy::request_head head = {
        .size = count,
        .dst = dest,
      };

      while (send_block)
        poll();

      char *data = writer.append_head(head);
      if (!data) {
        flush();
        while (!poll() && !(data = writer.append_head(head)));
      }
      memset(data, 0xcc, count);
      actual_send_count++;
    }

    void play_recv(int source, int dest, int count)
    {
        CHECK(dest == rank);
        recv_meta r = {.count = count, .source = source};

        if (validate)
          expect_recv.push_back(r);
    }

    void ygm_barrier()
    {
      flush();

      for (;;) {
        while (poll());

        uint64_t local_counts[2] = {actual_send_count, actual_recv_count};
        uint64_t global_counts[2] = {};

        CHECK_MPI(MPI_Allreduce(local_counts, global_counts, 2, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD));
        if (global_counts[0] == global_counts[1])
          break;
      } 
    }

    void play_barrier(int send_count, int recv_count)
    {
      if (send_count != actual_send_count) {
        std::cerr << "wrong send count. expected " << send_count << " but got " << actual_send_count << std::endl;
        abort();
      }

      ygm_barrier();

      if (recv_count != actual_recv_count) {
        std::cerr << "wrong recv count. expected " << recv_count << " but got " << actual_recv_count << std::endl;
        abort();
      }

      check_recvs();
    }

    void check_recvs()
    {
      if (!validate)
        return;

      if (expect_recv.size() != actual_recv.size()) {
        std::cerr << "wrong recvs. expect list size is " << expect_recv.size() << " but actual list size is " << actual_recv.size() << std::endl;
        abort();
      }

      std::sort(expect_recv.begin(), expect_recv.end());
      std::sort(actual_recv.begin(), actual_recv.end());

      for (size_t i = 0; i < expect_recv.size(); i++) {
        if (expect_recv[i].source != actual_recv[i].source) {
          std::cerr << "mismatched recvs. expected source " << expect_recv[i].source << " but got " << actual_recv[i].source << std::endl;
          abort();
        }

        if (expect_recv[i].count != actual_recv[i].count) {
          std::cerr << "mismatched recvs. expected count " << expect_recv[i].count << " but got " << actual_recv[i].count << std::endl;
          abort();
        }
      }

      expect_recv.clear();
      actual_recv.clear();
    }

    void finalize()
    {
      ygm_barrier();
      check_recvs();
    }

  private:
    int rank;
    bool validate;
    bool check_count;

    std::vector<recv_meta> expect_recv;
    std::vector<recv_meta> actual_recv;

    buddy_buf *send_buf;
    buddy_buf *recv_buf;

    bool send_block = false;
    buddy::ReqBufWrite writer;

    int actual_send_count = 0;
    int actual_recv_count = 0;
};

void usage()
{
  std::cerr << "ygm-trace filename" << std::endl
            << "-x  enable validation" << std::endl
            << "-c  enable count check (implies validation)" << std::endl;
}

int main(int argc, char **argv)
{
  bool validate = false;
  bool check_count = false;

  int opt;
  while ((opt = getopt(argc, argv, "xch")) != -1) {
    switch (opt) {
      case 'x':
        validate = true;
        break;

      case 'c':
        check_count = true;
        validate = true;
        break;

      case 'h':
        usage();
        exit(0);
        break;

      default:
        usage();
        exit(1);
        break;
    }
  }

  CHECK_MPI(MPI_Init(&argc, &argv));

  int rank, num_ranks;
  CHECK_MPI(MPI_Comm_rank(MPI_COMM_WORLD, &rank));
  CHECK_MPI(MPI_Comm_size(MPI_COMM_WORLD, &num_ranks));

  int status = 0;

  if (argc - optind != 1) {
    if (rank == 0)
      usage();
    status = 1;
  } else {
    buddy_init(MPI_COMM_WORLD, MAXLEN, MAXLEN);

    std::ifstream ifs(argv[optind]);
    json jtrace = json::parse(ifs);

    absl::flat_hash_map<int, recv_meta> real_sends;

    if (check_count) {
      for (auto& line: jtrace) {
        auto args = line["args"];
        auto tid = line["tid"].template get<std::string>();
        if (!tid.compare("send")) {
          int id = std::stoi(line["id"].template get<std::string>());

          int count = std::stoi(args["message_size"].template get<std::string>());
          int source = std::stoi(args["from"].template get<std::string>());

          real_sends[id] = {.count = count, .source = source};
        }
      }
    }

    TracePlayer player(rank, validate, check_count);

    MPI_Barrier(MPI_COMM_WORLD);
    double t0 = MPI_Wtime();

    for (auto& line: jtrace) {
      int pid = std::stoi(line["pid"].template get<std::string>());
      if (pid != rank)
        continue;

      auto args = line["args"];
      auto tid = line["tid"].template get<std::string>();
      if (!tid.compare("barrier")) {
        auto trace_send = std::stoi(args["m_send_count"].template get<std::string>());
        auto trace_recv = std::stoi(args["m_recv_count"].template get<std::string>());

        if (validate)
          std::cout << "rank " << rank << " barrier id " << line["id"].template get<std::string>() << std::endl;

        player.play_barrier(trace_send, trace_recv);
      } else if (!tid.compare("send")) {
        int count = std::stoi(args["message_size"].template get<std::string>());
        int dest = std::stoi(args["to"].template get<std::string>());
        int source = std::stoi(args["from"].template get<std::string>());

        player.play_send(source, dest, count);
      } else if (!tid.compare("receive")) {
        int count = 0;
        if (check_count) {
          int id = std::stoi(line["id"].template get<std::string>());
          auto it = real_sends.find(id);
          if (it == real_sends.end())
            FAIL("find send for id " << id);
          count = it->second.count;
        }

        int dest = std::stoi(args["to"].template get<std::string>());
        int source = std::stoi(args["from"].template get<std::string>());

        player.play_recv(source, dest, count);
      } else {
        std::cerr << "unknown tid " << tid << std::endl;
        return 1;
      }
    }

    player.finalize();

    //MPI_Barrier(MPI_COMM_WORLD);
    double t1 = MPI_Wtime();
    if (rank == 0)
      std::cout << "time: " << t1-t0 << std::endl;
  }

  buddy_finalize();
  CHECK_MPI(MPI_Finalize());

  if (rank == 0 && status == 0 && validate)
    std::cout << "ok!" << std::endl;

  return status;
}
