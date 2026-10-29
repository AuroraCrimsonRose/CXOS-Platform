// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/spawn.c */
/* Aurora Tejeda / CATX Systems */
/*
 * ABI v1 process launch. Every ring-3 process - the executive AND its apps -
 * is a normal scheduler thread with its own address space, kernel stack (esp0),
 * and capabilities. proc_start() is the one path that creates them; cxex_exec()
 * (the executive, kernel-launched, trusted, GRANT_OS_BASELINE, no broker) and
 * sys_spawn() (apps, executive-launched, caps=0, a broker handle) both call it.
 * There is no special "thread 0 runs the executive" case anymore.
 */

#include "spawn.h"
#include "sched.h"
#include "proc.h"               /* ->background */
#include "ipc.h"
#include "caps.h"
#include "usermode.h"          /* user_ptr_readable / user_ptr_writable */
#include "sysfile.h"           /* sysfile_entry_doomed */
#include "arch/paging.h"
#include "arch/usermode.h"     /* enter_usermode */
#include "pmm.h"
#include "vmregion.h"
#include "heap.h"
#include "cxex.h"               /* cxex_parse_header: is this the executive */
#include "cxex_load.h"
#include "syspage.h"           /* SYSPAGE_VIRT, syspage_map */
#include "exec.h"
#include "launch.h"
#include "cxfs.h"
#include "logging.h"
#include "diag/crashlog.h"     /* crashlog_save_faults */
#include <stddef.h>

#define USER_STACK_TOP    0xBFFFF000u
/* 256 KB. It was 16 KB, which an ordinary program fits in and a recursive
   one does not: the X compiler's type checker, run on its own source, needs
   more than 32 KB. The pages are allocated up front; the page below the stack
   is never mapped, so running past it is a fault that ends the process, not a
   write into whatever lies beneath. */
#define USER_STACK_PAGES  64u
/* The top of the user half as syspage.h draws it: the system page below
   the stack's guard page, and no image section above USER_IMAGE_END. */
_Static_assert(USER_STACK_TOP == USER_ARGS_BASE, "the argument page sits on the stack");
_Static_assert(SYSPAGE_VIRT + 0x2000u == USER_STACK_TOP - USER_STACK_PAGES * 0x1000u,
               "one unmapped guard page between the stack and the system page");
_Static_assert(USER_IMAGE_END < SYSPAGE_VIRT - 0x1000u, "images stay below the system page");
#define PROC_MAX_IMAGE    (1u << 20)   /* 1 MiB cap on an image */

/* The record proc_start hands the new thread (thread_set_arg) for its
   trampoline, which runs later, on that thread, in its own address space. */
struct proc_rec {
    const void       *image;     /* kernel-heap copy of the CXEX bytes */
    uint32_t          image_len;
    const char       *args;      /* kernel-heap copy of the argument blob, or NULL */
    uint32_t          args_len;
};

/* Freed with the thread, however it ended: the heap copies first, if the
   trampoline never got as far as freeing them. */
static void proc_rec_drop(void *arg) {
    struct proc_rec *r = arg;
    if (r->image) kfree((void *)r->image);
    if (r->args)  kfree((void *)r->args);
    kfree(r);
}

extern const struct cxex_load_ops cxex_kernel_load_ops;   /* cxex_loadk.c */

/* Map and fill the argument page in the CURRENT (new) address space.
 *
 * The blob arrives as argc NUL-terminated strings back to back; the page it
 * becomes holds argc, then one offset per argument, then the strings. See the
 * layout in cxk_abi.h. The page is mapped whatever happens, so a program with
 * no arguments reads argc == 0 instead of faulting on its first look.
 *
 * Anything that will not fit - too many arguments, or strings longer than the
 * page has room for after the offset table - is dropped rather than truncated
 * into something half-valid. A program cannot tell a dropped tail from an
 * argument it was never given, and a silently shortened string is worse than
 * a missing one. The caller-side checks in sys_spawn and sys_exec_path reject
 * an oversized blob up front, so reaching this is a kernel bug, not a user
 * one. Returns 0 on success, negative if the page could not be mapped. */
