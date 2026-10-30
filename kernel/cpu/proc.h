// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/proc.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Processes (docs/kernel/CX_KERNEL_PLAN.md section 3).
 *
 * A process owns what its threads share: the address space, the handle table,
 * the capabilities, the uid and the working directory. A thread owns its
 * registers, its stacks and its scheduling state, and names its process.
 * Until this existed a process WAS a thread, and those fields sat on the
 * thread, so two threads could not share a handle table.
 *
 * A process is a kernel object (KOBJ_PROCESS) from its own cache. Every
 * thread in it holds a reference. When the last of them is reaped, the process
 * closes every handle it holds, frees its address space and gives up its
 * reference, notes how it ended and sets SIGNAL_PROCESS_ENDED. The record goes
 * when the last reference does: a process handle (SYS_SPAWN, SYS_EXEC_PATH)
 * holds one, which is how a parent waits without zombies and without reaping
 * (plan section 4). The pid stays taken while the record lives.
 *
 * A pid names a process, not a thread. Thread ids and pids are allocated
 * separately, so a process keeps its pid whichever of its threads exits.
 * Its creator (cpu/spawn.c) builds the address space and hands it over with
 * thread_set_space; from then on it is the process's, and it goes when the
 * last thread does, however that thread ended. It goes from the reaper, in
 * another thread's context, so the space being freed is never the one loaded.
 */
#ifndef CXK_PROC_H
#define CXK_PROC_H

#include <stdint.h>
#include "cxk_abi.h"          /* SPAWN_HANDLES_MAX */
#include "object/kobj.h"
#include "object/htab.h"
#include "memman/vmregion.h"
#include "arch/paging.h"   /* paddr_t */

/* Pids are below this. Pids come from a table that grows (object/idtab.h);
   this only bounds it. A process needs a thread to do anything, so the bound
   matches THREAD_LIMIT_MAX (cpu/sched.h). */
#define PROC_PID_MAX 256

struct endpoint;   /* cpu/ipc.h */
struct job;        /* cpu/job.h */

struct proc {
    struct kobj hdr;        /* first; KOBJ_PROCESS */
    int         pid;
    int         threads;    /* threads naming this process, exited-unreaped included */
    int         running;    /* of those, the ones not yet through thread_exit */
    int         is_user;    /* runs ring-3 code: never uid 0 (thread_set_uid) */
    uint32_t    uid;        /* owning user; 0 = SYSTEM */
    uint32_t    cwd;        /* CXFS entry id of the working directory; 0 = root */
    uint32_t    caps;       /* ABI capability bits */
    int         background; /* console output goes to the log (SPAWN_BACKGROUND); inherited */
    uint32_t    outlen;     /* bytes waiting in outbuf for their newline */
    char        outbuf[48]; /* one log line in the making (SLOG_EVT_W) */
    struct htab handles;
    int32_t     broker;     /* its SEND handle on its launcher's broker, or -1 */
    int32_t     start[SPAWN_HANDLES_MAX];   /* by slot, the handles its launcher named (SYS_ARGS_HANDLE); -1 none */
    paddr_t     pd_phys;    /* its own page directory, freed with it; 0 = the kernel's */
    struct vm_proc vm;      /* its mappings and memory quota (memman/vmregion.h) */
    /* The job it is in, for its whole life (cpu/job.h), with a reference;
       in_job until it has ended and left the job's count. */
    struct job *job;
    int         in_job;
    /* How it ended (proc_note_end), read once SIGNAL_PROCESS_ENDED is set. */
    uint32_t    ended;      /* PROC_*; PROC_RUNNING until noted */
    int32_t     end_code;   /* PROC_EXITED */
    uint32_t    end_vector; /* PROC_CRASHED */
    uint64_t    end_ip;     /* PROC_CRASHED */
    uint64_t    end_addr;   /* PROC_CRASHED by a page fault */
    uint32_t    end_killer; /* PROC_KILLED, PROC_STOPPED: the pid that asked */
    /* Set by proc_kill: its threads leave at their next safe point, and none
       of them blocks again (cpu/sched.h, thread_killed). */
    int         killing;
    /* Where it is asked to stop (SYS_PROCESS_STOP_ENDPOINT), with a reference
       of its own, or NULL. Dropped when the last thread stops. */
    struct endpoint *stop_ep;
    /* A stop's deadline (proc_stop): on the armed list until it is due at
       stop_due on timer_now_ns, when the process is ended PROC_STOPPED for
       stop_by, or until the process ends first. */
    int          stop_armed;
    uint64_t     stop_due;
    int          stop_by;
    struct proc *stop_next;
};

