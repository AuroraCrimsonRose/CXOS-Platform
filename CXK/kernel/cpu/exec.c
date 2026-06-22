/* /CXK/kernel/cpu/exec.c */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/* cxex_exec: verify -> own address space -> load -> ring 3. See exec.h. */

#include "exec.h"
#include "cxex.h"
#include "cxex_verify.h"
#include "cxex_load.h"
#include "addr_space.h"
#include "pmm.h"
#include "paging.h"

/* ring-3 entry trampoline (cpu/usermode.asm) and the per-thread save slot it
   uses to return here on SYS_EXIT. */
extern int       enter_usermode(uint32_t entry_eip, uint32_t user_esp, uint32_t *save_slot);
extern uint32_t *thread_current_usave(void);

/* user stack: a few pages at the top of the user half (below the kernel base) */
#define USER_STACK_TOP    0xBFFFF000u
#define USER_STACK_PAGES  4u

int cxex_exec(const uint8_t *file, size_t len) {
    /* 1. IDENTITY + INTEGRITY: only run images signed by the trusted key. */
    if (cxex_verify_trusted(file, len) != CXEX_VERIFY_OK)
        return CXEX_EXEC_VERIFY_FAILED;

    /* 2. POLICY (minimal): only OS/USER executables run in ring 3 here. The
       full identity->capability-tier mapping lives in the policy layer; for now
       we just refuse to "run" a kernel/boot image through the ring-3 path. */
    struct cxex_header h;
    if (cxex_parse_header(file, len, &h) != 0) return CXEX_EXEC_LOAD_FAILED;
    if (h.type_code != CXEX_TYPE_OS && h.type_code != CXEX_TYPE_USER)
        return CXEX_EXEC_BAD_TYPE;

    /* 3. give the image its own address space and switch into it (the kernel is
       shared into every space, so our code + stack stay mapped across CR3). */
    struct addr_space space, kspace;
    addr_space_kernel(&kspace);
    if (addr_space_create(&space) != 0) return CXEX_EXEC_NOSPACE;
    addr_space_switch(&space);

    /* 4. place the image's sections into this space. */
    uint32_t entry = 0;
    if (cxex_load(file, len, &cxex_kernel_load_ops, &entry) != CXEX_LOAD_OK) {
        addr_space_switch(&kspace);
        addr_space_destroy(&space);
        return CXEX_EXEC_LOAD_FAILED;
    }

    /* 5. build a ring-3 stack in this space. */
    for (uint32_t i = 0; i < USER_STACK_PAGES; i++) {
        void *p = pmm_alloc();
        if (!p) {
            addr_space_switch(&kspace);
            addr_space_destroy(&space);
            return CXEX_EXEC_NOMEM;
        }
        paging_map(USER_STACK_TOP - (i + 1) * 0x1000u, (uint32_t)p,
                   PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
    }
    uint32_t ustack_top = USER_STACK_TOP - 16u;   /* a little headroom */

    /* 6. drop to ring 3 at the entry point; returns when the image SYS_EXITs.
       (TSS esp0 - the ring-0 stack the CPU uses for the syscall/fault back into
       the kernel - is established by the boot/scheduler setup, same as the
       existing ring-3 path.) */
    int rc = enter_usermode(entry, ustack_top, thread_current_usave());

    /* 7. back in the kernel: restore the kernel space. (Freeing the image's
       frames + page tables is addr_space teardown - not yet implemented; this
       unregisters the space from PDE-sync.) */
    addr_space_switch(&kspace);
    addr_space_destroy(&space);
    return rc;
}