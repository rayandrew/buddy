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

**How the two ends meet.** Not through `doca_rdma_export`: that blob is opaque to ibverbs, and the
host runs DOCA 3.2.1025 against the DPU's 3.0.0058 anyway, with no SDK headers installed host-side.
DOCA RDMA includes `<rdma/rdma_cma.h>` and exposes a bridge for an application that owns its own
listen:

```c
doca_rdma_bridge_prepare_connection(rdma, cm_id, &rdma_connection);
doca_rdma_bridge_accept(...);
```

So the DPU listens with plain RDMA CM, hands the `rdma_cm_id` to DOCA, and the host connects as an
ordinary rdmacm client. No DOCA on the host, no blob, and the version skew stops mattering.

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

`src/dpa_d2d.cpp` and `src/dpa_d2d_dev.c`, one peer, 4 KB buffers, 32 records each. These numbers
are from healthy hardware: an earlier set was taken while one DPU's NIC was failing with transmit
timeouts, and it understated the engine by about 2x at four engines.

| engines | msg/s (three 10 s runs) | errors |
|---|---|---|
| 8 | 378k - 588k | 1 - 5 |
| 12 | 521k - **1.35M** | 0 - 5 |

Against ~150k for the DOCA CPU path at buddy's two threads, doing strictly less work: the engine
routes every record. Engines are the scaling axis, not queue depth. **The spread is the error rate**:
the run that reached 1.35M and 43.1M records per second took no errors, and every slower run took
some. A single figure quoted from one run is misleading, which an earlier version of this table did.

**The errors do not lose data.** Client sends and server receives agree to within 0.1 percent
(477,081 against 477,263 per second at eight engines), and the sender never reports an error. So a
`RECV_ERR` here is a wasted receive slot rather than a message that arrived and could not be placed,
and no retry is needed for correctness. It costs throughput, and the cost is large.

**Errors used to cascade, and that is fixed.** A failed receive still consumed its queue entry, but
the handler did not take it off the receive FIFO, so every later receive on that engine mapped to
the wrong buffer and produced more errors. One bring-up error would take the engine down for the
rest of the run: the worst run before the fix collapsed to 127k with 13 errors, and after it the
worst is 378k. The first error is still unexplained; it arrives early, between 200 and 900 messages.

### Two limits found, and what they mean

**An error kills the engine that hit it.** Throughput tracks the error count exactly: 2.91M messages
in four seconds with none, 2.02M with one, 1.85M with two. Traffic continues only on the surviving
engines, which is what made the errors look harmless at first.

**A receive limit that has since gone, cause unattributed.** For a while a single engine died at
exactly 128 posts plus 383 re-posts, every run, with `RECV_ERR` and nothing in the kernel log. It no
longer happens: the same configuration now sustains 374k messages with zero errors, reproducibly.
The fix is somewhere between carrying the receive index in a FIFO and the edits after it, and which
one is not established. Recorded because the arithmetic looked like receive-queue exhaustion and was
not, and because a limit that vanishes without explanation may come back.

`doca_dpa_dev_rdma_receive_ack` is not a reclaim mechanism here: calling it per completion makes the
engine die immediately, at exactly the initial pool size.

**Forwarding out of the receive buffer can deadlock.** In forward mode both sides stall after about
1255 buffers with no errors at all. A buffer is unavailable for receiving while a forward taken from
it is in flight, so with symmetric traffic each side can hold every buffer waiting for a send
completion the peer can no longer produce. Zero copy buys the copy elimination and pays for it with
this coupling, which is precisely what buddy's credit protocol exists to prevent. Credits therefore
have to move onto the device as part of the routing step, not after it.

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

## Results

### Host leg (step 2), verified

An x86 host running plain rdmacm and ibverbs connects to a queue pair whose data path is on the
DPU's DPA, and its messages land in DPA memory where the routing kernel walks them without the Arm
seeing anything. Measured sm7 to sm7-bf: 64 messages of 4096 bytes, 2048 records, no malformed
records and no errors.

