#include <mpi.h>
#include <buddy.h>
#include <request.h>
#include "util_mpi.h"

size_t pkgSize = 1*1024;
size_t MAXLEN = sizeof(buddy::request_head) + pkgSize;

enum {
  ID_SEND,
  ID_RECV
};


int bandwidthTest(bool isSender, int pkgNum, buddy_buf* sendBuf, buddy_buf* recvBuf);

int main(int argc, char **argv)
{
  CHECK_MPI(MPI_Init(&argc, &argv));

  int rank, size;
  CHECK_MPI(MPI_Comm_rank(MPI_COMM_WORLD, &rank));
  CHECK_MPI(MPI_Comm_size(MPI_COMM_WORLD, &size));

  CHECK(size % 2 == 0);
  bool isSend = rank < size / 2;
  int counterPart = isSend ? rank + size / 2 : rank - size / 2;

  buddy_init(MPI_COMM_WORLD, MAXLEN, MAXLEN);

  buddy_buf *send_buf = buddy_alloc(MAXLEN);
  buddy_buf *recv_buf = buddy_alloc(MAXLEN);

  buddy::request_head head = {.size = pkgSize, .dst = counterPart};
  memcpy(send_buf->addr, &head, sizeof(head));
  for (size_t i = 0; i < pkgSize; i++) {
    ((char *)send_buf->addr)[sizeof(head) + i] = 0xff;
  }

  {

    const int pkgNum = 512;

    bandwidthTest(isSend, pkgNum, send_buf, recv_buf);

    MPI_Barrier(MPI_COMM_WORLD);

    bandwidthTest(!isSend, pkgNum, send_buf, recv_buf);

  }

  buddy_free(recv_buf);
  buddy_free(send_buf);

  buddy_finalize();
  MPI_Finalize();
}



void wait_for(uint64_t id, size_t size)
{
  for (;;) {
    uint64_t ids[2];
    uint64_t sizes[2];
    int n = buddy_poll(ids, sizes, 2);
    for (int j = 0; j < n; j++) {
      if (ids[j] == id) {
        CHECK(id != ID_RECV || sizes[j] == size);
        return;
      }
    }
  }
}

auto bandwidthTestSender(int pkgNum, buddy_buf* sendBuf, buddy_buf* recvBuf) {
  auto start = MPI_Wtime();

  ((buddy::request_head *)sendBuf->addr)->size = pkgSize;

  for (int i = 0; i < pkgNum; i++) {
    buddy_send(sendBuf, sizeof(buddy::request_head) + pkgSize, 0, ID_SEND);
  }

  buddy_recv(recvBuf, MAXLEN, 0, ID_RECV);
  wait_for(ID_RECV, sizeof(buddy::request_head) + 1);
  auto end = MPI_Wtime();

  auto elapsed = end - start;
  return elapsed;
}


int bandwidthTestReceiver(int pkgNum, buddy_buf* sendBuf, buddy_buf* recvBuf) {

  int recvCount = 0;

  for(;;) {
    buddy_recv(recvBuf, MAXLEN, 0, ID_RECV);

    wait_for(ID_RECV, sizeof(buddy::request_head) + pkgSize);
    recvCount++;

    if(recvCount == pkgNum) {
      ((buddy::request_head *)sendBuf->addr)->size = 1;
      buddy_send(sendBuf, sizeof(buddy::request_head) + 1, 0, ID_SEND);
      break;
    }
  }

  wait_for(ID_SEND, 0); // ?

  return 0;

}


int bandwidthTest(bool isSender, int pkgNum, buddy_buf* sendBuf, buddy_buf* recvBuf) {
  MPI_Barrier(MPI_COMM_WORLD);

  if (isSender) {
    auto timeElapsed = bandwidthTestSender(pkgNum, sendBuf, recvBuf);

    auto loadInByte = pkgNum * (sizeof(buddy::request_head) + pkgSize);
    double bandwidth = loadInByte / timeElapsed / (1024 * 1024);  // MB/s
    std::cout << "Sender Bandwidth: " << bandwidth << " MB/s" << std::endl;

  } else {
    bandwidthTestReceiver(pkgNum, sendBuf, recvBuf);
  }

  return 0;
}

