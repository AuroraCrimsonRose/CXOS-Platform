// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/ktest_loader.c */
/* Aurora Tejeda / CATX Systems */
/*
 * Adversarial cases for the CXEX runtime loader, run at every boot.
 *
 * These matter more than the usual self-test. The loader is where an untrusted
 * image first becomes mapped memory, and until Phase 1 it would map whatever
 * virtual address a section named - including kernel addresses, with the USER
 * bit set. "It still boots" does not demonstrate that any of the checks added
 * there actually fire; these do, by handing the loader an image built to break
 * each rule in turn and requiring the matching refusal.
 *
 * Nothing here maps a real page. cxex_load takes its allocator and mapper
 * through cxex_load_ops, so these supply mock ops that hand back one scratch
 * page and record the calls. That is exactly what the indirection is for, and
 * it means a case that SHOULD be refused cannot damage anything if it is not -
 * the test reports the failure instead of corrupting the address space.
 */

#include <stdint.h>
#include <stddef.h>
#include "ktest_loader.h"
#include "cxex.h"
#include "cxex_load.h"
#include "pmm.h"
#include "paging.h"
#include "usermode.h"
#include "heap.h"
#include "cxfs.h"
#include "cxex_verify.h"
#include "spawn.h"
#include "exec.h"
#include "ipc.h"
#include "sched.h"
#include "logging.h"
#include "cxk_abi.h"

#define KERNEL_VBASE 0xC0000000u

/* ---- a CXEX image, built in a buffer ------------------------------------- */

#define IMG_CAP     1024
#define IMG_SECS    2
#define IMG_BODY    (CXEX_HEADER_SIZE + IMG_SECS * CXEX_SECTION_SIZE)

static uint8_t *g_img;          /* heap, not .bss - see ktest_loader_adversarial */
static uint32_t g_img_len;

static void wr16(uint32_t off, uint16_t v) {
    g_img[off] = (uint8_t)v; g_img[off + 1] = (uint8_t)(v >> 8);
}
static void wr32(uint32_t off, uint32_t v) {
    g_img[off] = (uint8_t)v;        g_img[off + 1] = (uint8_t)(v >> 8);
    g_img[off + 2] = (uint8_t)(v >> 16); g_img[off + 3] = (uint8_t)(v >> 24);
}

/* One executable section at 0x00400000 with 64 bytes of content, laid out the
   way the DevKit writer lays one out. Every case below is one mutation of it,
   so a refusal can only be caused by the field the case changed. */
static void img_valid(void) {
    for (uint32_t i = 0; i < IMG_CAP; i++) g_img[i] = 0;

    g_img[0] = CXEX_MAGIC0; g_img[1] = CXEX_MAGIC1;
    g_img[2] = CXEX_MAGIC2; g_img[3] = CXEX_MAGIC3;
    wr16(4, 0x5545);                      /* type .xuex */
    wr16(6, 1);                           /* format version */
    wr16(8, 1);                           /* arch */
    wr16(10, 1);                          /* abi */
    wr32(12, CXEX_FLAG_EXECUTABLE);
    wr32(16, 0x00400000);                 /* entry */
    wr32(20, 0x00400000);                 /* load base */
    wr32(24, 0x00400000);                 /* image min */
    wr32(28, 0x00400040);                 /* image max */
    wr16(32, 1);                          /* section count */
    wr16(34, CXEX_HEADER_SIZE);           /* section offset */
    wr32(40, 0);                          /* signature offset: unsigned */

    uint32_t s = CXEX_HEADER_SIZE;
    g_img[s + 0] = '.'; g_img[s + 1] = 't'; g_img[s + 2] = 'e'; g_img[s + 3] = 'x';
    g_img[s + 4] = 't';
    wr32(s + 8,  IMG_BODY);               /* file offset: after the table */
    wr32(s + 12, 0x00400000);             /* virt addr */
    wr32(s + 16, 64);                     /* file size */
    wr32(s + 20, 64);                     /* mem size */
    wr32(s + 24, CXEX_SEC_READ | CXEX_SEC_EXEC);

    for (uint32_t i = 0; i < 64; i++) g_img[IMG_BODY + i] = (uint8_t)(0x90 + (i & 7));
    g_img_len = IMG_BODY + 64;
}

