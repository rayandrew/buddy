#include <mpi.h>
#include <buddy.h>
#include <request.h>
#include "util_mpi.h"

#include <vector>

size_t payloadSize = -1;
size_t pktSize = -1;
int windowSize = 64;
int aggregation = 256;
size_t MAXLEN = -1;

enum {
  ID_SEND = 114514,
  ID_RECV = 191981,
};

int rank, size;

int bandwidthTest(bool isSender, int counterPart, int pktNum);

int main(int argc, char **argv)
{
  if (argc != 4){
    std::cerr << "Usage: " << argv[0] << " <payloadSize> <windowSize> <aggregation>\n";
    return 1;
  } else {
    payloadSize = std::stoul(argv[1]);
    windowSize = std::stoi(argv[2]);
    aggregation = std::stoi(argv[3]);
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
    // bandwidthTest(!isSend, counterPart, pktNum);
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

auto bandwidthTestSender(const int pktNum, std::vector<buddy_buf*>& sendBuf, std::vector<buddy_buf*>& recvBuf) {
  auto start = MPI_Wtime();

  int sentRegisterPktNum = 0;

  for (int i = 0; i < windowSize; i++) buddy_send(sendBuf[i], pktSize*aggregation, 0, i);
  sentRegisterPktNum = windowSize * aggregation;

  uint64_t ids[windowSize];
  uint64_t sizes[windowSize];

  for(;sentRegisterPktNum < pktNum;) {
    int n = buddy_poll(ids, sizes, windowSize);

    for (int j = 0; j < n; j++) {
      if(ids[j] < (uint64_t)windowSize) {
        if (sentRegisterPktNum < pktNum) {
          const int pktSendThisTime = (pktNum-sentRegisterPktNum) >= aggregation ? aggregation : (pktNum-sentRegisterPktNum);
          buddy_send(sendBuf[ids[j]], pktSendThisTime * pktSize, 0, ids[j]); // reuse the buffer
          sentRegisterPktNum += pktSendThisTime;
          if (sentRegisterPktNum == pktNum) {
            break;
          }
        }
      }
      else CHECK(false);
    }

  }

  buddy_recv(recvBuf[0], MAXLEN, 0, ID_RECV);
  wait_for(ID_RECV, sizeof(buddy::request_head) + 1);
  auto end = MPI_Wtime();

  auto elapsed = end - start;
  return elapsed;
}


int bandwidthTestReceiver(const int pktNum, std::vector<buddy_buf*>& sendBuf, std::vector<buddy_buf*>& recvBuf) {

  int recvPktCount = 0;

  for (int i = 0; i < windowSize; i++) buddy_recv(recvBuf[i], pktSize*aggregation, 0, i);

  uint64_t ids[windowSize];
  uint64_t sizes[windowSize];

  for(;recvPktCount < pktNum;) {

    int n = buddy_poll(ids, sizes, windowSize);

    for (int j = 0; j < n; j++) {
      if (ids[j] < (uint64_t)windowSize) {
        CHECK(sizes[j] % pktSize == 0);
        recvPktCount += sizes[j] / pktSize;
      } else {
        CHECK(false);
      }

      // really don't know how to exactly recv the pktNum
      buddy_recv(recvBuf[ids[j]], MAXLEN, 0, ids[j]); // reuse the buffer
    }

  }

  ((buddy::request_head *)sendBuf[0]->addr)->size = 1;
  buddy_send(sendBuf[0], sizeof(buddy::request_head) + 1, 0, ID_SEND);

  wait_for(ID_SEND, 0); // keep the send buf valid

  return 0;

}


int bandwidthTest(bool isSender, int counterPart, int pktNum) {

  std::vector<buddy_buf*> sendBuf;
  std::vector<buddy_buf*> recvBuf;

  double senderBW = 0.0;

  if (isSender) {
    {
      // Buffer preparation
      buddy::request_head head = {.size = (uint32_t)payloadSize, .dst = counterPart};

      for (int i = 0; i < windowSize; i++) { // mempool
        sendBuf.push_back(buddy_alloc(MAXLEN));
        for (int j = 0; j < aggregation; j++) { // aggregation
          memcpy((char *)sendBuf[i]->addr + j*pktSize, &head, sizeof(head)); // header
          for (size_t k = 0; k < payloadSize; k++) { // payload
            ((char *)sendBuf[i]->addr)[j*pktSize + sizeof(head) + k] = 0xff;
          }
        }
      }

      recvBuf.push_back(buddy_alloc(1024)); // for ack only
    }

    MPI_Barrier(MPI_COMM_WORLD);
    auto timeElapsed = bandwidthTestSender(pktNum, sendBuf, recvBuf);

    auto loadInByte = pktNum * (sizeof(buddy::request_head) + payloadSize);
    senderBW = loadInByte / timeElapsed / (1024 * 1024);  // MB/s
    std::cout << "Sender " << rank << " Bandwidth: " << senderBW << " MB/s" << std::endl;

  } else {

    for (int i = 0; i < windowSize; i++) {
      recvBuf.push_back(buddy_alloc(MAXLEN));
    }

    sendBuf.push_back(buddy_alloc(1024)); // for ack only
    buddy::request_head head = {.size = (uint32_t)payloadSize, .dst = counterPart};
    memcpy((char *)sendBuf[0]->addr, &head, sizeof(head));
    ((char *)sendBuf[0]->addr)[sizeof(head)] = 0xff;

    MPI_Barrier(MPI_COMM_WORLD);
    bandwidthTestReceiver(pktNum, sendBuf, recvBuf);
  }

  MPI_Barrier(MPI_COMM_WORLD);

  for (auto buf : sendBuf) buddy_free(buf);
  for (auto buf : recvBuf) buddy_free(buf);

  // aggregate
  double totalBW = 0.0;
  MPI_Reduce(&senderBW, &totalBW, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
  if (rank == 0) {
    std::cout << "----- Total Bandwidth: " << totalBW << " MB/s -----" << std::endl;
  }

  return 0;
}

