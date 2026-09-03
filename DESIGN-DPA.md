# DPA-native proxy

How the DPA leg should be built, and why the first version was slower than the CPU one.

## What the first version got wrong

The DPA was placed *underneath* the Arm proxy as a transport. Buffers stayed in Arm memory, and
the two sides met through a submit ring and a completion ring:

```
host --ibverbs--> Arm memory --Arm routes--> submit ring --window--> DPA --> NIC
NIC --> DPA --> completion ring --window--> Arm --Arm routes--> host
```

The Arm still did every routing decision, so the DPA added two crossings per message instead of
removing any. Those crossings are not cheap and cannot be made cheap: the DPA reaches Arm memory
through a NIC window that is not coherent with the Arm's caches, so the Arm pays a full round trip
to the point of coherency for every observation.

Measured on sm7-bf (Cortex-A78AE), `reverse-eng/dpa/barrier_cost.c`:

| operation | cost |
|---|---|
| spin on a cached line (what a coherent CQ poll costs) | 2.35 ns |
| `dc civac` + `dsb sy` + load (what a DPA ring poll costs) | 285 ns |

The result, measured with `src/fabric_lat.cpp` (identical code compiled against either fabric):

| | DOCA CPU path | DPA transport |
|---|---|---|
| per-operation round trip | 7.8 us | 14.2 us |
| triangle, depth 2 | 0.86 s | 12.3 s |

The transport itself is competitive (144k msg/s against DOCA's 134k at the same shape). The loss is
structural: an extra processor in the path that does not own the work is pure added latency.

## What NVIDIA's samples do instead

`doca_dpa/dpa_initiator_target` and `flexio/samples/packet_processor` share one shape:

- **Buffers live in DPA memory.** `doca_dpa_mem_alloc` gives a device pointer, and
  `doca_mmap_set_dpa_memrange` registers it for RDMA. The kernel dereferences it directly:
  `*((uint64_t *)thread_arg->local_buf_addr)`. No window, no cache maintenance, no rings.
- **Threads are woken by completions**, not by spinning. `doca_dpa_thread_run` arms the thread; the
  kernel ends with ack, `doca_dpa_dev_completion_request_notification`, and
  `doca_dpa_dev_thread_reschedule`. No execution unit is held.
- **The host is not in the data path.** It sets queues up once and passes handles.

`doca_dpa_dev_thread_notify` works DPA to DPA, so one thread can hand work to another. It does not
work from the host, which is what forced the first version into a launched spin loop.

## The design

Both legs move onto the DPA. This is not optional: the proxy's job is to move a request from one
leg to the other, so terminating either leg on the Arm puts the Arm back in the path.

```
host x86 --ibverbs RC--> [ DPA memory ] --DOCA RDMA on DPA--> [ remote DPA memory ] --> remote host
                              ^                                       ^
                              +-- DPA thread routes, never wakes the Arm
```

The host side of the host leg is unchanged: plain ibverbs against a standard RC queue pair. Only
the DPU side of that connection becomes DOCA RDMA with `doca_ctx_set_datapath_on_dpa`.

### Measured, with `src/dpa_echo.cpp`

The architecture, before any routing: buffers in DPA memory, a thread woken by completions, the Arm
sampling a counter and nothing else.

| design | msg/s at 4 KB |
|---|---|
| DPA under the Arm, windows and rings | 180k |
| DOCA CPU path at buddy's two threads | ~150k |
| DPA native, Arm out of the data path | **723k** |

Sustained for ten seconds, zero errors over 7.2M messages.

The header walk a routing decision needs, one DPA thread:

| records per message | msg/s | records/s | marginal cost per record |
|---|---|---|---|
| 0 | 714k | - | - |
| 8 | 545k | 4.4M | 54 ns |
| 32 | 311k | 9.9M | 57 ns |
| 128 | 140k | 17.9M | 45 ns |

A record costs about 50 ns to walk, which is slow next to an Arm core but affordable: even at 128
records per message a single thread matches the CPU path. The walk is serial inside a buffer, since
each step depends on the previous `size`, but independent across buffers, so the receive pool is
split across several threads.

**One API rule.** `doca_dpa_dev_completion_ack(comp, 1)` must be called for each element as it is
read. Acking a whole drain in one call frees nothing, and traffic stops silently after exactly
completion-queue-size elements; the stall point tracks the queue size across a 16x range.

### Routing on the device

A request is eight bytes of header plus payload:

```c
struct request_head { uint32_t size; int32_t dst; };
```

The kernel walks a landed buffer, reads each header with an ordinary load, maps `dst` to a peer or
a local host rank, and posts the forward. It never copies: the forward is posted directly from a
sub-range of the buffer the message already occupies, so the NIC moves the bytes. The Arm version
memcpys every request into a per-destination buffer, so the device version does strictly less work.

### Credits

Credit accounting and ACK generation move into the same kernel. At depth 2 the current design pays
three serialised operations per message (data, ACK, credit); on the device those become local state
updates and a single posted operation.

## Order of work

1. **`DpaFabric` rewrite.** Device-memory buffers, one mmap per pool, event-driven thread, no submit
   or completion ring, no `flush_line` or `inv_line`. Validate with `fabric_lat`.
2. **Host leg onto the DPA.** Replace the DPU side of the ibverbs host connection with DOCA RDMA on
   the DPA datapath, landing in the same device memory.
3. **Routing kernel.** Header walk, destination table, zero-copy forward.
4. **Credits and ACKs on device.**

Each step is measurable on its own: 1 and 2 should leave throughput unchanged and remove the
coherency cost; 3 and 4 are where the win appears.

## Known risks

- **DPA heap size is undocumented.** Depth 128 needs about 2 MB of device memory per peer pair.
  Allocation failure must be loud, not silent.
- **The host leg must interoperate**: x86 ibverbs against DOCA RDMA on the DPA. Both are RC, so this
  should hold, but it is unverified and it gates step 2.
- **A correctness bug is open in the current fabric**: roughly 5 to 10 percent of triangle runs
  return a wrong count, and a `RECV_ERR` of unknown cause stalls small windows. The rewrite deletes
  the machinery both are suspected to live in, but that must be confirmed rather than assumed.
