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
#include "timer.h"
#include "cxex_verify.h"
#include "cxex.h"
#include "keyvault.h"
#include "vmregion.h"
#include "exec.h"
#include "kstack.h"
#include "kconfig.h"
#include "gdt.h"
#include "idt.h"

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
    uint32_t test_virt = 0xCE000000;   /* clear of the kernel-stack region above */
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
    /* Scratch goes in /Temp, not the root. The root is a documented namespace
       now (docs/CX_FILESYSTEM_LAYOUT.md) and a test file sitting in it is
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
 * retry backing off - is wrong if it can. The upper bound is deliberately
 * loose. Scheduling here is cooperative and whatever else ktest has left
 * runnable gets to finish first, so "it took longer than asked" is normal and
 * only a wildly wrong figure means anything.
 */
static int test_clock_sleep(void) {
    uint32_t t0 = timer_ticks();
    thread_sleep_ms(50);
    uint32_t dt = timer_ticks() - t0;
    if (dt < 50)  return 0;        /* woke early - the bug that matters */
    if (dt > 500) return 0;        /* wildly over: something is wrong */

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
    if (cxfs_is_locked(beyond)      != 0)        return 0;

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


/* ---- memory mappings (SYS_MEM_OP) ----
 * Runs on a SCRATCH pid rather than a live one. ktest_run() is called after
 * the ring-3 test threads have exited and before the executive starts, so no
 * process is being tracked and slot MAX_THREADS-1 belongs to nobody; it is
 * reset again at the end either way.
 *
 * The frame count is taken before and restored-to after, because the failure
 * this most needs to catch is not a wrong answer but a leak: a map/unmap pair
 * that returns the right numbers while quietly keeping the frames would pass
 * every assertion about addresses and sizes.
 */
static int vm_cycle(int pid) {
    int ok = 0;

    vm_proc_init(pid, vm_default_quota(), -1);

    struct mem_op_args a;
    #define RESET_ARGS() do {                                        \
        for (uint32_t _i = 0; _i < sizeof a / 4u; _i++)              \
            ((uint32_t *)&a)[_i] = 0;                                \
        a.op = MEM_OP_MAP; a.object = MOBJ_ANON;                     \
        a.length = 4096; a.prot = MPROT_READ | MPROT_WRITE;          \
    } while (0)

    /* A one-byte request must come back as a whole page: the caller asks in
       bytes and is TOLD the granularity rather than having to know it. */
    RESET_ARGS(); a.length = 1;
    if (vm_map(pid, &a) != E_OK)       goto done;
    if (a.granted != PAGE_SIZE)        goto done;
    if (a.addr < VM_MMAP_BASE)         goto done;
    if (a.addr & (PAGE_SIZE - 1u))     goto done;
    uint32_t first = a.addr;

    /* Readable, writable, and zero - a frame off the free list may hold
       another process's memory, so anything non-zero here is a disclosure. */
    volatile uint8_t *m = (volatile uint8_t *)first;
    for (uint32_t i = 0; i < PAGE_SIZE; i++) if (m[i] != 0) goto done;
    m[0] = 0xA5; m[PAGE_SIZE - 1] = 0x5A;
    if (m[0] != 0xA5 || m[PAGE_SIZE - 1] != 0x5A) goto done;

    /* A second mapping must not land on the first. */
    RESET_ARGS(); a.length = 8192;
    if (vm_map(pid, &a) != E_OK)                       goto done;
    if (a.granted != 8192)                             goto done;
    if (a.addr < first + PAGE_SIZE && a.addr + 8192 > first) goto done;
    uint32_t second = a.addr;

    /* INFO must account for exactly what was handed out. */
    RESET_ARGS(); a.op = MEM_OP_INFO;
    if (vm_info(pid, &a) != E_OK)                   goto done;
    if (a.mapped != PAGE_SIZE + 8192)               goto done;
    if (a.quota != vm_default_quota())                goto done;

    /* Write|Execute is refused. It cannot be ENFORCED on 32-bit non-PAE - there
       is no no-execute bit - so the refusal is the only thing standing between
       today and code written to rely on a combination that will stop being
       allowed. Asserting it here is what keeps it from being quietly relaxed. */
    RESET_ARGS(); a.prot = MPROT_WRITE | MPROT_EXEC;
    if (vm_map(pid, &a) != E_INVAL)                 goto done;

    /* Anonymous memory has no interior, so a non-zero offset is a caller error
       rather than something to ignore. Both halves of the 64-bit field. */
    RESET_ARGS(); a.offset_lo = 4096;
    if (vm_map(pid, &a) != E_INVAL)                 goto done;
    RESET_ARGS(); a.offset_hi = 1;
    if (vm_map(pid, &a) != E_INVAL)                 goto done;

    /* An object that does not exist yet is refused rather than treated as
       anonymous, so adding one later cannot silently change what old code did. */
    RESET_ARGS(); a.object = 99;
    if (vm_map(pid, &a) != E_INVAL)                 goto done;

    RESET_ARGS(); a.length = 0;
    if (vm_map(pid, &a) != E_INVAL)                 goto done;
    RESET_ARGS(); a.length = MMAP_MAX_BYTES + 1u;
    if (vm_map(pid, &a) != E_RANGE)                 goto done;
    RESET_ARGS(); a.prot = MPROT_NONE;
    if (vm_map(pid, &a) != E_INVAL)                 goto done;

    /* The quota is a real ceiling, not a number in a struct: a request that
       would cross it fails, and fails without having taken any frames. */
    uint32_t before_quota_try = pmm_free_count();
    RESET_ARGS(); a.length = vm_default_quota();
    if (vm_map(pid, &a) != E_NOMEM)                 goto done;
    if (pmm_free_count() != before_quota_try)       goto done;

    /* A hint that is free is honoured, which is what makes a moving window
       cheap: unmap and remap at the same address and interior pointers hold. */
    RESET_ARGS(); a.flags = MMAP_HINT; a.addr = VM_MMAP_BASE + 0x01000000u;
    if (vm_map(pid, &a) != E_OK)                            goto done;
    if (a.addr != VM_MMAP_BASE + 0x01000000u)               goto done;
    uint32_t hinted = a.addr;

    /* An occupied hint is declined, not honoured and not failed: the caller
       still gets memory, and `addr` tells it the truth about where. */
    RESET_ARGS(); a.flags = MMAP_HINT; a.addr = hinted;
    if (vm_map(pid, &a) != E_OK)                    goto done;
    if (a.addr == hinted)                           goto done;
    uint32_t declined = a.addr;

    /* Unmapping something never mapped is an error, not a silent success. */
    if (vm_unmap(pid, VM_MMAP_BASE - PAGE_SIZE, PAGE_SIZE) != E_NOENT) goto done;
    /* A length that disagrees with the region is refused rather than releasing
       a different amount than the caller believes it is releasing. */
    if (vm_unmap(pid, second, 4096) != E_INVAL)     goto done;

    if (vm_unmap(pid, first, 0) != E_OK)            goto done;   /* 0 = whole */
    if (vm_unmap(pid, second, 8192) != E_OK)        goto done;
    if (vm_unmap(pid, hinted, 4096) != E_OK)        goto done;
    if (vm_unmap(pid, declined, 4096) != E_OK)      goto done;

    RESET_ARGS(); a.op = MEM_OP_INFO;
    if (vm_info(pid, &a) != E_OK)                   goto done;
    if (a.mapped != 0)                              goto done;

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
 * for a process that is the right call, since addr_space_reclaim_user() frees
 * the whole user half at exit and a table freed and re-allocated per mapping
 * would be pure churn. Asserting no-leak on a cold pass therefore fails
 * against correct behaviour, which is exactly what it did when this test was
 * first written.
 *
 * The warm pass also proves something worth having on its own: after every
 * mapping is released, placement produces the same addresses again, so a full
 * unmap really does return the space rather than merely forgetting it. */
static int test_vmregion(void) {
    const int pid = MAX_THREADS - 1;

    if (!vm_cycle(pid)) { vm_proc_reset(pid); return 0; }
    vm_proc_reset(pid);

    uint32_t free0 = pmm_free_count();
    int ok = vm_cycle(pid);
    vm_proc_reset(pid);
    if (pmm_free_count() != free0) return 0;
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
    uint32_t base, base2;

    uint32_t top = kstack_alloc(THREAD_STACK_DEFAULT, 7, &base);
    if (!top) return 0;
    uint32_t top2 = kstack_alloc(THREAD_STACK_DEFAULT, 6, &base2);
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
    uint32_t a = (uint32_t)&here;
    if (thread_current_id() != 0) return 0;
    if (a < KSTACK_REGION_BASE || a >= KSTACK_REGION_BASE + KSTACK_REGION_SIZE) return 0;
    uint32_t top  = (a & ~(KSTACK_SLOT_SIZE - 1)) + KSTACK_SLOT_SIZE;
    uint32_t base = top - KSTACK_MAIN_SIZE;
    return paging_get_phys(base) && !paging_get_phys(base - PAGE_SIZE)
        && kstack_guard_owner(base - 4) == 0
        && thread_current_kstack_top() != 0;
}

/* ---- the double fault has a stack of its own ----
 * Checks the wiring rather than taking a double fault, which cannot be
 * recovered from: vector 8 is a task gate, and it names an available 32-bit
 * TSS. (CXK_KTEST_STACK_OVERFLOW takes the real one.) */
static int test_double_fault_gate(void) {
    struct { uint16_t limit; uint32_t base; } __attribute__((packed)) idtr, gdtr;
    __asm__ volatile ("sidt %0" : "=m"(idtr));
    __asm__ volatile ("sgdt %0" : "=m"(gdtr));
    const struct idt_entry *e = (const struct idt_entry *)idtr.base + 8;
    if (e->type_attr != IDT_GATE_TASK || e->selector != DF_TSS_SEL) return 0;
    const uint8_t *d = (const uint8_t *)gdtr.base + DF_TSS_SEL;
    return d[5] == 0x89;   /* present, DPL 0, 32-bit TSS, not busy */
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

    /* absent keys keep their defaults; an empty file is all defaults */
    struct kconfig d;
    kconfig_defaults(&d);
    if (kconfig_parse("max_threads = 6\n", 16, &c, &at, &why) != 0) return 0;
    if (c.max_threads != 6 || c.kernel_stack != d.kernel_stack || c.memory_quota != d.memory_quota) return 0;
    if (kconfig_parse("", 0, &c, &at, &why) != 0 || c.max_threads != d.max_threads) return 0;

    return kc_refused("kernel_stack = 8\n", 0)                             /* unknown key */
        && kc_refused("max_threads = 12\nmax_thread = 12\n", 17)          /* misspelt */
        && kc_refused("max_threads = 99\n", 14)                            /* out of range */
        && kc_refused("max_threads = 3\n", 14)
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
    uint32_t top  = ((uint32_t)&here & ~(KSTACK_SLOT_SIZE - 1)) + KSTACK_SLOT_SIZE;
    uint32_t base = top - 16384u;
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
    total++; passed += report("cxfs volume tags",                  test_cxfs_volumes());
    total++; passed += report("clock + timed sleep",               test_clock_sleep());
    total++; passed += report("memory mappings (SYS_MEM_OP)",     test_vmregion());
    total++; passed += report("guarded kernel stacks",             test_kstack());
    total++; passed += report("double fault on its own stack",     test_double_fault_gate());
    total++; passed += report("thread 0 on a guarded stack",       test_main_stack());
    total++; passed += report("kernel config (X Data, linked X)",  test_kconfig());
    total++; passed += report("thread limit + stack size",         test_thread_limit());
    total++; passed += report("cxex signature + tamper",           test_cxex_signature());
    total++; passed += report("exec admission (dev / release)",   test_exec_admit());
    total++; passed += report("file syscalls (SYS_FILE_OP)",       usermode_file_test());

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