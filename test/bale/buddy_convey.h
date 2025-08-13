#pragma once

#include <buddy.h>

enum convey_status {
  convey_DONE = 0,         ///< returned by convey_advance()
  convey_FAIL = 0,         ///< returned by convey_push(), convey_pull(), etc.
  convey_OK   = 1,         ///< returned by convey_push(), convey_advance(), etc.
  convey_NEAR = 2,         ///< returned by convey_advance()
};

struct convey_t;
struct convey_alc8r_t;

convey_t* convey_new(size_t max_bytes, size_t n_local, const convey_alc8r_t* alloc, uint64_t options);
int convey_begin(convey_t* c, size_t item_bytes, size_t align);
int convey_push(convey_t* c, const void* item, int64_t pe);
int convey_pull(convey_t* c, void* item, int64_t* from);
int convey_advance(convey_t* c, bool done);
int convey_unpull(convey_t *c);
int convey_free(convey_t* c);
