# The lock order
### CX Design Spec: Aurora Tejeda / CATX Systems

> **Status: 1.9a done (2026-11-05): every spinlock ranked, the checker in
> test-hook builds, the heap's large path locked; 1.9b done (2026-11-05):
> the scheduler's lock placed, the checker's state per CPU; 1.9c
> (2026-11-06): the kernel stacks' lock (`LR_KSTACK`)**
> (`planning/CX_ROADMAP.md`, Phase 1 step 9).

On one CPU a spinlock never waits: taking one turns interrupts off, so
nothing else on the CPU runs until it is released. Two locks taken in
opposite orders by two paths are therefore harmless today, and a deadlock on
the first SMP boot. Before SMP (1.9d), every lock gets a place in one order,
and a debug build refuses any nesting that breaks it.

## 1. The rule

Each `spinlock_t` carries a **rank** (`kernel/sync/lockorder.h`), given where
the lock is made: `SPINLOCK_INIT(LR_x)` for a static lock, `spin_init(&l,
LR_x)` for one inside an object. A CPU may take a lock only while every lock
it holds has a **lower** rank. So:

- the ranks are the order, outermost first;
- two locks of one rank (two handle tables, two caches) are never held
  together; a path that needed that would get two ranks or an address order,
  and none does today;
- release order is free: giving back an outer lock before an inner one cannot
  deadlock, so it is not checked (Serenity's `track_lock_release` does check
  it; nothing here needs it);
- no lock is held across a thread switch but the scheduler's: a spinlock
  belongs to the CPU, and the next thread would inherit it. `sched_lock` is
  the one that must be (§2a).

## 2. The order

Read off a boot, not guessed: the first build recorded every pair (held, taken)
with no check, over the 201 self-tests (the report is printed after them);
the table was then written to agree with every pair seen, and the enforcing
boots since run the executive and the shell under it too. Two first guesses were wrong:
`pids` is taken under `vmregion` (a demand fault charging a process), and the
cache registry `ocache-slots` under a cache's own lock.

