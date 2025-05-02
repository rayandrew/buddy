#pragma once

#include <stdint.h>
#include <mpi.h>
#include <infiniband/verbs.h>

#ifdef __cplusplus
extern "C" {
#endif

void buddy_init(MPI_Comm comm, size_t max_send, size_t min_recv);
void buddy_finalize();

// Use buf->addr to get a pointer to the buffer
//struct ibv_mr;
typedef struct ibv_mr buddy_buf;

buddy_buf *buddy_alloc(size_t len);
void buddy_free(buddy_buf *buf);

// Initiate an asynchronous send or receive
// User-specified id to identify the operation
void buddy_send(buddy_buf *buf, size_t len, size_t offset, uint64_t id);
void buddy_recv(buddy_buf *buf, size_t len, size_t offset, uint64_t id);

// Places the user-specified id of at most max completed operations
// in the ids array. Returns the number of completed operations.
// After receiving the id from poll, the buffer is safe to reuse.
// The sizes array contains the length of any recvs.
int buddy_poll(uint64_t *ids, size_t *sizes, int max);

#ifdef __cplusplus
}
#endif