static int build_args_page(const char *blob, uint32_t len) {
    paddr_t frame = pmm_alloc();
    if (!frame) return -1;
    if (paging_map_user(USER_ARGS_BASE, frame, VM_WRITE) != 0) {
        pmm_free(frame);
        return -1;
    }

    uint8_t  *page = (uint8_t *)USER_ARGS_BASE;
    uint32_t *head = (uint32_t *)page;
    for (uint32_t i = 0; i < USER_ARGS_MAX; i++) page[i] = 0;

    if (!blob || len == 0 || blob[len - 1] != '\0') { head[0] = 0; return 0; }

    uint32_t argc = 0;
    for (uint32_t i = 0; i < len; i++) if (blob[i] == '\0') argc++;
    if (argc > USER_ARGS_MAXC) { head[0] = 0; return 0; }

    uint32_t strings = 4u + 4u * argc;            /* where the strings begin */
    if (strings + len > USER_ARGS_MAX) { head[0] = 0; return 0; }

    head[0] = argc;
    uint32_t at = strings, n = 0;
    head[1 + n] = at;                             /* the first string starts here */
    for (uint32_t i = 0; i < len; i++) {
        page[at++] = (uint8_t)blob[i];
        if (blob[i] == '\0' && ++n < argc) head[1 + n] = at;
    }
    return 0;
}

/* Every way out of the trampoline below is a plain thread_exit. What the
   process took goes when its last thread is reaped (cpu/proc.c): the address
   space, its frames and page tables, its handles; and this record, with the
   heap copies, goes with the thread (proc_rec_drop). A spawn that died before
   enter_usermode once just thread_exit()ed while the space was torn down only
   on the normal way out, so an image the loader REFUSES could be spawned in a
   loop to drain the PMM; and a process that faulted leaked the same way.
   Tying the release to the process instead of to a line here is what makes
   every exit, including ones not written yet, release it. */

/* A way out before ring 3. For the executive it is said in so many words:
   kmain only knows a pid was made, and `cxk verify-boot --full` fails on this
   line rather than waiting out its timeout for a start that will not come. */
static void not_started(int exec) {
    if (exec) klog("EXEC", SEV_ERR, "executive refused at load");
    thread_exit();
}

/* Runs on the new thread, in its own address space (the scheduler loaded its CR3
   before switching here). Places the image, builds a ring-3 stack, drops to user
   mode. Returns only when the process SYS_EXITs. */
static void proc_trampoline(void) {
    int pid = thread_current_pid();
    struct proc_rec *r = thread_current_arg();

    /* The executive is the one OS-tier image (.xoex, platform-signed only). */
    struct cxex_header h;
    int exec = cxex_parse_header(r->image, r->image_len, &h) == 0 &&
               h.type_code == CXEX_TYPE_OS;

    uint32_t entry = 0;
    int lrc = cxex_load(r->image, r->image_len, &cxex_kernel_load_ops, &entry);
    if (lrc != CXEX_LOAD_OK) {
        /* The reason, not just the fact: these are the loader's refusals - a
           W+X section, a range outside the user half, data past the signature,
           a quota that has no room - and which one fired is the whole content of
           the message. */
        klog("PROC", SEV_ERR, cxex_load_strerror(lrc));
        not_started(exec);
    }

    /* The stack is the process's memory as much as the image is, so it comes out
       of the same quota. Charged before a frame is allocated, so a process whose
       ceiling cannot cover a stack is refused having taken nothing. */
    if (vm_charge(pid, USER_STACK_PAGES * 0x1000u) != E_OK) {
        klog("PROC", SEV_ERR, "the memory quota has no room for a user stack");
        not_started(exec);
    }
    for (uint32_t i = 0; i < USER_STACK_PAGES; i++) {
        paddr_t p = pmm_alloc();
        if (!p) { klog("PROC", SEV_ERR, "no stack memory"); not_started(exec); }
        if (paging_map_user(USER_STACK_TOP - (i + 1) * 0x1000u, p,
                            VM_WRITE) != 0) {
            klog("PROC", SEV_ERR, "user stack is not in the user half");
            pmm_free(p); not_started(exec);
        }
    }
    uint32_t ustack_top = USER_STACK_TOP - 16u;

    /* After the stack, so a failure here cannot leave a half-built process
       holding stack pages it will never use. */
    if (vm_charge(pid, 0x1000u) != E_OK) {
        klog("PROC", SEV_ERR, "the memory quota has no room for the argument page");
        not_started(exec);
    }
    if (build_args_page(r->args, r->args_len) != 0) {
        klog("PROC", SEV_ERR, "no memory for the argument page");
        not_started(exec);
    }

    /* Last, as the one page every program calls the kernel through: charged
       like the others, and freed with the space like them. */
    if (vm_charge(pid, 0x1000u) != E_OK) {
        klog("PROC", SEV_ERR, "the memory quota has no room for the system page");
        not_started(exec);
    }
    if (syspage_map(SYSPAGE_VIRT, NULL) != 0) {
        klog("PROC", SEV_ERR, "no memory for the system page");
        not_started(exec);
    }

    kfree((void *)r->image); r->image = NULL;   /* image placed; heap copy done */
    if (r->args) { kfree((void *)r->args); r->args = NULL; }   /* and copied in */

    /* Here, not where the pid was made: by now the image has passed every
       load check (signature tier before, ABI hash, W^X, ranges, quota). */
    if (exec) klog_u32("EXEC", SEV_OK, "executive started, pid ", (uint32_t)pid, LOG_COLOR_VALUE, "");
    enter_usermode(entry, ustack_top, thread_current_usave(), SYSPAGE_VIRT);

    /* SYS_EXIT, or a fault (usermode_fault returns here the same way, and a
       fault it noted is saved now that this thread is out of the exception). */
    crashlog_save_faults();
    klog_u32("PROC", SEV_INFO, "exited, pid ", (uint32_t)pid, LOG_COLOR_VALUE, "");
    thread_exit();   /* never returns; the reaper frees the stacks, the process its space */
}