| Rank | Lock | Taken under it (seen at boot) |
|---|---|---|
| `LR_STORAGE` | `storage/hardware/disk.c` `storage` (1.9e3): CXFS and every disk transfer; CPU-recursive (`sync/rspin.h`) | htab, pids, sysfile, ocache, log (a syscall's file handles, a file closed, a line) |
| `LR_IPC` | `cpu/ipc.c` `ipc_lock` | htab, kwait, sched |
| `LR_FUTEX` | `cpu/futex.c` `futex_lock` | vmregion, pids (a user copy's demand fault), sched |
| `LR_HTAB` | `object/htab.c`, per handle table | sysfile (a handle closed: `of_destroy`) |
| `LR_VMREGION` | `memman/vmregion.c`, per address space | pids, kmem, pagedb |
| `LR_PIDS` | `cpu/proc.c` `pids_lock` | sched, ocache, ocache-slots, pagedb |
| `LR_KMEM` | `object/kmem.c`, per memory object | pagedb |
| `LR_KTIMER` | `object/ktimer.c` `armed_lock` | kwait |
| `LR_KWAIT` | `object/kwait.c` `wait_lock` | sched |
| `LR_SYSFILE` | `cpu/sysfile.c` `files_lock` (1.9e3): the graveyard, the advisory locks and their waiters, unlink's open check | sched, ocache, ocache-slots, pagedb (a waiter blocked, a lock record) |
| `LR_SCHED` | `cpu/sched.c` `sched_lock` (1.9b) | ocache, ocache-slots (a thread id's slot) |
| `LR_RANDOM` | `drivers/char/random.c` | - |
| `LR_HEAP` | `memman/heap.c` `heap_lock` (new) | pagedb |
| `LR_OCACHE` | `memman/ocache.c`, per cache | ocache-slots, pagedb |
| `LR_OCACHE_SLOTS` | `memman/ocache.c` `slots_lock` | - |
| `LR_DMA` | `memman/dma.c` | - |
| `LR_KSTACK` | `memman/kstack.c` `slots_lock` (1.9c) | - |
| `LR_PAGEDB` | `memman/pagedb.c` | - |
| `LR_CMOS` | `platform/pc/rtc.c` `cmos_lock` (1.9e3): the CMOS index and data ports | - |
| `LR_LOG` | `drivers/char/console.c` (1.9e3): the screen, the serial tee and the system log's ring; CPU-recursive | - (last: a line may be logged under any lock) |

Pairs not in the table's right column put no constraint on each other; the
ranks between them are free. `LR_HEAP` sits before `LR_OCACHE` so that
`heap_allocs_live`, which counts the caches, never has to be split. Every
boot of a test-hook build prints what it saw, after the self-tests:

```
[LOCKS   ] - [INFO] - ipc > htab kwait
```

one line per outer rank, so a new nesting shows up in the serial log before
it shows up as a panic. The lock-order self-test's own locks are ranked as
real ones and appear there too (`heap > ocache` is its heap-ranked lock
held over `heap_allocs_live`, not a nesting in the heap).

The two CPU-recursive locks (1.9e3, `sync/rspin.h`) are spinlocks with an
owner and a depth, as Serenity's `Mutex::lock` counts `m_times_locked` and
Haiku's `recursive_lock_lock` its `recursion`: a CXFS call makes CXFS calls,
and a line is made of console calls. Only the outermost take is in the
order; the inner ones only count. Each is broken, never waited for, by a
panic (`storage_break`, `log_break` in `panic_begin`): a CPU that held one
when another panicked would keep the crash record and the screen from
being written.

## 2a. The scheduler's lock (1.9b)

Until 1.9b the scheduler's critical sections were "interrupts off"
(`irq_save`), which on one CPU is a lock and on two is nothing.
`sched_lock` (rank `LR_SCHED`) now covers every thread's state, the
sleepers' deadlines, the id table and the switch. It ranks after `kwait`
(a wait wakes threads under its lock, and `OBJINFO_THREAD` reads scheduler
state there), and after `ipc`, `futex` and `pids` for the same reason; the
only things taken under it are the cache a new thread's id slot comes from.

**It is held across the switch.** Whoever switches takes it, picks the next
thread, and `context_switch`es with it held; the thread switched to gives
it back: `yield` and `sched_preempt_point` after their `context_switch`,
or `thread_launch` the first time a thread runs (a new thread's frame
starts with interrupts off for that reason; launch releases the lock, then
enables them). This is Serenity's handoff (`Kernel/Tasks/Scheduler.cpp`:
`enter_current` asserts `g_scheduler_lock` is held by this processor on
the far side of a switch, and `leave_on_first_switch` gives it back for a
thread switched into for the first time). The
checker allows exactly this: `switch_to` asserts that the sched lock is
held and nothing else is (`lockorder_assert_switch`).

Three things leave its lock early, because what they call ranks before it:
- **the reaper** claims an exited thread under the lock (`THREAD_REAPING`)
  and frees it outside, since the free ends kernel waits (`kwait`) and
  tells the process (`pids`). Since 1.9e2 it also skips a thread still on
  a CPU (`on_cpu`), and runs on the boot CPU only until 1.9e3;
- **idling** is each CPU's idle thread since 1.9e2: it picks under the
  lock, and when there is nothing gives it back with interrupts still off
  before the halt, since the interrupt that ends it takes the lock;
- **the timer** never calls into the scheduler holding it
  (`sched_next_due` takes it itself).

Another thread's record is read under the lock (1.9e2): the id table grows
by freeing its old array, which a thread made on another CPU can do at any
moment. A thread reads its own record without it, through the per-CPU
`cur` pointer, since nothing frees the record of a running thread. The
getters (`thread_proc`, `thread_prio`, `thread_cpu_ns`, `thread_is_alive`,
`thread_name_copy`, `thread_affinity`, `thread_cpu` and the rest) go
through one helper, `look`, which takes the lock for any id but the
caller's own.

The checker's held list, the spinlock count of the test hooks and the
running thread are each CPU's own (`struct cpu`, `cpu/percpu.h`, reached
through GS). The list is read with interrupts off; the running thread and
the count are read in one GS-relative load, so a thread cannot move CPUs
partway through reading them.

## 3. The checker

With `CXK_LOCK_CHECK` (on with `CXK_TEST_HOOKS`, so in every boot the
self-tests run in), `spin_lock` calls `lockorder_acquire` **before** it
spins and `spin_unlock` calls `lockorder_release`. Checking before the spin
means an inversion panics the first time the two locks nest, on any run,
instead of only on the run where the other CPU happens to hold the second
lock (Linux's lockdep works this way, from memory; it is not indexed).

The checker keeps, per CPU (`struct cpu`, 1.9b), a list of what is held:
lock, rank, and the address it was taken at (a label in the function the
inline `spin_lock` landed in). A failure panics with all of it:

```
lock order: out of order, taking htab@0xffffffff8012a3c4; held: kwait@0xffffffff80119e02
```

It panics on: a lock taken at or below a held rank; a lock with no rank
(`LR_NONE`, a lock made without one); more than `LOCKORDER_DEPTH` (8) held;
a release of a lock not held; and a thread switch with any held but the
sched lock, or without it (`sched.c` `switch_to`). The panic path stops the checker first
(`panic_begin`), since the crash log takes locks in whatever state it finds.

The pure part (`lockorder_take`, `lockorder_give`) builds for the host
tests. In the kernel, a self-test can arm the checker so a failure is
counted rather than a panic (`lockorder_test_arm`/`_disarm`).

**Cost.** A release build (`CXK_TEST_HOOKS=0`) compiles all of it out:
`spinlock_t` keeps its rank word, nothing reads it. With the checker, each
take walks a list of at most 8.

### Prior art

- **Serenity**, `Kernel/Locking/LockRank.h`: five ranks as bits, a per-thread
  mask, `Thread::track_lock_acquire` refuses a rank not below the mask.
  Coarser (one rank for all memory-manager locks) and per thread; the ranks
  here are per lock class and per CPU, since a spinlock cannot move threads.
- **WRK**, `public/sdk/inc/ntkeapi.h` `KSPIN_LOCK_QUEUE_NUMBER`: the queued
  spinlocks are numbered (dispatcher, PFN, system space, ...), a fixed table
  of the kernel's hottest locks; the numbering is the documentation of what
  exists, with no checker in the WRK sources.
- **XNU**, `osfmk/kern/lock_rw.c` `add_held_rwlock`/`remove_held_rwlock`: a
  per-thread record of held rwlocks, for assertions on what a thread holds,
  not for order.

## 4. The heap's lock

`kmalloc`'s size classes have always gone through `ocache`, which locks; the
large path (over 2 KiB: the first-fit list, `heap_grow`) took no lock at all.
Syscalls run with interrupts off, so the race needed two kernel threads
preempted mid-list; SMP would make it ordinary. `heap_lock` (rank
`LR_HEAP`) now covers the list, the counts and the window's next address,
across `heap_grow`'s frame allocation and mapping (pagedb, ranked after).

## 5. Tests

- Host (`test_lockorder.c`, 6): order, refusal of lower and equal ranks
  (both sides of the bound), unranked, release in any order, the depth
  limit, a name for every rank.
- Boot (`hooks: the lock order is checked`): with locks of its own, ranked
  as real ones: in order (listed, the pair recorded, the list empty after);
  each failure caught once with the checker armed (out of order, same rank,
  unranked, a release not held, a switch holding one); and the heap's
  alloc, free and count each caught taking a heap-ranked lock under one
  already held, which is how a test on one CPU shows a path takes a lock
  (it cannot hold the lock and watch another thread wait, since the holder
  has interrupts off).
- Boot (1.9e3, four CPUs): the storage lock, the log lock and sysfile's
  `files_lock` each held on the boot CPU while a thread on an AP calls what
  takes it (`cxfs_get_id`, `slog_record`, `sysfile_lock_waiting`): still in
  the call after 30 ms, through it once given back; the storage lock
  recursive (held through an inner take and give) and only its outer take
  ordered (one failure under a test lock of its rank, for a CXFS call made
  holding it already); the ring with both CPUs recording 200 each at once,
  every record whole, its sequence number the next, each CPU's in order; a
  wall-clock read one failure under a test lock of the CMOS rank.
- Boot (`hooks: the scheduler takes its lock`, 1.9b): nine scheduler
  entries (unblock, start, a process's wake, the sleepers' wake, others
  ready, the next deadline, the active count, priority, TLS) each caught
  taking the sched lock under a heap-ranked one; a new thread, and the
  thread switched back to, each find nothing held and no spinlock counted.
- Boot (`GS is this CPU's; the IST vectors swap it in`, 1.9b): GS is
  `cpu_boot`, ring 3's base the low half, the running thread and the
  syscall stack follow a switch; `int 1` from the kernel leaves GS alone,
  and `int 1`, `int 2` and `int 18` with ring 3's base swapped in find the
  kernel's in the handler and ring 3's after.
- Every boot of a test-hook build: any inversion anywhere panics, so the
  204 self-tests and the executive's start are the wide test.

**Untested:** paths no boot reaches (each driver's error paths, disks the
boot does not mount) are checked only when something reaches them. Of the
scheduler's entries, `sched_yield_if_outranked` and
`thread_block_prepare_until` have no test that they take the lock (each
is called where nothing ranked after sched can be held); a switch without the sched lock is
shown caught only by hand (every boot panics). The early releases of §2a
are contended since 1.9e2; the getters on another thread are tested to
take the lock (`ktest_sched_lock`), and the rest is a design argument.