/* field offsets into section 0, for the mutations */
#define SEC0        ((uint32_t)CXEX_HEADER_SIZE)
#define SEC0_FOFF   (SEC0 + 8)
#define SEC0_VADDR  (SEC0 + 12)
#define SEC0_FSIZE  (SEC0 + 16)
#define SEC0_MSIZE  (SEC0 + 20)
#define SEC0_FLAGS  (SEC0 + 24)

/* ---- mock ops ------------------------------------------------------------ */

static uint8_t *g_scratch;      /* heap, not .bss */
static uint32_t g_maps;          /* how many pages were mapped */
static uint32_t g_max_virt;      /* the highest virtual address mapped */

static void *mock_get_page(void *ctx, uint32_t *out_phys) {
    (void)ctx;
    *out_phys = 0x1000;                  /* never used: nothing is really mapped */
    for (uint32_t i = 0; i < 4096; i++) g_scratch[i] = 0;
    return g_scratch;
}

static int mock_map_page(void *ctx, uint32_t virt, uint32_t phys, uint32_t prot) {
    (void)ctx; (void)phys; (void)prot;
    g_maps++;
    if (virt > g_max_virt) g_max_virt = virt;
    return 0;
}

static uint32_t g_charged;       /* pages the loader asked to charge */
static uint32_t g_charge_maps;   /* g_maps at the moment it asked */
static int      g_charge_deny;   /* make the charge fail, as a full quota would */

/* Stands in for the quota. What it records is the part that is easy to get wrong
   and impossible to see from the outside: the loader must ask for the WHOLE bill
   before it has allocated anything, so g_charge_maps has to be 0. */
static int mock_charge(void *ctx, uint32_t pages) {
    (void)ctx;
    g_charged     = pages;
    g_charge_maps = g_maps;
    return g_charge_deny ? -1 : 0;
}

static struct cxex_load_ops mock_ops = {
    0, KERNEL_VBASE, mock_get_page, mock_map_page, mock_charge
};

/* Run the loader over the image as currently built and return its result,
   resetting the mapping counters first. */
static int run_load(void) {
    g_maps = 0;
    g_max_virt = 0;
    g_charged = 0;
    g_charge_maps = 0xFFFFFFFFu;         /* "never asked", distinct from "asked at 0" */
    uint32_t entry = 0;
    return cxex_load(g_img, (size_t)g_img_len, &mock_ops, &entry);
}

/* Expect a specific refusal, and - just as important - expect NOTHING to have
   been mapped. A loader that refuses after mapping half the image has still
   put the attacker's pages in the address space. */
static int refuses(int want) {
    int rc = run_load();
    return rc == want && g_maps == 0;
}

