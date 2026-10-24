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
 * reference; the record goes when the last reference does (later, process
 * handles will hold some, which is how a parent waits without zombies: plan
 * section 4).
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
#include "object/kobj.h"
#include "object/htab.h"
#include "memman/vmregion.h"

/* Pids are below this. Pids come from a table that grows (object/idtab.h);
   this only bounds it. A process needs a thread to do anything, so the bound
   matches THREAD_LIMIT_MAX (cpu/sched.h). */
#define PROC_PID_MAX 256

struct proc {
    struct kobj hdr;        /* first; KOBJ_PROCESS */
    int         pid;
    int         threads;    /* threads naming this process, exited-unreaped included */
    int         is_user;    /* runs ring-3 code: never uid 0 (thread_set_uid) */
    uint32_t    uid;        /* owning user; 0 = SYSTEM */
    uint32_t    cwd;        /* CXFS entry id of the working directory; 0 = root */
    uint32_t    caps;       /* ABI capability bits */
    int         background; /* console output goes to the log (SPAWN_BACKGROUND); inherited */
    uint32_t    outlen;     /* bytes waiting in outbuf for their newline */
    char        outbuf[48]; /* one log line in the making (SLOG_EVT_W) */
    struct htab handles;
    int32_t     broker;     /* its SEND handle on its launcher's broker, or -1 */
    uint32_t    pd_phys;    /* its own page directory, freed with it; 0 = the kernel's */
    struct vm_proc vm;      /* its mappings and memory quota (memman/vmregion.h) */
};

/* Register the type and make the cache. Once, before the first proc_create. */
void proc_sys_init(void);

/* A new process with no threads yet, inheriting uid and cwd from `parent`
   (SYSTEM and the root if NULL) and nothing else: no caps, no handles, the
   kernel's address space. The caller holds the one reference; proc_thread_in
   gives one to each thread. NULL if out of pids or memory. */
struct proc *proc_create(const struct proc *parent);

/* A thread now names `p` (a reference), or no longer does. When the last
   thread is gone, every handle closes and the address space is freed; the
   record follows its last reference. */
void proc_thread_in(struct proc *p);
void proc_thread_gone(struct proc *p);

/* A background process's console output, collected into lines for the log:
   each newline, or a full line's worth, records one entry tagged "pid N".
   proc_out_flush records what is left without a newline; the last thread
   going calls it, so a final line without one is not lost. */
void proc_out_write(struct proc *p, const char *s, uint32_t len);
void proc_out_flush(struct proc *p);

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
