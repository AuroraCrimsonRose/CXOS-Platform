// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/job.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Jobs (docs/kernel/CX_KERNEL_PLAN.md section 4): a group of processes under
 * one memory and thread budget, which can be ended together. NT's job objects
 * (WRK ps/psjob.c), nested as Fuchsia nests them.
 *
 * Every process is in exactly one job for its whole life, and holds a
 * reference on it. Every job but the root has a parent, on which it holds a
 * reference; the root is the kernel's, has no limits and is never ended. A
 * process is placed by its creator: in the job its spawn named, or else in
 * its creator's own.
 *
 * WHAT IS CHARGED, and to whom. A charge is made to the process's job and to
 * every job above it, and is refused if any of them would pass its limit, so
 * a job's usage is everything its own processes and its descendants' hold,
 * and a child can never take more than its parent allows whatever its own
 * limit says (a limit of 0 is "none of its own").
 *   memory   what vm_charge and vm_map count for a process (memman/
 *            vmregion.h): its image, stack, argument and system pages, and
 *            its mappings. Strict, as NT's commit is: running out is a clean
 *            error from the call that asked, never a process killed later.
 *            The per-process quota stays; both must have room.
 *   threads  each thread of a process in it, from its creation until it is
 *            reaped (it holds its stacks until then).
 *
 * ENDING ONE: job_kill ends every process in the job and below it as
 * SYS_PROCESS_KILL would, and the job and every job below it are closed: no
 * process, thread or job joins them again. Once killed and empty, a job is
 * signalled SIGNAL_JOB_ENDED, which stays set.
 *
 * Everything here changes with interrupts off: one CPU today, and the
 * reaper's thread_free charges back from the timer's side of the world.
 */
#ifndef CXK_JOB_H
#define CXK_JOB_H

#include <stdint.h>
#include "object/kobj.h"

struct job {
    struct kobj hdr;          /* first; KOBJ_JOB */
    struct job *parent;       /* a reference; NULL only for the root */
    struct job *children;     /* jobs made under it, linked through sibling;
                                 no reference: a child unlinks itself as it goes */
    struct job *sibling;
    uint32_t    depth;        /* the root is 0 */
    uint64_t    mem_limit;    /* bytes; 0 = none of its own */
    uint64_t    mem_used;     /* in it and below */
    uint64_t    mem_peak;     /* the most mem_used has been */
    uint32_t    thread_limit; /* 0 = none of its own */
    uint32_t    prio_limit;   /* the highest level a thread sets itself to; 0 = none of its own */
    uint32_t    threads;      /* in it and below */
    uint32_t    procs;        /* processes in it and below that have not ended */
    int         killed;       /* job_kill reached it: nothing joins it again */
};

/* Register the type and make the root job. Once, before the first process. */
void job_sys_init(void);

/* The kernel's job, at the top. Never NULL after job_sys_init. */
struct job *job_root(void);

/* A new job under `parent`, with these limits (0 = none of its own). The
   caller holds the one reference. Returns NULL with *rc set: E_CANCELED if
   the parent has been killed, E_OUT_OF_RANGE past JOB_DEPTH_MAX, E_NO_MEMORY. */
struct job *job_create(struct job *parent, uint64_t mem_limit, uint32_t thread_limit,
                       uint32_t prio_limit, int *rc);

/* New limits. Ones below what is in use refuse the next charge, and take
   nothing back: a lower priority limit leaves threads where they are. */
void job_set_limits(struct job *j, uint64_t mem_limit, uint32_t thread_limit, uint32_t prio_limit);

/* The highest priority a thread in `j` may set itself to: the lowest limit
   of `j` and the jobs above it, PRIO_MAX if none has one. */
uint32_t job_prio_ceiling(const struct job *j);

/* Charge `j` and every job above it, or none of them: E_OK, or E_NO_MEMORY /
   E_NO_RESOURCES if one would pass its limit. The uncharges give back what a
   charge took, never more than is in use. */
int  job_charge_mem(struct job *j, uint64_t bytes);
void job_uncharge_mem(struct job *j, uint64_t bytes);
int  job_charge_thread(struct job *j);
void job_uncharge_thread(struct job *j);

/* A process joins `j` (proc_create), taking a reference, and leaves it when it
   ends (its last thread stops, or it never had one). Leaving may signal
   SIGNAL_JOB_ENDED. job_proc_in never refuses: a process in a killed job is
   refused its first thread (proc_thread_in) instead. */
void job_proc_in(struct job *j);
void job_proc_out(struct job *j);

/* Is `j` `top` or below it? */
int job_within(const struct job *j, const struct job *top);

/* End every process in `j` and below for the process `killer`, and close
   them all to new members. Returns before the processes have gone. */
void job_kill(struct job *j, int killer);

/* Drop a reference. */
void job_put(struct job *j);

/* Jobs that exist now, the root included, for the self-tests. */
uint32_t job_live(void);

#endif
