# ringdev

A Linux kernel module that exposes `/dev/ringdev`, a simulated accelerator.
Programs submit jobs through a submission ring, a kernel engine thread runs
them after a simulated delay, and results come back through a completion ring.
The bulk data sits in a buffer mmap'd into the program. See
[DESIGN.md](DESIGN.md) for locking, lifetimes and the reasoning.

Developed and tested on Ubuntu 24.04, kernel 7.0.0-38-generic (x86-64, 12 CPUs,
AMD Ryzen 5 5600H), gcc 13.3.

## Build and run

```
make                         # ringdev.ko plus bin/{test_*,bench,demo}
./demo.sh                    # build, insmod, demo, all tests, rmmod
sudo insmod ringdev.ko ring_entries=64 data_pages=256 tick_us=100
./bin/demo
sudo rmmod ringdev
```

Parameters (all read-only after load):

| parameter | default | meaning |
|-----------|---------|---------|
| `ring_entries` | 64 | SQ and CQ size per context; power of two, at most 4096 |
| `data_pages` | 256 | data buffer size per context (1 MiB), at most 16384 |
| `tick_us` | 100 | simulated latency per job; 0 disables the sleep |

Stats are also in `/sys/kernel/debug/ringdev/stats` (root only): global
counters plus one row per open context. Checked by hand with and without an
open context.

`/dev/ringdev` is created mode 0666 by the module so tests run unprivileged.

## Using it

```c
#include "user/libringdev.h"

struct rd_dev d;
rd_open(&d, 0); rd_map(&d);                  // d.buf is the shared buffer
struct ringdev_job j = rd_vadd(1, 0, 4096, 8192, 4096);
rd_submit(&d, &j, 1);                        // returns how many were accepted
struct ringdev_cqe c;
rd_reap(&d, &c, 1);                          // blocks; c.status, c.result
```

Jobs are byte offsets into the buffer: `FILL` (32-bit pattern), `COPY`
(non-overlapping), `VADD_U32`, `CHECKSUM` (byte sum). A context's jobs run in
submit order, so a job can use the result of an earlier one. `read()` returns
whole 24-byte completion entries and `poll()` reports `POLLIN` when one is
ready. `ioctl`s: `GET_INFO`, `SUBMIT`, `RESET`, `GET_STATS`. Every context may
have at most `ring_entries` jobs outstanding (queued + running + unread
completions); `SUBMIT` takes as many as fit and returns `-EAGAIN` if none do.
The full contract is in [ringdev_uapi.h](ringdev_uapi.h).

## Tests

All were run against the loaded module in this environment. Run them with the
module loaded (`./demo.sh` does that).

| test | what it checks | result |
|------|----------------|--------|
| `test_basic` | open, `GET_INFO`, independent contexts, bad ioctl | pass |
| `test_mmap` | strict mmap validation; mapping is the kernel's buffer in both directions; buffers are per-context | pass |
| `test_ops` | all four ops against a CPU computation, boundaries, batch ordering and latency | pass |
| `test_blocking` | `read()` blocks and wakes, `O_NONBLOCK`, `EINTR`, partial reads | pass |
| `test_poll` | `POLLIN` semantics, blocking `poll()` woken by another thread | pass |
| `test_invalid` | bad pointers/counts, bad offsets, overflowing offsets, huge `len`, unknown ops, bad flags, wrong mmap sizes, short read buffer, flow control and `RESET` | pass |
| `test_stress` | N processes x M threads, a CPU model predicts every status, result and final buffer; plus a shared-fd phase; no lost completions | pass: 4x4 (22,400 jobs) and 8x8 (51,200 jobs) |
| `test_kill` | children SIGKILLed while flooding, blocked in `read`/`poll`, or exiting with work pending; a survivor keeps getting correct, timely results | pass: 200 rounds x 8 children; 47,424 queued jobs dropped with their contexts; 0 contexts left; `rmmod` succeeds |

`rmmod` with an open file descriptor is refused ("Module is in use") and
succeeds as soon as it is closed; both were checked.

## Benchmark

`./bin/bench -n 20000` (module defaults: `ring_entries=64`, `tick_us=100`).
Each process loops "submit one batch with one ioctl, reap that batch" with 4 KB
`CHECKSUM` jobs. Latency is the kernel's submit-to-completion time per job, so
jobs late in a batch include the time queued behind the earlier ones.
`fair` is the slowest process's rate over the fastest one's.

```
--- throughput and latency vs batch size (1 process)
procs batch       jobs/s     p50 us     p99 us     max us    fair
    1     1         5646      139.2      184.3      630.3    1.00
    1     4         6387      389.0      665.2     1284.1    1.00
    1    16         6394     1295.2     2567.2     3616.0    1.00
    1    64         6503     4969.1    10037.0    10910.9    1.00

--- vs number of processes (batch 16)
procs batch       jobs/s     p50 us     p99 us     max us    fair
    1    16         6381     1296.6     2566.4     3112.7    1.00
    2    16         6759     2455.7     5002.7     6512.1    1.00
    4    16         6596     5025.1    10073.9    11295.4    1.00
```

How to read it. There is one engine thread, and each job costs about 150 us
(100 us simulated plus timer slack and wakeup). Throughput is therefore pinned
near 6.5k jobs/s however the work is split. Batching helps a little (5.6k to
6.4k) because it removes one syscall and one wakeup per job. Latency is mostly
queueing: it grows with batch size and with the number of processes, and the
extra load is shared evenly (`fair` stays at 1.00). These are numbers from one
run on a laptop VM; the exact values move with timer slack and CPU load.

## Not done / not verified

* **`sparse`** was not run. The packaged 0.6.4 cannot parse the 7.0 kernel
  headers (`__typeof_unqual__`), and Kbuild refuses to use it; a newer
  upstream sparse is needed.
  `checkpatch.pl --no-tree` reports one warning, the suggestion to use
  `kzalloc_obj`, which exists only in very recent kernels; the code keeps
  `kzalloc` to stay buildable on Ubuntu 22.04/24.04 stock kernels. The module
  also builds without warnings at `-Wall -Wextra` and with `W=1`.
* **Stretch goals skipped**: a QEMU debug kernel with KASAN, lockdep, kmemleak
  and KCSAN, the hrtimer interrupt model, per-context priority, and fault
  injection. The running kernel has none of KASAN, lockdep or kmemleak
  enabled, and building a debug kernel was out of scope for the time spent.
  The lock annotations (`READ_ONCE`/`WRITE_ONCE` on the few lockless reads)
  were written with KCSAN in mind but never run under it.
* Timing assertions in `test_blocking` and `test_poll` use generous bounds
  (tens of milliseconds) and could flake on a very loaded machine.
# ringdev