int ktest_loader_adversarial(void) {
    int ok = 1;

    /* Heap rather than .bss, which keeps 5 KB out of the kernel image. That is
       no longer load-bearing - stage 2's budget is 512 KB now and the build
       refuses to exceed it - but a self-test has no business spending image
       space it can borrow at run time instead. */
    g_img = (uint8_t *)kmalloc(IMG_CAP);
    g_scratch = (uint8_t *)kmalloc(4096);
    if (!g_img || !g_scratch) { if (g_img) kfree(g_img); if (g_scratch) kfree(g_scratch); return 0; }

    /* The control. Without it every case below could pass by the loader
       refusing everything, which would prove nothing at all. */
    img_valid();
    uint32_t entry = 0;
    g_maps = 0; g_max_virt = 0;
    g_charged = 0; g_charge_maps = 0xFFFFFFFFu; g_charge_deny = 0;
    ok = ok && cxex_load(g_img, (size_t)g_img_len, &mock_ops, &entry) == CXEX_LOAD_OK;
    ok = ok && entry == 0x00400000 && g_maps == 1;
    /* The one-page image was charged as one page, and charged while nothing had
       been mapped yet. A charge that arrived after the pages did would be a
       count with no power to refuse anything. */
    ok = ok && g_charged == 1 && g_charge_maps == 0;

    /* A section naming a kernel address. This is the Critical finding: it used
       to be mapped, with PAGE_USER, handing ring 3 the kernel. */
    img_valid();
    wr32(SEC0_VADDR, KERNEL_VBASE);
    ok = ok && refuses(CXEX_LOAD_BAD_RANGE);

    /* Just below the limit, but long enough to cross it. */
    img_valid();
    wr32(SEC0_VADDR, KERNEL_VBASE - 0x1000);
    wr32(SEC0_MSIZE, 0x4000);
    ok = ok && refuses(CXEX_LOAD_BAD_RANGE);

    /* virt_addr + mem_size wraps past the top of the address space. Computed in
       32 bits this lands back at a small address and looks like the user half. */
    img_valid();
    wr32(SEC0_VADDR, 0xFFFFF000u);
    wr32(SEC0_MSIZE, 0x8000);
    ok = ok && refuses(CXEX_LOAD_BAD_RANGE);

    /* file_offset + file_size past the end of the image, by wrapping. */
    img_valid();
    wr32(SEC0_FOFF, 0xFFFFFFF0u);
    ok = ok && refuses(CXEX_LOAD_OOB);

    /* ...and without wrapping. */
    img_valid();
    wr32(SEC0_FSIZE, 0x100000);
    wr32(SEC0_MSIZE, 0x100000);
    ok = ok && refuses(CXEX_LOAD_OOB);

    /* More file bytes than the section occupies in memory. */
    img_valid();
    wr32(SEC0_MSIZE, 32);                 /* file_size is still 64 */
    ok = ok && refuses(CXEX_LOAD_OOB);

    /* Writable AND executable. On x86 without PAE a writable page is executable
       regardless, so refusing the pair at load is the only enforcement there is. */
    img_valid();
    wr32(SEC0_FLAGS, CXEX_SEC_READ | CXEX_SEC_WRITE | CXEX_SEC_EXEC);
    ok = ok && refuses(CXEX_LOAD_WX);

    /* An image claiming far more memory than any program needs, which would
       otherwise drain the PMM one frame at a time before anything noticed. */
    img_valid();
    wr32(SEC0_MSIZE, 0x8000000);          /* 128 MB, past CXEX_LOAD_MAX_PAGES */
    ok = ok && refuses(CXEX_LOAD_TOO_BIG);

    /* An image that fits the global page cap but not the owner's quota. The cap
       is 64 MB and a quota is measured in a few; without this charge an image of
       any size under the cap was placed whatever the process's ceiling said. */
    img_valid();
    g_charge_deny = 1;
    ok = ok && refuses(CXEX_LOAD_QUOTA);
    g_charge_deny = 0;

    /* Section bytes reaching past signature_offset: data the signature does not
       cover, which the loader would map anyway. */
    img_valid();
    wr32(40, IMG_BODY + 8);               /* signature starts inside the section */
    wr32(12, CXEX_FLAG_EXECUTABLE | CXEX_FLAG_SIGNED);
    ok = ok && refuses(CXEX_LOAD_UNSIGNED);

    /* An entry point outside the user half, even with every section in it. */
    img_valid();
    wr32(16, KERNEL_VBASE + 0x1000);
    ok = ok && refuses(CXEX_LOAD_BAD_RANGE);

    /* A caller that forgets va_limit must fail loudly rather than lose the
       check: this is the one mistake that silently disables everything above. */
    img_valid();
    {
        struct cxex_load_ops no_limit = mock_ops;
        no_limit.va_limit = 0;
        g_maps = 0;
        uint32_t e2 = 0;
        ok = ok && cxex_load(g_img, (size_t)g_img_len, &no_limit, &e2) == CXEX_LOAD_BAD_FORMAT;
        ok = ok && g_maps == 0;
    }

    /* ---- two sections sharing a page (2026-10-09 review §3) ----
     *
     * check_section validates one section alone, and until this landed nothing
     * compared two. Pass 2 allocates a fresh page per page with the start
     * rounded DOWN, so overlapping sections each allocate and map one: the
     * second replaces the first, the first's bytes vanish, its frame leaks,
     * and the page's protection becomes whichever section was mapped last -
     * W^X decided by section ordering rather than by the rule.
     *
     * The first case is the one that matters, and it is the one byte-granular
     * overlap checking would MISS: two sections that do not overlap in bytes
     * at all, but land in the same 4 KiB page. That is the exact shape that
     * broke the executive in Phase 1, when .rodata shared a page with .text.
     */
    {
        /* .text at 0x400000+0x100 (RX), .data at 0x400200+0x100 (RW).
           Disjoint in bytes, same page. */
        img_valid();
        wr16(32, 2);                                   /* two sections */
        /* mem_size only: the body is 64 bytes, so raising file_size would trip
           the OOB check first and this would be testing that instead. */
        wr32(SEC0_MSIZE, 0x100);

        uint32_t s1 = SEC0 + CXEX_SECTION_SIZE;
        g_img[s1 + 0] = '.'; g_img[s1 + 1] = 'd'; g_img[s1 + 2] = 'a';
        g_img[s1 + 3] = 't'; g_img[s1 + 4] = 'a';
        wr32(s1 + 8,  IMG_BODY);                       /* share the file bytes; irrelevant here */
        wr32(s1 + 12, 0x00400200);                     /* same page as section 0 */
        wr32(s1 + 16, 0x40);
        wr32(s1 + 20, 0x40);
        wr32(s1 + 24, CXEX_SEC_READ | CXEX_SEC_WRITE);
        ok = ok && refuses(CXEX_LOAD_OVERLAP);

        /* Byte-for-byte the same address: overlap at its most obvious. */
        img_valid();
        wr16(32, 2);
        s1 = SEC0 + CXEX_SECTION_SIZE;
        for (int k = 0; k < (int)CXEX_SECTION_SIZE; k++) g_img[s1 + k] = g_img[SEC0 + k];
        wr32(s1 + 24, CXEX_SEC_READ | CXEX_SEC_WRITE);
        ok = ok && refuses(CXEX_LOAD_OVERLAP);

        /* A section whose tail runs into the next section's page. */
        img_valid();
        wr16(32, 2);
        wr32(SEC0_MSIZE, 0x1800);                      /* spills into the second page */
        s1 = SEC0 + CXEX_SECTION_SIZE;
        for (int k = 0; k < (int)CXEX_SECTION_SIZE; k++) g_img[s1 + k] = g_img[SEC0 + k];
        wr32(s1 + 12, 0x00401000);                     /* the page section 0 spills into */
        wr32(s1 + 16, 0x40);
        wr32(s1 + 20, 0x40);
        ok = ok && refuses(CXEX_LOAD_OVERLAP);

        /* THE CONTROL, and the one that stops this being a check that refuses
           every multi-section image: two sections one page apart are fine, and
           both are mapped. Without this the three refusals above would pass on
           a loader that rejected any image with two sections. */
        img_valid();
        wr16(32, 2);
        wr32(SEC0_MSIZE, 0x40);
        wr32(SEC0_FSIZE, 0x40);
        s1 = SEC0 + CXEX_SECTION_SIZE;
        for (int k = 0; k < (int)CXEX_SECTION_SIZE; k++) g_img[s1 + k] = g_img[SEC0 + k];
        wr32(s1 + 12, 0x00401000);                     /* the next page along */
        wr32(s1 + 24, CXEX_SEC_READ | CXEX_SEC_WRITE);
        {
            uint32_t e3 = 0;
            g_maps = 0; g_charged = 0; g_charge_maps = 0xFFFFFFFFu; g_charge_deny = 0;
            ok = ok && cxex_load(g_img, (size_t)g_img_len, &mock_ops, &e3) == CXEX_LOAD_OK;
            ok = ok && g_maps == 2;                    /* both placed, not merged */
        }

        /* More sections than the loader will consider. The kernel had no cap
           at all where the DevKit has had one since Phase 1. */
        img_valid();
        wr16(32, (uint16_t)(CXEX_LOAD_MAX_SECTIONS + 1));
        ok = ok && refuses(CXEX_LOAD_TOO_MANY);

        /* Exactly at the cap is not refused BY the cap - it fails later, on the
           section table running off the end of the image, which is a different
           answer and the right one. The cap must be an upper bound, not an
           off-by-one that rejects the largest legal count. */
        img_valid();
        wr16(32, (uint16_t)CXEX_LOAD_MAX_SECTIONS);
        {
            int rc = run_load();
            ok = ok && rc != CXEX_LOAD_TOO_MANY && rc != CXEX_LOAD_OK && g_maps == 0;
        }
    }

    kfree(g_img);
    kfree(g_scratch);
    g_img = 0;
    g_scratch = 0;
    return ok;
}

