#include "buddy_convey.h"
#include <request.h>
#include <util.h>
#include <util_mpi.h>

enum {
  TAG_RECV,
  TAG_SEND,
};

const size_t MAXLEN = 1*1024*1024;

struct convey_t {
  buddy_buf *send_buf;
  buddy_buf *recv_buf;
  size_t item_bytes;
  buddy::ReqBufWrite writer;
  buddy::ReqBufRead reader;
  bool sending;
  bool recving;
  int64_t local_balance;
  int64_t global_balance;
  MPI_Request balance_req;
};

convey_t* convey_new(size_t max_bytes, size_t n_local, const convey_alc8r_t* alloc, uint64_t options)
{
  buddy_init(MPI_COMM_WORLD, MAXLEN, MAXLEN);

  convey_t *conv = new convey_t{};

  conv->send_buf = buddy_alloc(MAXLEN);
  conv->recv_buf = buddy_alloc(MAXLEN);

  conv->writer = buddy::ReqBufWrite((char*)conv->send_buf->addr, MAXLEN);
  conv->reader = buddy::ReqBufRead((char*)conv->recv_buf->addr, 0);

  buddy_recv(conv->recv_buf, MAXLEN, 0, TAG_RECV);
  conv->recving = true;

  conv->balance_req = MPI_REQUEST_NULL;

  return conv;
}

int convey_begin(convey_t* c, size_t item_bytes, size_t align)
{
  c->item_bytes = item_bytes;
  CHECK(align == 0);
  return convey_OK;
}

int convey_push(convey_t* c, const void* item, int64_t pe)
{
  buddy::request_head head = {.size = (uint32_t)c->item_bytes, .dst = (int32_t)pe};
  assert(c->item_bytes == head.size);
  assert(pe == head.dst);

  if (c->writer.append(head, (char*)item)) {
    c->local_balance++;
    return 1;
  } else {
    return 0;
  }
}

int convey_pull(convey_t* c, void* item, int64_t* from)
{
  buddy::request_head *head;
  char *data;

  if (!c->reader.next(&head, &data))
    return 0;

  assert(head->size == c->item_bytes);
  memcpy(item, data, head->size);

  assert(!from);

  c->local_balance--;

  return 1;
}

int convey_unpull(convey_t *c)
{
  if (c->reader.unpull()) {
    c->local_balance++;
    return 1;
  } else {
    return 0;
  }
}

int convey_advance(convey_t* c, bool done)
{
  if (!c->writer.empty() && !c->sending && (done || !c->writer.space_for(c->item_bytes))) {
    c->sending = true;
    buddy_send(c->send_buf, c->writer.get_pos(), 0, TAG_SEND);
  }

  if (!c->recving && c->reader.finished()) {
    c->recving = true;
    buddy_recv(c->recv_buf, MAXLEN, 0, TAG_RECV);
    c->reader.reset_len(0);
    c->reader.reset_pos();
  }

  uint64_t ids[2];
  size_t sizes[2];
  int npoll = buddy_poll(ids, sizes, 2);
  for (int i = 0; i < npoll; i++) {
    if (ids[i] == TAG_SEND) {
      c->writer.reset_pos();
      c->sending = false;
    } else if (ids[i] == TAG_RECV) {
      c->reader.reset_len(sizes[i]);
      c->recving = false;
    } else {
      FAIL("");
    }
  }

  if (done) {
    if (c->balance_req == MPI_REQUEST_NULL) {
      CHECK_MPI(MPI_Iallreduce(&c->local_balance, &c->global_balance, 1, MPI_INT64_T, MPI_SUM, MPI_COMM_WORLD, &c->balance_req));
    }
    int flag;
    CHECK_MPI(MPI_Test(&c->balance_req, &flag, MPI_STATUS_IGNORE));
    if (flag) {
      if (c->global_balance == 0)
        return convey_DONE;
    }
  }

  return convey_OK;
}

int convey_free(convey_t* c)
{
  buddy_free(c->send_buf);
  buddy_free(c->recv_buf);

  buddy_finalize();

  return convey_OK;
}