`doca_rdma_export` cannot carry this connection. Its blob is opaque to ibverbs, and the host runs
DOCA 3.2.1025 against the DPU's 3.0.0058 with no SDK headers installed at all. The path that works
is `doca_rdma_bridge_prepare_connection` plus `doca_rdma_bridge_accept`: the DPU owns the listen and
hands DOCA the accepted `rdma_cm_id`. This closes the risk listed above.

### Zero-copy forwarding (step 3), verified

A buffer whose records all route to one peer is sent out of the receive buffer it arrived in. The
win depends entirely on buffer size, because the copy is not what limits the small case:

| Buffer | Zero copy | Copy | Ratio |
|---|---|---|---|
| 4 KB | 15276 buf/s, 489k rec/s | 15765 buf/s, 504k rec/s | 0.97 |
| 64 KB | 27891 buf/s, 893k rec/s | 4077 buf/s, 130k rec/s | 6.8 |

At 4 KB the send and receive rate is the ceiling and holding buffers for an outstanding forward
costs slightly more than the copy saves. Both configurations are lossless: forwards equal receives,
nothing dropped, no errors, no malformed records.

### The transmit pool was never accounted for

Slots were taken with `next_tx++ % ntx` and no check that the previous send had completed, so the
NIC could be reading a slot that had already been overwritten. Bounding it uncovered three further
faults, each of which presents as a stall rather than an error:

- A copy forward's completion was indistinguishable from a generated send, so it re-armed the
  generator. The window grew by every forward ever made until no slot was free again.
- One ack per freed buffer makes a single receive post two sends. The send queue overruns, posts
  stop completing, and every counter freezes: measured at about 10k sends. Acks are now batched.
- A buffer that could not be forwarded was re-posted, which silently loses its records. It is now
  parked instead, and an un-reposted receive is what tells the peer to slow down.

### Flow control is back-pressure, not credits

Explicit credits turn out not to be needed. RNR retry is infinite, so a send to a peer with no
posted receive waits rather than failing, and the only real hazard is the symmetric case where both
sides hold every buffer and neither can post a receive. Keeping half of each pool posted at all
times removes that, and parking the rest supplies the back-pressure. This replaces step 4.

The generator is a benchmark artifact and needs care: with both sides forwarding, traffic
circulates in a closed loop, and a node that also injects new load exceeds its own send ceiling and
must drop. The router-only shape (one side generates and sinks, the other routes) is the
measurement that means anything, and it is lossless.

### Full path (step 5), verified

The path buddy's proxy actually needs now runs entirely on the device:

```
host sm7  --ibverbs-->  sm7-bf DPA  --RoCE-->  sm8-bf DPA  --ibverbs-->  host sm8
```

300000 messages of 32 KB, 9.6M records, conserved exactly at all four hops: no dropped buffers, no
malformed records, no errors, every forward zero copy. 29677 msg/s and 0.97 GB/s end to end, which
is the host client's limit rather than the device's - the router alone sustains 135823 buffers/s and
4.35M records/s at the same 32 KB.

Two things made this work.

**One inbound leg per engine.** A completion does not say which queue pair produced it, so an engine
receiving on both its host leg and its peer leg cannot tell which buffer was just filled. Each
engine receives on exactly one leg and forwards on the other, and owns every queue pair it posts to,
so no two threads ever post to the same one.

**Parking limits are per leg.** Half the pool is the right cap only between two routers, where both
holding everything is a deadlock. An engine fed by a host may park its entire pool: stalling the
host is exactly the intent, and unlike a peer router the host holds nothing the engine is waiting
on. Capping the host leg at half made it drop 96% of what arrived, because the drop path ran long
before the back-pressure could.

Note `d2d_size` defaults to 32 KB, which is inside the range where zero copy wins by a wide margin.

### Still on the Arm

The proxy's own loop (`proxy.cpp`) still does the routing for the ibverbs and DOCA legs. What is
proven here is the replacement data path, not yet its substitution into `Proxy::rdma_loop`. That
substitution is the remaining work, and it is now a wiring exercise rather than an open question.
