# ringdev design notes

A character device `/dev/ringdev` that simulates an accelerator. User space
queues jobs through an ioctl, one kernel thread (the "engine") executes them
after a simulated delay, and results come back as fixed-size entries from
`read()`. Bulk data never crosses the syscall boundary: it lives in a buffer
that is mmap'd into the process.

```
open()   -> ctx (kref), vmalloc_user data buffer, SQ, CQ; added to rd.ctxs
mmap()   -> remap_vmalloc_range of ctx->data
SUBMIT   -> copy descriptors into kernel memory, enqueue on SQ, wake engine
engine   -> pick ctx (round-robin), pop job, fsleep(tick_us), run, push CQE
read()   -> pop CQEs (blocks on ctx->cq_wait); poll() -> EPOLLIN if CQ non-empty
release  -> mark dead, drop queued jobs, wait for in-flight job, free
```

## Locks

There is one lock that matters. Everything else is a helper with a narrow job.

| Lock | Protects | Never held across | Order |
|------|----------|-------------------|-------|
| `rd.lock` (mutex) | `rd.ctxs` list and its order; `rd.active`; global counters (`submitted`, `completed`, `failed`, `busy_ns`); and for every ctx: `sq_head/tail`, `cq_head/tail`, `outstanding`, `busy`, `dead`, per-ctx counters, and the contents of the SQ/CQ slots | `copy_to_user`/`copy_from_user`, job execution, `fsleep`, `wake_up`, `kvmalloc` | innermost |
| `ctx->read_lock` (mutex) | Keeps the CQ range that `read()` peeked at from being consumed or cleared by another `read()` or `RESET` while it is being copied to user space | nothing else; it is held across `copy_to_user` on purpose | taken before `rd.lock` |
| `rd.queued` (`atomic_t`) | Number of jobs queued in all SQs. Only the engine's wait condition reads it without the lock; all writes happen under `rd.lock` | | |
| waitqueue locks (`rd.eng_wait`, `ctx->cq_wait`, `ctx->idle_wait`) | their own queues (internal spinlocks) | | leaf |

Lock-free reads, all deliberate and paired with `WRITE_ONCE` under `rd.lock`:
`ctx->cq_head`, `ctx->cq_tail` (wait condition in `read()`, `poll()`) and
`ctx->busy` (wait condition in `release()`). They only decide whether to sleep;
anything acted upon is re-read under the lock.

Why `read()` needs a second mutex: it must copy entries to user space without
holding `rd.lock` (a page fault, or a userfaultfd handler, must not stall the
engine and every other context). So it peeks at the entries under `rd.lock`,
copies them out, and only then advances `cq_head`. If the copy faults, nothing
is consumed and nothing is lost. `read_lock` makes that peek/commit pair
exclusive against other readers and `RESET`.

## Pointers: owner and lifetime

| Pointer | Created | Owner | Freed |
|---------|---------|-------|-------|
| `struct ringdev_ctx *` | `ringdev_open` (`kzalloc`) | `file->private_data` holds one kref; the engine holds one more for as long as it runs a job of that ctx | `ctx_free`, when the last kref is dropped (normally `release`, but the engine if it was last) |
| `ctx->data` | `ringdev_open` (`vmalloc_user`, zeroed) | the ctx | `ctx_free`. A mapping pins the file, so `release` cannot run while it exists; the mapped pages are refcounted by `vm_insert_page` besides |
| `ctx->sq`, `ctx->cq` | `ringdev_open` (`kvcalloc`) | the ctx | `ctx_free` |
| `ctx->node` in `rd.ctxs` | `ringdev_open` | `rd.lock` | `ringdev_release`, after the in-flight job has finished |
| `jobs` array in `ringdev_submit` | `kvmalloc_array` | that call | same call, `out:` label |
| `tmp[]` in `ringdev_read` | stack | that call | n/a |
| `rd.engine` | `ringdev_init` (`kthread_run`) | the module | `kthread_stop` in `ringdev_exit` or the init error path |
| `rd.cls`, `rd.dev`, `rd.cdev`, `rd.devt`, `rd.dbg` | `ringdev_init` | the module | `ringdev_exit`, reverse order |
| `struct ringdev_job` seen by the engine | a *copy* in `ctx->sq[]` made from kernel memory | the engine, via a stack copy in `engine_thread` | n/a |