/* ---- user pointer writability -------------------------------------------- */

int ktest_user_ptr_writability(void) {
    /* A page mapped present + user but NOT writable. The old single user_ptr_ok
       accepted this, and the kernel's write went through anyway, because ring 0
       ignored the read-only bit while CR0.WP was clear (security review §4).
       WP is set now, so that write would also fault - but this test is about the
       checks themselves, and asserts on what they answer, not on what the
       hardware would do with the result. */
    const uint32_t va = 0x00800000u;

    void *frame = pmm_alloc();
    if (!frame) return 0;

    if (paging_map_user(va, (uint32_t)frame, PAGE_PRESENT) != 0) { pmm_free(frame); return 0; }   /* no PAGE_WRITE */

    int ok = 1;
    ok = ok && user_ptr_readable(va, 64) == 1;
    ok = ok && user_ptr_writable(va, 64) == 0;

    /* The same page, now writable, must pass both - otherwise the check could be
       refusing everything and this test would not notice. */
    if (paging_map_user(va, (uint32_t)frame, PAGE_PRESENT | PAGE_WRITE) != 0) { pmm_free(frame); return 0; }
    ok = ok && user_ptr_readable(va, 64) == 1;
    ok = ok && user_ptr_writable(va, 64) == 1;

    /* A buffer straddling the kernel boundary is refused either way. */
    ok = ok && user_ptr_readable(KERNEL_VBASE - 8, 64) == 0;
    ok = ok && user_ptr_writable(KERNEL_VBASE - 8, 64) == 0;

    paging_unmap(va);
    pmm_free(frame);
    return ok;
}

