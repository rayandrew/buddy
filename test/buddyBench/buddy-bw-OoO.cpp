#include <mpi.h>
#include <buddy.h>
#include <request.h>
#include "util_mpi.h"

#include <vector>
#include <random>
#include <thread>
#include <chrono>


/*
Out-of-Order access bandwidth test, simualting apps like graph processing.
*/

size_t payloadSize = -1;
size_t pktSize = -1;
int windowSize = 64;
int aggregation = 256;
size_t MAXLEN = -1;

double sigma = 3.0; // for normal distribution

enum {
  ID_SEND = 114514,
  ID_RECV = 191981,
};

int rank, size;

int bandwidthTest(int pktNum);

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


  buddy_init(MPI_COMM_WORLD, MAXLEN, MAXLEN);
  

  {
    const int pktNum = 128 * 1024; 
    const int buddyPktNum = pktNum / aggregation;

    CHECK(buddyPktNum >= windowSize);

    if(rank == 0)std::cout << "[*]Packet Size: " << pktSize   << " Payload Size: " << payloadSize
                        << " Packet Num: " << pktNum       << " Aggregation: " << aggregation
                        << " Window Size: " << windowSize   << " MAXLEN: " << MAXLEN << std::endl;

    bandwidthTest(pktNum);
    MPI_Barrier(MPI_COMM_WORLD);
  }


  buddy_finalize();
  MPI_Finalize();
}



int bandwidthTest(int pktNum) {

  std::vector<buddy_buf*> sendBuf;
  std::vector<buddy_buf*> recvBuf;

  for (int i = 0; i < windowSize; i++) { // mempool
    sendBuf.push_back(buddy_alloc(MAXLEN));
    recvBuf.push_back(buddy_alloc(MAXLEN));
  }

  for (int i = 0; i < windowSize; i++) buddy_recv(recvBuf[i], MAXLEN, 0, ID_RECV + i);

  // prepare the send buffers
  buddy::request_head head = {.size = (uint32_t)payloadSize, .dst = 0};
  const double mean = rank;
  const double stdDev = sigma;
  std::random_device rd;
  std::mt19937 gen(rd());
  std::normal_distribution<> dist(mean, stdDev);

  int pktProcessed = 0; // total processed count, some might be discarded
  int pktSent = 0; // really sent out count

  auto fillBuf = [&](buddy_buf* buf) -> int {
    int bufferCount = 0;

    for (; pktProcessed < pktNum;) {
      pktProcessed++;
      const double randDest = dist(gen);
      const int randDestInt = std::round(randDest);
      if (randDestInt < 0 || randDestInt > (size - 1) || randDestInt == rank) continue; // skip invalid

      head.dst = randDestInt;
      memcpy((char *)buf->addr + bufferCount * pktSize, &head, sizeof(head));
      for (int j = 0; j < (uint32_t)payloadSize; j++) ((char *)buf->addr)[bufferCount * pktSize + sizeof(head) + j] = 0xff;

      bufferCount++;
      if (bufferCount == aggregation) break;
    }

    return bufferCount;
  }; 

  auto start = MPI_Wtime();
  double end = 0;

  // initial fill and send
  for (int i = 0; i < windowSize; i++) {
    auto filledPkt = fillBuf(sendBuf[i]);
    if (filledPkt > 0) {
      buddy_send(sendBuf[i], filledPkt * pktSize, 0, ID_SEND + i);
      pktSent += filledPkt;
    } else {
      break;
    }
  }

  // poll events
  uint64_t ids[2*windowSize];
  uint64_t sizes[2*windowSize];

  uint64_t idle = 0;

  for(;;) {
    int n = buddy_poll(ids, sizes, 2*windowSize);
    
    for (int j = 0; j < n; j++) {
      if(ids[j] >= ID_SEND && ids[j] < ID_SEND + (uint64_t)windowSize) { //send event
        const int bufIdx = ids[j] - ID_SEND;
        auto filledPkt = fillBuf(sendBuf[bufIdx]);
        if (filledPkt > 0) {
          buddy_send(sendBuf[bufIdx], filledPkt * pktSize, 0, ids[j]); // reuse the buffer
          pktSent += filledPkt;
        } 
      } else if (ids[j] >= ID_RECV && ids[j] < ID_RECV + (uint64_t)windowSize) { // recv event
        const int bufIdx = ids[j] - ID_RECV;
        buddy_recv(recvBuf[bufIdx], MAXLEN, 0, ids[j]); // reuse the buffer
      } else CHECK(false);
    }

    // quit condition
    if (pktProcessed == pktNum && n == 0) { // no update
      if(idle > 10000) break; // wait for a while to make sure all done
      std::this_thread::sleep_for(std::chrono::microseconds(10));
      idle++;
    } else { // update
      idle=0;
      if (pktProcessed == pktNum) {
        end = MPI_Wtime();
      }
    }
  }

  // calculate bandwidth
  auto timeElapsed = end - start;
  auto loadInByte = pktSent * (sizeof(buddy::request_head) + payloadSize);
  double senderBW = loadInByte / timeElapsed / (1024 * 1024);  // MB/s
  std::cout << "Rank " << rank << " Bandwidth: " << senderBW << " MB/s, pktSent: " << pktSent << std::endl;

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

