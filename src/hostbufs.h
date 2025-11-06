#include <buddy.h>
#include <request.h>
#include <memory>
#include <deque>

namespace buddy {

struct recv_info {
  unsigned id;
  size_t len;
};

class HostBufs {
  public:
    HostBufs();
    ~HostBufs();

    HostBufs (const HostBufs&) = delete;
    HostBufs& operator= (const HostBufs&) = delete;

    char *push(request_head head);
    bool pull(void **data, size_t *size);
    bool unpull();
    bool advance(bool done);
    void reset();

  // recv buffer flow:
  // buddy_recv() -> buddy_poll() -> recv_list -> curr_recv/reader -> buddy_recv()
  private:
    bool select_next_sendbuf();

    size_t bufsize = 1*1024*1024;
    unsigned num_sendbuf = 1;
    unsigned num_recvbuf = 1;
    std::unique_ptr<bool[]> send_busy;

    buddy_buf *send_buf;
    buddy_buf *recv_buf;

    int curr_send = -1;
    int curr_recv = -1;
    std::deque<recv_info> recv_list;

    buddy::ReqBufWrite writer = {};
    buddy::ReqBufRead reader = {};

    int64_t local_balance;
    int64_t global_balance;
    MPI_Request balance_req;
    double done_time = 0.0;

    bool cycle_complete = false;
};

}
