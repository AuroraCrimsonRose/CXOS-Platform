/* /CXK/kernel/cpu/spawn.c */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/*
 * ABI v1 process launch. Every ring-3 process - the executive AND its apps -
 * is a normal scheduler thread with its own address space, kernel stack (esp0),
 * and capabilities. proc_start() is the one path that creates them; cxex_exec()
 * (the executive, kernel-launched, trusted, GRANT_OS_BASELINE, no broker) and
 * sys_spawn() (apps, executive-launched, caps=0, broker handle 0) both call it.
 * There is no special "thread 0 runs the executive" case anymore.
 */

#include "spawn.h"
#include "sched.h"
#include "handle.h"
#include "ipc.h"
#include "caps.h"
#include "usermode.h"          /* user_ptr_ok */
#include "addr_space.h"
#include "paging.h"
#include "pmm.h"
#include "heap.h"
#include "cxex_load.h"
#include "exec.h"
#include "launch.h"
#include "cxfs.h"
#include "logging.h"
#include <stddef.h>

#define USER_STACK_TOP    0xBFFFF000u
#define USER_STACK_PAGES  4u
#define PROC_MAX_IMAGE    (1u << 20)   /* 1 MiB cap on an image */

/* per-pid record handed from proc_start to the trampoline (which runs later, on
   the new thread, in that thread's own address space). */
struct proc_rec {
    const void       *image;     /* kernel-heap copy of the CXEX bytes */
    uint32_t          image_len;
    const char       *args;      /* kernel-heap copy of the argument blob, or NULL */
    uint32_t          args_len;
    struct addr_space space;
};
static struct proc_rec proc_recs[MAX_THREADS];

extern const struct cxex_load_ops cxex_kernel_load_ops;   /* cxex_loadk.c */
extern int enter_usermode(uint32_t entry_eip, uint32_t user_esp, uint32_t *save_slot);

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
    void *frame = pmm_alloc();
    if (!frame) return -1;
    paging_map(USER_ARGS_BASE, (uint32_t)frame,
               PAGE_PRESENT | PAGE_WRITE | PAGE_USER);

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

/* Runs on the new thread, in its own address space (the scheduler loaded its CR3
   before switching here). Places the image, builds a ring-3 stack, drops to user
   mode. Returns only when the process SYS_EXITs. */