/* Create a ring-3 scheduler thread running `image` (which must be readable in
   the CURRENT address space). caps = its capability set; broker = an endpoint
   to give it a SEND handle on, derived from broker_grant (or NULL for a root
   executive). Returns pid or -E_*. */
int proc_start(const void *image, uint32_t image_len, uint32_t caps,
               struct endpoint *broker, uint32_t broker_grant,
               const char *args, uint32_t args_len, int32_t *handle) {
    if (image_len == 0 || image_len > PROC_MAX_IMAGE) return E_OUT_OF_RANGE;
    if (args_len > USER_ARGS_MAX)                     return E_OUT_OF_RANGE;
    if (!args) args_len = 0;

    uint8_t *kimg = (uint8_t *)kmalloc(image_len);
    if (!kimg) return E_NO_MEMORY;
    for (uint32_t i = 0; i < image_len; i++)
        kimg[i] = ((const uint8_t *)image)[i];          /* copy from current space */

    /* The blob is read here, in the CALLER's space, and written much later in
       the child's - proc_trampoline runs on the new thread, after the CR3
       switch, where the caller's pages are gone. So it gets a kernel-heap copy
       of its own, exactly as the image does. */
    char *kargs = 0;
    if (args_len) {
        kargs = (char *)kmalloc(args_len);
        if (!kargs) { kfree(kimg); return E_NO_MEMORY; }
        for (uint32_t i = 0; i < args_len; i++) kargs[i] = args[i];
    }

    paddr_t space;
    if (addr_space_create(&space) != 0) { kfree(kimg); kfree(kargs); return E_NO_MEMORY; }

    /* Held until every record below is filled in: see thread_create_process. */
    struct proc_rec *rec = kmalloc(sizeof *rec);
    int tid = rec ? thread_create_process("proc", proc_trampoline) : -1;
    if (tid < 0) { kfree(rec); kfree(kimg); kfree(kargs); addr_space_release(space); return E_NO_MEMORY; }

    rec->image     = kimg;
    rec->image_len = image_len;
    rec->args      = kargs;
    rec->args_len  = args_len;
    thread_set_arg(tid, rec, proc_rec_drop);

    thread_mark_user(tid);
    thread_set_space(tid, space);   /* now the process's: loaded for it, freed with it */
    /* SPAWN_BACKGROUND rides in the caps word but is not a grant. */
    thread_set_caps(tid, caps & ~SPAWN_BACKGROUND);
    if (caps & SPAWN_BACKGROUND) thread_proc(tid)->background = 1;

    /* Memory authority attenuates down the spawn chain the same way grants do.
       The parent is whoever is running this call: the kernel for the executive
       (not tracked, so nothing narrows the default) or a ring-3 process for
       everything it launches. */
    int pid = thread_pid(tid);
    vm_proc_init(pid, vm_default_quota(), thread_current_pid());

    if (broker) {
        /* The child's handle is a reference on the broker's endpoint, which
           has to outlive every one of them (see the lifetime note in
           cpu/ipc.h). Its number is whatever the table gives, so the child
           asks for it with SYS_ARGS rather than assuming one. */
        int32_t bh = ep_install_handle(tid, broker, HRIGHT_SEND, broker_grant);
        if (bh < 0) klog_u32("PROC", SEV_WARN, "broker handle refused: ", (uint32_t)-bh, LOG_COLOR_VALUE, "");
        else        thread_set_broker(tid, bh);
    }
    /* The caller's handle on it, last, while the thread is still held: if
       there is no room for it, the process is discarded before it has run,
       not left running with nobody able to wait for it. */
    if (handle) {
        int32_t ph = htab_install(thread_htab(thread_current_id()), &thread_proc(tid)->hdr,
                                  HRIGHT_WAIT | HRIGHT_INSPECT | HRIGHT_DUPLICATE, 0);
        if (ph < 0) { thread_discard(tid); return ph; }
        *handle = ph;
    }
    thread_start(tid);
    return pid;   /* the process, not its thread */
}

