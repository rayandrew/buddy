#include <hostbufs.h>
#include <util_mpi.h>

namespace buddy {

HostBufs::HostBufs()
{
  char *env;

  env = getenv("BUDDY_BUFSIZE");
  if (env && *env)
    bufsize = atoi(env);

  env = getenv("BUDDY_SENDBUF");
  if (env && *env)
    num_sendbuf = atoi(env);

  env = getenv("BUDDY_RECVBUF");
  if (env && *env)
    num_recvbuf = atoi(env);

  buddy_init(MPI_COMM_WORLD, bufsize, bufsize);

  send_buf = buddy_alloc(bufsize*num_sendbuf);
  recv_buf = buddy_alloc(bufsize*num_recvbuf);

  writer = buddy::ReqBufWrite((char*)send_buf->addr, bufsize);
  curr_send = 0;

  send_busy = std::make_unique<bool[]>(num_sendbuf);
  for (unsigned i = 0; i < num_sendbuf; i++)
    send_busy[i] = false;

  for (unsigned i = 0; i < num_recvbuf; i++) {
    buddy_recv(recv_buf, bufsize, bufsize*i, num_sendbuf+i);
  }

  balance_req = MPI_REQUEST_NULL;
}

HostBufs::~HostBufs()
{
  buddy_free(send_buf);
  buddy_free(recv_buf);

  buddy_finalize();
}

bool HostBufs::select_next_sendbuf()
{
  assert(curr_send >= -1);
  assert(curr_send < (int)num_sendbuf);

  // If curr_send >= 0, we check all other indices (from curr_send+1)
  // If curr_send == -1, we check all indices (from 0)
  for (unsigned i = 1; i < num_sendbuf + (curr_send == -1); i++) {
    int j = (curr_send + i) % num_sendbuf;
    if (!send_busy[j]) {
      curr_send = j;
      writer = ReqBufWrite((char *)send_buf->addr + curr_send*bufsize, bufsize);

      return true;
    }
  }

  curr_send = -1;
  writer = ReqBufWrite();

  return false;
}

char *HostBufs::push(request_head head)
{
  char *data = NULL;

  while (curr_send >= 0 && !(data = writer.append_head(head))) {
    assert(!send_busy[curr_send]);
    buddy_send(send_buf, writer.get_pos(), curr_send*bufsize, curr_send);
    send_busy[curr_send] = true;

    if (!select_next_sendbuf())
      break;
  }

  if (data)
    local_balance++;

  return data;
}

bool HostBufs::pull(void **data, size_t *size)
{
  request_head *head = NULL;

  while (curr_recv >= 0 && !reader.next(&head, (char **)data)) {
    // Repost previous buffer
    buddy_recv(recv_buf, bufsize, bufsize*curr_recv, num_sendbuf+curr_recv);

    // Get next buffer in line
    if (recv_list.empty()) {
      curr_recv = -1;
    } else {
      recv_info ri = recv_list.back();
      recv_list.pop_back();
      curr_recv = ri.id;
      reader = ReqBufRead((char *)recv_buf->addr + bufsize*curr_recv, ri.len);
    }
  }

  if (head) {
    local_balance--;
    *size = head->size;
    return true;
  } else {
    return false;
  }
}

bool HostBufs::unpull()
{
  if (reader.unpull()) {
    local_balance++;
    return true;
  } else {
    return false;
  }
}

// while true: more data is potentially arriving
// done = i will not send more data (until advance = false)
bool HostBufs::advance(bool done)
{
  if (cycle_complete)
    return false;

  if (done && !writer.empty()) {
    assert(!send_busy[curr_send]);
    buddy_send(send_buf, writer.get_pos(), curr_send*bufsize, curr_send);
    send_busy[curr_send] = true;
    select_next_sendbuf();
  }

  int num_buf = num_sendbuf + num_recvbuf;
  uint64_t ids[num_buf];
  size_t sizes[num_buf];
  int npoll = buddy_poll(ids, sizes, num_buf);

  for (int i = 0; i < npoll; i++) {
    if (ids[i] < num_sendbuf) {
      send_busy[ids[i]] = false;
      if (curr_send < 0)
        select_next_sendbuf();
    } else {
      recv_info ri = {};
      ri.id = ids[i] - num_sendbuf;
      ri.len = sizes[i];

      if (curr_recv < 0) {
        curr_recv = ri.id;
        reader = ReqBufRead((char *)recv_buf->addr + bufsize*curr_recv, ri.len);
      } else {
        recv_list.push_front(ri);
      }
    }
  }

  if (done) {
    /*
    const double TIMEOUT = 360.0;
    if (done_time == 0.0)
      done_time = MPI_Wtime();
    else if (MPI_Wtime() - done_time > TIMEOUT) {
      FAIL("Stuck in done state for " << TIMEOUT << " s."
          << " Local balance: " << local_balance << "."
          << " Global balance: " << global_balance);
    }
    */

    if (balance_req == MPI_REQUEST_NULL) {
      CHECK_MPI(MPI_Iallreduce(&local_balance, &global_balance, 1, MPI_INT64_T, MPI_SUM, MPI_COMM_WORLD, &balance_req));
    }
    int flag;
    CHECK_MPI(MPI_Test(&balance_req, &flag, MPI_STATUS_IGNORE));
    if (flag) {
      if (global_balance == 0) {
        cycle_complete = true;

        assert(writer.empty());
        assert(reader.finished());

        return false;
      }
    }
  }

  return true;
}

void HostBufs::reset()
{
  assert(cycle_complete);
  cycle_complete = false;
  done_time = 0.0;
}

} // namespace buddy
