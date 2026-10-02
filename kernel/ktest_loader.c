/* /kernel/ktest_loader.c */
/* Aurora Tejeda / CATX SYSTEMS LLC */
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

static struct cxex_load_ops mock_ops = {
    0, KERNEL_VBASE, mock_get_page, mock_map_page
};

/* Run the loader over the image as currently built and return its result,
   resetting the mapping counters first. */
static int run_load(void) {
    g_maps = 0;
    g_max_virt = 0;
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
    ok = ok && cxex_load(g_img, (size_t)g_img_len, &mock_ops, &entry) == CXEX_LOAD_OK;
    ok = ok && entry == 0x00400000 && g_maps == 1;

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

    kfree(g_img);
    kfree(g_scratch);
    g_img = 0;
    g_scratch = 0;
    return ok;
}

/* ---- user pointer writability -------------------------------------------- */

int ktest_user_ptr_writability(void) {
    /* A page mapped present + user but NOT writable. user_ptr_ok accepted this
       and the kernel's write went through anyway, because ring 0 ignores the
       read-only bit unless CR0.WP is set (security review §4). */
    const uint32_t va = 0x00800000u;

    void *frame = pmm_alloc();
    if (!frame) return 0;

    paging_map(va, (uint32_t)frame, PAGE_PRESENT | PAGE_USER);   /* no PAGE_WRITE */

    int ok = 1;
    ok = ok && user_ptr_readable(va, 64) == 1;
    ok = ok && user_ptr_writable(va, 64) == 0;

    /* The same page, now writable, must pass both - otherwise the check could be
       refusing everything and this test would not notice. */
    paging_map(va, (uint32_t)frame, PAGE_PRESENT | PAGE_USER | PAGE_WRITE);
    ok = ok && user_ptr_readable(va, 64) == 1;
    ok = ok && user_ptr_writable(va, 64) == 1;

    /* A buffer straddling the kernel boundary is refused either way. */
    ok = ok && user_ptr_readable(KERNEL_VBASE - 8, 64) == 0;
    ok = ok && user_ptr_writable(KERNEL_VBASE - 8, 64) == 0;

    paging_unmap(va);
    pmm_free(frame);
    return ok;
}