/* Check a caller-supplied argument blob before anything copies it.
 *
 * Both launch syscalls take the same two fields, and both must refuse the same
 * things: a length with no pointer, a pointer the caller cannot actually read,
 * more than a page, or a blob whose last byte is not a terminator. That last
 * one matters more than it looks - build_args_page counts arguments by
 * counting NULs, so an unterminated blob would leave the final string running
 * off the end of what was copied. Rejecting it here means the page builder can
 * trust its input.
 *
 * Returns E_OK, or the error to hand back to the caller. */
static int check_args(const struct spawn_args *a) {
    if (!a->args) return a->args_len ? E_INVALID_ARGS : E_OK;   /* length without a blob */
    if (a->args_len == 0 || a->args_len > USER_ARGS_MAX) return E_OUT_OF_RANGE;
    if (!user_ptr_readable(a->args, (uint32_t)a->args_len)) return E_BAD_ADDRESS;
    if (((const char *)user_ptr(a->args))[a->args_len - 1] != '\0') return E_INVALID_ARGS;
    return E_OK;
}

/* SYS_SPAWN: an executive (GRANT_SPAWN) launches a capability-less app, brokered
   through one of its endpoints. */
int sys_spawn(uint64_t ua) {
    struct spawn_args a;
    if (user_copy_in(&a, ua, sizeof a) != 0) return E_BAD_ADDRESS;

    if (a.image_len == 0 || a.image_len > PROC_MAX_IMAGE) return E_OUT_OF_RANGE;
    if (!user_ptr_readable(a.image, (uint32_t)a.image_len)) return E_BAD_ADDRESS;

    int arc = check_args(&a);
    if (arc != E_OK) return arc;

    uint32_t bgrant = 0;
    struct endpoint *bep = ep_from_handle(a.broker_endpoint, HRIGHT_RECV, &bgrant);
    if (!bep) return E_BAD_HANDLE;

    /* Attenuation: child gets requested caps masked by the spawner's own caps.
       Pass a subset of your authority, never amplify. App-spawner (caps=0) -> 0. */
    uint32_t granted = (a.caps & thread_current_caps()) | (a.caps & SPAWN_BACKGROUND);

    /* COPY FIRST, THEN VERIFY, THEN LOAD THE COPY.
     *
     * This call used to go straight to proc_start, which does no verification
     * at all - so SYS_SPAWN ran whatever bytes it was handed. The comment below
     * on SYS_EXEC_PATH has described that as a way to run code the trusted key
     * never signed since SYS_FILE_OP landed, and the answer taken at the time
     * was to add a safe path beside it rather than to close this one. The only
     * thing standing between a GRANT_SPAWN holder and unsigned ring-3 code was
     * that nobody had tried.
     *
     * The order is the whole point. Verifying `a.image` in place and then
     * letting proc_start copy it would authenticate one set of bytes and load
     * another - the caller holds that memory, so it can change underneath.
     * Unreachable today, because one ring-3 process runs at a time; reachable
     * the moment concurrent user threads exist, which is the same window §6
     * closed for IPC. Copying into the kernel first means the bytes that are
     * verified are the bytes that are loaded, by construction, exactly as
     * SYS_EXEC_PATH achieves it by reading the file itself.
     *
     * user_copy_in rather than a raw loop: the range is re-checked at the
     * instant of the copy and a fault during it is recoverable (§6). The
     * user_ptr_readable above is an early reject, not the authority. */
    uint8_t *kimg = (uint8_t *)kmalloc((size_t)a.image_len);
    if (!kimg) { ep_put(bep); return E_NO_MEMORY; }
    if (user_copy_in(kimg, a.image, (uint32_t)a.image_len) != 0) {
        kfree(kimg);
        ep_put(bep);
        return E_BAD_ADDRESS;
    }

    int32_t ph = 0;
    int pid = cxex_exec_as(kimg, (size_t)a.image_len, granted, bep, bgrant, user_ptr(a.args), (uint32_t)a.args_len, &ph);
    kfree(kimg);        /* proc_start took its own copy for the new address space */
    ep_put(bep);        /* the child holds its own handle on it now */

    if (pid < 0) return pid;
    klog_u32("SPAWN", SEV_OK, "pid ", (uint32_t)pid, LOG_COLOR_VALUE,
             (granted & ~SPAWN_BACKGROUND) ? " (privileged, ring 3)" : " (caps=0, ring 3)");
    return ph;   /* the pid is OBJINFO_PROCESS */
}
/* ---- SYS_EXEC_PATH ---------------------------------------------------------
 * Run a program straight off the disk.
 *
 * The reason this is a syscall of its own, and not a shell that reads a file
 * and calls SYS_SPAWN with the bytes:
 *
 * HISTORICAL, and corrected 2026-10-09. What this said, accurately at the
 * time: SYS_SPAWN takes an image ALREADY in the caller's memory and does not
 * verify it - only the kernel's own load path calls cxex_verify_trusted. That
 * was harmless while ring 3 had no filesystem,
 * because the only images that could reach SYS_SPAWN came in through a trusted
 * build. SYS_FILE_OP ended that: a process can now read arbitrary bytes off a
 * disk. Handing those to SYS_SPAWN would be a way to run code the trusted key
 * never signed.
 *
 * So the kernel reads the file itself. The bytes that are verified are exactly
 * the bytes that are loaded - there is no window in which the caller could
 * swap them, because the caller never holds them. CX_ABI.md section 7.10 calls
 * this closing the gap "by construction".
 *
 * WHAT CHANGED: that paragraph went on to call this "the better fix than
 * adding a verify to SYS_SPAWN and hoping every future caller goes through
 * it" - and the 2026-10-09 review made the flaw in that reasoning plain.
 * Adding a safe path beside an unsafe one does not close the unsafe one. A
 * verify placed in SYS_SPAWN *itself* is not hope, it is enforcement, and
 * every caller goes through it by definition. SYS_SPAWN now copies the image
 * into the kernel and verifies the copy, so it is safe by the same
 * construction this path is, and both reach cxex_exec_as with bytes the
 * caller can no longer touch.
 *
 * This syscall still earns its place: it reads the file, so a caller does not
 * need the whole image in its own memory, and the shell stays EMBEDDED in the
 * executive rather than sitting on the disk where it could be replaced. Two
 * routes to ring 3, both verified, neither trusting the caller's buffer.
 */