/* ====================================================================
 * SYS_SPAWN verifies the image it is handed
 *
 * The 2026-10-09 review's §1 finding (GHSA-2c4w-cgcw-9732) was that sys_spawn
 * called proc_start directly and did no signature check at all, so GRANT_SPAWN
 * was the only thing between a process and unsigned ring-3 code. The fix
 * copies the image into the kernel and routes the copy through cxex_exec_as.
 *
 * Nothing pinned that. exec_admit has a test, and it covers the POLICY in
 * isolation - which verdict maps to which trust level - but it says nothing
 * about whether sys_spawn consults it. Replacing the cxex_exec_as call with
 * the pre-fix proc_start call left all 39 self-tests green, which is exactly
 * the regression this file exists to prevent elsewhere.
 *
 * So the assertion is made THROUGH the syscall, with real user pointers, and
 * on a signed image with one byte of its signed payload flipped. A tampered
 * signature is refused in every build configuration, unlike an unsigned image,
 * which a --dev build admits deliberately - so this does not quietly become
 * vacuous in the configuration that ships.
 *
 * The positive direction is not asserted here, because a valid image would
 * actually start a process. It is covered at boot instead: the executive
 * spawns the shell through this same syscall, so a sys_spawn that refused
 * valid signed images would never reach "boot complete".
 * ==================================================================== */

#define SPV_ARGS_VA   0x00900000u             /* user page for the spawn_args */
#define SPV_IMG_VA    (SPV_ARGS_VA + 0x1000u) /* user pages for the image */
#define SPV_MAX_PAGES 16                       /* 64KB of image is plenty for hi.xuex */

