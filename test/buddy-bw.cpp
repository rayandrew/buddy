#include <mpi.h>
#include <buddy.h>
#include <request.h>
#include "util_mpi.h"

size_t payloadSize = -1;
size_t pktSize = -1;
const int windowSize = 64;
const int aggregation = 256;
size_t MAXLEN = -1;

enum {
  ID_SEND,
  ID_RECV
};

int rank, size;

int bandwidthTest(bool isSender, int counterPart, int pktNum);

int main(int argc, char **argv)
{
  if (argc != 2){
    std::cerr << "Usage: " << argv[0] << " <payloadSize>\n";
    return 1;
  } else {
    payloadSize = std::stoul(argv[1]);
    pktSize = sizeof(buddy::request_head) + payloadSize;
    MAXLEN = pktSize * aggregation;
  }

  CHECK_MPI(MPI_Init(&argc, &argv));
  CHECK_MPI(MPI_Comm_rank(MPI_COMM_WORLD, &rank));
  CHECK_MPI(MPI_Comm_size(MPI_COMM_WORLD, &size));

  CHECK(size % 2 == 0);
  bool isSend = rank < size / 2;
  int counterPart = isSend ? rank + size / 2 : rank - size / 2;

  buddy_init(MPI_COMM_WORLD, MAXLEN, MAXLEN);
  

  {
    const int pktNum = 128 * 1024; 
    const int buddyPktNum = pktNum / aggregation;

    CHECK(buddyPktNum >= windowSize);

    if(isSend)std::cout << "[*]Packet Size: " << pktSize   << " Payload Size: " << payloadSize
                        << " Packet Num: " << pktNum       << " Aggregation: " << aggregation
                        << " Window Size: " << windowSize   << " MAXLEN: " << MAXLEN << std::endl;

    bandwidthTest(isSend, counterPart, pktNum);
    MPI_Barrier(MPI_COMM_WORLD);
    bandwidthTest(!isSend, counterPart, pktNum);
  }


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

auto bandwidthTestSender(const int pktNum, buddy_buf* sendBuf, buddy_buf* recvBuf) {
  auto start = MPI_Wtime();

  int sentRegisterPktNum = 0;

  for (int i = 0; i < windowSize; i++) buddy_send(sendBuf, pktSize*aggregation, 0, ID_SEND);
  sentRegisterPktNum = windowSize * aggregation;

  uint64_t ids[windowSize];
  uint64_t sizes[windowSize];

  for(;sentRegisterPktNum < pktNum;) {
    int n = buddy_poll(ids, sizes, windowSize);

    for (int j = 0; j < n; j++) {
      if(ids[j] == ID_SEND){
        if (sentRegisterPktNum < pktNum) {
          const int pktSendThisTime = (pktNum-sentRegisterPktNum) >= aggregation ? aggregation : (pktNum-sentRegisterPktNum);
          buddy_send(sendBuf, pktSendThisTime * pktSize, 0, ID_SEND);
          sentRegisterPktNum += pktSendThisTime;
          if (sentRegisterPktNum == pktNum) {
            break;
          }
        }
      }
      else CHECK(false);
    }

  }

  buddy_recv(recvBuf, MAXLEN, 0, ID_RECV);
  wait_for(ID_RECV, sizeof(buddy::request_head) + 1);
  auto end = MPI_Wtime();

  auto elapsed = end - start;
  return elapsed;
}


int bandwidthTestReceiver(const int pktNum, buddy_buf* sendBuf, buddy_buf* recvBuf) {

  // int recvRegisterPktNum = 0;
  int recvPktCount = 0;

  for (int i = 0; i < windowSize; i++) buddy_recv(recvBuf, pktSize*aggregation, 0, ID_RECV);
  // recvRegisterPktNum = windowSize * aggregation;

  uint64_t ids[windowSize];
  uint64_t sizes[windowSize];

  for(;recvPktCount < pktNum;) {

    int n = buddy_poll(ids, sizes, windowSize);

    for (int j = 0; j < n; j++) {
      if (ids[j] == ID_RECV) {
        CHECK(sizes[j] % pktSize == 0);
        recvPktCount += sizes[j] / pktSize;
      } else {
        CHECK(false);
      }

      // really don't know how to exactly recv the pktNum
      buddy_recv(recvBuf, MAXLEN, 0, ID_RECV);
    }

  }

  ((buddy::request_head *)sendBuf->addr)->size = 1;
  buddy_send(sendBuf, sizeof(buddy::request_head) + 1, 0, ID_SEND);

  wait_for(ID_SEND, 0); // keep the send buf valid

  return 0;

}


int bandwidthTest(bool isSender, int counterPart, int pktNum) {
  MPI_Barrier(MPI_COMM_WORLD);

  buddy_buf *sendBuf = nullptr;
  buddy_buf *recvBuf = nullptr;

  double senderBW = 0.0;

  if (isSender) {
    // buf preparation
    sendBuf = buddy_alloc(MAXLEN);
    recvBuf = buddy_alloc(1024); // for ack only

    buddy::request_head head = {.size = (uint32_t)payloadSize, .dst = counterPart};
    for (int j = 0; j < aggregation; j++) {
      memcpy((char *)sendBuf->addr + j*pktSize, &head, sizeof(head));
      for (size_t i = 0; i < payloadSize; i++) {
        ((char *)sendBuf->addr)[j*pktSize + sizeof(head) + i] = 0xff;
      }
    }

    auto timeElapsed = bandwidthTestSender(pktNum, sendBuf, recvBuf);

    auto loadInByte = pktNum * (sizeof(buddy::request_head) + payloadSize);
    senderBW = loadInByte / timeElapsed / (1024 * 1024);  // MB/s
    std::cout << "Sender " << rank << " Bandwidth: " << senderBW << " MB/s" << std::endl;

  } else {
    sendBuf = buddy_alloc(1024); // for ack only
    recvBuf = buddy_alloc(MAXLEN);

    buddy::request_head head = {.size = (uint32_t)payloadSize, .dst = counterPart};
    memcpy((char *)sendBuf->addr, &head, sizeof(head));
    ((char *)sendBuf->addr)[sizeof(head)] = 0xff;

    bandwidthTestReceiver(pktNum, sendBuf, recvBuf);
  }

  MPI_Barrier(MPI_COMM_WORLD);

  buddy_free(recvBuf);
  buddy_free(sendBuf);

  // aggregate
  double totalBW = 0.0;
  MPI_Reduce(&senderBW, &totalBW, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
  if (rank == 0) {
    std::cout << "----- Total Bandwidth: " << totalBW << " MB/s -----" << std::endl;
  }

  return 0;
}

