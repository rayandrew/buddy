#include <iostream>
#include <fstream>
#include <cstring>
#include <mpi.h>
#include "util_mpi.h"
#include "json.hpp"


#include <unistd.h>

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
    // TODO count
    return false;
  }
};

const size_t MAXLEN = 1*1024*1024;

class Tracer {
  public:
    Tracer(int rank)
      : rank(rank)
    {
      send_buf = new char[MAXLEN];
      recv_buf = new char[MAXLEN];

      memset(send_buf, 0, MAXLEN);
      memset(recv_buf, 0, MAXLEN);

      start_recv();
    }

    void start_recv()
    {
      CHECK_MPI(MPI_Irecv(recv_buf, MAXLEN, MPI_BYTE, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &recv_req));
    }

    bool poll_recv()
    {
      MPI_Status stat = {};
      int flag;
      CHECK_MPI(MPI_Test(&recv_req, &flag, &stat));

      if (!flag)
        return false;

      start_recv();
      actual_recv_count++;

      int count;
      CHECK_MPI(MPI_Get_count(&stat, MPI_BYTE, &count));

      CHECK(stat.MPI_SOURCE >= 0);

      actual_recv.push_back({.count = count, .source = stat.MPI_SOURCE});

      return true;
    }

    void trace_send(int source, int dest, int count)
    {
        CHECK(source == rank);

        MPI_Request req;
        CHECK_MPI(MPI_Isend(send_buf, count, MPI_BYTE, dest, 0, MPI_COMM_WORLD, &req));
        CHECK_MPI(MPI_Wait(&req, MPI_STATUS_IGNORE));
        actual_send_count++;
    }

    void trace_recv(int source, int dest, int count)
    {
        CHECK(dest == rank);
        recv_meta r = {.count = count, .source = source};
        expect_recv.push_back(r);
    }

    void trace_barrier(int send_count, int recv_count)
    {
        if (send_count != actual_send_count) {
          std::cerr << "wrong send count. expected " << send_count << " but got " << actual_send_count << std::endl;
          abort();
        }

        MPI_Barrier(MPI_COMM_WORLD);

        // Do we need a timeout?
        sleep(1);
        while (poll_recv());

        if (recv_count != actual_recv_count) {
          std::cerr << "wrong recv count. expected " << recv_count << " but got " << actual_recv_count << std::endl;
          abort();
        }

        check_recvs();

        MPI_Barrier(MPI_COMM_WORLD);
    }

    void check_recvs()
    {
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
        // TODO check count.
      }

      expect_recv.clear();
      actual_recv.clear();
    }

    void finalize()
    {
        while (poll_recv());
        check_recvs();
    }

  private:
    int rank;

    std::vector<recv_meta> expect_recv;
    std::vector<recv_meta> actual_recv;

    char *send_buf;
    char *recv_buf;

    MPI_Request recv_req;

    int actual_send_count = 0;
    int actual_recv_count = 0;
};

int main(int argc, char **argv)
{
  CHECK_MPI(MPI_Init(&argc, &argv));

  int rank, size;
  CHECK_MPI(MPI_Comm_rank(MPI_COMM_WORLD, &rank));
  CHECK_MPI(MPI_Comm_size(MPI_COMM_WORLD, &size));

  int status = 0;

  if (argc != 2) {
    if (rank == 0)
      std::cerr << "usage: ygm-trace filename" << std::endl;
    status = 1;
  } else {
    /*
    if (rank == 0) {
      std::cout << "pid: " << getpid() << std::endl;
      sleep(5);
    }
    */

    std::ifstream ifs(argv[1]);
    json jtrace = json::parse(ifs);

    Tracer tracer(rank);

    int lineno = 0;
    for (auto& line: jtrace) {
      lineno++;

      int pid = std::stoi(line["pid"].template get<std::string>());
      if (pid != rank)
        continue;

      auto args = line["args"];
      auto tid = line["tid"].template get<std::string>();
      if (!tid.compare("barrier")) {
        auto trace_send = std::stoi(args["m_send_count"].template get<std::string>());
        auto trace_recv = std::stoi(args["m_recv_count"].template get<std::string>());

        std::cout << "rank " << rank << " barrier id " << line["id"].template get<std::string>() << std::endl;
        tracer.trace_barrier(trace_send, trace_recv);
      } else if (!tid.compare("send")) {
        // TODO count
        //int count = std::stoi(args["message_size"].template get<std::string>());
        int count = 0;
        int dest = std::stoi(args["to"].template get<std::string>());
        int source = std::stoi(args["from"].template get<std::string>());

        tracer.trace_send(source, dest, count);
      } else if (!tid.compare("receive")) {
        //int count = std::stoi(args["message_size"].template get<std::string>());
        int count = 0;
        int dest = std::stoi(args["to"].template get<std::string>());
        int source = std::stoi(args["from"].template get<std::string>());

        tracer.trace_recv(source, dest, count);
      } else {
        std::cerr << "unknown tid " << tid << std::endl;
        return 1;
      }

      while (tracer.poll_recv());
    }

    tracer.finalize();
  }

  CHECK_MPI(MPI_Finalize());

  if (rank == 0 && status == 0)
    std::cout << "ok!" << std::endl;

  return status;
}