/* Register the type and make the cache. Once, before the first proc_create. */
void proc_sys_init(void);

/* A new process with no threads yet, inheriting uid and cwd from `parent`
   (SYSTEM and the root if NULL) and nothing else: no caps, no handles, the
   kernel's address space. It is in `job`, or if NULL in its parent's (the
   root job without one). The caller holds the one reference; proc_thread_in
   gives one to each thread. NULL if out of pids or memory. */
struct proc *proc_create(const struct proc *parent, struct job *job);

/* A thread now names `p` (a reference), stops running, or no longer names it.
   Joining is charged to p's job (cpu/job.h) until the thread is gone, and
   refused if the job has been killed (E_CANCELED) or has no thread to spare
   (E_NO_RESOURCES); E_OK otherwise. When the last thread stops, every handle
   closes, the job's count drops and ENDED is signalled; when the last is
   gone (reaped), the address space is freed; the record follows its last
   reference. */
int  proc_thread_in(struct proc *p);
void proc_thread_stopped(struct proc *p);
void proc_thread_gone(struct proc *p);

/* A background process's console output, collected into lines for the log:
   each newline, or a full line's worth, records one entry tagged "pid N".
   proc_out_flush records what is left without a newline; the last thread
   going calls it, so a final line without one is not lost. */
void proc_out_write(struct proc *p, const char *s, uint32_t len);
void proc_out_flush(struct proc *p);

/* How `p` is ending, said by the way out it took: SYS_EXIT (PROC_EXITED and
   the code), a ring-3 fault or a refused call (PROC_CRASHED, the vector, where
   and, for a page fault, the address). The first one noted stands. A process
   whose last thread goes with nothing noted never reached ring 3 and ended
   PROC_NOT_STARTED. Waiters see it once the last thread has gone. */
void proc_note_exit(struct proc *p, int32_t code);
void proc_note_crash(struct proc *p, uint32_t vector, uint64_t ip, uint64_t addr);

/* End `p` for the process `killer` (SYS_PROCESS_KILL). Noted PROC_KILLED at
   once, so a way out noted later loses; then every thread that is blocked is
   woken, and each leaves at its next safe point: back out of the wait with
   E_CANCELED, then on the way to ring 3 (usermode_kill_point). Returns before
   any of them has gone. A process with no thread running is left as it is. */
void proc_kill(struct proc *p, int killer);

/* After a note: one thread's SYS_EXIT or fault ends the whole process, so
   every other thread of it leaves the way proc_kill makes them, the note
   standing (pid `by`, the process itself, is the killer only if nothing was
   noted). Nothing for a process that does not run ring-3 code. */
void proc_end_threads(struct proc *p, int by);

/* proc_kill every process in `top` or a job below it that has not ended
   (job_kill). */
void proc_kill_within(struct job *top, int killer);

/* Ask `p` to end, for the process `by`, within `ms` (SYS_PROCESS_STOP): a
   stop notice on its stop endpoint, and a deadline after which it is ended as
   proc_kill would end it, noted PROC_STOPPED. A second stop moves the
   deadline closer, never later. With no stop endpoint, or one nobody can
   receive on, it is ended at once. A process that has ended is left as it
   is. */
void proc_stop(struct proc *p, int by, uint32_t ms);

/* `p`'s stop endpoint is now `ep` (a reference is taken), or none if NULL. */
void proc_set_stop_endpoint(struct proc *p, struct endpoint *ep);

/* From the timer interrupt (drivers/char/timer.c): end each process whose
   stop deadline is at or before `now`. proc_stop_next_due is the earliest
   armed, or TIMER_NEVER. Interrupts off, both. */
void     proc_stop_tick(uint64_t now);
uint64_t proc_stop_next_due(void);

/* Stop deadlines armed, for the self-tests. */
uint32_t proc_stops_armed(void);

/* Drop a reference taken by proc_create (or kobj_ref). */
void proc_put(struct proc *p);

/* The live process with this pid, or NULL. No reference is taken. */
struct proc *proc_by_pid(int pid);

/* Live processes, for the self-tests. */
/* The handle limit for processes made from now on (kernel.xkco). */
void proc_set_handle_limit(uint32_t n);
uint32_t proc_handle_limit(void);

uint32_t proc_live(void);

#endif
