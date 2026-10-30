// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/uthread.c */
/* Aurora Tejeda / CATX Systems */
/*
 * User threads (SYS_THREAD_CREATE; docs/kernel/CX_KERNEL_PLAN.md section 7).
 *
 * A further thread in a running process: the scheduler's record (a sibling
 * of the process's threads, sharing its space, handles, caps and job), a
 * stack the kernel maps for it with a guard page below (vmregion.h's
 * vm_map_stack; Haiku create_thread_user_stack, where Serenity and XNU take
 * the caller's), and a thread object its handle names (object/kthread.h), so
 * a join is a wait on SIGNAL_THREAD_ENDED.
 *
 * Ways out. SYS_THREAD_EXIT ends this thread (THR_EXITED). SYS_EXIT, a fault
 * or a kill in any thread ends the process, and with it every thread
 * (THR_WITH_PROCESS): proc_end_threads, then each leaves at its kill point.
 * Either way the thread comes back out of enter_usermode_thread here, gives
 * its stack back, and goes through thread_exit, which ends its object.
 */
#include "sched.h"
#include "proc.h"
#include "usermode.h"          /* user_copy_in */
#include "vmregion.h"
#include "heap.h"
#include "arch/paging.h"       /* USER_VLIMIT, PAGE_SIZE */
#include "arch/usermode.h"     /* enter_usermode_thread */
#include "object/htab.h"
#include "object/kthread.h"
#include "diag/crashlog.h"     /* crashlog_save_faults */
#include "cxk_abi.h"

/* What the thread starts with, and the stack to give back. Its record's arg,
   so it goes when the thread is reaped or discarded, however it ended. */
struct uthread_rec {
    uint64_t entry, a0, a1;
    uint64_t stack_base;   /* the region's, the guard's */
    uint64_t stack_top;
    int      pid;
    int      stack_live;   /* still mapped: the thread has not given it back */
};

static void uthread_drop(void *arg) {
    struct uthread_rec *r = arg;
    /* A thread that never ran never reached the trampoline's own unmap. */
    if (r->stack_live) vm_unmap_stack(r->pid, r->stack_base);
    kfree(r);
}

static void uthread_trampoline(void) {
    struct uthread_rec *r = thread_current_arg();
    /* rsp 8 below the top, as after a call: the word there is the anonymous
       page's 0, a return address that faults. */
    enter_usermode_thread((uintptr_t)r->entry, (uintptr_t)(r->stack_top - 8u),
                          thread_current_usave(), (uintptr_t)r->a0, (uintptr_t)r->a1);

    /* SYS_THREAD_EXIT, SYS_EXIT, a fault or a kill. The stack goes now, not
       at the reap, so a joiner that wakes finds its memory back. */
    crashlog_save_faults();
    vm_unmap_stack(r->pid, r->stack_base);
    r->stack_live = 0;
    thread_exit();
}

int syscall_thread_create(uint64_t a) {
    /* Unprivileged: the job's thread limit and the process's memory quota
       bound it. Only a ring-3 process: the kernel's own threads (the
       self-tests') run in a process no ring-3 thread belongs in. */
    struct proc *p = thread_proc(thread_current_id());
    if (!p || !p->is_user) return E_ACCESS_DENIED;
    struct thread_create_args ta;
    if (user_copy_in(&ta, a, sizeof ta) != 0) return E_BAD_ADDRESS;
    if (ta.flags || ta.reserved)                     return E_INVALID_ARGS;
    if (ta.entry == 0 || ta.entry >= USER_VLIMIT)    return E_INVALID_ARGS;
    /* As SYS_THREAD_SET_TLS: a base wrmsr would refuse (#GP, in the kernel)
       never reaches the switch. */
    if (ta.tls >= USER_VLIMIT)                       return E_INVALID_ARGS;
    uint64_t size = ta.stack_size ? ta.stack_size : THREAD_USTACK_DEFAULT;
    if (size < THREAD_USTACK_MIN || size > THREAD_USTACK_MAX) return E_OUT_OF_RANGE;
    uint32_t len = ((uint32_t)size + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);

    int pid = thread_current_pid();
    struct uthread_rec *r = kmalloc(sizeof *r);
    if (!r) return E_NO_MEMORY;
    r->entry = ta.entry;
    r->a0    = ta.arg0;
    r->a1    = ta.arg1;
    r->pid   = pid;
    int rc = vm_map_stack(pid, len, &r->stack_base, &r->stack_top);
    if (rc != E_OK) { kfree(r); return rc; }
    r->stack_live = 1;

    struct kthread *k = kthread_create(0);
    if (!k) { uthread_drop(r); return E_NO_MEMORY; }
    int id = thread_create_user("thread", uthread_trampoline);
    if (id < 0) {
        uthread_drop(r);
        kobj_unref(&k->hdr);
        return id == -1 ? E_NO_RESOURCES : id;
    }
    k->tid = id;
    thread_set_tls(id, ta.tls);            /* before it can run: none of its code runs without */
    thread_set_arg(id, r, uthread_drop);   /* the record's from here */
    thread_set_obj(id, k);

    int32_t h = htab_install(thread_htab(thread_current_id()), &k->hdr,
                             HRIGHT_WAIT | HRIGHT_INSPECT | HRIGHT_DUPLICATE, 0);
    kobj_unref(&k->hdr);   /* the thread's and the handle's references keep it */
    /* A thread whose creator could not be told of it does not run. */
    if (h < 0) { thread_discard(id); return h; }
    thread_start(id);
    return h;
}