int ktest_spawn_verifies_image(void) {
    if (!cxfs_is_mounted()) return 1;                 /* nothing mounted - skip */

    int id = cxfs_resolve("/Shared/Programs/hi.xuex", 0);
    if (id < 0) return 1;                             /* nothing staged - skip */

    struct cxfs_entry e;
    if (cxfs_read_entry((uint32_t)id, &e) != 0) return 0;
    if (e.size == 0) return 0;
    if (e.size > (uint64_t)SPV_MAX_PAGES * PAGE_SIZE) {
        klog("KTEST", SEV_WARN,
             "spawn verification untested: staged image larger than the test's window");
        return 1;
    }

    uint32_t pages = ((uint32_t)e.size + PAGE_SIZE - 1) / PAGE_SIZE;

    /* One page for the args, then as many as the image needs. The frames do not
       have to be contiguous; the mapping makes them look it. */
    void *frames[SPV_MAX_PAGES + 1];
    uint32_t got = 0;
    for (uint32_t i = 0; i < pages + 1; i++) {
        frames[i] = pmm_alloc();
        if (!frames[i]) break;
        if (paging_map_user(SPV_ARGS_VA + i * PAGE_SIZE, (uint32_t)frames[i],
                            PAGE_PRESENT | PAGE_WRITE) != 0) {
            pmm_free(frames[i]);
            break;
        }
        got++;
    }
    int ok = 0;
    int h  = -1;
    if (got != pages + 1) goto done;

    uint8_t *img = (uint8_t *)SPV_IMG_VA;
    if (cxfs_read_file((uint32_t)id, img, (uint32_t)e.size) != (int)e.size) goto done;

    /* An unsigned build stages an unsigned image, and admits it on purpose.
       Tampering one would still be admitted, so there is nothing to assert -
       say so rather than passing silently. */
    if (cxex_verify_self(img, (size_t)e.size) == CXEX_VERIFY_UNSIGNED) {
        klog("KTEST", SEV_WARN,
             "spawn verification untested: staged image is unsigned (dev build)");
        ok = 1;
        goto done;
    }
    if (cxex_verify_self(img, (size_t)e.size) != CXEX_VERIFY_OK) goto done;

    h = ep_create();                       /* sys_spawn needs a RECV broker handle */
    if (h < 0) goto done;

    struct spawn_args *a = (struct spawn_args *)SPV_ARGS_VA;
    a->image           = img;
    a->image_len       = (uint32_t)e.size;
    a->name            = 0;
    a->broker_endpoint = h;
    a->caps            = 0;                /* attenuated to nothing: caps are not the point */
    a->args            = 0;
    a->args_len        = 0;

    /* ---- the controls ----
       Distinct codes for distinct causes, which is what stops this suite from
       passing on a sys_spawn that refuses everything with one error. They also
       prove the call plumbing reaches past the early gates, so the refusal
       below is the signature check and not a malformed request. */
    a->image_len = 0;
    ok = (sys_spawn(a) == E_RANGE);
    a->image_len = (uint32_t)e.size;
    if (!ok) goto done;

    a->broker_endpoint = 9999;             /* no such handle */
    ok = (sys_spawn(a) == E_BADF);
    a->broker_endpoint = h;
    if (!ok) goto done;

    a->image = (const void *)KERNEL_VBASE; /* a kernel address is not a user pointer */
    ok = (sys_spawn(a) == E_FAULT);
    a->image = img;
    if (!ok) goto done;

    /* ---- the finding itself ----
       One byte of the signed payload flipped. Before the fix this spawned. */
    img[16] ^= 0xFF;
    int rc = sys_spawn(a);
    img[16] ^= 0xFF;
    ok = (rc == CXEX_EXEC_VERIFY_FAILED);

done:
    if (h >= 0) thread_handle_close(thread_current_id(), h);
    for (uint32_t i = 0; i < got; i++) {
        paging_unmap(SPV_ARGS_VA + i * PAGE_SIZE);
        pmm_free(frames[i]);
    }
    return ok;
}
