// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/ktest.c */
/* Aurora Tejeda / CATX Systems */
/*
 * Kernel self-tests, extracted from kmain. Each ktest_* function exercises one
 * subsystem and logs a concise pass/fail line. ktest_run() calls them in order.
 *
 * These quietly verify (single pass/fail line each) rather than printing the
 * verbose per-thread demo chatter the bring-up used - the goal now is
 * regression detection, not a live demo.
 */

#include <stdint.h>
#include "config.h"
#include "ktest.h"
#include "pmm.h"
#include "arch/paging.h"
#include "heap.h"
#include "rsa.h"
#include "console.h"
#include "logging.h"
#include "color.h"
#include "sched.h"
#include "ipc.h"
#include "object/htab.h"
#include "usermode.h"
#include "sysrec.h"
#include "memman/ocache.h"
#include "uid.h"
#include "disk.h"
#include "usb_storage.h"
#include "cxfs.h"
#include "partition.h"
#include "string.h"
#include "pci.h"
#include "timer.h"
#include "cxex_verify.h"
#include "cxex.h"
#include "keyvault.h"
#include "ktest_loader.h"
#include "ktest_net.h"
#include "ktest_fs.h"
#include "xfnt.h"
#include "font.h"
#include "fb.h"
#include "vmregion.h"
#include "exec.h"
#include "kstack.h"
#include "kconfig.h"
#include "arch/cpu.h"          /* arch_kernel_stack_current / _default */
#include "arch/cpu_ops.h"
#include "arch/usermode.h"     /* enter_usermode */
#include "arch/ktest_arch.h"
#include "drivers/iowin.h"
#include "acpi.h"
#include "keyboard.h"
#include "mouse.h"
#include "platform/ktest_platform.h"
#include "platform/platform.h"
#include "xhci_spad.h"
#include "boot/cxbi.h"
#include "diag/crashlog.h"
#include "memman/dma.h"
#include "e1000.h"
#include "rtl8169.h"
#include "ahci.h"
#include "diag/crashrec.h"


/* concise pass/fail reporter */
/* Report a test result. Stays SILENT on success - only failures are printed,
   so a clean boot is quiet and any problem stands out. Returns 1 if passed,
   0 if failed, so ktest_run can tally a summary. */
static int report(const char *name, int ok) {
    if (!ok)
        klog("KTEST", SEV_FAIL, name);

    return ok;
}

/* ---- IPC endpoints: reference counted, and reclaimed (security §7) ----
   Before this, closing an endpoint handle blanked the handle slot but left
   the endpoint's in_use set, so the 32-entry pool drained permanently: a
   process that created and closed endpoints could deny them to everyone, and
   every exited process leaked its own. The fix is a count, because an
   endpoint legitimately has several handles - the owner's RECV plus a SEND in
   every spawned child - so freeing on the first close would be a
   use-after-free rather than merely early.

   Each stage fails distinctly, so a regression says which rule broke. */
static int ep_refs(struct endpoint *ep)    { return (int)atomic_load(&ep->hdr.refs); }
static int ep_handles(struct endpoint *ep) { return (int)atomic_load(&ep->hdr.handles); }

/* ---- a process owns what its threads share (cpu/proc.h) ----
 * Two threads in one process, which has an address space of its own. The
 * sibling makes an endpoint and exits; the first thread, running after it,
 * must still find that handle, because the table is the process's and the
 * process has a thread left. Both see the same table, CR3 and pid, and a pid
 * that is not their thread id's business. When both are gone the process
 * has closed the endpoint and its record is freed. */
static volatile int      pt_sib_done, pt_first_done, pt_first_ok;
static volatile int32_t  pt_handle;
static volatile int      pt_pid[2], pt_tid[2];
static volatile paddr_t pt_space[2];
static struct htab *volatile pt_table[2];

static void pt_note(int i) {
    pt_space[i]   = addr_space_current();
    pt_pid[i]   = thread_current_pid();
    pt_tid[i]   = thread_current_id();
    pt_table[i] = thread_htab(thread_current_id());
}

static void pt_sibling(void) {
    pt_note(1);
    pt_handle = ep_create();      /* into the process's table, then leave it open */
    pt_sib_done = 1;
}

static void pt_first(void) {
    pt_note(0);
    for (int i = 0; i < 2000 && !pt_sib_done; i++) yield();
    for (int i = 0; i < 50; i++) yield();   /* let the sibling exit, and be reaped */
    struct endpoint *ep = pt_sib_done && pt_handle > 0
                        ? ep_from_handle(pt_handle, HRIGHT_RECV, NULL) : NULL;
    pt_first_ok = ep != NULL;
    ep_put(ep);
    pt_first_done = 1;
}

static int test_proc_threads(void) {
    uint32_t procs0 = proc_live();
    uint32_t eps0   = kobj_live(KOBJ_ENDPOINT);
    uint32_t free0  = pmm_free_count();
    pt_sib_done = pt_first_done = pt_first_ok = 0;
    pt_handle = -1;

    paddr_t space;
    if (addr_space_create(&space) != 0) return 0;
    int first = thread_create_process("pt-first", pt_first);
    if (first < 0) { addr_space_release(space); return 0; }
    thread_set_space(first, space);   /* the process's from here */
    int sib = thread_create_sibling(first, "pt-sibling", pt_sibling);
    if (sib < 0) { thread_discard(first); return 0; }
    if (proc_live() != procs0 + 1) return 0;   /* one process, two threads */
    thread_start(first);

    for (int i = 0; i < 400 && !(pt_first_done && !thread_is_alive(first)
                                 && !thread_is_alive(sib) && proc_live() == procs0); i++)
        thread_sleep_ms(1);

    int ok = pt_first_done && pt_first_ok
          && pt_table[0] && pt_table[0] == pt_table[1]
          && pt_space[0] == space && pt_space[1] == space
          && pt_pid[0] == pt_pid[1] && pt_pid[0] != thread_current_pid()
          && pt_tid[0] == first && pt_tid[1] == sib
          && proc_live() == procs0                       /* the record went */
          && kobj_live(KOBJ_ENDPOINT) == eps0            /* and closed its handle */
          && pmm_free_count() == free0;                  /* and freed its space */
    return ok;
}

/* ---- object caches on real pages (memman/ocache.h) ----
 * The host tests prove the allocator's logic; this proves its pages. A slab
 * is mapped in the window when it is made, and when its last object goes its
 * frames go back to the page allocator and the address stops being mapped,
 * so a stale pointer faults rather than reading what was there. */
static int test_ocache_pages(void) {
    static struct ocache c;
    if (ocache_init(&c, "ktest", 1000) != 0) return 0;
    if (c.pages != 1 || c.per_slab != 4) return 0;

    uint32_t free0 = pmm_free_count();
    void *o[9];
    for (int i = 0; i < 9; i++) {
        o[i] = ocache_alloc(&c);
        uintptr_t a = (uintptr_t)o[i];
        if (!o[i]) return 0;
        if (a < OCACHE_WINDOW_BASE || a >= OCACHE_WINDOW_BASE + OCACHE_SLOTS * OCACHE_SPAN) return 0;
        if (paging_get_phys(a) == 0) return 0;
    }
    if (ocache_slabs(&c) != 3) return 0;
    if (free0 - pmm_free_count() != 3 * c.pages) return 0;

    uintptr_t first = (uintptr_t)o[0] & ~(uintptr_t)0xFFF;
    for (int i = 0; i < 9; i++) ocache_free(&c, o[i]);
    if (ocache_slabs(&c) != 0 || ocache_live(&c) != 0) return 0;
    if (pmm_free_count() != free0) return 0;
    if (paging_get_phys(first) != 0) return 0;
    return 1;
}

static int test_ipc_endpoint_lifetime(void) {
    int me       = thread_current_id();
    struct htab *t = thread_htab(me);
    int baseline = ep_in_use_count();
    uint32_t live = kobj_live(KOBJ_ENDPOINT);

    /* 1. create then close: the slot comes back. A handle is never a small
       number, so nothing can assume one. */
    int h = ep_create();
    if (h < 256) return 0;
    if (ep_in_use_count() != baseline + 1) return 0;
    if (kobj_live(KOBJ_ENDPOINT) != live + 1) return 0;

    struct endpoint *ep = ep_from_handle(h, HRIGHT_RECV, NULL);
    /* the handle's reference plus this lookup's */
    if (!ep || ep_refs(ep) != 2 || ep_handles(ep) != 1 || !ep->active) return 0;
    if (!ep_is_cached(ep)) { ep_put(ep); return 0; }   /* from its cache, not the heap */
    ep_put(ep);

    if (htab_close(t, h) != 0) return 0;
    if (ep_in_use_count() != baseline) return 0;      /* the leak this fixes */
    if (kobj_live(KOBJ_ENDPOINT) != live) return 0;

    /* 2. a closed number is refused, even once its slot holds something else */
    int stale = h;
    h = ep_create();
    if (h < 0 || h == stale) return 0;
    if (ep_from_handle(stale, 0, NULL) != NULL) return 0;
    if (htab_close(t, stale) != E_BAD_HANDLE) return 0;

    /* 3. a second handle is a second reference: the first close must not
       free. It records the grant it came from. */
    uint32_t grant = 0;
    ep = ep_from_handle(h, HRIGHT_RECV, &grant);
    if (!ep || grant == 0) return 0;

    int h2 = ep_install_handle(me, ep, HRIGHT_SEND | HRIGHT_DUPLICATE, grant);
    if (h2 < 0) { ep_put(ep); return 0; }
    if (ep_refs(ep) != 3 || ep_handles(ep) != 2) { ep_put(ep); return 0; }
    ep_put(ep);

    struct htab_info info;
    struct kobj *o = htab_ref(t, h2, KOBJ_ENDPOINT, 0, &info);
    if (!o) return 0;
    kobj_unref(o);
    if (info.parent != grant || info.grant == grant || info.rights != (HRIGHT_SEND | HRIGHT_DUPLICATE)) return 0;

    if (htab_close(t, h) != 0) return 0;
    if (!ep_is_cached(ep) || !ep->active) return 0;   /* must still be alive */
    if (ep_refs(ep) != 1 || ep_handles(ep) != 1) return 0;
    if (ep_in_use_count() != baseline + 1) return 0;

    /* 4. rights only shrink: the SEND handle cannot become a RECV one, and
       no handle can carry a right endpoints do not define. */
    if (htab_dup(t, h2, HRIGHT_RECV) != E_ACCESS_DENIED) return 0;
    if (htab_dup(t, h2, HRIGHT_SEND | HRIGHT_RECV) != E_ACCESS_DENIED) return 0;
    if (ep_from_handle(h2, HRIGHT_RECV, NULL) != NULL) return 0;
    if (ep_install_handle(me, ep, 0x80u, grant) != E_ACCESS_DENIED) return 0;
    int h3 = htab_dup(t, h2, HRIGHT_SEND);
    if (h3 < 0) return 0;
    if (ep_handles(ep) != 2) return 0;
    if (htab_dup(t, h3, HRIGHT_SEND) != E_ACCESS_DENIED) return 0;   /* h3 was not given the duplicate right */
    if (htab_close(t, h3) != 0) return 0;

    /* 5. the last handle frees it, and the stale handle is a dead name. */
    if (htab_close(t, h2) != 0) return 0;
    if (ep_in_use_count() != baseline) return 0;
    if (ep_is_cached(ep)) return 0;                   /* back in its cache; not read, it may be unmapped */
    if (ep_from_handle(h2, HRIGHT_SEND, NULL) != NULL) return 0;
    if (kobj_live(KOBJ_ENDPOINT) != live) return 0;

    /* 6. the per-process quota holds, and releases. */
    int hs[MAX_ENDPOINTS_PER_PROC];
    int made = 0;
    for (int i = 0; i < MAX_ENDPOINTS_PER_PROC; i++) {
        hs[i] = ep_create();
        if (hs[i] < 0) break;
        made++;
    }
    int ok = (made == MAX_ENDPOINTS_PER_PROC) && (ep_create() < 0);
    for (int i = 0; i < made; i++) htab_close(t, hs[i]);
    if (!ok) return 0;
    if (ep_in_use_count() != baseline) return 0;
    if (kobj_live(KOBJ_ENDPOINT) != live) return 0;

    return 1;
}

/* ---- user copies: validated at the copy (security §6) ----
   The range check's half of user_copy_out: policy, decided before any byte
   moves. A page that is mapped and ring-3-readable but NOT ring-3-writable is
   refused here rather than by the hardware.

   Since CR0.WP was set, reaching the copy would refuse it too - the write
   faults and recovery turns that into the same E_BAD_ADDRESS - so the return value
   no longer says which layer acted. The assertions below are on the recovered
   fault COUNT for that reason: refused by the check means no fault at all.
   Delete the user_ptr_writable call in user_copy_out and the count moves,
   and this test alone goes red. Without that count the sabotage would pass
   unnoticed, which is the trap WP introduces here. */
