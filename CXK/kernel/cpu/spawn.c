/* /CXK/kernel/cpu/spawn.c */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/* ABI v1 spawn: launch a capability-less USER app in its own address space. */

#include "spawn.h"
#include "sched.h"
#include "handle.h"
#include "ipc.h"
#include "caps.h"
#include "usermode.h"          /* user_ptr_ok, enter_usermode */
#include "addr_space.h"
#include "paging.h"
#include "pmm.h"
#include "heap.h"
#include "cxex_load.h"
#include "logging.h"
#include <stddef.h>

#define USER_STACK_TOP    0xBFFFF000u
#define USER_STACK_PAGES  4u
#define SPAWN_MAX_IMAGE   (1u << 20)   /* 1 MiB cap on an app image */

/* per-pid record handed from sys_spawn (executive context) to the trampoline
   (app context, running in the app's own address space). */
struct spawn_rec {
    const void     *image;     /* kernel-heap copy of the app's CXEX bytes */
    uint32_t        image_len;
    struct addr_space space;
};
static struct spawn_rec spawn_recs[MAX_THREADS];

extern const struct cxex_load_ops cxex_kernel_load_ops;   /* cxex_loadk.c */
extern int enter_usermode(uint32_t entry_eip, uint32_t user_esp, uint32_t *save_slot);

/* Runs on the new thread, in the app's address space (the scheduler loaded its
   CR3 before switching here). Places the image, builds a ring-3 stack, and
   drops to user mode. Returns only when the app SYS_EXITs. */
static void spawn_trampoline(void) {
    int pid = thread_current_id();
    struct spawn_rec *r = &spawn_recs[pid];

    uint32_t entry = 0;
    if (cxex_load(r->image, r->image_len, &cxex_kernel_load_ops, &entry) != CXEX_LOAD_OK) {
        klog("SPAWN", SEV_ERR, "app image load failed");
        kfree((void *)r->image); r->image = NULL;
        thread_exit();
    }

    /* ring-3 stack, mirroring cxex_exec */
    for (uint32_t i = 0; i < USER_STACK_PAGES; i++) {
        void *p = pmm_alloc();
        if (!p) { klog("SPAWN", SEV_ERR, "no stack memory"); kfree((void *)r->image); thread_exit(); }
        paging_map(USER_STACK_TOP - (i + 1) * 0x1000u, (uint32_t)p,
                   PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
    }
    uint32_t ustack_top = USER_STACK_TOP - 16u;

    /* image is placed into the app's pages now; the heap copy is no longer needed */
    kfree((void *)r->image); r->image = NULL;

    enter_usermode(entry, ustack_top, thread_current_usave());

    /* app SYS_EXITed. sched_reap frees the kernel stack. NOTE: the app's address
       space frames/PTs are not yet reclaimed (addr_space teardown is stubbed -
       a known leak, fine for a one-shot demo). */
    thread_exit();   /* never returns */
}

int sys_spawn(const struct spawn_args *ua) {
    /* validate + copy the arg struct out of the caller's space */
    if (!user_ptr_ok((uint32_t)ua, sizeof *ua)) return E_FAULT;
    struct spawn_args a = *ua;

    if (a.image_len == 0 || a.image_len > SPAWN_MAX_IMAGE) return E_RANGE;
    if (!user_ptr_ok((uint32_t)a.image, a.image_len))      return E_FAULT;

    /* the broker channel must be a RECV endpoint the caller owns */
    struct endpoint *bep = ep_from_handle(a.broker_endpoint, HRIGHT_RECV);
    if (!bep) return E_BADF;

    /* copy the image into the kernel heap so the app thread (running in its own
       space) can read it; the heap is shared into every address space. */
    uint8_t *kimg = (uint8_t *)kmalloc(a.image_len);
    if (!kimg) return E_NOMEM;
    for (uint32_t i = 0; i < a.image_len; i++)
        kimg[i] = ((const uint8_t *)a.image)[i];

    struct addr_space space;
    if (addr_space_create(&space) != 0) { kfree(kimg); return E_NOMEM; }

    int pid = thread_create("app", spawn_trampoline);
    if (pid < 0)               { kfree(kimg); addr_space_destroy(&space); return E_NOMEM; }
    if (thread_alloc_kstack(pid) < 0) {
        kfree(kimg); addr_space_destroy(&space);
        return E_NOMEM;   /* (thread slot leaks, as in process_create_ring3) */
    }

    spawn_recs[pid].image     = kimg;
    spawn_recs[pid].image_len = a.image_len;
    spawn_recs[pid].space     = space;

    thread_mark_user(pid);
    thread_set_space(pid, space.pd_phys);   /* scheduler loads this CR3 for the app */
    thread_set_caps(pid, 0);                /* capability-less: must broker via IPC */

    /* broker channel: a SEND handle to the executive's endpoint, as handle 0
       (the app's table is empty, so this lands in slot 0 per the ABI). */
    int bh = thread_handle_install(pid, HANDLE_ENDPOINT, HRIGHT_SEND, bep);
    if (bh != 0) klog_u32("SPAWN", SEV_WARN, "broker handle not 0: ", (uint32_t)bh, LOG_COLOR_VALUE, "");

    klog_u32("SPAWN", SEV_OK, "app pid ", (uint32_t)pid, LOG_COLOR_VALUE, " (caps=0, ring 3)");
    return pid;
}