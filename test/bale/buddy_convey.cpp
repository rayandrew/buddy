#include "buddy_convey.h"
#include <hostbufs.h>
#include <request.h>
#include <util.h>
#include <util_mpi.h>

enum cstate {
  STATE_DORMANT,
  STATE_WORKING,
  STATE_COMPLETE,
};

struct convey_t {
  size_t item_bytes;
  cstate state;
  buddy::HostBufs bud;
};

convey_t* convey_new(size_t max_bytes, size_t n_local, const convey_alc8r_t* alloc, uint64_t options)
{
  convey_t *conv = new convey_t{};

  conv->state = STATE_DORMANT;

  return conv;
}

int convey_begin(convey_t* c, size_t item_bytes, size_t align)
{
  CHECK(c->state == STATE_DORMANT);
  c->state = STATE_WORKING;

  c->item_bytes = item_bytes;
  CHECK(align == 0);

  return convey_OK;
}

int convey_push(convey_t* c, const void* item, int64_t pe)
{
  CHECK(c->state == STATE_WORKING);

  buddy::request_head head = {.size = (uint32_t)c->item_bytes, .dst = (int32_t)pe};
  assert(c->item_bytes == head.size);
  assert(pe == head.dst);

  char *data = c->bud.push(head);

  if (data) {
    memcpy(data, item, head.size);
    return 1;
  } else {
    return 0;
  }
}

int convey_pull(convey_t* c, void* item, int64_t* from)
{
  CHECK(c->state != STATE_DORMANT);

  char *data;
  size_t size;

  if (!c->bud.pull((void **)&data, &size))
    return 0;

  assert(size == c->item_bytes);
  memcpy(item, data, size);

  assert(!from);

  return 1;
}

int convey_unpull(convey_t *c)
{
  CHECK(c->state != STATE_DORMANT);

  return c->bud.unpull();
}

int convey_advance(convey_t* c, bool done)
{
  if (c->state == STATE_COMPLETE) {
    return convey_DONE;
  }

  if (c->bud.advance(done))
    return convey_OK;
  else {
    c->state = STATE_COMPLETE;
    return convey_DONE;
  }
}

int convey_reset(convey_t* c)
{
  CHECK(c->state == STATE_COMPLETE);

  c->state = STATE_DORMANT;

  c->bud.reset();

  return convey_OK;
}

int convey_free(convey_t* c)
{
  delete c;

  return convey_OK;
}