static int test_user_copy_validation(void) {
    const uint32_t va = 0x00810000u;   /* clear of ktest_user_ptr_writability's page */
    const uint8_t  src[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    paddr_t frame = pmm_alloc();
    if (!frame) return 0;

    volatile uint8_t *p = (volatile uint8_t *)user_ptr(va);
    int ok = 1;

    /* Writable: the copy must succeed and the bytes must actually land. A test
       that only checks refusals passes just as well against a copy that always
       refuses. */
    if (paging_map_user(va, frame, VM_WRITE) != 0) { ok = 0; goto done; }
    for (int i = 0; i < 8; i++) p[i] = 0xAA;
    ok = ok && (user_copy_out(va, src, 8) == 0);
    for (int i = 0; i < 8; i++) ok = ok && (p[i] == src[i]);

    /* Read-only: refused by the CHECK, before the copy runs - which is the
       part that matters now that CR0.WP is set. With WP, reaching the copy
       would also refuse it, by faulting and recovering, and the return value
       would look identical. The fault count is what tells the two apart:
       refused by the check means no fault was taken at all. Delete the
       user_ptr_writable call in user_copy_out and this goes red on the count,
       not on the return value. */
    for (int i = 0; i < 8; i++) p[i] = 0xAA;
    if (paging_map_user(va, frame, 0) != 0) { ok = 0; goto done; }
    {
        uint32_t faults_before = usercopy_faults_recovered();
        ok = ok && (user_copy_out(va, src, 8) == E_BAD_ADDRESS);
        ok = ok && (usercopy_faults_recovered() == faults_before);   /* the sabotage signal */
    }
    for (int i = 0; i < 8; i++) ok = ok && (p[i] == 0xAA);

    /* Reading out of the same read-only page is legitimate - readable is all a
       copy-in needs - so the check must not have become "refuse everything". */
    {
        uint8_t dst[8] = { 0 };
        ok = ok && (user_copy_in(dst, va, 8) == 0);
        for (int i = 0; i < 8; i++) ok = ok && (dst[i] == 0xAA);
    }

    /* A length of zero touches nothing and succeeds, including through a null
       pointer: call sites rely on this for an absent buffer. */
    ok = ok && (user_copy_out(0, src, 0) == 0);
    ok = ok && (user_copy_in((void *)src, 0, 0) == 0);

    /* A buffer straddling the kernel boundary is refused by both. */
    ok = ok && (user_copy_out(USER_VLIMIT - 4, src, 8) == E_BAD_ADDRESS);
    {
        uint8_t dst[8] = { 0 };
        ok = ok && (user_copy_in(dst, USER_VLIMIT - 4, 8) == E_BAD_ADDRESS);
    }

done:
    paging_unmap(va);
    pmm_free(frame);
    return ok;
}

/* ---- user copies: a fault inside one is recoverable (security §6) ----
   The other half. An unmapped user page is what the range check cannot be
   made to cover, because a mapping can change after any check and before the
   instruction that uses it: the copy itself has to be allowed to fail.

   user_copy_out refuses this before the copy ever runs, which is the point of
   it, so the primitive is called directly - there is no other way to reach the
   recovery path.

   The sabotage signal here is a PANIC, not a red test, and it cannot be
   anything else: without the recovery in idt.c this write is an unrecoverable
   ring-0 page fault. Remove recover_user_copy_fault and the boot dies here
   with a page fault inside usermode.asm rather than reporting a failure. */
static int test_user_copy_fault_recovery(void) {
    const uint32_t va = 0x00820000u;
    const uint8_t  src[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    /* If something has mapped this, the test proves nothing - say so rather
       than passing vacuously. */
    if (paging_get_phys(va) != 0) return 0;

    uint32_t faults_before = usercopy_faults_recovered();

    if (user_copy_bytes(user_ptr(va), src, 8) != 1) return 0;   /* faulted, recovered */

    /* Reading from it is recoverable in the same way. */
    {
        uint8_t dst[8] = { 0 };
        if (user_copy_bytes(dst, user_ptr(va), 8) != 1) return 0;
    }

    /* Exactly two faults, not none and not a storm: the recovery really fired,
       once per copy, rather than the copies having succeeded for some other
       reason. */
    if (usercopy_faults_recovered() != faults_before + 2) return 0;

    /* A copy that does not fault still reports success afterwards, and takes no
       fault: recovery must not have left anything latched that makes every
       later copy look faulted. */
    {
        uint8_t dst[8] = { 0 };
        uint32_t f = usercopy_faults_recovered();
        if (user_copy_bytes(dst, src, 8) != 0) return 0;
        for (int i = 0; i < 8; i++) if (dst[i] != src[i]) return 0;
        if (usercopy_faults_recovered() != f) return 0;
    }

    /* And the validated path refuses it up front, without reaching the copy. */
    if (user_copy_out(va, src, 8) != E_BAD_ADDRESS) return 0;

    return 1;
}

/* ---- CR0.WP: the kernel obeys the read-only bit ----
   x86 lets ring 0 write through a read-only page unless CR0.WP is set
   (paging_kernel_obeys_readonly asks). For most of this kernel's life it was
   clear, which is why `user_ptr_writable` was the *only* thing standing
   between a syscall handed a pointer into a process's own text and the kernel
   scribbling on it (security review §4): the hardware would not have objected.

   Asserting the bit is set proves almost nothing on its own - a constant can
   be wrong in the same direction as the code reading it - so the real check is
   behavioural: a ring-0 write to a read-only user page must now FAULT. It is
   driven through user_copy_bytes because that is the one place a fault is
   survivable; anywhere else it would be a panic rather than a test result. */
static int test_cr0_write_protect(void) {
    const uint32_t va = 0x00830000u;
    const uint8_t  src[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    if (!paging_kernel_obeys_readonly()) return 0;   /* WP must be set */

    paddr_t frame = pmm_alloc();
    if (!frame) return 0;

    int ok = 1;
    volatile uint8_t *p = (volatile uint8_t *)user_ptr(va);

    /* Seed while it is still writable, then take the write permission away. */
    if (paging_map_user(va, frame, VM_WRITE) != 0) { ok = 0; goto done; }
    for (int i = 0; i < 8; i++) p[i] = 0xAA;
    if (paging_map_user(va, frame, 0) != 0) { ok = 0; goto done; }

    {
        uint32_t faults_before = usercopy_faults_recovered();
        /* Before WP this returned 0 and the bytes changed. */
        ok = ok && (user_copy_bytes(user_ptr(va), src, 8) == 1);
        ok = ok && (usercopy_faults_recovered() == faults_before + 1);
        for (int i = 0; i < 8; i++) ok = ok && (p[i] == 0xAA);
    }

    /* Writable again: the same write must go through, so this is testing WP
       and not simply a mapping that never worked. */
    if (paging_map_user(va, frame, VM_WRITE) != 0) { ok = 0; goto done; }
    {
        uint32_t faults_before = usercopy_faults_recovered();
        ok = ok && (user_copy_bytes(user_ptr(va), src, 8) == 0);
        ok = ok && (usercopy_faults_recovered() == faults_before);
        for (int i = 0; i < 8; i++) ok = ok && (p[i] == src[i]);
    }

done:
    paging_unmap(va);
    pmm_free(frame);
    return ok;
}

/* ---- I/O windows (drivers/iowin.h): every driver brought up so far has stayed
   inside its windows, and a window refuses an access that is not wholly inside
   it, port-mapped or memory-mapped, without reaching the device. ---- */
static int test_iowin(void) {
    int ok = 1;
    /* Taken first: a driver that reached past its window during boot has
       counted here - the port drivers (serial, the PIC, ATA, ACPI) and the
       memory-mapped ones on whatever bus they found (the APICs, AHCI, the
       NICs, the USB hosts, an MSI-X entry). */
    if (iowin_refusals != 0) {
        klog("KTEST", SEV_FAIL, "iowin: a driver reached outside its window");
        ok = 0;
    }
    uint32_t before = iowin_refusals;

    /* Memory-mapped, over ordinary memory: what is inside reaches it, what is
       past the end does not, even though the memory is there. */
    static volatile uint8_t regs[16];
    for (int i = 0; i < 16; i++) regs[i] = 0;
    struct iowin m;
    ok = ok && iowin_mmio(&m, regs, 8) == 0;
    iowin_write32(&m, 4, 0x11223344u);
    ok = ok && regs[4] == 0x44 && regs[7] == 0x11 && iowin_read16(&m, 6) == 0x1122;
    iowin_write32(&m, 6, 0xFFFFFFFFu);      /* straddles the end */
    iowin_write8(&m, 8, 0xFF);              /* just past it */
    ok = ok && iowin_read32(&m, 5) == 0xFFFFFFFFu;
    for (int i = 8; i < 16; i++) ok = ok && regs[i] == 0;
    ok = ok && regs[6] == 0x22;

    /* A port window: the POST diagnostic port, which any read or write is safe
       on. One byte wide, so a 16- or 32-bit access is refused before it runs. */
    struct iowin p;
    ok = ok && iowin_port(&p, 0x80, 1) == 0;
    iowin_read8(&p, 0);
    ok = ok && iowin_read16(&p, 0) == 0xFFFF && iowin_read32(&p, 0) == 0xFFFFFFFFu;
    ok = ok && iowin_port(&p, 0xFFFF, 2) == -1;

    ok = ok && iowin_refusals - before == 5;
    iowin_refusals = before;   /* leave the count for real drivers */
    return ok;
}

/* ---- paging: map a scratch frame, write+read it back; and the refusals of
   arch/paging.h: a kernel mapping is not reachable from ring 3, a user mapping
   cannot name kernel memory, and a flag that is not VM_* maps nothing. ---- */
static int test_paging(void) {
    uintptr_t test_virt = KVA_KTEST_SCRATCH;   /* kernel half, mapped by nothing else */
    uintptr_t user_virt = 0x3F000000;          /* user half, unused by the kernel space */
    paddr_t frame = pmm_alloc();
    if (!frame) return 0;
    paging_map_kernel(test_virt, frame, VM_WRITE);
    volatile uint32_t *p = (volatile uint32_t *)test_virt;
    *p = 0xCAFEBABE;
    int ok = (*p == 0xCAFEBABE) && (paging_get_phys(test_virt) == frame);
    ok = ok && !paging_is_user(test_virt);
    paging_unmap(test_virt);

    ok = ok && paging_map_user(test_virt, frame, VM_WRITE) == -1
            && paging_get_phys(test_virt) == 0;
    ok = ok && paging_map_user(user_virt, frame, VM_ALL + 1) == -1
            && paging_get_phys(user_virt) == 0;
    paging_map_kernel(test_virt, frame, VM_ALL + 1);
    ok = ok && paging_get_phys(test_virt) == 0;
    paging_unmap(test_virt);   /* whichever of these a broken pager made */
    paging_unmap(user_virt);
    pmm_free(frame);
    return ok;
}

/* ---- heap: alloc three blocks, verify no cross-corruption, free+reuse ---- */
static int test_heap(void) {
    char *a = (char *)kmalloc(64);
    char *b = (char *)kmalloc(128);
    char *c = (char *)kmalloc(64);
    int ok = (a && b && c);
    if (ok) {
        for (int i = 0; i < 64; i++)  a[i] = (char)i;
        for (int i = 0; i < 128; i++) b[i] = (char)(i ^ 0x5A);
        for (int i = 0; i < 64; i++)  c[i] = (char)(i + 1);
        for (int i = 0; i < 64; i++)  if (a[i] != (char)i)          ok = 0;
        for (int i = 0; i < 128; i++) if (b[i] != (char)(i ^ 0x5A)) ok = 0;
        for (int i = 0; i < 64; i++)  if (c[i] != (char)(i + 1))    ok = 0;
        kfree(b);
        char *d = (char *)kmalloc(100);
        if (!d) ok = 0;
        kfree(a); kfree(c); kfree(d);
    }

    /* Up to 2048 bytes: the size-class caches, zeroed, never across a page.
       Larger: the list, which the heap's own count sees. */
    uint32_t used0 = heap_bytes_used();
    uint8_t *s1 = kmalloc(1), *s2 = kmalloc(2048), *big = kmalloc(2049);
    if (!s1 || !s2 || !big) ok = 0;
    if (ok) {
        if (!ocache_owner(s1) || !ocache_owner(s2) || ocache_owner(s1) == ocache_owner(s2)) ok = 0;
        if (ocache_in_window(big)) ok = 0;
        if (((uintptr_t)s2 & 0xFFFu) + 2048u > 0x1000u) ok = 0;
        for (int i = 0; i < 2048; i++) if (s2[i]) { ok = 0; break; }
        if (heap_bytes_used() < used0 + 2049u) ok = 0;
        for (int i = 0; i < 2048; i++) s2[i] = 0xEE;
        kfree(s2);
        s2 = kmalloc(2000);              /* the same class: handed out zeroed again */
        for (int i = 0; s2 && i < 2000; i++) if (s2[i]) { ok = 0; break; }
    }
    struct ocache *c1 = s1 ? ocache_owner(s1) : 0;
    uint32_t live1 = c1 ? ocache_live(c1) : 0;
    kfree(s1); kfree(s2); kfree(big);
    if (!c1 || ocache_live(c1) != live1 - 1) ok = 0;   /* a small free really frees */
    if (heap_bytes_used() != used0) ok = 0;
    return ok;
}

/* ---- cooperative scheduling: two threads that yield ---- */
static volatile int coop_a_ran = 0, coop_b_ran = 0;
static void coop_thread_a(void) { for (int i = 0; i < 3; i++) { coop_a_ran++; yield(); } }
static void coop_thread_b(void) { for (int i = 0; i < 3; i++) { coop_b_ran++; yield(); } }

static int test_sched_coop(void) {
    coop_a_ran = coop_b_ran = 0;
    thread_create("coop_a", coop_thread_a);
    thread_create("coop_b", coop_thread_b);
    for (int i = 0; i < 8 && sched_active_count() > 1; i++) yield();
    /* both threads ran their iterations and exited */
    return (coop_a_ran == 3) && (coop_b_ran == 3) && (sched_active_count() == 1);
}

/* ---- preemptive scheduling: threads that never yield, driven by the timer ---- */
static volatile uint32_t pre_a = 0, pre_b = 0;
static void pre_thread_a(void) { for (uint32_t i = 0; i < 30000000u; i++) pre_a = i; }
static void pre_thread_b(void) { for (uint32_t i = 0; i < 30000000u; i++) pre_b = i; }

static int test_sched_preempt(void) {
    pre_a = pre_b = 0;
    thread_create("pre_a", pre_thread_a);
    thread_create("pre_b", pre_thread_b);
    sched_preempt_enable(5);
    while (sched_active_count() > 1) { arch_cpu_relax(); }
    sched_preempt_disable();
    /* both spinners completed only because the timer preempted them */
    return (pre_a != 0) && (pre_b != 0) && (sched_active_count() == 1);
}

/* ---- the esp0 invariant (PROCESS_MODEL.md §8) ----
 * After every switch tss.esp0 is the running thread's own esp0 stack, or the
 * dedicated gdt_init stack when it has none. Checked from both sides: a
 * kernel-only thread must see the dedicated stack, not the previous thread's,
 * and the boot thread must get its own back. Before 2026-10-16 a kernel-only
 * thread inherited whatever esp0 the last process left behind. */
static volatile uintptr_t esp0_seen_by_kthread = 0;
static void esp0_kthread(void) { esp0_seen_by_kthread = arch_kernel_stack_current(); }

static int test_esp0_follows_thread(void) {
    uintptr_t mine = thread_current_kstack_top();
    /* Vacuous unless the two stacks differ: kmain gives thread 0 its own esp0. */
    if (mine == 0 || mine == arch_kernel_stack_default()) return 0;

    esp0_seen_by_kthread = 0;
    if (thread_create("esp0_k", esp0_kthread) < 0) return 0;
    for (int i = 0; i < 8 && sched_active_count() > 1; i++) yield();

    return esp0_seen_by_kthread == arch_kernel_stack_default()
        && arch_kernel_stack_current() == mine
        && sched_active_count() == 1;
}

/* ---- ring 3: single round-trip (3a) ---- */
static int test_ring3_single(void) {
    return usermode_test() == 0;
}

/* ---- the one recording point (cpu/sysrec.h) ----
 * Every syscall passes it on the way in, and every call that returns passes it
 * on the way out. Checked against two witnesses counted elsewhere: the traps
 * syscall_stub took, and the calls the dispatcher handled. A path that reaches
 * the dispatcher, or returns to ring 3, without passing the point moves the
 * witnesses and not the recorder. */
static int test_sysrec(void) {
    struct sysrec_stats s0, s1, s2;

    /* a kernel-side call: through the point, not through the gate */
    sysrec_snapshot(&s0);
    uint32_t t0 = syscall_trapped_count(), d0 = syscall_dispatch_count();
    int pid = syscall_dispatch(SYS_GETPID, 0, 0, 0, 0, 0);
    sysrec_snapshot(&s1);
    if (s1.entered - s0.entered != 1 || s1.exited - s0.exited != 1) return 0;
    if (syscall_dispatch_count() - d0 != 1) return 0;
    if (syscall_trapped_count() != t0) return 0;
    if (s1.last_num != SYS_GETPID || s1.last_rc != pid) return 0;

    /* ring 3: the routine traps twice, SYS_CONSOLE_WRITE then SYS_EXIT. Exit
       does not return, so it has an entry and no exit, and the last call out
       is the write. */
    uint32_t t1 = syscall_trapped_count(), d1 = syscall_dispatch_count();
    if (usermode_test() != 0) return 0;
    sysrec_snapshot(&s2);
    uint32_t traps = syscall_trapped_count() - t1;
    if (traps != 2) return 0;
    if (s2.entered - s1.entered != traps) return 0;
    if (syscall_dispatch_count() - d1 != traps) return 0;
    if (s2.exited - s1.exited != traps - 1) return 0;
    if (s2.last_num != SYS_CONSOLE_WRITE) return 0;

    /* The exit came in with all five registers: the routine loads markers into
       a2..a5 (each arch's usermode.asm, user_blob), which the gate must
       carry even though SYS_EXIT reads only a1. */
    if (s2.in_num != SYS_EXIT || s2.in_args[0] != 0) return 0;
    for (uint32_t i = 1; i < SYSCALL_ARGS; i++)
        if (s2.in_args[i] != 0xC0DE0001u + i) return 0;
    return 1;
}

/* ---- the generated dispatch table (systable.h) ----
 * A number the table has no handler for is E_NOT_SUPPORTED, whether it falls in a hole
 * between syscalls or past the last one, and it still passes the recording
 * point. A number past the end must be refused before it indexes the table:
 * 0xFFFFFFFF would otherwise read four gigabytes beyond it. */
static int test_syscall_table(void) {
    static const uint32_t unknown[] = { 0x07, 0x7F, SYSCALL_TABLE_SIZE, 0xFFFFFFFFu };
    for (uint32_t i = 0; i < sizeof unknown / sizeof unknown[0]; i++) {
        struct sysrec_stats s0, s1;
        sysrec_snapshot(&s0);
        if (syscall_dispatch(unknown[i], 1, 2, 3, 4, 5) != E_NOT_SUPPORTED) return 0;
        sysrec_snapshot(&s1);
        if (s1.entered - s0.entered != 1 || s1.exited - s0.exited != 1) return 0;
        if (s1.in_num != unknown[i] || s1.last_rc != E_NOT_SUPPORTED) return 0;
        for (uint32_t k = 0; k < SYSCALL_ARGS; k++)
            if (s1.in_args[k] != k + 1) return 0;
    }

    /* every slot the interface fills, and no other */
    uint32_t filled = 0;
    for (uint32_t n = 0; n < SYSCALL_TABLE_SIZE; n++) filled += syscall_table[n] != 0;
    return filled == SYSCALL_COUNT && syscall_table[SYS_GETPID] && syscall_table[SYS_VM_MAP];
}

/* ---- ring 3: scheduler-integrated + preemptible processes (3b/3c) ---- */
extern uint8_t user_blob_start[];
extern uint8_t user_blob_end[];
extern uint8_t user_blob_busy_start[];
extern uint8_t user_blob_busy_end[];

static int test_ring3_processes(void) {
    /* Every helper must end by SYS_EXIT, not by a fault: they share the
       kernel space's system page, and one that unmapped it under the others
       would still let them all end. */
    uint32_t faults0 = usermode_faults(), refused0 = syscall_refused_count();

    /* 3b: cooperative ring-3 processes */
    uint32_t blen = (uint32_t)(user_blob_end - user_blob_start);
    process_create_ring3("user1", user_blob_start, blen, "");
    process_create_ring3("user2", user_blob_start, blen, "");
    for (int i = 0; i < 12 && sched_active_count() > 1; i++) yield();
    int coop_ok = (sched_active_count() == 1);

    /* 3c: preemptible ring-3 processes */
    uint32_t blen2 = (uint32_t)(user_blob_busy_end - user_blob_busy_start);
    process_create_ring3("busy1", user_blob_busy_start, blen2, "");
    process_create_ring3("busy2", user_blob_busy_start, blen2, "");
    sched_preempt_enable(5);
    while (sched_active_count() > 1) { arch_cpu_relax(); }
    sched_preempt_disable();
    int preempt_ok = (sched_active_count() == 1);

    return coop_ok && preempt_ok && usermode_faults() == faults0
        && syscall_refused_count() == refused0;
}

/* ---- identity: user can never be UID 0 (SYSTEM) ---- */
static int test_identity(void) {
    uint32_t blen = (uint32_t)(user_blob_end - user_blob_start);

    /* 1. launching a user process as UID 0 must be REJECTED */
    int bad = process_create_ring3_as_user("baduser", user_blob_start, blen, 0, UID_SYSTEM);
    if (bad != -1) return 0;   /* should have been refused */

    /* 2. launching as a real user (UID >= 1) must succeed */
    int uid7 = process_create_ring3_as_user("user7", user_blob_start, blen,"", 7);
    if (uid7 < 0) return 0;

    /* drive it to completion so it reaps cleanly */
    for (int i = 0; i < 12 && sched_active_count() > 1; i++) yield();

    /* the launch-as-SYSTEM rejection above is the observable proof of the
       "user can never be UID 0" invariant. */
    return sched_active_count() == 1;
}

/* An LBA past what the ATA driver can address must be refused, not truncated.
   ata.c is LBA28 - the top nibble goes in the drive-select register - so
   anything over 0x0FFFFFFF used to be cast down to a real but WRONG sector.
   On a write that is the worst kind of failure: success reported, damage done
   somewhere else entirely. */
static int test_disk_lba_range(void) {
    if (disk_count() == 0) return 1;          /* diskless config: nothing to check */

    const struct disk *d = 0;
    for (unsigned i = 0; i < disk_count(); i++) {
        const struct disk *c = disk_get(i);
        if (c && c->driver == DISK_DRV_ATA) { d = c; break; }
    }
    if (!d) {
        /* Say so rather than return a quiet pass. On q35 the disk arrives through
           AHCI, which is LBA48 and not what this checks, so the whole test is
           vacuous there - and a vacuous pass that looks identical to a real one
           is how a check stops meaning anything. Boot with `-machine pc` to get
           legacy IDE and exercise it. */
        klog("KTEST", SEV_WARN, "ATA LBA range NOT checked: no ATA disk (AHCI is LBA48)");
        return 1;
    }

    static uint8_t sector[512];

    /* The control: a legal read still works, so this cannot pass by the path
       being broken for everything. */
    if (disk_read(d->id, 0, 1, sector) != DISK_OK) return 0;

    /* One past the LBA28 ceiling, and far past it.
     *
     * These asserted DISK_ERR_PARAMS until 2026-10-09, which is the code
     * Phase 1 chose on the review's recommendation (engineering §1). Writing
     * the block-device contract down (§8, disk.h) made that untenable: the
     * enum documents BOUNDS as "LBA/count outside the device" and PARAMS as
     * "bad arguments (null buffer, zero count)", and an unreachable sector is
     * the former by the enum's own words. A caller cannot act differently on
     * the two either - both mean "that sector is not there" - so keeping both
     * was a distinction without a decision behind it.
     *
     * The refusal now also comes from a different place, which is the real
     * improvement: disk_read compares the request against the capacity the
     * registry already knew, so EVERY backend refuses an over-range LBA
     * rather than only ATA, and only because ATA's own limit happened to be
     * narrower. ata_range_ok stays as defence in depth and keeps returning
     * PARAMS; it is simply no longer the thing that catches this. */
    if (disk_read(d->id, 0x10000000ull, 1, sector) != DISK_ERR_BOUNDS) return 0;
    if (disk_read(d->id, 0xFFFFFFFFFull, 1, sector) != DISK_ERR_BOUNDS) return 0;

    return 1;
}

/* ---- storage: read sector 0 from a registered disk ---- */
static int test_storage(void) {
    if (disk_count() == 0) {
        /* no disks registered - not a failure on a diskless config, but report
           it so it's visible. treat as "skipped/pass" if genuinely none. */
        return 1;
    }
    const struct disk *d = disk_get(0);
    if (!d) return 0;

    static uint8_t sector[512];
    int rc = disk_read(d->id, 0, 1, sector);
    if (rc != DISK_OK) return 0;

    /* a CXK-imaged disk has the 0x55AA boot signature at offset 510 of LBA0
       (boot disk) - or for a data disk, we at least confirmed the read path
       returned data without error. The read succeeding is the core proof. */
    return 1;
}

/* ---- cxfs: create a file, write, read back, verify ---- */
/* read-only by default; full write round-trip only in dev builds */
static int test_cxfs(void) {
    if (!cxfs_is_mounted()) return 1;   /* no CXFS mounted - skip (not a failure) */

#if CXK_ALLOW_DISK_WRITE
    /* DEV ONLY (scratch disk): create a file, write, read back, verify. */
    /* Scratch goes in /Temp, not the root. The root is a documented namespace
       now (docs/x86_32/system/CX_FILESYSTEM_LAYOUT.md) and a test file sitting in it is
       litter that every `ls /` shows forever. Falls back to the root on a
       volume old enough to have no /Temp. */
    int dir = cxfs_resolve("/Temp", 0);
    if (dir < 0) dir = 0;
    int fid = cxfs_find_in_dir((uint32_t)dir, "ktest.txt");
    if (fid < 0) fid = cxfs_create_entry((uint32_t)dir, "ktest.txt", CXFS_TYPE_FILE);
    if (fid < 0) return 0;
    const char *msg = "CXK CXFS round-trip: hello from the filesystem!";
    uint32_t len = 0;
    while (msg[len]) len++;
    if (cxfs_write_file((uint32_t)fid, msg, len) != 0) return 0;
    char buf[128];
    int n = cxfs_read_file((uint32_t)fid, buf, sizeof(buf));
    if (n != (int)len) return 0;
    for (uint32_t i = 0; i < len; i++) if (buf[i] != msg[i]) return 0;
    return 1;
#else
    /* SAFE DEFAULT: read-only check - confirm the mounted filesystem is
       readable by resolving the root directory. No writes, no formatting. */
    return cxfs_resolve("/", 0) >= 0;
#endif
}


/* ---- cxfs: the offset-based layer (read_at / write_at / truncate) ----
 *
 * The whole-file API could not express any of this: it frees every block and
 * rewrites from zero, so there was nothing to test but "the bytes came back".
 * These are the cases the offset layer has to get right for anything that
 * builds a file incrementally, and the last one is the case the old code simply
 * refused - a file that has run out of extents.
 */
static uint8_t off_buf[CXFS_BLOCK_SIZE];   /* static: 4KB has no business on an 8KB thread stack */

#if CXK_ALLOW_DISK_WRITE
/* Fetch-or-create a scratch file at an absolute path, truncated to empty.
   Splits at the last '/' so the file lands in /Temp rather than the root. */
static int scratch_file(const char *path) {
    int id = cxfs_resolve(path, 0);
    if (id < 0) {
        int slash = -1;
        for (int i = 0; path[i]; i++) if (path[i] == '/') slash = i;
        if (slash < 0) return -1;

        char dir[128];
        int n = 0;
        for (; n < slash && n < (int)sizeof dir - 1; n++) dir[n] = path[n];
        dir[n] = '\0';
        int parent = (slash == 0) ? 0 : cxfs_resolve(dir, 0);
        if (parent < 0) parent = 0;            /* no /Temp: fall back to the root */

        id = cxfs_create_entry((uint32_t)parent, path + slash + 1, CXFS_TYPE_FILE);
        if (id < 0) return -1;
    }
    if (cxfs_truncate((uint32_t)id, 0) != CXFS_E_OK) return -1;
    return id;
}
#endif

static int test_cxfs_offset(void) {
    if (!cxfs_is_mounted()) return 1;   /* nothing mounted - skip */

#if CXK_ALLOW_DISK_WRITE
    int a = scratch_file("/Temp/kt_off.bin");
    if (a < 0) return 0;
    uint32_t A = (uint32_t)a;
    char buf[32];

    /* --- a partial write must leave the bytes either side alone --- */
    memset(off_buf, 'A', CXFS_BLOCK_SIZE);
    if (cxfs_write_at(A, 0, off_buf, CXFS_BLOCK_SIZE) != (int)CXFS_BLOCK_SIZE) return 0;
    if (cxfs_write_at(A, CXFS_BLOCK_SIZE, off_buf, CXFS_BLOCK_SIZE) != (int)CXFS_BLOCK_SIZE) return 0;
    if (cxfs_write_at(A, 100, "XYZ", 3) != 3) return 0;
    if (cxfs_read_at(A, 98, buf, 7) != 7) return 0;
    if (buf[0] != 'A' || buf[1] != 'A' || buf[2] != 'X' || buf[3] != 'Y' ||
        buf[4] != 'Z' || buf[5] != 'A' || buf[6] != 'A') return 0;

    /* --- a write straddling a block boundary must cross it correctly --- */
    if (cxfs_write_at(A, CXFS_BLOCK_SIZE - 2, "LMNO", 4) != 4) return 0;
    if (cxfs_read_at(A, CXFS_BLOCK_SIZE - 2, buf, 4) != 4) return 0;
    if (buf[0] != 'L' || buf[1] != 'M' || buf[2] != 'N' || buf[3] != 'O') return 0;

    /* --- a hole reads back as zeros, not as whatever the block used to hold ---
       This is the one that matters for more than correctness: the block handed
       out here was previously owned by another file. */
    if (cxfs_truncate(A, 0) != CXFS_E_OK) return 0;
    if (cxfs_write_at(A, 2 * CXFS_BLOCK_SIZE, "tail", 4) != 4) return 0;
    struct cxfs_entry e;
    if (cxfs_read_entry(A, &e) != 0) return 0;
    if (e.size != 2 * CXFS_BLOCK_SIZE + 4) return 0;
    if (cxfs_read_at(A, 0, buf, 16) != 16) return 0;
    for (int i = 0; i < 16; i++) if (buf[i] != 0) return 0;
    if (cxfs_read_at(A, CXFS_BLOCK_SIZE, buf, 16) != 16) return 0;
    for (int i = 0; i < 16; i++) if (buf[i] != 0) return 0;
    if (cxfs_read_at(A, 2 * CXFS_BLOCK_SIZE, buf, 4) != 4) return 0;
    if (buf[0] != 't' || buf[3] != 'l') return 0;

    /* --- a read is short at EOF and empty past it --- */
    if (cxfs_read_at(A, 2 * CXFS_BLOCK_SIZE + 2, buf, 100) != 2) return 0;
    if (cxfs_read_at(A, e.size, buf, 100) != 0) return 0;
    if (cxfs_read_at(A, e.size + 1000, buf, 100) != 0) return 0;

    /* --- shrinking then growing must not resurrect the bytes it dropped --- */
    if (cxfs_truncate(A, 2 * CXFS_BLOCK_SIZE + 1) != CXFS_E_OK) return 0;  /* keeps "t" */
    if (cxfs_truncate(A, 2 * CXFS_BLOCK_SIZE + 4) != CXFS_E_OK) return 0;  /* back again */
    if (cxfs_read_at(A, 2 * CXFS_BLOCK_SIZE, buf, 4) != 4) return 0;
    if (buf[0] != 't') return 0;
    if (buf[1] != 0 || buf[2] != 0 || buf[3] != 0) return 0;   /* "ail" is gone */

    /* --- extent exhaustion has to compact, not fail ---
     * CXFS_MAX_EXTENTS is 8. Appending a block to A and then to B in turn means
     * every one of A's blocks is separated from the last by one of B's, so A
     * gains a new extent each time instead of extending the one it has. The
     * ninth append has no extent slot left, which is exactly where the old
     * whole-file write gave up with "too fragmented / too big for v1".
     */
    int b = scratch_file("/Temp/kt_frag.bin");
    if (b < 0) return 0;
    uint32_t B = (uint32_t)b;
    if (cxfs_truncate(A, 0) != CXFS_E_OK) return 0;

    const int NB = 11;                        /* > CXFS_MAX_EXTENTS */
    for (int i = 0; i < NB; i++) {
        memset(off_buf, 'a' + i, CXFS_BLOCK_SIZE);   /* per-block marker */
        if (cxfs_write_at(A, (uint64_t)i * CXFS_BLOCK_SIZE, off_buf, CXFS_BLOCK_SIZE)
            != (int)CXFS_BLOCK_SIZE) return 0;
        memset(off_buf, 'Z', CXFS_BLOCK_SIZE);       /* B takes the next block */
        if (cxfs_write_at(B, (uint64_t)i * CXFS_BLOCK_SIZE, off_buf, CXFS_BLOCK_SIZE)
            != (int)CXFS_BLOCK_SIZE) return 0;
    }

    /* every block of A still holds its own marker: compaction moved the data
       without reordering or dropping any of it */
    for (int i = 0; i < NB; i++) {
        if (cxfs_read_at(A, (uint64_t)i * CXFS_BLOCK_SIZE, buf, 4) != 4) return 0;
        for (int j = 0; j < 4; j++) if (buf[j] != 'a' + i) return 0;
    }
    /* and B's blocks were not disturbed by A compacting around them */
    for (int i = 0; i < NB; i++) {
        if (cxfs_read_at(B, (uint64_t)i * CXFS_BLOCK_SIZE, buf, 4) != 4) return 0;
        for (int j = 0; j < 4; j++) if (buf[j] != 'Z') return 0;
    }

    /* A is describable in the 8 extents it has, which is the point of compacting */
    if (cxfs_read_entry(A, &e) != 0) return 0;
    int used = 0;
    for (int i = 0; i < CXFS_MAX_EXTENTS; i++) if (e.extent_len[i]) used++;
    if (used < 1 || used > CXFS_MAX_EXTENTS) return 0;
    if (e.size != (uint64_t)NB * CXFS_BLOCK_SIZE) return 0;

    /* tidy up so a re-run starts from the same state */
    cxfs_delete_entry(A);
    cxfs_delete_entry(B);
    return 1;
#else
    /* read-only build: prove the offset reader agrees with the whole-file
       reader on the root directory's existence and nothing more. */
    return cxfs_resolve("/", 0) >= 0;
#endif
}

/* ---- pci: enumeration found devices ---- */
static int test_pci(void) {
    /* a PC always has at least a host bridge; QEMU i440FX has several devices.
       finding zero means config-space access (0xCF8/0xCFC) isn't working. */
    if (pci_device_count() == 0) return 0;

    /* sanity: the first device should have a real vendor id (not 0xFFFF). */
    const struct pci_device *d = pci_get(0);
    if (!d || d->vendor_id == 0xFFFF) return 0;

    return 1;
}

/* ---- ahci: if an AHCI controller + disk is present, read sector 0 ---- */
static int test_ahci(void) {
    /* find an AHCI disk in the registry (may be none on an IDE-only machine,
       which is not a failure - we report pass/skip). */
    const struct disk *ad = 0;
    for (unsigned i = 0; i < disk_count(); i++) {
        const struct disk *d = disk_get(i);
        if (d && d->driver == DISK_DRV_AHCI) { ad = d; break; }
    }
    if (!ad) return 1;   /* no AHCI controller/disk present - skip (pass) */

    static uint8_t sector[512];
    return disk_read(ad->id, 0, 1, sector) == DISK_OK;
}

/* ---- device DMA memory is the allocator's, below 4 GB (memman/dma.h, roadmap step 5f2) ----
 *
 * A device bus-masters by physical address with no page tables in the way, so
 * memory it was given that the allocator still thinks is free gets handed to
 * someone else and overwritten under them with no fault. The NICs once used
 * fixed regions that only a reserve in pmm.c kept safe, and x86-64's page
 * database never had that reserve. Every region dma_alloc gave out must be
 * held by the allocator, lie below 4 GB (each driver writes 32-bit addresses)
 * and overlap no other. And since a ledger only speaks for what went through
 * dma_alloc, the addresses the NICs and the AHCI ports actually hold are read
 * back from their registers and must fall inside a region of their own name. */
static int dma_owned_by(uint64_t phys, const char *name) {
    for (unsigned i = 0; i < dma_region_count(); i++) {
        const struct dma_region *r = dma_region_at(i);
        if (strcmp(r->name, name) == 0 && phys >= r->phys
            && phys < (uint64_t)r->phys + (uint64_t)r->pages * 0x1000)
            return 1;
    }
    return 0;
}

static int test_dma_regions(void) {
    unsigned n = dma_region_count();
    for (unsigned i = 0; i < n; i++) {
        const struct dma_region *r = dma_region_at(i);
        uint64_t lo = r->phys, hi = lo + (uint64_t)r->pages * 0x1000;
        if (r->pages == 0 || (lo & 0xFFF) || hi > 0x100000000ull) return 0;
        for (uint64_t p = lo; p < hi; p += 0x1000)
            if (!pmm_page_in_use((paddr_t)p)) return 0;
        for (unsigned j = 0; j < i; j++) {
            const struct dma_region *o = dma_region_at(j);
            if (lo < (uint64_t)o->phys + (uint64_t)o->pages * 0x1000 && o->phys < hi) return 0;
        }
    }
    if (dma_region_at(n) != 0) return 0;   /* the ledger ends where it says */

    int checked = 0;
    if (e1000_present()) {
        if (!dma_owned_by(e1000_ring_base(), "e1000")) return 0;
        checked++;
    }
    if (rtl8169_present()) {
        if (!dma_owned_by(rtl8169_ring_base(), "rtl8169")) return 0;
        checked++;
    }
    for (int p = 0; p < 32; p++) {   /* AHCI's port limit; ahci_present refuses past it */
        if (!ahci_present(p)) continue;
        if (!dma_owned_by(ahci_command_list(p), "ahci")) return 0;
        checked++;
    }
    if (!checked) klog("KTEST", SEV_WARN, "dma regions: no NIC or AHCI disk, partly untested");
    return 1;
}

/* ---- signature verification ----
 *
 * The CXSG block now carries the signer's public key, and the verifier checks
 * integrity against THAT key rather than against one we already hold. That is
 * what lets an unknown publisher's image be shown intact - and it is also the
 * thing that would be catastrophic to get subtly wrong, because every failure
 * mode here looks like success.
 *
 * So this tampers on purpose. A real signed image off the disk is verified,
 * then three single-byte edits are made to a copy and each must be caught:
 *
 *   a byte of the payload     - the signature no longer covers these bytes
 *   a byte of the fingerprint - the block now names a key it does not carry
 *   a byte of the key itself  - the signature no longer matches the modulus
 *
 * The fingerprint case is the interesting one. Nothing is DECIDED by the
 * fingerprint - trust is settled by comparing key bytes - so a lax verifier
 * would pass it. It still has to be rejected, because that field is what ends
 * up in a log line naming who signed something, and a block allowed to name a
 * publisher it cannot produce is a block that can lie about its author.
 *
 * Skipped on an unsigned build: there is nothing to tamper with.
 */
static int test_cxex_signature(void) {
    if (!cxfs_is_mounted()) return 1;

    int id = cxfs_resolve("/Shared/Programs/hi.xuex", 0);
    if (id < 0) return 1;                        /* nothing staged - skip */

    struct cxfs_entry e;
    if (cxfs_read_entry((uint32_t)id, &e) != 0) return 0;
    if (e.size == 0 || e.size > (1u << 20))     return 0;

    uint8_t *img = (uint8_t *)kmalloc((size_t)e.size);
    if (!img) return 0;
    int ok = 0;

    if (cxfs_read_file((uint32_t)id, img, (uint32_t)e.size) != (int)e.size) goto done;

    /* An unsigned build stages an unsigned image; that is not a failure. */
    if (cxex_verify_self(img, (size_t)e.size) == CXEX_VERIFY_UNSIGNED) { ok = 1; goto done; }

    /* The real thing verifies against its own carried key, and the vault has
       an opinion about the signer. Deliberately NOT asserting that opinion is
       "platform": this image is a .xuex, and a .xuex signed by a publisher is
       a legitimate thing that must still pass every integrity check here. */
    if (cxex_verify_self(img, (size_t)e.size) != CXEX_VERIFY_OK) goto done;
    if (keyvault_trust_of(img, (size_t)e.size) < 0)              goto done;

    uint16_t pklen = 0;
    const uint8_t *pk = cxex_signer_key(img, (size_t)e.size, &pklen);
    if (!pk || pklen == 0) goto done;

    struct cxex_header h;
    struct cxex_sig sig;
    if (cxex_parse_header(img, (size_t)e.size, &h) != 0) goto done;
    if (cxex_get_sig(img, (size_t)e.size, &h, &sig) != 0) goto done;

    /* 1. a byte of the signed payload */
    img[16] ^= 0xFF;
    if (cxex_verify_self(img, (size_t)e.size) != CXEX_VERIFY_BAD_SIGNATURE) goto done;
    img[16] ^= 0xFF;

    /* 2. a byte of the fingerprint - the block must describe what it carries */
    uint32_t fp_off = h.signature_offset + 8;
    img[fp_off] ^= 0xFF;
    if (cxex_verify_self(img, (size_t)e.size) != CXEX_VERIFY_BAD_KEY) goto done;
    img[fp_off] ^= 0xFF;

    /* 3. a byte of the carried key itself */
    img[sig.pubkey_file_offset + pklen - 1] ^= 0xFF;
    if (cxex_verify_self(img, (size_t)e.size) == CXEX_VERIFY_OK) goto done;
    img[sig.pubkey_file_offset + pklen - 1] ^= 0xFF;

    /* and it still verifies once every edit is undone, which is what says the
       failures above were the edits and not something left broken */
    if (cxex_verify_self(img, (size_t)e.size) != CXEX_VERIFY_OK) goto done;

    ok = 1;
done:
    kfree(img);
    return ok;
}

/* ---- clock and timed sleep ----
 *
 * The failure worth catching is a sleep that returns immediately, and that is
 * exactly the one a "did it come back?" test cannot see. So this measures.
 *
 * The lower bound is the assertion: a sleep must not finish early, because
 * everything built on it - a scheduler waiting for the next due task, a
 * retry backing off - is wrong if it can.
 *
 * The upper bound is the other half of engineering §7: a wakeup bug that
 * turns a 50 ms sleep into a multi-second stall is a real failure and a
 * "did it come back?" test cannot see it either. The bound is 100 ms, which
 * is twice what this actually takes - measured, not guessed: instrumented on
 * 2026-10-09, `thread_sleep_ms(50)` returns after exactly 50 ticks, so the
 * previous 500 ms ceiling was ten times looser than the behaviour it was
 * bounding and would have passed a sleep that overran by 9x. The 2x headroom
 * is for scheduling: this is cooperative, and whatever ktest has left
 * runnable finishes first.
 *
 * That the ceiling matters is not an assumption either: with thread_sleep_ms
 * sabotaged to wait four times as long, this test fails at 100 ms and
 * **passes at 500**, all 32 green. The old bound was wide enough to admit the
 * exact failure it was there to catch.
 */
static int test_clock_sleep(void) {
    uint32_t t0 = timer_ticks();
    thread_sleep_ms(50);
    uint32_t dt = timer_ticks() - t0;
    if (dt < 50)  return 0;        /* woke early - the bug that matters */
    if (dt > 100) return 0;        /* a 50 ms sleep that became a long stall */

    /* 0 ms is documented as a yield, not a wait. */
    t0 = timer_ticks();
    thread_sleep_ms(0);
    if ((timer_ticks() - t0) > 50) return 0;

    /* The clock must advance, and must do so monotonically. */
    uint32_t a = timer_ticks();
    thread_sleep_ms(10);
    uint32_t b = timer_ticks();
    if ((int32_t)(b - a) < 10) return 0;

    return 1;
}

/* ---- a tick is a millisecond, measured by a clock the PIT does not drive ----
 *
 * test_clock_sleep compares the tick with itself, and the TSC is calibrated
 * from it, so a timer programmed at the wrong rate passes both: left at the
 * BIOS's 18.2 Hz, every "millisecond" is 55. The ACPI PM timer runs from its
 * own crystal. 100 ticks must be 100 ms of it, within a fifth: QEMU delivers
 * timer interrupts late under load, and a wrong divisor is out by far more. */
static int test_tick_rate(void) {
    uint32_t p0, p1;
    if (acpi_pm_timer_read(&p0) != 0) {
        klog("KTEST", SEV_WARN, "tick rate untested: no ACPI PM timer");
        return 1;
    }
    const uint32_t want = ACPI_PM_TIMER_HZ / 10;          /* 100 ms of PM timer */
    const uint32_t limit = ACPI_PM_TIMER_HZ * 2;          /* give up after 2 s */

    uint32_t t0 = timer_ticks();
    acpi_pm_timer_read(&p0);
    while (timer_ticks() == t0) {                        /* align to a tick edge */
        acpi_pm_timer_read(&p1);
        if (((p1 - p0) & 0xFFFFFFu) > limit) goto stalled;
    }
    t0 = timer_ticks();
    acpi_pm_timer_read(&p0);
    for (;;) {
        acpi_pm_timer_read(&p1);
        uint32_t pm = (p1 - p0) & 0xFFFFFFu;
        if (timer_ticks() - t0 >= 100) {
            if (pm < want - want / 5 || pm > want + want / 5) {
                klog("KTEST", SEV_FAIL, "tick rate: 100 ticks were not 100 ms of the PM timer");
                return 0;
            }
            return 1;
        }
        if (pm > limit) goto stalled;
    }
stalled:
    klog("KTEST", SEV_FAIL, "tick rate: 100 ticks took over 2 s of the PM timer");
    return 0;
}

/* ---- xHCI scratchpad pages, as a controller asking for them would get ----
 * QEMU's xHCI asks for none, so no boot gives a controller any; this builds
 * what one asking for 600 would get (two array pages, far past the 16 the
 * driver once had room for) and checks it: every entry a distinct, page-
 * aligned, 32-bit page, every page zero, and all of it back to the page
 * allocator afterwards. */
#define SPAD_TEST_N 600u
static int test_xhci_scratchpad(void) {
    static uint32_t pages[SPAD_TEST_N];
    uint32_t free0 = pmm_free_count();
    paddr_t arr;
    if (xhci_spad_build(SPAD_TEST_N, &arr) != 0 || arr == 0 || (arr & 0x3F)) {
        klog("KTEST", SEV_FAIL, "scratchpad: not built, or the array is misaligned");
        return 0;
    }
    int ok = 1;
    for (uint32_t i = 0; i < SPAD_TEST_N && ok; i++) {
        uint64_t e = xhci_spad_entry(arr, i);
        if (e == 0 || (e & 0xFFF) || (e >> 32)) {
            klog_u32("KTEST", SEV_FAIL, "scratchpad: bad entry ", i, LOG_COLOR_VALUE, "");
            ok = 0;
        }
        pages[i] = (uint32_t)e;
        for (uint32_t j = 0; j < i && ok; j++)
            if (pages[j] == pages[i]) {
                klog_u32("KTEST", SEV_FAIL, "scratchpad: page given twice, entry ", i,
                         LOG_COLOR_VALUE, "");
                ok = 0;
            }
    }
    for (uint32_t i = 0; i < SPAD_TEST_N && ok; i++) {
        const volatile uint32_t *v = (const volatile uint32_t *)paging_temp_map(pages[i]);
        for (uint32_t w = 0; w < PAGE_SIZE / 4; w++)
            if (v[w]) { ok = 0; break; }
        paging_temp_unmap();
        if (!ok) klog_u32("KTEST", SEV_FAIL, "scratchpad: page not zero, entry ", i,
                          LOG_COLOR_VALUE, "");
    }
    xhci_spad_release(arr, SPAD_TEST_N);
    if (pmm_free_count() != free0) {
        klog("KTEST", SEV_FAIL, "scratchpad: pages not all returned");
        ok = 0;
    }
    return ok;
}

/* ---- the boot handover came through CXBI (boot/cxbi.h, roadmap step 4) ----
 * Everything the kernel takes from the loader is in one structure: a memory
 * map with usable memory in it and no type CXBI lacks, the RSDP ACPI is
 * running from, and the framebuffer the screen is drawn on. A BIOS loader
 * also poisons where it used to leave the E820 map (0x500) and the VBE mode
 * (0x1C40), so a reader of those old addresses gets nonsense; that it did is
 * checked here, and the boot failing when pmm.c or fb.c is pointed back at
 * them is the sabotage that shows nothing reads them. */

/* 16 bits of low memory, through the scratch mapping: x86-64 does not keep the
   loader's view of it at KERNEL_VBASE. `phys` must not straddle a page. */
static uint16_t low_u16(paddr_t phys) {
    const volatile uint8_t *page = paging_temp_map(phys & ~(paddr_t)0xFFF);
    uint16_t v = *(const volatile uint16_t *)(page + (phys & 0xFFF));
    paging_temp_unmap();
    return v;
}

static int test_cxbi(void) {
    const struct cxk_boot_info *b = cxbi();
    if (b->firmware != CXBI_FW_BIOS && b->firmware != CXBI_FW_UEFI) {
        klog("KTEST", SEV_FAIL, "CXBI: the handover names no firmware");
        return 0;
    }
    uint64_t usable = 0;
    for (uint32_t i = 0; i < cxbi_mmap_count(); i++) {
        const struct cxk_mmap_entry *e = cxbi_mmap(i);
        if (e->type < CXBI_MEM_USABLE || e->type > CXBI_MEM_FRAMEBUFFER) {
            klog_u32("KTEST", SEV_FAIL, "CXBI: memory map type not in CXBI: ", e->type,
                     LOG_COLOR_VALUE, "");
            return 0;
        }
        if (e->type == CXBI_MEM_USABLE && e->base >= 0x100000) usable += e->length;
    }
    if (usable < (uint64_t)pmm_total_pages() * PAGE_SIZE / 2 || cxbi_mmap_dropped()) {
        klog("KTEST", SEV_FAIL, "CXBI: the memory map is short of usable memory");
        return 0;
    }
    if (b->acpi_rsdp == 0 || !acpi_can_shutdown()) {
        klog("KTEST", SEV_FAIL, "CXBI: no RSDP handed over, or ACPI did not start from it");
        return 0;
    }
    uint32_t fp, fs;
    if (fb_get_region(&fp, &fs) &&
        (fp != b->fb_addr || fs != b->fb_size || fb_width() != b->fb_width)) {
        klog("KTEST", SEV_FAIL, "CXBI: the screen is not the framebuffer handed over");
        return 0;
    }
    if (b->firmware == CXBI_FW_BIOS && (low_u16(0x500) != 0xA5A5u || (low_u16(0x1C40) & 0xFFu) != 0xA5u)) {
        klog("KTEST", SEV_FAIL, "CXBI: the loader's old addresses are not poisoned");
        return 0;
    }
    return 1;
}

/* ---- writes reach only the CXOS disk (the install guard) --------------------
 * With automatic install off the kernel must write to no disk but the one it
 * booted from. Checked three ways: the restriction is on, it names the disk
 * the loader read the kernel from (the XBPT id in CXBI boot_disk_id) and the
 * system volume and the crash area are on, and the disk layer applies it -
 * with every disk refused for a moment, a write to the CXOS disk itself must
 * come back DISK_ERR_DENIED and leave the sector as it was. The sector is the
 * last of the crash area, which nothing else uses while the tests run. A boot
 * with a second disk that must come out unchanged is `cxk verify-boot
 * --foreign-disk`.
 *
 * The moment every disk is refused cannot cost another thread a write: the
 * scheduler is cooperative (sched_preempt_enable is for contained tests only),
 * and a refused disk_write returns before the driver, so nothing yields
 * between closing the disks and opening the CXOS one again.
 *
 * With automatic install on, the promise is the opposite one - nothing is
 * restricted - and that is what is checked. */
static int test_disk_write_guard(void) {
#if CXK_AUTO_INSTALL
    if (disk_write_home() != DISK_WRITES_ANY) {
        klog("KTEST", SEV_FAIL, "write guard: automatic install on, but writes are restricted");
        return 0;
    }
    return 1;
#elif !CXK_ALLOW_DISK_WRITE
    klog("KTEST", SEV_WARN, "write guard untested: this build writes to no disk");
    return 1;
#else
    int home = disk_write_home();
    if (home < 0) {
        klog("KTEST", SEV_FAIL, "write guard: writes are not limited to the CXOS disk");
        return 0;
    }
    /* and the disk is the one the loader read, by the id it handed over */
    uint64_t have = 0, want = cxbi()->boot_disk_id;
    if (want == 0 || part_disk_id((uint8_t)home, &have) != 0 || have != want) {
        klog("KTEST", SEV_FAIL, "write guard: the CXOS disk is not the one the loader named");
        return 0;
    }
    uint8_t disk;
    uint64_t lba, secs;
    if (crashlog_area(&disk, &lba, &secs) != 0 || disk != (uint8_t)home ||
        !cxfs_is_mounted() || cxfs_get_id() != (uint8_t)home) {
        klog("KTEST", SEV_FAIL, "write guard: the CXOS disk is not the one in use");
        return 0;
    }
    static uint8_t before[512], probe[512], after[512];
    uint64_t at = lba + secs - 1;
    if (disk_read(disk, at, 1, before) != DISK_OK) return 0;
    for (int i = 0; i < 512; i++) probe[i] = (uint8_t)(before[i] ^ 0xA5);

    disk_restrict_writes(-1);
    int r = disk_write(disk, at, 1, probe);
    /* nothing lifts the restriction, and an id that is no disk closes them all */
    disk_restrict_writes(DISK_WRITES_ANY);
    int lifted = disk_write_home();
    disk_restrict_writes(0xFE);
    int bogus = disk_find_by_id(0xFE) ? -1 : disk_write_home();
    disk_restrict_writes(home);

    int ok = 1;
    if (r != DISK_ERR_DENIED) {
        klog("KTEST", SEV_FAIL, "write guard: a refused disk took a write");
        ok = 0;
    }
    if (lifted != -1 || bogus != -1) {
        klog("KTEST", SEV_FAIL, "write guard: the restriction was lifted, or opened to no disk");
        ok = 0;
    }
    if (disk_read(disk, at, 1, after) != DISK_OK || memcmp(before, after, 512) != 0) {
        klog("KTEST", SEV_FAIL, "write guard: the refused sector changed");
        ok = 0;
        (void)disk_write(disk, at, 1, before);
    }
    /* and the CXOS disk is writable again */
    if (disk_write(disk, at, 1, before) != DISK_OK) ok = 0;
    return ok;
#endif
}

/* The system log goes to the crash area after the crash record and reads back
   whole: a valid XCRL_KIND_BOOTLOG header at XCRL_BOOTLOG_LBA whose CRC the text
   passes, the text starting with the summary line and holding a line logged
   just before - a child line, which the ring did not used to keep. The crash
   record's own sector is left as it was. */
static int test_bootlog_saved(void) {
#if !CXK_ALLOW_DISK_WRITE
    klog("KTEST", SEV_WARN, "boot log untested: this build writes to no disk");
    return 1;
#endif
    uint8_t disk;
    uint64_t lba, secs;
    if (crashlog_area(&disk, &lba, &secs) != 0) return 0;
    static uint8_t h0[XCRL_SECTOR], h[XCRL_SECTOR], h0after[XCRL_SECTOR];
    static char text[XCRL_BOOTLOG_MAX];
    if (disk_read(disk, lba, 1, h0) != DISK_OK) return 0;

    klog("KTEST", SEV_INFO, "boot log test line");
    klog_child("bootlog-child-marker");
    if (crashlog_save_bootlog() != DISK_OK) return 0;
    if (crashlog_bootlog_stale()) return 0;                      /* nothing logged since */

    uint16_t kind = 0; uint32_t len = 0;
    if (disk_read(disk, lba + XCRL_BOOTLOG_LBA, 1, h) != DISK_OK) return 0;
    if (xcrl_check_header(h, &kind, &len) != XCRL_VALID || kind != XCRL_KIND_BOOTLOG) return 0;
    if (len == 0 || len > XCRL_BOOTLOG_MAX) return 0;
    uint32_t n = (len + XCRL_SECTOR - 1) / XCRL_SECTOR;
    if (disk_read(disk, lba + XCRL_BOOTLOG_LBA + 1, n, text) != DISK_OK) return 0;
    if (xcrl_check_text(h, text, len) != XCRL_VALID) return 0;

    static const char head[] = "CXOS system log: ";
    for (uint32_t i = 0; head[i]; i++) if (text[i] != head[i]) return 0;
    static const char mark[] = "-> bootlog-child-marker";
    int found = 0;
    for (uint32_t i = 0; !found && i + sizeof mark - 1 <= len; i++) {
        uint32_t k = 0;
        while (mark[k] && text[i + k] == mark[k]) k++;
        found = mark[k] == 0;
    }
    if (!found) return 0;

    if (disk_read(disk, lba, 1, h0after) != DISK_OK) return 0;
    for (uint32_t i = 0; i < XCRL_SECTOR; i++) if (h0[i] != h0after[i]) return 0;
    return 1;
}

/* ---- a crash record goes to the crash area and reads back (step 4b) -------
 * The writes a panic makes (crashlog_write_record), read back raw and checked
 * as the next boot checks them; then one byte of the text is changed on disk
 * and the check must refuse it; then the area is cleared, so the next boot
 * does not recover this test's record as a crash. The two-boot version, a
 * real panic recovered by the next boot, is `cxk verify-crashlog`. */
static int test_crashlog(void) {
#if !CXK_ALLOW_DISK_WRITE
    klog("KTEST", SEV_WARN, "crash log untested: this build writes to no disk");
    return 1;
#endif
    uint8_t disk;
    uint64_t lba, secs;
    if (crashlog_area(&disk, &lba, &secs) != 0) {
        klog("KTEST", SEV_FAIL, "crash log: no crash area on any disk");
        return 0;
    }
    int ok = 1;

    /* A boot that recovered a record must have put exactly that in the file:
       the area is cleared once the file is written, so a short or wrong file
       would be all that is left of the crash. */
    uint32_t rlen, rcrc;
    if (crashlog_recovered(&rlen, &rcrc)) {
        static char back[XCRL_TEXT_MAX];
        int id = cxfs_resolve("/System/crash.log", 0);
        int got = id < 0 ? -1 : cxfs_read_file((uint32_t)id, back, sizeof back);
        if (got != (int)rlen || xcrl_crc32(0, back, rlen) != rcrc) {
            klog("KTEST", SEV_FAIL, "crash log: /System/crash.log is not the record recovered");
            ok = 0;
        }
    }

    static char wr[2 * XCRL_SECTOR], rd[2 * XCRL_SECTOR];
    static uint8_t h[XCRL_SECTOR];

    /* By now the boot has dealt with whatever the area held: recovered and
       cleared it, cleared a request, or discarded a torn record. Anything
       still there was left behind, and is not overwritten here - it may be
       the only copy of a crash. */
    if (disk_read(disk, lba, 1, h) != DISK_OK || xcrl_check_header(h, 0, 0) != XCRL_EMPTY) {
        klog("KTEST", SEV_FAIL, "crash log: the area still holds a record; left as it is");
        return 0;
    }

    const uint32_t len = XCRL_SECTOR + 77;          /* into a second sector */
    for (uint32_t i = 0; i < sizeof wr; i++) wr[i] = i < len ? (char)('A' + i % 26) : 0;

    uint16_t kind = 0;
    uint32_t n = 0;
    if (crashlog_write_record(XCRL_KIND_CRASH, wr, len) != DISK_OK ||
        disk_read(disk, lba, 1, h) != DISK_OK || disk_read(disk, lba + 1, 2, rd) != DISK_OK) {
        klog("KTEST", SEV_FAIL, "crash log: the area could not be written or read");
        ok = 0;
    } else if (xcrl_check_header(h, &kind, &n) != XCRL_VALID || kind != XCRL_KIND_CRASH ||
               n != len || xcrl_check_text(h, rd, n) != XCRL_VALID || memcmp(rd, wr, len) != 0) {
        klog("KTEST", SEV_FAIL, "crash log: the record did not read back whole");
        ok = 0;
    } else {
        rd[100] ^= 0x01;
        if (disk_write(disk, lba + 1, 2, rd) != DISK_OK || disk_read(disk, lba + 1, 2, rd) != DISK_OK ||
            xcrl_check_text(h, rd, n) != XCRL_BAD_CRC) {
            klog("KTEST", SEV_FAIL, "crash log: a torn record was not refused");
            ok = 0;
        }
    }
    if (crashlog_clear() != DISK_OK || disk_read(disk, lba, 1, h) != DISK_OK ||
        xcrl_check_header(h, 0, 0) != XCRL_EMPTY) {
        klog("KTEST", SEV_FAIL, "crash log: the area did not clear");
        ok = 0;
    }
    return ok;
}

static int kt_has(const char *hay, uint32_t n, const char *needle) {
    uint32_t m = 0;
    while (needle[m]) m++;
    for (uint32_t i = 0; i + m <= n; i++)
        if (memcmp(hay + i, needle, m) == 0) return 1;
    return 0;
}

/* ---- a user fault after the self-tests is saved (crashlog_save_faults) ----
 * A ring-3 process whose first instruction is ud2 faults the way pids 2 and
 * 3 do on the FX: through usermode_fault, which notes it, and back out of
 * enter_usermode into the trampoline, which saves it. So the record read
 * back here was written by the same calls a real fault makes, not by the
 * test. Faults are armed only for this test until the executive starts. */
static const uint8_t kt_ud2[2] = { 0x0F, 0x0B };

static int test_user_fault_saved(void) {
#if !CXK_ALLOW_DISK_WRITE
    return 1;
#endif
    uint8_t disk;
    uint64_t lba, secs;
    if (crashlog_area(&disk, &lba, &secs) != 0) return 1;   /* test_crashlog says why */

    static char rd[3 * XCRL_SECTOR];
    if (disk_read(disk, lba, 1, rd) != DISK_OK || xcrl_check_header((const uint8_t *)rd, 0, 0) != XCRL_EMPTY)
        return 0;                                /* a record left behind: not overwritten */

    uint32_t saved0 = crashlog_faults_saved();
    crashlog_faults_arm(1);
    int pid = process_create_ring3("faultrec", kt_ud2, sizeof kt_ud2, "");
    for (int i = 0; i < 3000 && pid >= 0 && crashlog_faults_saved() == saved0; i++)
        thread_sleep_ms(1);
    crashlog_faults_arm(0);

    int ok = pid >= 0 && crashlog_faults_saved() == saved0 + 1;
    uint16_t kind = 0;
    uint32_t n = 0;
    ok = ok && disk_read(disk, lba, 3, rd) == DISK_OK
            && xcrl_check_header((const uint8_t *)rd, &kind, &n) == XCRL_VALID
            && kind == XCRL_KIND_FAULT;
    uint32_t look = n < 2 * XCRL_SECTOR ? n : 2 * XCRL_SECTOR;
    ok = ok && kt_has(rd + XCRL_SECTOR, look, "user faults since the self-tests: 1\n")
            && kt_has(rd + XCRL_SECTOR, look, "(faultrec) exception 6 at 0x")
            && kt_has(rd + XCRL_SECTOR, look, "---- system log");

    crashlog_faults_forget();
    if (crashlog_clear() != DISK_OK) ok = 0;
    return ok;
}

/* ---- the wall clock reads a plausible date (platform_wallclock, step 3) ----
 * File times and SYS_CLOCK's epoch come from here. The clock's own encoding
 * (BCD, the century, 12-hour mode on a PC) is the platform's to decode; a
 * field it left undecoded is out of range for most of the year (October is
 * BCD 0x10, read raw as month 16), and an epoch of 0 means no date at all.
 * A platform with no clock is untested, with a WARN. */
static int test_wallclock(void) {
    struct datetime d;
    if (platform_wallclock(&d) != 0) {
        klog("KTEST", SEV_WARN, "wall clock untested: the platform has none");
        return 1;
    }
    if (d.year < 2020 || d.year > 2199 || d.month < 1 || d.month > 12 ||
        d.day < 1 || d.day > 31 || d.hour > 23 || d.minute > 59 || d.second > 59 ||
        datetime_to_epoch(&d) == 0) {
        klog_u32("KTEST", SEV_FAIL, "wall clock: implausible date, year ", d.year,
                 LOG_COLOR_VALUE, "");
        klog_u32("KTEST", SEV_FAIL, "wall clock: month ", d.month, LOG_COLOR_VALUE, "");
        return 0;
    }
    return 1;
}

/* ---- the cycle counter measures time (arch_cycles32, roadmap step 3e) ----
 * Ping's round-trip times come from it, and before this nothing looked: a
 * counter that never moved was "calibrated" at 1 MHz, logged as OK, and every
 * interval read 0. 20 ms of sleep must read as 20 ms, within the slack QEMU's
 * late timer interrupts need. The calibration came from the tick, so this
 * proves the counter runs and the arithmetic holds, not the tick (that is
 * test_tick_rate). */
static int test_cycle_counter(void) {
    uint32_t c0 = timer_tsc32();
    thread_sleep_ms(20);
    uint32_t us = timer_us_since(c0);
    if (timer_tsc_mhz() < 2 || us < 15000 || us > 60000) {
        klog_u32("KTEST", SEV_FAIL, "cycle counter: 20 ms read as us ", us, LOG_COLOR_VALUE, "");
        return 0;
    }
    return 1;
}

/* ---- the tick counter wraps, and the waits built on it must survive it
 *      (engineering §7) ----
 *
 * The counter is 32 bits of milliseconds, so it returns to zero after 49.7
 * days of uptime. Nothing here had ever run that long, which is why two of
 * the kernel's three wait primitives were computing a deadline as `now + ms`
 * and comparing it directly - correct for 49.7 days and then wrong, in
 * opposite directions:
 *
 *   timer_sleep        `while (ticks < target)` - a wrapped target compares
 *                      small, so the sleep returns at once. Used by every USB
 *                      host controller for its reset and port-settle delays.
 *   timer_timeout_*    `ticks >= deadline` - a wrapped deadline is already
 *                      past, so the wait expires before the hardware is asked.
 *                      Used by ATA and AHCI, which would report
 *                      DISK_ERR_TIMEOUT on a disk that was working.
 *
 * sched_wake_sleepers had the wrap-safe form already, with a comment saying
 * why; the idiom simply never made it back into the driver the scheduler got
 * its time from.
 *
 * Testing this needs the counter moved rather than waited out, which is what
 * timer_ticks_set_for_test is for. The test puts it 16 ticks from the wrap,
 * drives the real primitives across it, and restores it.
 */
static int test_timer_wraparound(void) {
    int ok = 1;

    /* 1. The comparison itself, at the boundary. These are the values a
          deadline actually takes when `now + ms` carries past 2^32. */
    ok = ok && (timer_tick_after (0x00000000u, 0xFFFFFFFFu) == 1);  /* one tick on */
    ok = ok && (timer_tick_after (0xFFFFFFFFu, 0x00000000u) == 0);  /* one tick back */
    ok = ok && (timer_tick_after (0x00000009u, 0xFFFFFFF5u) == 1);  /* 20 ticks on */
    ok = ok && (timer_tick_before(0xFFFFFFF5u, 0x00000009u) == 1);
    ok = ok && (timer_tick_after (0x00000064u, 0x00000064u) == 1);  /* the deadline tick
                                                                      itself has arrived */
    ok = ok && (timer_tick_before(0x00000064u, 0x00000064u) == 0);
    if (!ok) return 0;

    uint32_t saved = timer_ticks();
    timer_ticks_set_for_test(0xFFFFFFF0u);      /* 16 ticks from the wrap */

    /* 2. A timeout opened just before the wrap is not already expired. */
    struct timeout to;
    timer_timeout_start(&to, 50);

    /* Both primitives have an interrupts-off fallback that does not use the
       counter at all. If ktest ever ran with interrupts masked this test
       would pass while exercising none of the arithmetic above, so say so
       rather than returning a quiet pass. */
    if (!to.use_timer) {
        klog("KTEST", SEV_FAIL, "timer wraparound: interrupts off, test is vacuous");
        timer_ticks_set_for_test(saved);
        return 0;
    }
    ok = ok && (timer_timeout_expired(&to) == 0);

    /* 3. A real sleep across the wrap still waits. */
    uint32_t t0 = timer_ticks();
    timer_sleep(32);
    uint32_t waited = timer_ticks() - t0;       /* unsigned: spans the wrap */
    ok = ok && (waited >= 32);
    ok = ok && (waited <= 500);
    /* and the counter went through zero rather than stopping at the top */
    ok = ok && (timer_ticks() < 0x10000000u);

    /* 4. Past the deadline, it does expire - so step 2 was not passing by way
          of a timeout that never expires at all. The budget was 50 ms and
          step 3 spent 32 of them, both on the far side of the wrap. */
    ok = ok && (timer_timeout_expired(&to) == 0);   /* still inside the budget */
    timer_sleep(30);
    ok = ok && (timer_timeout_expired(&to) == 1);

    /* Put the clock back where it was, plus what the test spent, so uptime
       stays monotonic for anything already sleeping on it. */
    timer_ticks_set_for_test(saved + (timer_ticks() - 0xFFFFFFF0u));
    return ok;
}

/* ---- volumes ----
 * The part of the volume layer worth asserting is what happens to an id whose
 * volume tag is wrong. Every public entry point runs it through vol_select,
 * and if that check were missing the tag would be masked off and the call
 * would quietly operate on the ROOT volume instead - reading, or worse
 * writing, whatever entry happens to share that index. So: a tag naming a
 * volume that is not mounted, and a tag past the end of the table, must both
 * be refused.
 *
 * This runs the same on a machine with one disk and a machine with four,
 * because it asserts about tags nothing is mounted under.
 */
static int test_cxfs_volumes(void) {
    if (!cxfs_is_mounted()) return 1;   /* nothing mounted - skip */

    /* The root of the root volume is id 0, and always has been. If this ever
       stops holding, every "0 = root" in the file syscalls is wrong. */
    struct cxfs_entry root;
    if (cxfs_read_entry(0, &root) != 0) return 0;
    if (root.type != CXFS_TYPE_DIR)     return 0;
    if (root.parent_id != 0)            return 0;   /* root's parent is itself */

    /* A tag naming an unmounted slot, and one past the end of the table. Slot
       CXFS_MAX_VOLUMES-1 is left alone by the boot-time scan on any machine
       with fewer disks than slots; if something is mounted there, skip rather
       than assert about a volume that really exists. */
    uint32_t unmounted = (3u << 24) | 0u;   /* last slot, root index */
    uint32_t beyond    = (9u << 24) | 0u;   /* no such slot at all */
    if (cxfs_volume_mounted(3)) unmounted = beyond;

    struct cxfs_entry e;
    if (cxfs_read_entry(unmounted, &e) == 0) return 0;
    if (cxfs_read_entry(beyond, &e)    == 0) return 0;
    if (cxfs_find_in_dir(beyond, "System") >= 0) return 0;
    if (cxfs_count_children(beyond) != 0)        return 0;

    /* cxfs_path_of must say "nothing", not walk off into the root volume. */
    char p[64];
    p[0] = 'x';
    cxfs_path_of(beyond, p, (int)sizeof p);
    if (p[0] != '\0') return 0;

    /* Nothing on one volume may become the child of something on another: a
       parent pointer is a manifest index, and one volume's index 2 has nothing
       to do with another's. */
    if (cxfs_is_ancestor(0, beyond) != 0) return 0;
    if (cxfs_move(beyond, 0)        == 0) return 0;

    /* And the root volume still resolves, which is the proof that none of the
       above left `vol` pointing somewhere it should not. */
    if (cxfs_resolve("/", 0) != 0) return 0;
    return 1;
}


/* ---- memory mappings (SYS_VM_*) ----
 * Runs on a SCRATCH pid rather than a live one. ktest_run() is called after
 * the ring-3 test threads have exited and before the executive starts, so no
 * process is being tracked. The scratch pid is a process of its own made for
 * the test (proc_create), with no thread, reset and released at the end.
 *
 * The frame count is taken before and restored-to after, because the failure
 * this most needs to catch is not a wrong answer but a leak: a map/unmap pair
 * that returns the right numbers while quietly keeping the frames would pass
 * every assertion about addresses and sizes.
 */
static int vm_cycle(int pid) {
    int ok = 0;

    vm_proc_init(pid, vm_default_quota(), -1);

    struct vm_map_args a;
    struct vm_usage u;
    #define RESET_ARGS() do {                                        \
        for (uint32_t _i = 0; _i < sizeof a / 4u; _i++)              \
            ((uint32_t *)&a)[_i] = 0;                                \
        a.object = MOBJ_ANON;                     \
        a.length = 4096; a.prot = MPROT_READ | MPROT_WRITE;          \
    } while (0)

    /* A one-byte request must come back as a whole page: the caller asks in
       bytes and is TOLD the granularity rather than having to know it. */
    RESET_ARGS(); a.length = 1;
    if (vm_map(pid, &a) != E_OK)       goto done;
    if (a.granted != PAGE_SIZE)        goto done;
    if (a.addr < VM_MMAP_BASE)         goto done;
    if (a.addr & (PAGE_SIZE - 1u))     goto done;
    uint64_t first = a.addr;

    /* Readable, writable, and zero - a frame off the free list may hold
       another process's memory, so anything non-zero here is a disclosure. */
    volatile uint8_t *m = (volatile uint8_t *)user_ptr(first);
    for (uint32_t i = 0; i < PAGE_SIZE; i++) if (m[i] != 0) goto done;
    m[0] = 0xA5; m[PAGE_SIZE - 1] = 0x5A;
    if (m[0] != 0xA5 || m[PAGE_SIZE - 1] != 0x5A) goto done;

    /* A second mapping must not land on the first. */
    RESET_ARGS(); a.length = 8192;
    if (vm_map(pid, &a) != E_OK)                       goto done;
    if (a.granted != 8192)                             goto done;
    if (a.addr < first + PAGE_SIZE && a.addr + 8192 > first) goto done;
    uint64_t second = a.addr;

    /* INFO must account for exactly what was handed out. */
    if (vm_info(pid, &u) != E_OK)                   goto done;
    if (u.mapped != PAGE_SIZE + 8192)               goto done;
    if (u.quota != vm_default_quota())                goto done;

    /* Write|Execute is refused. It cannot be ENFORCED on 32-bit non-PAE - there
       is no no-execute bit - so the refusal is the only thing standing between
       today and code written to rely on a combination that will stop being
       allowed. Asserting it here is what keeps it from being quietly relaxed. */
    RESET_ARGS(); a.prot = MPROT_WRITE | MPROT_EXEC;
    if (vm_map(pid, &a) != E_INVALID_ARGS)                 goto done;

    /* Anonymous memory has no interior, so a non-zero offset is a caller error
       rather than something to ignore. Both halves of the 64-bit field. */
    RESET_ARGS(); a.offset = 4096;
    if (vm_map(pid, &a) != E_INVALID_ARGS)                 goto done;
    RESET_ARGS(); a.offset = 1ull << 32;
    if (vm_map(pid, &a) != E_INVALID_ARGS)                 goto done;

    /* An object that does not exist yet is refused rather than treated as
       anonymous, so adding one later cannot silently change what old code did. */
    RESET_ARGS(); a.object = 99;
    if (vm_map(pid, &a) != E_INVALID_ARGS)                 goto done;

    RESET_ARGS(); a.length = 0;
    if (vm_map(pid, &a) != E_INVALID_ARGS)                 goto done;
    RESET_ARGS(); a.length = MMAP_MAX_BYTES + 1u;
    if (vm_map(pid, &a) != E_OUT_OF_RANGE)                 goto done;
    RESET_ARGS(); a.length = (1ull << 32) + 4096u;           /* the high half counts */
    if (vm_map(pid, &a) != E_OUT_OF_RANGE)                 goto done;
    RESET_ARGS(); a.prot = MPROT_NONE;
    if (vm_map(pid, &a) != E_INVALID_ARGS)                 goto done;

    /* The quota is a real ceiling, not a number in a struct: a request that
       would cross it fails, and fails without having taken any frames. */
    uint32_t before_quota_try = pmm_free_count();
    RESET_ARGS(); a.length = vm_default_quota();
    if (vm_map(pid, &a) != E_NO_MEMORY)                 goto done;
    if (pmm_free_count() != before_quota_try)       goto done;

    /* A hint that is free is honoured, which is what makes a moving window
       cheap: unmap and remap at the same address and interior pointers hold. */
    RESET_ARGS(); a.flags = MMAP_HINT; a.addr = VM_MMAP_BASE + 0x01000000u;
    if (vm_map(pid, &a) != E_OK)                            goto done;
    if (a.addr != VM_MMAP_BASE + 0x01000000u)               goto done;
    uint64_t hinted = a.addr;

    /* An occupied hint is declined, not honoured and not failed: the caller
       still gets memory, and `addr` tells it the truth about where. */
    RESET_ARGS(); a.flags = MMAP_HINT; a.addr = hinted;
    if (vm_map(pid, &a) != E_OK)                    goto done;
    if (a.addr == hinted)                           goto done;
    uint64_t declined = a.addr;

    /* Unmapping something never mapped is an error, not a silent success. */
    if (vm_unmap(pid, VM_MMAP_BASE - PAGE_SIZE, PAGE_SIZE) != E_NOT_FOUND) goto done;
    /* A length that disagrees with the region is refused rather than releasing
       a different amount than the caller believes it is releasing. */
    if (vm_unmap(pid, second, 4096) != E_INVALID_ARGS)     goto done;

    if (vm_unmap(pid, first, 0) != E_OK)            goto done;   /* 0 = whole */
    if (vm_unmap(pid, second, 8192) != E_OK)        goto done;
    if (vm_unmap(pid, hinted, 4096) != E_OK)        goto done;
    if (vm_unmap(pid, declined, 4096) != E_OK)      goto done;

    if (vm_info(pid, &u) != E_OK)                   goto done;
    if (u.mapped != 0)                              goto done;

    /* The pages that are NOT mmap regions - the image the loader places, the
       stack and argument pages spawn builds - go through vm_charge, and the
       ceiling has to apply to them exactly as it does to a mapping. Before it
       did, `mapped` reported only what SYS_VM_MAP had asked for, so a process
       could hold an image and a stack that the quota never saw. */
    if (vm_charge(pid, PAGE_SIZE) != E_OK)           goto done;
    if (vm_info(pid, &u) != E_OK)                    goto done;
    if (u.mapped != PAGE_SIZE)                       goto done;

    if (vm_charge(pid, 1) != E_OK)                   goto done;   /* rounds up */
    if (vm_info(pid, &u) != E_OK)                    goto done;
    if (u.mapped != 2u * PAGE_SIZE)                  goto done;

    /* Past the ceiling, and a length whose rounding wraps to something small.
       Both are refused, and a refusal leaves the account untouched. */
    if (vm_charge(pid, vm_quota_of(pid)) != E_NO_MEMORY) goto done;
    if (vm_charge(pid, 0xFFFFFFFFu) != E_OUT_OF_RANGE)      goto done;
    if (vm_info(pid, &u) != E_OK)                    goto done;
    if (u.mapped != 2u * PAGE_SIZE)                  goto done;

    vm_uncharge(pid, 2u * PAGE_SIZE);
    /* An over-refund clamps at zero instead of wrapping, which would hand the
       process a ceiling far wider than the one it was given. */
    vm_uncharge(pid, 16u * PAGE_SIZE);
    if (vm_info(pid, &u) != E_OK)                    goto done;
    if (u.mapped != 0)                               goto done;

    /* Quota attenuation: a child may never be given a wider ceiling than its
       parent holds, which is the same rule grants follow. */
    vm_proc_init(pid, vm_default_quota() / 4u, -1);
    if (vm_quota_of(pid) != vm_default_quota() / 4u)  goto done;

    ok = 1;
done:
    #undef RESET_ARGS
    return ok;
}

/* Run the whole cycle twice and hold the SECOND pass to "every frame came
   back".
 *
 * Not the first, because the first legitimately consumes frames that never
 * return: paging_map allocates a PAGE TABLE the first time a 4 MB region is
 * touched, and paging_unmap frees the page but deliberately not the table -
 * for a process that is the right call, since addr_space_release() frees
 * the whole user half at exit and a table freed and re-allocated per mapping
 * would be pure churn. Asserting no-leak on a cold pass therefore fails
 * against correct behaviour, which is exactly what it did when this test was
 * first written.
 *
 * The warm pass also proves something worth having on its own: after every
 * mapping is released, placement produces the same addresses again, so a full
 * unmap really does return the space rather than merely forgetting it. */
static int test_vmregion(void) {
    struct proc *scratch = proc_create(0);
    if (!scratch) return 0;
    const int pid = scratch->pid;

    int ok = vm_cycle(pid);
    vm_proc_reset(pid);
    if (ok) {
        uint32_t free0 = pmm_free_count();
        ok = vm_cycle(pid);
        vm_proc_reset(pid);
        if (pmm_free_count() != free0) ok = 0;
    }
    proc_put(scratch);
    return ok;
}

/* ---- the block-device contract (engineering §8) ----
 * The contract is written down in disk.h; this holds every registered backend
 * to the part of it that can be checked at boot, on whatever disks this
 * machine actually has. The review's reason for wanting it frozen now is that
 * a VFS and a block cache written against incidental behaviour are much
 * harder to correct later than a driver is.
 *
 * Read-only throughout except on the data disk, and it says when a property
 * could not be exercised rather than counting an untested promise as kept.
 */
static int test_disk_contract(void) {
    unsigned n = disk_count();
    if (n == 0) {
        klog("KTEST", SEV_WARN, "disk contract: no disks registered, untested");
        return 0;              /* nothing to conclude from, so not a pass */
    }

    static uint8_t a[DISK_SECTOR_SIZE * 3];
    static uint8_t b[DISK_SECTOR_SIZE * 3];
    int checked_multi = 0, checked_unaligned = 0;

    for (unsigned i = 0; i < n; i++) {
        const struct disk *d = disk_get(i);
        if (!d) return 0;
        uint8_t id = d->id;

        /* Ids and names are stable: the same disk must come back by id and by
           name, and be the same object. Lifetime, as documented. */
        if (disk_find_by_id(id) != d)        return 0;
        if (disk_find_by_name(d->name) != d) return 0;

        /* Error codes stay distinct. A nonsense request is PARAMS; a sector
           past the end is BOUNDS. Collapsing these to one code would make a
           caching layer unable to tell "ask differently" from "ask less". */
        if (disk_read(id, 0, 1, 0)   != DISK_ERR_PARAMS) return 0;
        if (disk_read(id, 0, 0, a)   != DISK_ERR_PARAMS) return 0;
        if (d->sectors) {
            /* Exactly one past the end, and a count that runs off the end
               from a legal start. These are separate conditions in
               disk_check and the far-past-the-end case is separate again
               (test_disk_lba_range), because the subtraction underflows
               there - so all three are asserted rather than assuming one
               implies the others. */
            if (disk_read(id, d->sectors,     1, a) != DISK_ERR_BOUNDS) return 0;
            if (disk_read(id, d->sectors - 1, 2, a) != DISK_ERR_BOUNDS) return 0;
            if (disk_read(id, d->sectors + 4096, 1, a) != DISK_ERR_BOUNDS) return 0;
        }

        /* An unknown id is NO_DEVICE, not any of the above. */
        if (disk_read(0xFE, 0, 1, a) != DISK_ERR_NO_DEVICE) return 0;

        /* Sector 0 reads, synchronously: the buffer is filled by the time the
           call returns, with no completion to wait for. Non-removable media
           only - an empty optical drive legitimately has nothing to read. */
        if (d->media == DISK_MEDIA_CDROM || d->media == DISK_MEDIA_FDD) continue;
        if (d->sectors < 3) continue;

        for (unsigned k = 0; k < sizeof a; k++) { a[k] = 0x11; b[k] = 0x22; }
        if (disk_read(id, 0, 1, a) != DISK_OK) return 0;

        /* Sector size is 512: reading one sector must have written exactly
           that much. The byte after it is still the fill pattern. */
        if (a[DISK_SECTOR_SIZE] != 0x11) return 0;

        /* A multi-sector read is split internally and must agree with the
           single-sector reads it is made of - the splitting is where an
           8-bit count or a wrapped LBA used to go wrong. */
        if (disk_read(id, 0, 3, b) != DISK_OK) return 0;
        for (unsigned k = 0; k < DISK_SECTOR_SIZE; k++)
            if (a[k] != b[k]) return 0;
        checked_multi = 1;

        /* No alignment requirement: the same sector through a deliberately
           odd address must read identically. This is the promise AHCI's
           bounce buffer exists to keep, so it is worth asserting rather than
           trusting. */
        if (disk_read(id, 1, 1, b + 1) != DISK_OK) return 0;
        if (disk_read(id, 1, 1, a)     != DISK_OK) return 0;
        for (unsigned k = 0; k < DISK_SECTOR_SIZE; k++)
            if (a[k] != b[1 + k]) return 0;
        checked_unaligned = 1;
    }

    /* Refuse to report a pass for promises nothing exercised. */
    if (!checked_multi || !checked_unaligned) {
        klog("KTEST", SEV_WARN, "disk contract: no readable disk, partly untested");
        return 0;
    }
    return 1;
}

/* ---- the bounded-string contract (engineering §2) ----
 * The rules are stated once, in string.h; this is what holds the kernel to
 * them. Two of the four cases below were live defects rather than
 * hypotheticals, which is why the review asked for the convention to be
 * written down instead of left to each subsystem.
 */
static int test_bounded_strings(void) {
    int ok = 1;
    char buf[32];

    /* 1. cap == 0 writes nothing. disk_capacity_str used to put a terminator
          at buf[0] regardless, which is one byte past a zero-length buffer.
          A canary is the only way to see that from inside the kernel. */
    buf[0] = 0x7E;
    disk_capacity_str(2097152ull, buf, 0);
    ok = ok && (buf[0] == 0x7E);          /* untouched */
    buf[0] = 0x7E;
    disk_capacity_str(2097152ull, buf, -1);
    ok = ok && (buf[0] == 0x7E);          /* a negative cap is not a huge one */

    /* 2. cap > 0 always terminates, and a result that does not fit is empty
          rather than a fragment that reads as complete. "1 GB" needs five
          bytes; at four it used to emit "GB" and a 500 GB disk displayed as
          its unit alone. */
    for (int c = 1; c <= 4; c++) {
        for (unsigned i = 0; i < sizeof buf; i++) buf[i] = 0x7E;
        disk_capacity_str(2097152ull, buf, c);
        ok = ok && (buf[0] == '\0');                 /* terminated, and empty */
        ok = ok && (buf[c] == 0x7E);                 /* nothing past the cap */
    }

    /* 3. given room, the whole thing appears. */
    for (unsigned i = 0; i < sizeof buf; i++) buf[i] = 0x7E;
    disk_capacity_str(2097152ull, buf, (int)sizeof buf);
    ok = ok && (buf[0] == '1' && buf[1] == ' ' && buf[2] == 'G' &&
                buf[3] == 'B' && buf[4] == '\0');

    /* 4. truncation is refused, not performed. A name one byte too long for
          the on-disk field must be rejected by both entry points. Before
          this, both truncated the name into a 64-byte buffer and only then
          asked whether it was too long - so the check could never fire, a
          100-character name became a 63-character file, and two distinct
          names became one. */
    char longname[CXFS_NAME_LEN + 8];
    for (unsigned i = 0; i < sizeof longname - 1; i++) longname[i] = 'x';
    longname[sizeof longname - 1] = '\0';

    /* /Temp, the same writable directory the other cxfs tests use - the root
       is read-only on a release mount. */
    int dir = cxfs_resolve("/Temp", 0);
    if (dir < 0) return 0;
    uint32_t root = (uint32_t)dir;
    ok = ok && (cxfs_create_entry(root, longname, CXFS_TYPE_FILE) < 0);

    /* exactly at the limit is still refused: the field needs room for the
       terminator, so CXFS_NAME_LEN characters is one too many. */
    longname[CXFS_NAME_LEN] = '\0';
    ok = ok && (cxfs_create_entry(root, longname, CXFS_TYPE_FILE) < 0);

    /* and one byte under the limit is accepted, so this is not passing by
       refusing everything. */
    longname[CXFS_NAME_LEN - 1] = '\0';
    int id = cxfs_create_entry(root, longname, CXFS_TYPE_FILE);
    if (id < 0) return 0;                  /* read-only mount: cannot conclude */

    /* rename refuses the same way */
    char toolong[CXFS_NAME_LEN + 4];
    for (unsigned i = 0; i < sizeof toolong - 1; i++) toolong[i] = 'y';
    toolong[sizeof toolong - 1] = '\0';
    ok = ok && (cxfs_rename((uint32_t)id, toolong) != 0);

    cxfs_delete_entry((uint32_t)id);
    return ok;
}

/* ---- lifecycles, not helpers (engineering §16) ----
 * The review's point is that many kernel bugs live between individually
 * correct operations, and names three sequences this kernel could already run
 * end to end without asserting the end: what happens *after* the last step.
 *
 * Two of its five examples were already covered - endpoint create/call/reply/
 * close/reclaim by test_ipc_endpoint_lifetime, and block device register then
 * read by test_storage and test_ahci (there is no unregister to test; see the
 * contract note in disk.h). The three below were not:
 *
 *   map -> access -> unmap -> access FAILS    the last step was missing: the
 *                                             mappings were released and the
 *                                             accounting checked, but nothing
 *                                             ever confirmed the memory had
 *                                             actually stopped being reachable.
 *   process create -> run -> exit -> frames back
 *                                             test_ring3_processes ran this and
 *                                             only checked the process left.
 *                                             Phase 1 found proc_trampoline
 *                                             leaking a whole address space on
 *                                             its failure paths, which is
 *                                             exactly what this would catch.
 *   sleep -> wake -> reschedule               a sleep that blocks the machine
 *                                             instead of yielding it passes any
 *                                             test that only measures elapsed
 *                                             time, including this file's.
 */
static volatile uint32_t lifecycle_ticker = 0;
static volatile int      lifecycle_run    = 0;

static void lifecycle_counter_thread(void) {
    /* Runs until the test says stop, with a guard so a scheduler that never
       returns here cannot hang the boot either way. */
    uint32_t guard = 0;
    while (lifecycle_run && guard++ < 4000000u) {
        lifecycle_ticker++;
        yield();
    }
}

static int test_lifecycle_sequences(void) {
    struct proc *scratch = proc_create(0);   /* see test_vmregion */
    if (!scratch) return 0;
    const int pid = scratch->pid;
    int ok = 1;

    /* ---- 1. map -> access -> unmap -> access fails ---- */
    vm_proc_init(pid, vm_default_quota(), -1);

    struct vm_map_args a;
    for (uint32_t i = 0; i < sizeof a / 4u; i++) ((uint32_t *)&a)[i] = 0;
    a.object = MOBJ_ANON;
    a.length = PAGE_SIZE; a.prot = MPROT_READ | MPROT_WRITE;

    if (vm_map(pid, &a) != E_OK) { ok = 0; goto done_vm; }
    uint64_t va = a.addr;

    /* reachable while mapped, by both the check and the hardware */
    if (!user_ptr_writable(va, PAGE_SIZE)) { ok = 0; goto done_vm; }
    *(volatile uint8_t *)user_ptr(va) = 0xC7;
    if (*(volatile uint8_t *)user_ptr(va) != 0xC7)   { ok = 0; goto done_vm; }

    /* ---- 1b. the protection that was asked for is the protection applied ----
       Nothing asserted this before, which made a whole class of failure
       invisible: vm_map maps every page writable first so it can zero it
       through that mapping, then re-protects it to what the caller asked for.
       The re-protect's status was discarded (engineering §9), so a failure
       there would hand back a *writable* page to a caller that asked for
       read-only - and on x86-32 without PAE, where every readable page is
       executable, that is the W^X combination the loader refuses by name.
       Checked at both layers, as above. */
    for (uint32_t i = 0; i < sizeof a / 4u; i++) ((uint32_t *)&a)[i] = 0;
    a.object = MOBJ_ANON;
    a.length = PAGE_SIZE; a.prot = MPROT_READ;
    if (vm_map(pid, &a) != E_OK) { ok = 0; goto done_vm; }
    uint64_t ro = a.addr;

    if (!user_ptr_readable(ro, PAGE_SIZE)) { ok = 0; goto done_vm; }
    if (user_ptr_writable(ro, 1))          { ok = 0; goto done_vm; }

    /* and the hardware agrees, because CR0.WP is set */
    uint32_t ro_faults = usercopy_faults_recovered();
    uint8_t  one = 0x5C;
    if (user_copy_bytes(user_ptr(ro), &one, 1) == 0)                { ok = 0; goto done_vm; }
    if (usercopy_faults_recovered() != ro_faults + 1)             { ok = 0; goto done_vm; }
    if (vm_unmap(pid, ro, PAGE_SIZE) != E_OK)                     { ok = 0; goto done_vm; }

    if (vm_unmap(pid, va, PAGE_SIZE) != E_OK) { ok = 0; goto done_vm; }

    /* Now the step that was missing. Two layers, because they can disagree:
       the page-table check must refuse, and the CPU must refuse. The second
       is only askable at all because a fault inside user_copy_bytes is
       recoverable (security §6) - without that this would be a panic, which
       is why it had never been asserted. */
    if (user_ptr_readable(va, 1)) { ok = 0; goto done_vm; }
    if (user_ptr_writable(va, 1)) { ok = 0; goto done_vm; }

    uint32_t faults_before = usercopy_faults_recovered();
    uint8_t  sink = 0;
    if (user_copy_bytes(&sink, user_ptr(va), 1) == 0) { ok = 0; goto done_vm; }
    if (usercopy_faults_recovered() != faults_before + 1) { ok = 0; goto done_vm; }

done_vm:
    vm_proc_reset(pid);
    proc_put(scratch);
    if (!ok) return 0;

    /* ---- 2. process create -> run -> exit -> frames returned ----
       An address space is page directory + page tables + image + stack + argv,
       so a leak here is tens of pages per spawn, not one. */
    for (int i = 0; i < 8 && sched_active_count() > 1; i++) yield();
    uint32_t free_before = pmm_free_count();

    uint32_t blen = (uint32_t)(user_blob_end - user_blob_start);
    if (process_create_ring3("lifecycle", user_blob_start, blen, "") < 0) return 0;
    for (int i = 0; i < 64 && sched_active_count() > 1; i++) yield();
    if (sched_active_count() != 1) { return 0; }
    for (int i = 0; i < 8; i++) yield();              /* let sched_reap run */

    if (pmm_free_count() != free_before) return 0;

    /* ---- 3. sleep -> wake -> reschedule ----
       A sleeping thread must hand the CPU over, not hold it. */
    lifecycle_ticker = 0;
    lifecycle_run    = 1;
    if (thread_create("lc_count", lifecycle_counter_thread) < 0) { lifecycle_run = 0; return 0; }

    uint32_t seen_at_start = lifecycle_ticker;
    thread_sleep_ms(30);
    uint32_t advanced = lifecycle_ticker - seen_at_start;

    /* Stop it and let it leave, so it cannot outlive the test and perturb
       whatever runs next. */
    lifecycle_run = 0;
    for (int i = 0; i < 2000 && sched_active_count() > 1; i++) yield();

    /* It must have run *during* the sleep. A sleep that spun or halted without
       yielding leaves this at zero - and passes every elapsed-time assertion
       in this file, which is the reason to measure the other thread instead of
       the clock. */
    if (advanced == 0)             return 0;
    if (sched_active_count() != 1) return 0;
    return ok;
}

/* ---- admission: what a verification result lets run ----
 * Held to the rule of whichever build this is. Both kinds refuse a tampered
 * image, a forged or wrong-key signature and a file that is not a CXEX, and
 * pass a real trust level through untouched. They differ on exactly one case:
 * an image with no signature at all is refused by a release kernel and
 * admitted, at platform trust, by a DEV_UNSIGNED one.
 *
 * The refusals are the half that matters most. A dev flag that let a TAMPERED
 * image through would turn "skip signing while testing" into "ignore
 * corruption", and this is what makes sure it never can. */
/* ---- the key parser accepts exactly one cryptographic profile ----
 *
 * Security review §12.1 and §12.7. CXK is RSA-2048 / SHA-256 / PKCS#1 v1.5, and
 * rsa_parse_xkpk used to read `version` and `key_bits` and discard them both -
 * the comment said "(not strictly needed)" - while never looking at the exponent
 * or the reserved field at all. It would therefore hand back a key with
 * exponent 1, for which RSA verification is the identity function and every
 * signature is forgeable.
 *
 * Nothing reachable could exploit that, because trust is decided by comparing
 * the whole key byte for byte against the compiled-in root. But that is a
 * property of the CALLER, and a parser that returns a forgeable key is one
 * careless caller away from mattering. These cases exist so the checks cannot be
 * quietly removed again: each is a single mutation of a header the parser
 * accepts, and the accepted header is itself a case, so the suite cannot pass by
 * refusing everything. */
static void xkpk_hdr(uint8_t *b, uint16_t version, uint16_t key_bits,
                     uint32_t exponent, uint16_t mod_len, uint16_t reserved) {
    for (uint32_t i = 0; i < 16u + 256u; i++) b[i] = 0;
    b[0]='C'; b[1]='X'; b[2]='P'; b[3]='K';
    b[4]=(uint8_t)version;   b[5]=(uint8_t)(version >> 8);
    b[6]=(uint8_t)key_bits;  b[7]=(uint8_t)(key_bits >> 8);
    b[8]=(uint8_t)exponent;  b[9]=(uint8_t)(exponent >> 8);
    b[10]=(uint8_t)(exponent >> 16); b[11]=(uint8_t)(exponent >> 24);
    b[12]=(uint8_t)mod_len;  b[13]=(uint8_t)(mod_len >> 8);
    b[14]=(uint8_t)reserved; b[15]=(uint8_t)(reserved >> 8);
    b[16] = 0xC0;            /* a non-zero leading modulus byte */
}

/* ---- XFNT: a loadable font is untrusted input --------------------------
 *
 * The header is believed by nothing: every field is checked before it is used
 * to address a glyph, and the shape of the checks is the CXFS superblock
 * finding applied before rather than after. The length test is the important
 * one - the header must describe the file EXACTLY - because that is what stops
 * a glyph read running off the end of the buffer.
 *
 * Every case is one mutation of a known-good font, and the known-good font is a
 * case of its own, so the suite cannot pass by refusing everything.
 */
#define XF_W   8
#define XF_H   16
#define XF_N   4
#define XF_LEN (XFNT_HEADER_SIZE + XF_N * XF_H)

static void xf_good(uint8_t *f) {
    for (uint32_t i = 0; i < XF_LEN; i++) f[i] = 0;
    f[0] = 'X'; f[1] = 'F'; f[2] = 'N'; f[3] = 'T';
    f[4] = XFNT_VERSION; f[5] = 0;
    f[6] = XFNT_KIND_BITMAP;
    f[7] = 1;                      /* bytes per row */
    f[8] = XF_W;
    f[9] = XF_H;
    f[10] = XF_N; f[11] = 0;       /* glyph count */
    f[12] = (uint8_t)XFNT_FLAG_MSB_FIRST; f[13] = 0;
    /* slot 1 is a solid left column, so a wrong stride is visible */
    for (int r = 0; r < XF_H; r++) f[XFNT_HEADER_SIZE + 1 * XF_H + r] = 0x80;
    /* slot 3's first row is distinctive */
    f[XFNT_HEADER_SIZE + 3 * XF_H] = 0x5A;
}

static int test_xfnt(void) {
    uint8_t f[XF_LEN];
    int ok = 1;

    /* ---- the control ---- */
    xf_good(f);
    if (xfnt_install(f, XF_LEN) != XFNT_E_OK) { xfnt_clear(); return 0; }
    ok = ok && xfnt_active();
    ok = ok && (xfnt_width() == XF_W) && (xfnt_height() == XF_H);
    ok = ok && (xfnt_glyph_count() == XF_N);
    ok = ok && (xfnt_glyph_bytes() == XF_H);

    /* the glyph that came back is the glyph that went in, at the right stride */
    const uint8_t *g = xfnt_glyph(1);
    ok = ok && g && g[0] == 0x80 && g[XF_H - 1] == 0x80;
    g = xfnt_glyph(3);
    ok = ok && g && g[0] == 0x5A;
    ok = ok && (xfnt_glyph(XF_N) == 0);      /* one past the end is NULL */

    /* An 8x16 font becomes the console font, and ASCII keeps its own slot - so
       a byte the console printed before prints this font's glyph for it now. */
    ok = ok && (font_glyph(1)[0] == 0x80);

    /* ---- refusals: one mutation each ---- */
    xf_good(f); f[0] = 'Y';                  ok = ok && (xfnt_install(f, XF_LEN) == XFNT_E_MAGIC);
    xf_good(f); f[4] = XFNT_VERSION + 1;     ok = ok && (xfnt_install(f, XF_LEN) == XFNT_E_VERSION);
    xf_good(f); f[6] = XFNT_KIND_VECTOR;     ok = ok && (xfnt_install(f, XF_LEN) == XFNT_E_KIND);
    xf_good(f); f[6] = 99;                   ok = ok && (xfnt_install(f, XF_LEN) == XFNT_E_KIND);

    /* bytes_per_row is derived from width, so a file disagreeing is caught
       rather than believed - believing it walks the glyph data at the wrong
       stride, which reads real memory and looks like a corrupt font. */
    xf_good(f); f[7] = 4;                    ok = ok && (xfnt_install(f, XF_LEN) == XFNT_E_GEOMETRY);
    xf_good(f); f[7] = 0;                    ok = ok && (xfnt_install(f, XF_LEN) == XFNT_E_GEOMETRY);

    xf_good(f); f[8] = 0;                    ok = ok && (xfnt_install(f, XF_LEN) == XFNT_E_GEOMETRY);
    xf_good(f); f[8] = XFNT_MAX_WIDTH + 1;   ok = ok && (xfnt_install(f, XF_LEN) == XFNT_E_GEOMETRY);
    xf_good(f); f[9] = 0;                    ok = ok && (xfnt_install(f, XF_LEN) == XFNT_E_GEOMETRY);
    xf_good(f); f[9] = XFNT_MAX_HEIGHT + 1;  ok = ok && (xfnt_install(f, XF_LEN) == XFNT_E_GEOMETRY);
    xf_good(f); f[10] = 0; f[11] = 0;        ok = ok && (xfnt_install(f, XF_LEN) == XFNT_E_GEOMETRY);

    /* MSB-first is the only order the blit reads; an unknown flag may mean
       something this version does not implement. */
    xf_good(f); f[12] = 0;                   ok = ok && (xfnt_install(f, XF_LEN) == XFNT_E_FLAGS);
    xf_good(f); f[13] = 0x80;                ok = ok && (xfnt_install(f, XF_LEN) == XFNT_E_FLAGS);

    /* ---- the length, from both sides ---- */
    xf_good(f);
    ok = ok && (xfnt_install(f, XF_LEN - 1) == XFNT_E_LENGTH);
    ok = ok && (xfnt_install(f, XF_LEN + 1) == XFNT_E_LENGTH);
    ok = ok && (xfnt_install(f, XFNT_HEADER_SIZE - 1) == XFNT_E_SHORT);
    ok = ok && (xfnt_install(f, 0) == XFNT_E_SHORT);
    /* a count the file cannot hold: the header describes more than arrived */
    xf_good(f); f[10] = 200;
    ok = ok && (xfnt_install(f, XF_LEN) == XFNT_E_LENGTH);
    /* and a NULL file is a refusal, not a fault */
    ok = ok && (xfnt_install(0, XF_LEN) == XFNT_E_SHORT);

    /* ---- a refused font leaves the installed one alone ----
       This is the property that makes a bad font cost nothing: the commit
       happens only after every check has passed. */
    xf_good(f);
    ok = ok && (xfnt_install(f, XF_LEN) == XFNT_E_OK);
    uint8_t bad[XF_LEN];
    xf_good(bad); bad[0] = 'Z';
    ok = ok && (xfnt_install(bad, XF_LEN) != XFNT_E_OK);
    ok = ok && xfnt_active() && (xfnt_glyph(1) != 0) && (xfnt_glyph(1)[0] == 0x80);

    /* Teardown is UNCONDITIONAL. A 4-glyph font is installed as the console
       font right now, so returning without clearing would leave the rest of
       the boot drawing from it - and an `ok && xfnt_clear()` would skip exactly
       when the test had already failed. */
    xfnt_clear();
    ok = ok && !xfnt_active();
    ok = ok && (xfnt_glyph(1) == 0);
    ok = ok && (xfnt_width() == 0);
    return ok;
}

/* ---- variable font cell size -------------------------------------------
 *
 * The console's grid is derived from the active font's cell, so a font of
 * another size changes how many columns and rows fit the same viewport. What
 * this pins is that there is exactly ONE place that decides the cell: before
 * this, fb.h had FB_CHAR_W/FB_CHAR_H macros and the console, the blit, the
 * scroll step and the panic renderer each multiplied by them independently, so
 * a loaded font could only ever be 8x16.
 *
 * The teardown is unconditional, and it matters more here than usual: the
 * console being written to is the one this test is reconfiguring, so leaving a
 * 6x8 font installed would lay out the rest of the boot log on the wrong grid.
 */
static void xf_build(uint8_t *f, uint32_t w, uint32_t h, uint32_t n) {
    uint32_t len = XFNT_HEADER_SIZE + n * h;
    for (uint32_t i = 0; i < len; i++) f[i] = 0;
    f[0] = 'X'; f[1] = 'F'; f[2] = 'N'; f[3] = 'T';
    f[4] = XFNT_VERSION;
    f[6] = XFNT_KIND_BITMAP;
    f[7] = 1;
    f[8] = (uint8_t)w;
    f[9] = (uint8_t)h;
    f[10] = (uint8_t)n;
    f[12] = (uint8_t)XFNT_FLAG_MSB_FIRST;
}

static int test_font_cell(void) {
    int ok = 1;

    /* the compiled-in default, and the one place everything now asks */
    ok = ok && (font_cell_width() == 8) && (font_cell_height() == 16);
    ok = ok && (fb_font_width() == 8) && (fb_font_height() == 16);

    int cols0 = console_cols(), rows0 = console_rows();
    ok = ok && (cols0 > 0) && (rows0 > 0);
    if (!ok) return 0;

    /* ---- half the height: the same viewport holds twice the rows ----
       Integer division means the doubled count can be one more than 2x when the
       viewport is not a multiple of 16, so the assertion allows exactly that
       and nothing looser. */
    uint8_t f8[XFNT_HEADER_SIZE + 8 * 8];
    xf_build(f8, 8, 8, 8);
    if (xfnt_install(f8, sizeof f8) != XFNT_E_OK) { xfnt_clear(); console_font_changed(); return 0; }
    console_font_changed();

    ok = ok && (font_cell_height() == 8) && (fb_font_height() == 8);
    ok = ok && (font_cell_width() == 8);
    int rows8 = console_rows();
    ok = ok && (rows8 == rows0 * 2 || rows8 == rows0 * 2 + 1);
    ok = ok && (console_cols() == cols0);          /* width did not change */

    /* ---- a narrower cell gives more columns ----
       Width is variable too, not just height: a 6-wide glyph packs into the
       high bits of its row byte, which is the order the converter writes, so
       the blit needs no special case. */
    uint8_t f6[XFNT_HEADER_SIZE + 8 * 8];
    xf_build(f6, 6, 8, 8);
    if (xfnt_install(f6, sizeof f6) == XFNT_E_OK) {
        console_font_changed();
        ok = ok && (font_cell_width() == 6) && (fb_font_width() == 6);
        ok = ok && (console_cols() > cols0);
    } else {
        ok = 0;
    }

    /* ---- a glyph past what the font holds is blank, never the default ----
       Falling back to the compiled-in 8x16 default here would hand back 16 rows
       while the cell said 8, and fb_draw_char would read past the glyph. */
    const uint8_t *g = font_glyph(200);            /* the font has 8 slots */
    ok = ok && (g != 0) && (g == font_glyph(0));

    /* teardown: unconditional, then assert it took */
    xfnt_clear();
    console_font_changed();
    ok = ok && (font_cell_width() == 8) && (font_cell_height() == 16);
    ok = ok && (console_cols() == cols0) && (console_rows() == rows0);
    return ok;
}

/* ---- the console font on disk ------------------------------------------
 *
 * Covers the whole chain in one go: cxk font build converted Terminus's
 * BDF source for code page 437 at build time, the image staged the result at
 * /System/Fonts, and the kernel reads it back and validates it. A break
 * anywhere along that - a converter that writes the wrong slot, a stage line
 * that drops the file, a reader that disagrees with the writer - lands here.
 *
 * The SYSCALL's success path is deliberately not exercised. SYS_FB_SET_FONT
 * clears the console on a successful change, because what is on screen is the
 * old cell, and wiping the boot log out from under the rest of the tests is a
 * poor trade for a case the shell's `font` command exercises directly. What IS
 * tested through the syscall is every refusal, because that is where the name
 * handling lives and a name is the only thing a caller controls.
 */
static uint8_t font_disk_buf[XFNT_HEADER_SIZE + XFNT_MAX_GLYPHS * XFNT_MAX_HEIGHT
                             + XFNT_MAX_GLYPHS * 4];

static int test_console_font_disk(void) {
    if (!cxfs_is_mounted()) return 1;                 /* nothing mounted - skip */

    int ok = 1;
    int id = cxfs_resolve("/System/Fonts/console_16.xfnt", 0);
    if (id < 0) {
        klog("KTEST", SEV_WARN, "console font untested: /System/Fonts/console_16.xfnt is not staged");
        return 1;
    }

    struct cxfs_entry fe;
    if (cxfs_read_entry((uint32_t)id, &fe) != 0) return 0;
    if (fe.size == 0 || fe.size > sizeof font_disk_buf) return 0;

    int got = cxfs_read_file((uint32_t)id, font_disk_buf, (uint32_t)sizeof font_disk_buf);
    ok = ok && (got > 0) && ((uint64_t)got == fe.size);
    if (!ok) return 0;

    /* ---- it is the font the converter wrote ---- */
    if (xfnt_install(font_disk_buf, (uint32_t)got) != XFNT_E_OK) return 0;
    ok = ok && (xfnt_width() == 8) && (xfnt_height() == 16);
    ok = ok && (xfnt_glyph_count() == 256);

    /* 'A' at its own slot, with ink, and not a solid block - the shape of the
       converter bug that produced 206 filled cells and nothing else noticed. */
    const uint8_t *a = xfnt_glyph('A');
    ok = ok && (a != 0);
    if (a) {
        int any = 0, solid = 1;
        for (uint32_t r = 0; r < 16; r++) {
            if (a[r]) any = 1;
            if (a[r] != 0xFF) solid = 0;
        }
        ok = ok && any && !solid;
    }

    /* ASCII keeps its own slot, and the upper half is IBM code page 437 in its
       own order: byte 0x80 is C-cedilla, 0xC4 the horizontal box line. Pins the
       slot assignment across the whole build, not just the converter's own
       tests. */
    ok = ok && (xfnt_codepoint('A') == 'A');
    ok = ok && (xfnt_codepoint(0x80) == 0x00C7);
    ok = ok && (xfnt_codepoint(0xC4) == 0x2500);

    xfnt_clear();
    console_font_changed();

    /* ---- the syscall refuses every name that could leave /System/Fonts ---- */
    if (fb_active()) {
        const uint32_t va = 0x00A00000u;
        paddr_t frame = pmm_alloc();
        if (!frame) return 0;
        if (paging_map_user(va, frame, VM_WRITE) != 0) {
            pmm_free(frame);
            return 0;
        }

        char *nm = (char *)user_ptr(va + 256);

        /* Each of these is a way out of the directory, or a way to confuse the
           path the kernel builds. The '.' rule is what makes ".." need no case
           of its own. */
        const char *bad[] = {
            "..", "../x", "a/b", "/abs", "console_16.xfnt", "a.b", "", "a\\b",
        };
        for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            uint32_t k = 0;
            while (bad[i][k] && k < 200) { nm[k] = bad[i][k]; k++; }
            nm[k] = '\0';
            int rc = sys_fb_set_font(user_addr(nm));
            /* E_INVALID_ARGS specifically, NOT "some error". The distinction is the
               security property: E_INVALID_ARGS means the NAME RULES refused it,
               E_NOT_FOUND means the kernel built a path and went looking. Accepting
               either would pass a kernel that allowed "../x" straight through -
               it would compose /System/Fonts/../x.xfnt, find nothing, and
               answer E_NOT_FOUND, which reads like a refusal and is not one. */
            ok = ok && (rc == E_INVALID_ARGS);
            /* and nothing was installed by a refused call */
            ok = ok && !xfnt_active();
        }

        /* A well-formed name that simply is not there is NOT_FOUND, not
           INVALID - the two are different answers and a caller acts on them
           differently. */
        const char *absent = "no_such_font_here";
        uint32_t k = 0;
        while (absent[k]) { nm[k] = absent[k]; k++; }
        nm[k] = '\0';
        ok = ok && (sys_fb_set_font(user_addr(nm)) == E_NOT_FOUND);
        ok = ok && !xfnt_active();

        /* ---- where the name length bound actually is ----
         *
         * The filename is <name>.xfnt, so the extension counts against the 64
         * byte CXFS name field. 58 characters is the longest name that can
         * become one (58 + 5 = 63, with room for the terminator); 59 cannot.
         *
         * Both halves are asserted, because a bound set one too tight is as
         * wrong as one too loose and only the pair says where it is. And the
         * CODES are what distinguish them: a name that could never be a
         * filename is malformed - E_INVALID_ARGS - whereas measuring the bare name
         * against CXFS_NAME_LEN let 59 through to a lookup that always missed,
         * answering E_NOT_FOUND, "no such font", about something that could never
         * have been one. */
        const char *n58 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        const char *n59 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

        k = 0; while (n58[k]) { nm[k] = n58[k]; k++; } nm[k] = '\0';
        ok = ok && (sys_fb_set_font(user_addr(nm)) == E_NOT_FOUND);

        k = 0; while (n59[k]) { nm[k] = n59[k]; k++; } nm[k] = '\0';
        ok = ok && (sys_fb_set_font(user_addr(nm)) == E_INVALID_ARGS);
        ok = ok && !xfnt_active();

        /* ---- and a change that keeps the CELL does not clear the screen ----
         *
         * The one success path worth taking here, and it was not taken before.
         * Installing console_16 first makes the syscall's change a real one with an
         * unchanged cell, which is exactly the shape of the shell restoring a
         * saved font at startup - and an unconditional clear there wiped the
         * boot log at every boot once a font had been saved, with no test
         * anywhere near it because the success path was skipped as a whole.
         *
         * "Did not clear" is observable as "the cursor did not move": homing
         * the cursor is all console_clear leaves behind that is visible from
         * in here. The row is required to be non-zero first, or a console that
         * happened to be homed already would make the check vacuous. */
        if (fb_font_select("console_16") == 0) {
            const char *same = "console_16";
            uint32_t m = 0;
            while (same[m]) { nm[m] = same[m]; m++; }
            nm[m] = '\0';

            int c0 = 0, r0 = 0;
            console_cursor(&c0, &r0);
            ok = ok && (r0 > 0);

            ok = ok && (sys_fb_set_font(user_addr(nm)) == 0);
            ok = ok && xfnt_active() && (xfnt_height() == 16);

            int c1 = 0, r1 = 0;
            console_cursor(&c1, &r1);
            ok = ok && (c1 == c0) && (r1 == r0);
        }

        paging_unmap(va);
        pmm_free(frame);
    }

    /* ---- /System wins over /Shared, which is the whole point of the order ----
     *
     * /Shared/Fonts is group-writable, so if it were searched first anyone able
     * to write there could shadow a system font and decide what every kernel
     * message looks like. The hazard is the one CX_FILESYSTEM_LAYOUT.md names
     * for /System/Programs, so it gets the same answer and the same kind of
     * test: put a DIFFERENT font at the same name in /Shared, ask for the name,
     * and require the system one.
     *
     * fb_font_select rather than the syscall: these fonts differ in cell size,
     * which is precisely when the syscall clears, and this runs in the middle
     * of the boot log.
     */
    if (cxfs_resolve("/Shared/Fonts", 0) >= 0 && fb_active()) {
        /* an 8x8 decoy, distinguishable from the real console_16 at 8x16 */
        uint8_t decoy[XFNT_HEADER_SIZE + 4 * 8];
        xf_build(decoy, 8, 8, 4);

        int dir = cxfs_resolve("/Shared/Fonts", 0);
        int fid = cxfs_find_in_dir((uint32_t)dir, "console_16.xfnt");
        if (fid < 0) fid = cxfs_create_entry((uint32_t)dir, "console_16.xfnt", CXFS_TYPE_FILE);
        if (fid < 0) {
            ok = 0;
        } else {
            ok = ok && (cxfs_write_file((uint32_t)fid, decoy, (uint32_t)sizeof decoy) == 0);

            /* The name exists in BOTH. The system one is 8x16; the decoy is
               8x8. Getting 16 proves the order held. */
            ok = ok && (fb_font_select("console_16") == 0);
            ok = ok && (xfnt_height() == 16);
            ok = ok && (xfnt_glyph_count() == 256);   /* the real font, not the 4-slot decoy */

            /* And a name only /Shared has IS found - the second directory is
               searched, not merely declared. */
            int uid2 = cxfs_find_in_dir((uint32_t)dir, "ktshared.xfnt");
            if (uid2 < 0) uid2 = cxfs_create_entry((uint32_t)dir, "ktshared.xfnt", CXFS_TYPE_FILE);
            if (uid2 >= 0) {
                ok = ok && (cxfs_write_file((uint32_t)uid2, decoy, (uint32_t)sizeof decoy) == 0);
                ok = ok && (fb_font_select("ktshared") == 0);
                ok = ok && (xfnt_height() == 8);      /* the decoy, from /Shared */
                ok = ok && (xfnt_glyph_count() == 4);
                cxfs_delete_entry((uint32_t)uid2);
            }

            cxfs_delete_entry((uint32_t)fid);         /* unconditional: leave no decoy */
        }
    }

    /* teardown is unconditional: the default font must be back whatever
       happened above, or the rest of the boot draws from a cleared one */
    xfnt_clear();
    console_font_changed();
    ok = ok && (font_cell_height() == 16);
    return ok;
}

/* ---- durability: a write can be asked to be committed ------------------
 *
 * disk.h's contract used to say outright that it made no durability guarantee
 * across power loss, and it was right: ata_write issued FLUSH CACHE after every
 * transfer while AHCI and USB issued nothing at all, so whether a write
 * survived a power cut depended on which cable the disk was on.
 *
 * What this pins is that asking is possible and that the answer is honest. A
 * flush cannot be observed from software - the platter is not visible from
 * here - so what is asserted is the contract around it: a real disk accepts
 * the request, a disk that does not exist is refused rather than quietly
 * succeeding, and data written before a sync reads back after it.
 *
 * The thing that must never happen is a silent success from a backend that did
 * nothing, because a caller then believes data is safe when it is not. That is
 * untestable from here by construction, so disk_flush is written to return the
 * driver's answer rather than DISK_OK, and the default arm of its switch
 * refuses instead of falling through to success.
 */
static int test_disk_durability(void) {
    if (!cxfs_is_mounted()) return 1;              /* nothing mounted - skip */

    int ok = 1;

    /* A disk id that does not exist is refused. Were this to answer DISK_OK,
       every caller asking for durability would get a lie. */
    ok = ok && (disk_flush(0xFE) == DISK_ERR_NO_DEVICE);
    ok = ok && (disk_flush(0xFF) == DISK_ERR_NO_DEVICE);

    /* The mounted volume's own disk accepts the request. */
    ok = ok && (cxfs_sync() == 0);

    /* An unmounted volume slot is refused rather than flushing whatever disk
       happens to be in that slot's stale fields. */
    ok = ok && (cxfs_sync_volume(cxfs_volume_slots()) != 0);      /* past the end */
    ok = ok && (cxfs_sync_volume(cxfs_volume_slots() - 1) != 0);  /* a free slot */
    ok = ok && (cxfs_sync_volume(0) == 0);                        /* the root volume */

    /* Write, sync, read back. This does not prove the platter - nothing here
       can - but it does prove the sync is not destructive and that the write
       path still works around it, which is the failure a flush implemented
       against the wrong register would actually produce. */
    int dir = cxfs_resolve("/Temp", 0);
    if (dir < 0) return ok;                        /* no /Temp - the rest is moot */
    int fid = cxfs_find_in_dir((uint32_t)dir, "ktsync.txt");
    if (fid < 0) fid = cxfs_create_entry((uint32_t)dir, "ktsync.txt", CXFS_TYPE_FILE);
    if (fid < 0) return 0;

    const char *msg = "durable";
    ok = ok && (cxfs_write_file((uint32_t)fid, msg, 7) == 0);
    ok = ok && (cxfs_sync() == 0);

    char back[8];
    ok = ok && (cxfs_read_file((uint32_t)fid, back, 7) == 7);
    for (int i = 0; i < 7; i++) ok = ok && (back[i] == msg[i]);

    /* and a second sync with nothing outstanding is still fine - a flush is
       not a one-shot resource */
    ok = ok && (cxfs_sync() == 0);

    cxfs_delete_entry((uint32_t)fid);
    return ok;
}

/* When a USB stick is not flushed (usb_storage_flush). Real sticks cannot be
 * made to refuse on demand, so the two decisions are checked against crafted
 * replies: the caching page's WCE bit, found past any block descriptors the
 * device sent despite DBD, and ILLEGAL REQUEST in either sense format meaning
 * "no such command" while any other key stays an error. */
static int test_usb_flush_policy(void) {
    int ok = 1;

    /* header (length, medium, flags, bdl=0), then page 08h with WCE set */
    uint8_t m[24] = { 23, 0, 0, 0,  0x08, 0x12, 0x04 };
    ok = ok && (usb_storage_caching_wce(m, 24) == 1);
    m[6] = 0x00;                                    /* WCE clear */
    ok = ok && (usb_storage_caching_wce(m, 24) == 0);
    m[4] = 0x0A;                                    /* some other page */
    ok = ok && (usb_storage_caching_wce(m, 24) == -1);
    ok = ok && (usb_storage_caching_wce(m, 3) == -1);

    /* an 8-byte block descriptor the device sent anyway: the page moves */
    uint8_t d[32] = { 31, 0, 0, 8,  0, 0, 0, 0, 0, 0, 2, 0,  0x88, 0x12, 0x04 };
    ok = ok && (usb_storage_caching_wce(d, 32) == 1);
    /* a reply cut short before the page's byte 2 is not read past its end */
    ok = ok && (usb_storage_caching_wce(d, 14) == -1);
    d[0] = 13;                                      /* nor past its own length */
    ok = ok && (usb_storage_caching_wce(d, 32) == -1);

    uint8_t s[18] = { 0x70, 0, 0x05, 0, 0, 0, 0, 10, 0, 0, 0, 0, 0x20, 0 };
    ok = ok && (usb_storage_sync_unsupported(s, 18) == 1);
    s[2] = 0x02;                                    /* NOT READY: a real failure */
    ok = ok && (usb_storage_sync_unsupported(s, 18) == 0);
    s[2] = 0x03;                                    /* MEDIUM ERROR */
    ok = ok && (usb_storage_sync_unsupported(s, 18) == 0);
    uint8_t x[8] = { 0x72, 0x05, 0x20, 0x00 };      /* descriptor format */
    ok = ok && (usb_storage_sync_unsupported(x, 8) == 1);
    x[0] = 0x00;                                    /* not sense data at all */
    ok = ok && (usb_storage_sync_unsupported(x, 8) == 0);
    return ok;
}

static int test_crypto_profile(void) {
    uint8_t *b = (uint8_t *)kmalloc(16u + 256u);
    if (!b) return 0;
    struct rsa_pubkey k;
    int ok = 1;
    const uint32_t len = 16u + 256u;

    /* The control: the one profile, which must be ACCEPTED. Without it every
       case below could pass by the parser refusing everything. */
    xkpk_hdr(b, 1, 2048, 65537, 256, 0);
    ok = ok && rsa_parse_xkpk(b, len, &k) == 0;
    ok = ok && k.modulus_len == 256 && k.exponent == 65537;

    /* exponent 1: verification becomes the identity function. */
    xkpk_hdr(b, 1, 2048, 1, 256, 0);
    ok = ok && rsa_parse_xkpk(b, len, &k) != 0;

    /* an even exponent is not an RSA exponent at all. */
    xkpk_hdr(b, 1, 2048, 4, 256, 0);
    ok = ok && rsa_parse_xkpk(b, len, &k) != 0;

    /* a key size this kernel cannot verify, declared consistently. */
    xkpk_hdr(b, 1, 4096, 65537, 256, 0);
    ok = ok && rsa_parse_xkpk(b, len, &k) != 0;

    /* modulus_len disagreeing with key_bits: built by something that does not
       understand the format. */
    xkpk_hdr(b, 1, 2048, 65537, 128, 0);
    ok = ok && rsa_parse_xkpk(b, len, &k) != 0;

    /* a format version whose field offsets may not be where we just read them. */
    xkpk_hdr(b, 2, 2048, 65537, 256, 0);
    ok = ok && rsa_parse_xkpk(b, len, &k) != 0;

    /* reserved must be zero: a non-zero value means a field we do not know. */
    xkpk_hdr(b, 1, 2048, 65537, 256, 1);
    ok = ok && rsa_parse_xkpk(b, len, &k) != 0;

    /* and the magic still has to be right. */
    xkpk_hdr(b, 1, 2048, 65537, 256, 0);
    b[1] = 'Y';
    ok = ok && rsa_parse_xkpk(b, len, &k) != 0;

    kfree(b);
    return ok;
}

static int test_exec_admit(void) {
    if (exec_admit(CXEX_VERIFY_BAD_SIGNATURE) >= 0) return 0;
    if (exec_admit(CXEX_VERIFY_BAD_FORMAT)    >= 0) return 0;
    if (exec_admit(CXEX_VERIFY_WRONG_KEY)     >= 0) return 0;
    if (exec_admit(CXEX_VERIFY_BAD_SIG_BLOCK) >= 0) return 0;
    if (exec_admit(CX_TRUST_PUBLISHER)  != CX_TRUST_PUBLISHER)  return 0;
    if (exec_admit(CX_TRUST_UNVERIFIED) != CX_TRUST_UNVERIFIED) return 0;
#ifdef CXK_DEV_UNSIGNED
    if (exec_admit(CXEX_VERIFY_UNSIGNED) != CX_TRUST_PLATFORM) return 0;
#else
    if (exec_admit(CXEX_VERIFY_UNSIGNED) >= 0) return 0;
#endif
    return 1;
}

/* ---- guarded kernel stacks ----
 * The layout is what matters: the stack mapped at the top of its slot, the
 * page below it NOT mapped (that is the guard), and everything handed back on
 * free - frames and slot both. */
static int test_kstack(void) {
    uint32_t free0 = pmm_free_count();
    uint32_t live0 = kstack_live();
    uintptr_t base, base2;

    uintptr_t top = kstack_alloc(THREAD_STACK_DEFAULT, 7, &base);
    if (!top) return 0;
    uintptr_t top2 = kstack_alloc(THREAD_STACK_DEFAULT, 6, &base2);
    int ok = top2 != 0 && top2 != top;

    ok = ok && (top - base == THREAD_STACK_DEFAULT) && (top % KSTACK_SLOT_SIZE == 0);
    ok = ok && paging_get_phys(base) && paging_get_phys(top - 4);          /* stack mapped */
    ok = ok && !paging_get_phys(base - PAGE_SIZE);                         /* guard is not */
    ok = ok && !paging_get_phys(top - KSTACK_SLOT_SIZE);                   /* nor the slot's floor */
    if (ok) {
        volatile uint32_t *lo = (volatile uint32_t *)base;
        volatile uint32_t *hi = (volatile uint32_t *)(top - 4);
        *lo = 0x57AC4B07u; *hi = 0x0DDBA11u;
        ok = (*lo == 0x57AC4B07u) && (*hi == 0x0DDBA11u);
    }
    ok = ok && kstack_guard_owner(base - 4) == 7 && kstack_guard_owner(base2 - 4) == 6;
    ok = ok && kstack_guard_owner(base) == -1 && kstack_guard_owner(top - 16) == -1;
    ok = ok && kstack_alloc(KSTACK_SLOT_SIZE, 5, 0) == 0;                  /* no room for a guard */

    kstack_free(base2);
    kstack_free(base);
    ok = ok && kstack_guard_owner(base - 4) == -1 && !paging_get_phys(base);
    ok = ok && kstack_live() == live0 && pmm_free_count() == free0;
    return ok;
}

/* ---- thread 0 runs on a guarded stack too ----
 * The self-tests run on thread 0, so a local here is on its stack: that stack
 * must be in the guarded region with an unmapped page below it, and thread 0
 * must have an esp0 stack of its own for its trips to ring 3. */
static int test_main_stack(void) {
    volatile uint32_t here = 0;
    uintptr_t a = (uintptr_t)&here;
    if (thread_current_id() != 0) return 0;
    if (a < KSTACK_REGION_BASE || a >= KSTACK_REGION_BASE + KSTACK_REGION_SIZE) return 0;
    uintptr_t top  = (a & ~(uintptr_t)(KSTACK_SLOT_SIZE - 1)) + KSTACK_SLOT_SIZE;
    uintptr_t base = top - KSTACK_MAIN_SIZE;
    return paging_get_phys(base) && !paging_get_phys(base - PAGE_SIZE)
        && kstack_guard_owner(base - 4) == 0
        && thread_current_kstack_top() != 0;
}

/* ---- kernel configuration (kernel.xkco) ----
 * Parsed with the X Data reader linked in from os/std/xdata.xfxn, so this is
 * also the test that X code runs correctly inside the kernel. Every refusal is
 * checked for WHERE it points, not just that it happened - a wrong line number
 * sends someone to the wrong place - and for leaving the caller's values alone,
 * which is the "never half-applied" rule. */
static uint32_t kc_len(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }

static int kc_refused(const char *doc, uint32_t want_at) {
    struct kconfig c;
    kconfig_defaults(&c);
    c.max_threads = 999;                       /* sentinel: must survive */
    uint32_t at = 0xFFFFFFFFu;
    const char *why = 0;
    if (kconfig_parse(doc, kc_len(doc), &c, &at, &why) == 0) return 0;
    return c.max_threads == 999 && at == want_at && why && why[0];
}

static int test_kconfig(void) {
    struct kconfig c;
    uint32_t at;
    const char *why;
    const char *good = "// the kernel's limits\nkernel_stack_kib = 16\n"
                       "max_threads = 12, memory_quota_mib = 0x8   /* hex is fine */\n";
    if (kconfig_parse(good, kc_len(good), &c, &at, &why) != 0) return 0;
    if (c.kernel_stack != 16384 || c.max_threads != 12 || c.memory_quota != 8u * 1024u * 1024u) return 0;
    if (kconfig_parse("max_handles = 4096\n", 19, &c, &at, &why) != 0 || c.max_handles != 4096) return 0;

    /* absent keys keep their defaults; an empty file is all defaults */
    struct kconfig d;
    kconfig_defaults(&d);
    if (kconfig_parse("max_threads = 6\n", 16, &c, &at, &why) != 0) return 0;
    if (c.max_threads != 6 || c.kernel_stack != d.kernel_stack || c.memory_quota != d.memory_quota
        || c.max_handles != d.max_handles) return 0;
    if (kconfig_parse("", 0, &c, &at, &why) != 0 || c.max_threads != d.max_threads) return 0;

    return kc_refused("kernel_stack = 8\n", 0)                             /* unknown key */
        && kc_refused("max_threads = 12\nmax_thread = 12\n", 17)          /* misspelt */
        && kc_refused("max_threads = 257\n", 14)                           /* out of range */
        && kc_refused("max_threads = 3\n", 14)
        && kc_refused("max_handles = 4097\n", 14)
        && kc_refused("max_handles = 15\n", 14)
        && kc_refused("kernel_stack_kib = 10\n", 19)                       /* not a multiple of 4 */
        && kc_refused("kernel_stack_kib = 64\n", 19)
        && kc_refused("memory_quota_mib = \"16\"\n", 19)                   /* a string */
        && kc_refused("memory_quota_mib = -1\n", 19)
        && kc_refused("max_threads = 8 kernel_stack_kib = 8\n", 16)        /* no separator */
        && kc_refused("max_threads = 8\nmax_threads = 9\n", 16)            /* given twice */
        && kc_refused("max_threads = 12\nkernel_stack_kib = 12\nbogus = 1\n", 39);  /* valid, then not */
}

/* ---- the thread limit, and the stack size, are live settings ---- */
static void kc_exit_thread(void) { }

static volatile int kc_stack_ok = 0;
static void kc_stack_thread(void) {
    /* Find our own stack from a local: it sits at the top of its 64 KB slot.
       The configured size must be mapped, and the page just under it must not. */
    volatile uint32_t here = 0;
    uintptr_t top  = ((uintptr_t)&here & ~(uintptr_t)(KSTACK_SLOT_SIZE - 1)) + KSTACK_SLOT_SIZE;
    uintptr_t base = top - 16384u;
    kc_stack_ok = paging_get_phys(base) != 0 && paging_get_phys(base - PAGE_SIZE) == 0;
}

static int test_thread_limit(void) {
    uint32_t L = sched_thread_limit(), S = sched_stack_bytes();
    int ok = sched_configure(THREAD_LIMIT_MIN - 1, S) != 0 && sched_configure(THREAD_LIMIT_MAX + 1, S) != 0
          && sched_configure(L, 4096) != 0 && sched_configure(L, 12288 + 1) != 0;

    /* room for exactly `room` more threads, and not one more */
    uint32_t n    = sched_thread_count();
    uint32_t lim  = (n + 2 < THREAD_LIMIT_MIN) ? THREAD_LIMIT_MIN : n + 2;
    uint32_t room = lim - n;
    if (sched_configure(lim, S) != 0) ok = 0;
    uint32_t made = 0;
    while (made < 40 && thread_create("limit", kc_exit_thread) >= 0) made++;
    ok = ok && made == room;
    for (int i = 0; i < 32 && sched_thread_count() > n; i++) yield();
    ok = ok && sched_thread_count() == n;

    /* a limit below what is already running is refused */
    if (n > THREAD_LIMIT_MIN) ok = ok && sched_configure(n - 1, S) != 0;

    /* a new stack size applies to the next thread made */
    kc_stack_ok = 0;
    if (sched_configure(L, 16384) != 0) ok = 0;
    if (thread_create("stack16", kc_stack_thread) < 0) ok = 0;
    for (int i = 0; i < 32 && sched_thread_count() > n; i++) yield();
    ok = ok && kc_stack_ok;

    sched_configure(L, S);
    return ok && sched_thread_limit() == L && sched_stack_bytes() == S;
}

/* ---- the thread and pid tables grow (object/idtab.h) ----
 * Thread ids and pids were slots in fixed arrays of 64, with every per-thread
 * table beside them the same size. Here more than 64 threads are alive at
 * once, each in a process of its own, so both tables must have grown past it;
 * each must run, and once all have exited every id, record and process must
 * be given back. */
#define TG_THREADS 80
static volatile int tg_ran;
static volatile int tg_release;
static void tg_thread(void) {
    tg_ran++;
    while (!tg_release) yield();
}

static int test_thread_table_grows(void) {
    uint32_t L = sched_thread_limit(), S = sched_stack_bytes();
    uint32_t n0 = sched_thread_count(), p0 = proc_live();
    if (sched_configure(n0 + TG_THREADS, S) != 0) return 0;

    tg_ran = 0;
    tg_release = 0;
    int ok = 1, top_tid = -1, top_pid = -1, made = 0;
    for (int i = 0; i < TG_THREADS; i++) {
        int id = thread_create("grow", tg_thread);
        if (id < 0) { ok = 0; break; }
        made++;
        if (id > top_tid) top_tid = id;
        if (thread_pid(id) > top_pid) top_pid = thread_pid(id);
    }
    for (int i = 0; i < 64 && tg_ran < made; i++) yield();
    ok = ok && tg_ran == TG_THREADS
            && top_tid >= 64 && top_pid >= 64
            && sched_thread_count() == n0 + TG_THREADS
            && proc_live() == p0 + TG_THREADS;

    tg_release = 1;
    for (int i = 0; i < 256 && sched_thread_count() > n0; i++) yield();
    ok = ok && sched_thread_count() == n0 && proc_live() == p0;

    sched_configure(L, S);
    return ok;
}

/* ---- a handle table grows to its limit, and its process gives it all back ----
 * One thread, in a process of its own, fills its table with duplicates of one
 * endpoint until the table refuses, and exits holding all of them. Every
 * handle must work and be distinct, the table must stop at exactly the limit
 * set for new processes (100: it starts at 16 and is not a power of two), and once the process is gone the endpoint, the process
 * record and every byte of the table's array must be back. */
static volatile int hg_done, hg_ok;
static volatile uint32_t hg_count, hg_limit;

static void hg_thread(void) {
    struct htab *t = thread_htab(thread_current_id());
    int32_t first = ep_create();
    int ok = t && first > 0 && t->max >= HTAB_LIMIT_MIN;
    int32_t prev = first, past16 = 0, r = 0;
    while (ok && (r = htab_dup(t, first, HRIGHT_RECV | HRIGHT_DUPLICATE)) > 0) {
        if (r == prev || r == first) ok = 0;
        if (((uint32_t)r & (HTAB_MAX_SLOTS - 1)) >= 16) past16 = 1;
        prev = r;
    }
    hg_count = htab_count(t);
    hg_limit = t ? t->max : 0;
    ok = ok && r == E_NO_RESOURCES && past16 && hg_count == hg_limit;

    /* every one of them names the endpoint */
    uint32_t found = 0;
    for (uint32_t i = 0; ok && i < t->cap; i++) {
        if (!t->e[i].obj) continue;
        struct endpoint *ep = ep_from_handle((int32_t)((t->e[i].gen << HTAB_SLOT_BITS) | i),
                                             HRIGHT_RECV, NULL);
        if (ep) found++;
        ep_put(ep);
    }
    hg_ok = ok && found == hg_count;
    hg_done = 1;                  /* and exit with every handle still open */
}

static int test_handle_table_grows(void) {
    uint32_t L = sched_thread_limit(), S = sched_stack_bytes();
    uint32_t n0 = sched_thread_count(), p0 = proc_live();
    uint32_t eps0 = kobj_live(KOBJ_ENDPOINT), heap0 = heap_bytes_used(), allocs0 = heap_allocs_live();
    if (n0 + 1 > L && sched_configure(n0 + 1, S) != 0) return 0;   /* room for one */
    uint32_t H = proc_handle_limit();
    proc_set_handle_limit(100);

    hg_done = hg_ok = 0;
    int id = thread_create("hgrow", hg_thread);
    for (int i = 0; i < 400 && id >= 0 && !(hg_done && sched_thread_count() == n0
                                             && proc_live() == p0); i++)
        thread_sleep_ms(1);
    sched_configure(L, S);
    proc_set_handle_limit(H);

    return id >= 0 && hg_done && hg_ok && hg_limit == 100
        && sched_thread_count() == n0 && proc_live() == p0
        && kobj_live(KOBJ_ENDPOINT) == eps0      /* every handle closed */
        && heap_bytes_used() == heap0            /* and the array freed */
        && heap_allocs_live() == allocs0;
}

/* ---- a process that faults gives back everything (roadmap step 2d) ----
 * A process in its own address space, holding a handle, with a code and a
 * stack page mapped, faults in ring 3. Afterwards every count is back where it
 * started: the frames (code, stack, page table, directory), the endpoint, the
 * heap, the process record and the thread.
 *
 * Two separate things are proven. The process frees its space when its last
 * thread goes, however it went: a fault used to thread_exit() past the
 * teardown, which sat on the normal way out of the trampoline, so a spawned
 * process that crashed leaked its whole space. And the fault leaves the way
 * SYS_EXIT does, back out of enter_usermode with USERMODE_FAULTED, so whatever
 * entered ring 3 cleans up after it: px_back is set only on that path. */
#define PX_CODE   0x00400000u
#define PX_STACK  0x00401000u
static volatile int      px_mine, px_back, px_rc;
static volatile paddr_t px_pd;

static void px_thread(void) {
    if (addr_space_current() != px_pd) return;          /* not running in its own space */
    if (ep_create() <= 0) return;                  /* a handle only the exit closes */
    paddr_t c = pmm_alloc(), s = pmm_alloc();
    if (!c || !s) return;
    if (paging_map_user(PX_CODE,  c, VM_WRITE) != 0) return;
    if (paging_map_user(PX_STACK, s, VM_WRITE) != 0) return;
    ((volatile uint8_t *)PX_CODE)[0] = 0x0F;       /* ud2: an invalid opcode, at once */
    ((volatile uint8_t *)PX_CODE)[1] = 0x0B;
    if (paging_map_user(PX_CODE,  c, VM_EXEC) != 0) return;   /* W^X: now it runs */
    px_mine = 1;
    px_rc   = enter_usermode(PX_CODE, PX_STACK + 0x1000u - 16u, thread_current_usave(), 0);
    px_back = 1;
}

static int test_process_exit_releases(void) {
    uint32_t L = sched_thread_limit(), S = sched_stack_bytes();
    uint32_t n0 = sched_thread_count(), p0 = proc_live();
    if (n0 + 1 > L && sched_configure(n0 + 1, S) != 0) return 0;   /* room for one */
    uint32_t eps0 = kobj_live(KOBJ_ENDPOINT), allocs0 = heap_allocs_live();
    uint32_t faults0 = usermode_faults(), free0 = pmm_free_count();
    px_mine = px_back = 0;
    px_rc = 0;

    int ok = 0;
    paddr_t space;
    if (addr_space_create(&space) == 0) {
        int id = thread_create_process("pexit", px_thread);
        if (id < 0) addr_space_release(space);
        else {
            px_pd = space;
            thread_mark_user(id);
            thread_set_space(id, space);   /* the process's from here */
            thread_start(id);
            for (int i = 0; i < 400 && !(px_back && sched_thread_count() == n0
                                         && proc_live() == p0); i++)
                thread_sleep_ms(1);
            ok = 1;
        }
    }
    sched_configure(L, S);

    return ok && px_mine && px_back && px_rc == USERMODE_FAULTED
        && usermode_faults() == faults0 + 1
        && sched_thread_count() == n0 && proc_live() == p0
        && kobj_live(KOBJ_ENDPOINT) == eps0
        && heap_allocs_live() == allocs0
        && pmm_free_count() == free0;
}

#if CXK_KTEST_STACK_OVERFLOW
/* Recurse until the stack runs out. The volatile buffer keeps each frame real
   and the addition after the call keeps it from becoming a loop. */
static __attribute__((noinline)) uint32_t overflow_down(uint32_t n) {
    volatile uint8_t pad[256];
    pad[0] = (uint8_t)n;
    return overflow_down(n + 1) + pad[0];
}
static void overflow_thread(void) { overflow_down(0); }
#endif

void ktest_run(void) {
    int passed = 0, total = 0;

    /* each test: total++, and passed += its pass/fail. report() prints only on
       failure, so the only per-test output is failures. */

    /* the architecture's own first (arch/ktest_arch.h): some state what boot
       left, such as a kernel space with nothing in its user half, which the
       ring-3 tests below change. */
    unsigned n_arch;
    const struct ktest_arch_case *arch_case = ktest_arch_cases(&n_arch);
    for (unsigned i = 0; i < n_arch; i++) {
        total++; passed += report(arch_case[i].name, arch_case[i].fn());
    }

    total++; passed += report("paging maps, and refuses what it must not", test_paging());
    total++; passed += report("I/O windows refuse what lies outside them", test_iowin());
    total++; passed += report("heap kmalloc/kfree",               test_heap());
    total++; passed += report("scheduler (cooperative)",          test_sched_coop());
    total++; passed += report("scheduler (preemptive)",           test_sched_preempt());
    total++; passed += report("esp0 follows the running thread",  test_esp0_follows_thread());
    total++; passed += report("ring 3 single round-trip",         test_ring3_single());
    total++; passed += report("every syscall passes one point",   test_sysrec());
    total++; passed += report("syscall table refuses unknowns",   test_syscall_table());
    total++; passed += report("ring 3 processes (coop+preempt)",  test_ring3_processes());
    total++; passed += report("identity (user never UID 0)",      test_identity());
    total++; passed += report("storage (ATA read sector 0)",      test_storage());
    total++; passed += report("pci (bus enumeration)",            test_pci());
    total++; passed += report("ahci (controller + read)",         test_ahci());
    total++; passed += report("device DMA held, below 4 GB",      test_dma_regions());
    total++; passed += report("cxfs (read-only mount check)",     test_cxfs());
    total++; passed += report("cxfs offset I/O + compaction",      test_cxfs_offset());
    total++; passed += report("cxfs volume tags",                  test_cxfs_volumes());
    total++; passed += report("clock + timed sleep",               test_clock_sleep());
    total++; passed += report("tick counter wraparound",           test_timer_wraparound());
    total++; passed += report("a tick is a millisecond, by the PM timer", test_tick_rate());
    total++; passed += report("the cycle counter measures time",   test_cycle_counter());
    total++; passed += report("the wall clock reads a plausible date", test_wallclock());
    total++; passed += report("the boot handover came through CXBI", test_cxbi());
    total++; passed += report("a crash record goes to the crash area and back", test_crashlog());
    total++; passed += report("a user fault is saved to the crash area", test_user_fault_saved());
    total++; passed += report("the system log is saved to the crash area", test_bootlog_saved());
    total++; passed += report("disk writes reach only the CXOS disk", test_disk_write_guard());
    total++; passed += report("xHCI scratchpad pages are whole and returned", test_xhci_scratchpad());
    total++; passed += report("PS/2 keyboard and mouse reach the kernel", ktest_platform_input());
    total++; passed += report("memory mappings (SYS_VM_*)",     test_vmregion());
    total++; passed += report("lifecycles: unmap, exit, resched", test_lifecycle_sequences());
    total++; passed += report("bounded strings: cap, truncation", test_bounded_strings());
    total++; passed += report("block-device contract",             test_disk_contract());
    total++; passed += report("ipv4 parser refuses bad frames",   ktest_ip_parse_adversarial());
    total++; passed += report("arp parser refuses bad frames",    ktest_arp_input_adversarial());
    total++; passed += report("cxfs refuses bad superblocks",     ktest_cxfs_sb_adversarial());
    total++; passed += report("cxfs refuses bad extents + names", ktest_cxfs_entry_adversarial());
    total++; passed += report("guarded kernel stacks",             test_kstack());
    total++; passed += report("double fault on its own stack",     ktest_arch_double_fault_stack());
    total++; passed += report("thread 0 on a guarded stack",       test_main_stack());
    total++; passed += report("kernel config (X Data, linked X)",  test_kconfig());
    total++; passed += report("thread limit + stack size",         test_thread_limit());
    total++; passed += report("thread and pid tables grow past 64", test_thread_table_grows());
    total++; passed += report("a handle table grows to its limit and is freed", test_handle_table_grows());
    total++; passed += report("a process that faults gives back everything", test_process_exit_releases());
    total++; passed += report("the cpu feature table is this cpu's",   ktest_arch_cpu_features());
    total++; passed += report("a syscall leaves ring 3 its segments",  ktest_arch_user_segments_kept());
    total++; passed += report("syscalls through the system page",     usermode_syspage_test());
    total++; passed += report("cxex signature + tamper",           test_cxex_signature());
    total++; passed += report("disk: ATA LBA range refused",        test_disk_lba_range());
    total++; passed += report("cxex loader refuses bad images",    ktest_loader_adversarial());
    total++; passed += report("user pointer writability",          ktest_user_ptr_writability());
    total++; passed += report("SYS_SPAWN verifies its image",      ktest_spawn_verifies_image());
    total++; passed += report("xfnt: loadable font validated",     test_xfnt());
    total++; passed += report("variable font cell size",           test_font_cell());
    total++; passed += report("console font staged on disk",       test_console_font_disk());
    total++; passed += report("disk durability: flush is askable", test_disk_durability());
    total++; passed += report("USB flush: when a stick is not asked", test_usb_flush_policy());
    total++; passed += report("crypto profile: one key shape only", test_crypto_profile());
    total++; passed += report("exec admission (dev / release)",   test_exec_admit());
    total++; passed += report("file syscalls (SYS_FILE_*)",        usermode_file_test());
    total++; passed += report("memory syscalls (SYS_VM_*)",        usermode_vm_test());
    total++; passed += report("power and input syscalls",          usermode_power_input_test());
    total++; passed += report("framebuffer syscalls (SYS_FB_*)",   usermode_fb_test());
    total++; passed += report("network syscalls (SYS_NET_*)",       usermode_net_test());
    total++; passed += report("background output goes to the log", usermode_log_test());
    total++; passed += report("advisory locks follow the open file", usermode_flock_test());
    total++; passed += report("process handles: wait, how it ended", usermode_process_test());
    total++; passed += report("process kill: every wait, both safe points", usermode_kill_test());
    total++; passed += report("process stop: asked, then ended at the deadline", usermode_stop_test());
    total++; passed += report("ipc endpoints refcounted + reclaimed", test_ipc_endpoint_lifetime());
    total++; passed += report("object caches map and return pages", test_ocache_pages());
    total++; passed += report("two threads share one process",   test_proc_threads());
    total++; passed += report("user copies validated at the copy",  test_user_copy_validation());
    total++; passed += report("user copy faults are recoverable",   test_user_copy_fault_recovery());
    total++; passed += report("CR0.WP: kernel obeys read-only",     test_cr0_write_protect());

    /* single summary line: green if all passed, red if any failed. */
    if (passed == total) {
        klog_u32("KTEST", SEV_OK, "self-tests: all ",
                        (uint32_t)total,
                        LOG_COLOR_VALUE, " passed");
    } else {
        klog_u32("KTEST", SEV_FAIL, "self-tests failed: ",
                        (uint32_t)(total - passed),
                        LOG_COLOR_VALUE, " failure(s)");

        klog_child_u32("tests run: ",
                        (uint32_t)total,
                        LOG_COLOR_VALUE, "");

        klog_child_u32("tests passed: ",
                        (uint32_t)passed,
                        VGA_ATTR(VGA_LIGHT_GREEN, VGA_BLACK), "");
    }

#if CXK_KTEST_STACK_OVERFLOW
    /* Last, because nothing comes back from it: the next thing on screen must
       be a double-fault panic naming "overflow". */
    klog("KTEST", SEV_WARN, "overflowing a kernel stack on purpose (CXK_KTEST_STACK_OVERFLOW)");
#if CXK_KTEST_STACK_OVERFLOW == 2
    overflow_down(0);                    /* thread 0's own stack: "main" */
#else
    thread_create("overflow", overflow_thread);
    for (;;) yield();
#endif
#endif
}