int sys_exec_path(uint64_t upath, uint64_t ua) {
    struct spawn_args a;
    if (user_copy_in(&a, ua, sizeof a) != 0) return E_BAD_ADDRESS;

    int arc = check_args(&a);
    if (arc != E_OK) return arc;

    /* copy the path in a page at a time; user_ptr_readable walks page tables, so
       per-byte would be a page-table walk per character */
    char path[FILE_PATH_MAX];
    uint64_t addr = upath;
    if (!addr) return E_BAD_ADDRESS;
    uint32_t n = 0;
    for (;;) {
        if (n >= sizeof path) return E_OUT_OF_RANGE;
        if (n == 0 || (((addr + n) & 0xFFFu) == 0)) {
            if (!user_ptr_readable(addr + n, 1)) return E_BAD_ADDRESS;
        }
        path[n] = ((const char *)user_ptr(addr))[n];
        if (!path[n]) break;
        n++;
    }

    if (!cxfs_is_mounted()) return E_IO;

    /* Resolve against the caller's working directory, so `exec doc.xuex`
       means what it says from wherever the process happens to be. */
    int id = cxfs_resolve(path, thread_current_cwd());
    if (id < 0 || sysfile_entry_doomed((uint32_t)id)) return E_NOT_FOUND;   /* unlinked, still open */

    struct cxfs_entry e;
    if (cxfs_read_entry((uint32_t)id, &e) != 0) return E_IO;
    if (e.type == CXFS_TYPE_DIR)                return E_IS_DIR;
    if (e.type != CXFS_TYPE_FILE || e.size == 0) return E_INVALID_ARGS;
    if (e.size > PROC_MAX_IMAGE)                return E_OUT_OF_RANGE;

    /* The broker handle is checked BEFORE the image is read: a bad handle is
       the cheap failure and there is no reason to pull a megabyte off the disk
       to discover it.
     *
     * A NEGATIVE handle means "no broker", and the child simply gets no broker
     * handle. SYS_SPAWN requires one because it was written for the brokered model,
     * where an app holds no capabilities and reaches privilege only by calling
     * its executive. CX_ABI.md section 1 records that this is now a spectrum
     * rather than a rule: attenuation lets a launcher hand a child exactly the
     * authority it needs instead. A shell holds a SEND handle to its own
     * executive, not a RECV endpoint it owns, so requiring a broker here would
     * mean no shell could ever launch anything - it would first have to become
     * a broker and serve the child's IPC itself. */
    struct endpoint *bep = NULL;
    uint32_t bgrant = 0;
    if (a.broker_endpoint >= 0) {
        bep = ep_from_handle(a.broker_endpoint, HRIGHT_RECV, &bgrant);
        if (!bep) return E_BAD_HANDLE;
    }

    uint8_t *buf = (uint8_t *)kmalloc((size_t)e.size);
    if (!buf) { ep_put(bep); return E_NO_MEMORY; }
    if (cxfs_read_file((uint32_t)id, buf, (uint32_t)e.size) != (int)e.size) {
        kfree(buf);
        ep_put(bep);
        return E_IO;
    }

    /* Attenuation, exactly as SYS_SPAWN does it: a subset of your own set. */
    uint32_t granted = (a.caps & thread_current_caps()) | (a.caps & SPAWN_BACKGROUND);

    int32_t ph = 0;
    int rc = cxex_exec_as(buf, (size_t)e.size, granted, bep, bgrant, user_ptr(a.args), (uint32_t)a.args_len, &ph);
    kfree(buf);   /* proc_start took its own copy for the new address space */
    ep_put(bep);

    /* proc_start's own refusals are ABI errors already, below cxex_exec's
       -1..-5: no room for the caller's process handle, no memory. */
    if (rc == E_NO_RESOURCES || rc == E_NO_MEMORY) return rc;
    if (rc < 0) {
        /* cxex_exec_* has its own small error space; the one a caller most
           needs to tell apart is "this image is not signed by a key this
           kernel trusts", which is policy working rather than a broken file. */
        /* Name the path. Without it a refusal in the boot log is anonymous,
           and the self-tests deliberately trigger one - a reader who cannot
           tell the test's refusal from a real one learns to skip warnings. */
        klog("EXEC", SEV_WARN, "exec_path refused the image");
        klog_child(path);
        klog_child(cxk_launch_strerror(rc));
        return (rc == CXEX_EXEC_VERIFY_FAILED) ? E_ACCESS_DENIED : E_INVALID_ARGS;
    }

    klog_u32("EXEC", SEV_OK, "pid ", (uint32_t)rc, LOG_COLOR_VALUE,
             (granted & ~SPAWN_BACKGROUND) ? " from disk (privileged, ring 3)" : " from disk (caps=0, ring 3)");
    return ph;   /* the pid is OBJINFO_PROCESS */
}
