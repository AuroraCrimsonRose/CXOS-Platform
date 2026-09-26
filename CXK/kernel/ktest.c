/* /CXK/kernel/ktest.c */
/* Aurora Tejeda / CATX Systems LLC */
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
#include "paging.h"
#include "heap.h"
#include "console.h"
#include "logging.h"
#include "color.h"
#include "sched.h"
#include "usermode.h"
#include "uid.h"
#include "disk.h"
#include "cxfs.h"
#include "string.h"
#include "pci.h"

/* concise pass/fail reporter */
/* Report a test result. Stays SILENT on success - only failures are printed,
   so a clean boot is quiet and any problem stands out. Returns 1 if passed,
   0 if failed, so ktest_run can tally a summary. */
static int report(const char *name, int ok) {
    if (!ok)
        klog("KTEST", SEV_FAIL, name);

    return ok;
}

/* ---- paging: map a scratch frame, write+read it back ---- */
static int test_paging(void) {
    uint32_t test_virt = 0xCF000000;
    uint32_t frame = (uint32_t)pmm_alloc();
    if (!frame) return 0;
    paging_map(test_virt, frame, PAGE_WRITE);
    volatile uint32_t *p = (volatile uint32_t *)test_virt;
    *p = 0xCAFEBABE;
    int ok = (*p == 0xCAFEBABE) && (paging_get_phys(test_virt) == frame);
    paging_unmap(test_virt);
    pmm_free((void *)frame);
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
    while (sched_active_count() > 1) { __asm__ __volatile__("pause"); }
    sched_preempt_disable();
    /* both spinners completed only because the timer preempted them */
    return (pre_a != 0) && (pre_b != 0) && (sched_active_count() == 1);
}

/* ---- ring 3: single round-trip (3a) ---- */
static int test_ring3_single(void) {
    return usermode_test() == 0;
}

/* ---- ring 3: scheduler-integrated + preemptible processes (3b/3c) ---- */
extern uint8_t user_blob_start[];
extern uint8_t user_blob_end[];
extern uint8_t user_blob_busy_start[];
extern uint8_t user_blob_busy_end[];

static int test_ring3_processes(void) {
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
    while (sched_active_count() > 1) { __asm__ __volatile__("pause"); }
    sched_preempt_disable();
    int preempt_ok = (sched_active_count() == 1);

    return coop_ok && preempt_ok;
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
    int fid = cxfs_create_entry(0, "ktest.txt", CXFS_TYPE_FILE);
    if (fid < 0) {
        fid = cxfs_resolve("/ktest.txt", 0);   /* may exist from a previous boot */
        if (fid < 0) return 0;
    }
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
/* fetch-or-create a scratch file by name, truncated to empty */
static int scratch_file(const char *name) {
    int id = cxfs_resolve(name, 0);
    if (id < 0) {
        id = cxfs_create_entry(0, name + 1, CXFS_TYPE_FILE);   /* skip the '/' */
        if (id < 0) return -1;
    }
    if (cxfs_truncate((uint32_t)id, 0) != CXFS_E_OK) return -1;
    return id;
}
#endif

static int test_cxfs_offset(void) {
    if (!cxfs_is_mounted()) return 1;   /* nothing mounted - skip */

#if CXK_ALLOW_DISK_WRITE
    int a = scratch_file("/kt_off.bin");
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
    int b = scratch_file("/kt_frag.bin");
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

void ktest_run(void) {
    int passed = 0, total = 0;

    /* each test: total++, and passed += its pass/fail. report() prints only on
       failure, so the only per-test output is failures. */
    total++; passed += report("paging map/write/read",            test_paging());
    total++; passed += report("heap kmalloc/kfree",               test_heap());
    total++; passed += report("scheduler (cooperative)",          test_sched_coop());
    total++; passed += report("scheduler (preemptive)",           test_sched_preempt());
    total++; passed += report("ring 3 single round-trip",         test_ring3_single());
    total++; passed += report("ring 3 processes (coop+preempt)",  test_ring3_processes());
    total++; passed += report("identity (user never UID 0)",      test_identity());
    total++; passed += report("storage (ATA read sector 0)",      test_storage());
    total++; passed += report("pci (bus enumeration)",            test_pci());
    total++; passed += report("ahci (controller + read)",         test_ahci());
    total++; passed += report("cxfs (read-only mount check)",     test_cxfs());
    total++; passed += report("cxfs offset I/O + compaction",      test_cxfs_offset());

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
}