Job descriptors are copied from user space once, in `ringdev_submit`, before
anything is validated. Nothing in the kernel ever reads a descriptor from the
mmap'd buffer, so user space cannot change a job after it has been checked.

## Lifecycle and the races that matter

**Engine vs. `release()`.** The engine pops a job, sets `ctx->busy` and takes a
kref in one `rd.lock` critical section (`pick_ctx`). `release()` sets
`ctx->dead` and drops queued jobs under the same lock. So either the engine
already owns a job (and `release` waits for `busy` to clear) or it never will
(`pick_ctx` skips dead contexts). The kref alone would keep the memory valid;
the wait additionally means "no job runs after `close()` returns" and that
`active_ctx` going to zero really means idle.

**SIGKILL.** A killed process closes its files in `do_exit`, which calls
`release()`. The same path runs, so a context dying with work queued, with a
job in flight, with a reader blocked in `read()` (the reader is gone, the file
is not released until it is) or sleeping in `poll()` is covered by one
function. `test_kill` exercises each of these.

**Flow control.** `outstanding` counts queued + in flight + unread
completions per context. `SUBMIT` accepts at most `ring_entries - outstanding`
jobs and `read()` is the only thing that decrements it. Since the CQ is as
large as the SQ, the engine can always push a completion; there is no overflow
path to handle. `RESET` sets `outstanding` to 1 if a job is in flight (its
completion will still arrive) and 0 otherwise.

**Round-robin.** `pick_ctx` walks `rd.ctxs` from the head and takes the first
context with queued work, then moves it to the tail. A busy context therefore
gets one job per turn no matter how many it has queued. `bench` shows the
resulting fairness (`fair` column).

**Module exit.** An open file pins the module (`cdev.owner`), so `rmmod` of a
module with open files is refused with "in use" and, once it is allowed to
run, no context exists. Order: remove debugfs, `device_destroy` (no new opens),
`class_destroy`, `cdev_del`, `kthread_stop` (the engine is idle, there are no
contexts), unregister the region. The init error path unwinds in the reverse
order of setup.

## Job execution and errors

Offsets are byte offsets into the context's buffer. Every `offset + len` is
checked as `offset <= data_bytes && len <= data_bytes - offset`, which cannot
overflow. Work is done in 64 KB chunks with `cond_resched()` so a huge job
cannot hog a CPU on a non-preemptible kernel.

Errors are reported in `cqe.status`, never by failing the ioctl:

| status | meaning |
|--------|---------|
| `-ERANGE` | an offset/length range lies outside the buffer (including overflow) |
| `-EINVAL` | unknown op, `flags != 0`, misaligned offset or length, overlapping COPY |

Malformed `SUBMIT` calls fail the ioctl itself: `-EFAULT` (bad pointer),
`-EINVAL` (`count` 0 or above `ring_entries`, `reserved != 0`), `-EAGAIN`
(no room at all). A partial fit returns the accepted count in `count`.
If the final `copy_to_user` of that count fails, the jobs are already queued
and the call returns `-EFAULT`; they still complete and can be read.

## What the code does not defend against

* User space writing the shared buffer while a job runs gets a mix of old and
  new data. That is inherent to shared memory and does not threaten the kernel:
  all addresses are computed from kernel-side values.
* `VADD_U32` with partially overlapping `dst` and sources is defined as
  "ascending element order" and is accepted.
* Jobs dropped when their context closes still count as `submitted` in the
  global stats but never as `completed`.

## Debugfs

`/sys/kernel/debug/ringdev/stats` prints the global counters and one line per
context (pid of the opener, counters, outstanding, busy). It is read under
`rd.lock`. debugfs is root-only on Ubuntu; the same global numbers are
available to anyone through `RINGDEV_IOC_GET_STATS`.
