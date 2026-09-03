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

### Rules the event-driven model imposes

Each of these was found by measurement, and each fails silently with no error anywhere.

1. **Ack one element at a time, as it is read.** `doca_dpa_dev_completion_ack(comp, 1)` inside the
   drain. Acking a whole drain in one call at the end frees nothing: traffic stops after exactly
   completion-queue-size elements, and the stall point tracks the queue size across a 16x range.
2. **Bound the drain.** Handling a completion posts another operation whose completion can arrive
   before the drain ends, so an unbounded loop never empties under load and the handler never
   reaches `doca_dpa_dev_thread_reschedule`. The thread wakes once and goes silent. Eight
   completions in one round is what these engines use; the ceiling scales with work per completion,
   so the echo survives to a window of 1024 and the routing engine does not.
3. **Keep the argument block small.** A large block makes the h2d and d2h copies unreliable, and
   counters living past the damage read as zero. The pending FIFO gets its own device allocation.
4. **Keep the sender's window below the peer's posted receives.** The excess sits in RNR retry
   forever with `rnr_retry_count` at 7. buddy's credit protocol already enforces this, which makes
   that protocol load-bearing for liveness once it moves onto the device, not just for buffer reuse.

### The routing engine, measured

`src/dpa_d2d.cpp` and `src/dpa_d2d_dev.c`, one peer, 4 KB buffers, 32 records each:

| engines | window | msg/s | records/s | errors |
|---|---|---|---|---|
| 1 | 16 (256 buffers) | 183k | 5.88M | 0 |
| 1 | 16 (128 buffers) | 90k | 2.89M | 0 |
| 2 | 16 (128 buffers) | 90k | 2.89M | 1 |
| 4 | 16 (128 buffers) | 183k | 5.86M | 2 |
| 8 | 16 (128 buffers) | 483k | 15.5M | 7 |
| 12 | 16 (128 buffers) | **1.32M** | **42.3M** | 0 |
| 16 | 16 (128 buffers) | 1.26M | 40.4M | 1 |

**Engines are the scaling axis, not depth.** Twelve engines route 1.32M messages and 42.3M records
per second, about 5.4 GB/s of 4 KB buffers, against ~150k for the DOCA CPU path at buddy's two
threads. Sixteen engines is no better than twelve, so the knee is around twelve.

Depth is capped by work per wake and does not need raising: a window of 16 per engine is enough
once there are enough engines.

The occasional `RECV_ERR` is not proportional to engine count - seven at eight engines, none at
twelve, one at sixteen - so it is timing dependent rather than a per-engine start-up race. It does
not stop traffic: the runs above sustain full rate through it.

Against ~150k for the DOCA CPU path at buddy's two threads, and 180k for the first DPA fabric which
also stalled and returned wrong answers. The engine does strictly more work than either, since it
routes on the device.

Zero-copy forwarding works: a run of consecutive records sharing a destination is posted as one send
straight out of the received buffer, and the buffer is re-posted once its forwards complete.

**Open.** Engine counts above one produce occasional `RECV_ERR`, and the routing engine stalls above
a window of about 16 where the echo reaches 1024. Both need to be understood before this replaces
the proxy's data path.

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