static void proc_trampoline(void) {
    int pid = thread_current_id();
    struct proc_rec *r = &proc_recs[pid];

    uint32_t entry = 0;
    if (cxex_load(r->image, r->image_len, &cxex_kernel_load_ops, &entry) != CXEX_LOAD_OK) {
        klog("PROC", SEV_ERR, "image load failed");
        kfree((void *)r->image); r->image = NULL;
        thread_exit();
    }
    for (uint32_t i = 0; i < USER_STACK_PAGES; i++) {
        void *p = pmm_alloc();
        if (!p) { klog("PROC", SEV_ERR, "no stack memory"); kfree((void *)r->image); thread_exit(); }
        paging_map(USER_STACK_TOP - (i + 1) * 0x1000u, (uint32_t)p,
                   PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
    }
    uint32_t ustack_top = USER_STACK_TOP - 16u;

    /* After the stack, so a failure here cannot leave a half-built process
       holding stack pages it will never use. */
    if (build_args_page(r->args, r->args_len) != 0) {
        klog("PROC", SEV_ERR, "no memory for the argument page");
        kfree((void *)r->image); r->image = NULL;
        if (r->args) { kfree((void *)r->args); r->args = NULL; }
        thread_exit();
    }

    kfree((void *)r->image); r->image = NULL;   /* image placed; heap copy done */
    if (r->args) { kfree((void *)r->args); r->args = NULL; }   /* and copied in */

    enter_usermode(entry, ustack_top, thread_current_usave());

    /* process exited: reclaim its address space so spawning doesn't leak. Free
       the user frames/PTs while still in this space, switch to the kernel space,
       then free the page directory. */
    struct addr_space self = r->space;
    addr_space_reclaim_user();
    struct addr_space kspace;
    addr_space_kernel(&kspace);
    addr_space_switch(&kspace);
    thread_set_space(pid, 0);          /* stop referencing the freed space */
    addr_space_destroy(&self);         /* free the PD frame + unregister */
    klog_u32("PROC", SEV_INFO, "exited + reclaimed; free frames: ", pmm_free_count(), LOG_COLOR_VALUE, "");

    thread_exit();   /* never returns; sched_reap frees the kernel + esp0 stacks */
}

/* Create a ring-3 scheduler thread running `image` (which must be readable in
   the CURRENT address space). caps = its capability set; broker = a SEND handle
   to install as handle 0 (or NULL for a root executive). Returns pid or -E_*. */
int proc_start(const void *image, uint32_t image_len, uint32_t caps,
               struct endpoint *broker, const char *args, uint32_t args_len) {
    if (image_len == 0 || image_len > PROC_MAX_IMAGE) return E_RANGE;
    if (args_len > USER_ARGS_MAX)                     return E_RANGE;
    if (!args) args_len = 0;

    uint8_t *kimg = (uint8_t *)kmalloc(image_len);
    if (!kimg) return E_NOMEM;
    for (uint32_t i = 0; i < image_len; i++)
        kimg[i] = ((const uint8_t *)image)[i];          /* copy from current space */

    /* The blob is read here, in the CALLER's space, and written much later in
       the child's - proc_trampoline runs on the new thread, after the CR3
       switch, where the caller's pages are gone. So it gets a kernel-heap copy
       of its own, exactly as the image does. */
    char *kargs = 0;
    if (args_len) {
        kargs = (char *)kmalloc(args_len);
        if (!kargs) { kfree(kimg); return E_NOMEM; }
        for (uint32_t i = 0; i < args_len; i++) kargs[i] = args[i];
    }

    struct addr_space space;
    if (addr_space_create(&space) != 0) { kfree(kimg); kfree(kargs); return E_NOMEM; }

    int pid = thread_create("proc", proc_trampoline);
    if (pid < 0)                      { kfree(kimg); kfree(kargs); addr_space_destroy(&space); return E_NOMEM; }
    if (thread_alloc_kstack(pid) < 0) { kfree(kimg); kfree(kargs); addr_space_destroy(&space); return E_NOMEM; }

    proc_recs[pid].image     = kimg;
    proc_recs[pid].image_len = image_len;
    proc_recs[pid].args      = kargs;
    proc_recs[pid].args_len  = args_len;
    proc_recs[pid].space     = space;

    thread_mark_user(pid);
    thread_set_space(pid, space.pd_phys);   /* scheduler loads this CR3 for it */
    thread_set_caps(pid, caps);

    if (broker) {
        int bh = thread_handle_install(pid, HANDLE_ENDPOINT, HRIGHT_SEND, broker);
        if (bh != 0) klog_u32("PROC", SEV_WARN, "broker handle not 0: ", (uint32_t)bh, LOG_COLOR_VALUE, "");
    }
    return pid;
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
    if (!a->args) return a->args_len ? E_INVAL : E_OK;   /* length without a blob */
    if (a->args_len == 0 || a->args_len > USER_ARGS_MAX) return E_RANGE;
    if (!user_ptr_ok((uint32_t)a->args, a->args_len))    return E_FAULT;
    if (a->args[a->args_len - 1] != '\0')                return E_INVAL;
    return E_OK;
}

/* SYS_SPAWN: an executive (GRANT_SPAWN) launches a capability-less app, brokered
   through one of its endpoints. */
int sys_spawn(const struct spawn_args *ua) {
    if (!user_ptr_ok((uint32_t)ua, sizeof *ua)) return E_FAULT;
    struct spawn_args a = *ua;

    if (a.image_len == 0 || a.image_len > PROC_MAX_IMAGE) return E_RANGE;
    if (!user_ptr_ok((uint32_t)a.image, a.image_len))     return E_FAULT;

    int arc = check_args(&a);
    if (arc != E_OK) return arc;

    struct endpoint *bep = ep_from_handle(a.broker_endpoint, HRIGHT_RECV);
    if (!bep) return E_BADF;

    /* Attenuation: child gets requested caps masked by the spawner's own caps.
       Pass a subset of your authority, never amplify. App-spawner (caps=0) -> 0. */
    uint32_t granted = a.caps & thread_current_caps();

    int pid = proc_start(a.image, a.image_len, granted, bep, a.args, a.args_len);
    if (pid >= 0)
        klog_u32("SPAWN", SEV_OK, "pid ", (uint32_t)pid, LOG_COLOR_VALUE,
                 granted ? " (privileged, ring 3)" : " (caps=0, ring 3)");
    return pid;
}
/* ---- SYS_EXEC_PATH ---------------------------------------------------------
 * Run a program straight off the disk.
 *
 * The reason this is a syscall of its own, and not a shell that reads a file
 * and calls SYS_SPAWN with the bytes: SYS_SPAWN takes an image ALREADY in the
 * caller's memory and does not verify it - only the kernel's own load path
 * calls cxex_verify_trusted. That was harmless while ring 3 had no filesystem,
 * because the only images that could reach SYS_SPAWN came in through a trusted
 * build. SYS_FILE_OP ended that: a process can now read arbitrary bytes off a
 * disk. Handing those to SYS_SPAWN would be a way to run code the trusted key
 * never signed.
 *
 * So the kernel reads the file itself. The bytes that are verified are exactly
 * the bytes that are loaded - there is no window in which the caller could
 * swap them, because the caller never holds them. CX_ABI.md section 7.10 calls
 * this closing the gap "by construction", which is the better fix than adding
 * a verify to SYS_SPAWN and hoping every future caller goes through it.
 */
int sys_exec_path(const char *upath, const struct spawn_args *ua) {
    if (!user_ptr_ok((uint32_t)ua, sizeof *ua)) return E_FAULT;
    struct spawn_args a = *ua;

    int arc = check_args(&a);
    if (arc != E_OK) return arc;

    /* copy the path in a page at a time; user_ptr_ok walks page tables, so
       per-byte would be a page-table walk per character */
    char path[FILE_PATH_MAX];
    uint32_t addr = (uint32_t)upath;
    if (!addr) return E_FAULT;
    uint32_t n = 0;
    for (;;) {
        if (n >= sizeof path) return E_RANGE;
        if (n == 0 || (((addr + n) & 0xFFFu) == 0)) {
            if (!user_ptr_ok(addr + n, 1)) return E_FAULT;
        }
        path[n] = ((const char *)addr)[n];
        if (!path[n]) break;
        n++;
    }

    if (!cxfs_is_mounted()) return E_IO;

    /* Resolve against the caller's working directory, so `exec doc.xcex`
       means what it says from wherever the process happens to be. */
    int id = cxfs_resolve(path, thread_current_cwd());
    if (id < 0) return E_NOENT;

    struct cxfs_entry e;
    if (cxfs_read_entry((uint32_t)id, &e) != 0) return E_IO;
    if (e.type == CXFS_TYPE_DIR)                return E_ISDIR;
    if (e.type != CXFS_TYPE_FILE || e.size == 0) return E_INVAL;
    if (e.size > PROC_MAX_IMAGE)                return E_RANGE;

    /* The broker handle is checked BEFORE the image is read: a bad handle is
       the cheap failure and there is no reason to pull a megabyte off the disk
       to discover it.
     *
     * A NEGATIVE handle means "no broker", and the child simply gets no handle
     * 0. SYS_SPAWN requires one because it was written for the brokered model,
     * where an app holds no capabilities and reaches privilege only by calling
     * its executive. CX_ABI.md section 1 records that this is now a spectrum
     * rather than a rule: attenuation lets a launcher hand a child exactly the
     * authority it needs instead. A shell holds a SEND handle to its own
     * executive, not a RECV endpoint it owns, so requiring a broker here would
     * mean no shell could ever launch anything - it would first have to become
     * a broker and serve the child's IPC itself. */
    struct endpoint *bep = NULL;
    if (a.broker_endpoint >= 0) {
        bep = ep_from_handle(a.broker_endpoint, HRIGHT_RECV);
        if (!bep) return E_BADF;
    }

    uint8_t *buf = (uint8_t *)kmalloc((size_t)e.size);
    if (!buf) return E_NOMEM;
    if (cxfs_read_file((uint32_t)id, buf, (uint32_t)e.size) != (int)e.size) {
        kfree(buf);
        return E_IO;
    }

    /* Attenuation, exactly as SYS_SPAWN does it: a subset of your own set. */
    uint32_t granted = a.caps & thread_current_caps();

    int rc = cxex_exec_as(buf, (size_t)e.size, granted, bep, a.args, a.args_len);
    kfree(buf);   /* proc_start took its own copy for the new address space */

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
        return (rc == CXEX_EXEC_VERIFY_FAILED) ? E_PERM : E_INVAL;
    }

    klog_u32("EXEC", SEV_OK, "pid ", (uint32_t)rc, LOG_COLOR_VALUE,
             granted ? " from disk (privileged, ring 3)" : " from disk (caps=0, ring 3)");
    return rc;
}
