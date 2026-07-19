/* /CXLite/kernel/drivers/acpi.c */
/* Aurora Tejeda */
/* Minimal ACPI: RSDP/FADT discovery + targeted DSDT scan for _S5_/_S1_. */

#include "acpi.h"
#include "io.h"
#include "paging.h"

/* ---- ACPI table structures (packed, as they appear in memory) ---- */

struct acpi_rsdp {
    char     signature[8];     /* "RSD PTR " */
    uint8_t  checksum;
    char     oemid[6];
    uint8_t  revision;         /* 0 = ACPI 1.0 (use rsdt), 2 = 2.0+ (xsdt) */
    uint32_t rsdt_address;
    /* 2.0+ fields follow (length, xsdt_address, ...) - we use rsdt for simplicity */
} __attribute__((packed));

struct acpi_sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oemid[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

/* FADT (Fixed ACPI Description Table) - we only need a few fields */
struct acpi_fadt {
    struct acpi_sdt_header header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;              /* physical address of the DSDT */
    uint8_t  reserved0;
    uint8_t  preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  s4bios_req;
    uint8_t  pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;     /* PM1a control register (where we write SLP_TYP) */
    uint32_t pm1b_cnt_blk;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t  pm1_evt_len;
    uint8_t  pm1_cnt_len;
    uint8_t  pm2_cnt_len;
    uint8_t  pm_tmr_len;
    uint8_t  gpe0_blk_len;
    uint8_t  gpe1_blk_len;
    uint8_t  gpe1_base;
    uint8_t  cst_cnt;
    uint16_t p_lvl2_lat;
    uint16_t p_lvl3_lat;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t  duty_offset;
    uint8_t  duty_width;
    uint8_t  day_alrm;
    uint8_t  mon_alrm;
    uint8_t  century;
    uint16_t iapc_boot_arch;
    uint8_t  reserved1;
    uint32_t flags;
    /* reset register (Generic Address Structure) at offset 116 */
    uint8_t  reset_reg_space;  /* address space id (1 = system I/O) */
    uint8_t  reset_reg_bit_width;
    uint8_t  reset_reg_bit_offset;
    uint8_t  reset_reg_access_size;
    uint64_t reset_reg_address;
    uint8_t  reset_value;
    /* ... more fields after, not needed ... */
} __attribute__((packed));

#define SLP_EN  (1u << 13)   /* sleep enable bit in PM1 control */

/* ---- cached state ---- */
static int      have_acpi = 0;
static uint32_t pm1a_cnt = 0;
static uint32_t pm1b_cnt = 0;
/* PM1 event blocks: the enable register (where PWRBTN_EN lives) is in the upper
   half of the event block, at evt_blk + pm1_evt_len/2. */
static uint32_t pm1a_en = 0;       /* PM1a enable register address */
static uint32_t pm1b_en = 0;

static int      s5_ok = 0;
static uint16_t s5_typa = 0, s5_typb = 0;
static int      s1_ok = 0;
static uint16_t s1_typa = 0, s1_typb = 0;

static int      reset_ok = 0;
static uint8_t  reset_space = 0;
static uint64_t reset_addr = 0;
static uint8_t  reset_val = 0;

/* ---- helpers ---- */

static int sig_eq(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static uint8_t sum_bytes(const uint8_t *p, uint32_t n) {
    uint8_t s = 0;
    for (uint32_t i = 0; i < n; i++) s = (uint8_t)(s + p[i]);
    return s;
}

/* forward: identity-maps a physical range on demand (defined below). The RSDP
   scan needs it because the higher-half kernel has no identity map. */
static void *map_table(uint32_t phys, uint32_t len);

/* find the RSDP by scanning the BIOS areas for "RSD PTR " on a 16-byte boundary.
   search: the EBDA (first 1KB) and the BIOS ROM area 0xE0000-0xFFFFF. */
static struct acpi_rsdp *find_rsdp(void) {
    /* v5 is a HIGHER-HALF kernel: kernel.asm identity-maps the first 4 MB only
       long enough to enable paging, then drops the identity map. So low physical
       memory is NOT readable at its physical address any more - reading 0x40E
       directly page-faults (CR2 = 0x40E), which is what v4 could get away with.
       Map what we are about to scan first, using the same on-demand identity
       mapping the ACPI tables use. */
    map_table(0x0,     0x1000);    /* page 0: BIOS Data Area, EBDA pointer at 0x40E */
    map_table(0xE0000, 0x20000);   /* BIOS ROM area 0xE0000-0xFFFFF */

    /* EBDA segment pointer is at physical 0x40E (word, in paragraphs) */
    uint16_t ebda_seg = *(volatile uint16_t *)0x40E;
    uint32_t ebda = (uint32_t)ebda_seg << 4;

    const char *target = "RSD PTR ";

    if (ebda >= 0x80000 && ebda < 0xA0000) {
        map_table(ebda, 1024);     /* the EBDA itself sits above 4 MB-mapped range */
        for (uint32_t a = ebda; a < ebda + 1024; a += 16) {
            if (sig_eq((const char *)a, target, 8)) {
                struct acpi_rsdp *r = (struct acpi_rsdp *)a;
                if (sum_bytes((const uint8_t *)r, 20) == 0) { paging_unmap(0x0); return r; }
            }
        }
    }
    for (uint32_t a = 0xE0000; a < 0x100000; a += 16) {
        if (sig_eq((const char *)a, target, 8)) {
            struct acpi_rsdp *r = (struct acpi_rsdp *)a;
            if (sum_bytes((const uint8_t *)r, 20) == 0) { paging_unmap(0x0); return r; }
        }
    }
    /* restore the null-pointer guard: page 0 stays unmapped after the scan */
    paging_unmap(0x0);
    return 0;
}

/* map a physical region so we can read a table (ACPI tables are usually in low
   reserved memory, already identity-mapped by our 128MB floor, but map to be
   safe for higher addresses). */
static void *map_table(uint32_t phys, uint32_t len) {
    uint32_t start = phys & ~0xFFFu;
    uint32_t end = (phys + len + 0xFFF) & ~0xFFFu;
    for (uint32_t a = start; a < end; a += 0x1000)
        paging_map(a, a, PAGE_PRESENT | PAGE_WRITE);
    return (void *)phys;
}

/* scan the DSDT for a _Sx_ package and extract SLP_TYPa/b.
   The AML for "Name (_S5, Package (...) { typa, typb, ... })" appears as the
   bytes: '_' 'S' 'n' '_' then a PackageOp (0x12), a pkglen, a count byte, then
   the elements. The first two elements are SLP_TYPa and SLP_TYPb, each usually
   encoded as a BytePrefix (0x0A) + value, or a small-int opcode (0x00/0x01...).
   This targeted scan is not a real AML parser but handles the common encoding. */
static int dsdt_find_sx(const uint8_t *dsdt, uint32_t len, char digit,
                        uint16_t *typa, uint16_t *typb) {
    for (uint32_t i = 0; i + 8 < len; i++) {
        if (dsdt[i] == '_' && dsdt[i+1] == 'S' && dsdt[i+2] == (uint8_t)digit &&
            dsdt[i+3] == '_') {
            /* find the PackageOp 0x12 within the next few bytes */
            uint32_t j = i + 4;
            uint32_t limit = j + 8;
            while (j < limit && j < len && dsdt[j] != 0x12) j++;
            if (j >= len || dsdt[j] != 0x12) continue;
            j++;                       /* skip PackageOp */
            /* skip pkglength: top 2 bits of first byte say how many extra bytes */
            uint8_t lead = dsdt[j];
            uint32_t pkg_extra = (lead >> 6) & 0x3;
            j += 1 + pkg_extra;
            /* next byte = element count */
            if (j >= len) continue;
            j++;                       /* skip count */
            /* element 0 = SLP_TYPa */
            uint16_t a = 0, b = 0;
            if (j < len) {
                if (dsdt[j] == 0x0A) { a = dsdt[j+1]; j += 2; }   /* BytePrefix */
                else { a = dsdt[j]; j += 1; }                     /* small int */
            }
            if (j < len) {
                if (dsdt[j] == 0x0A) { b = dsdt[j+1]; j += 2; }
                else { b = dsdt[j]; j += 1; }
            }
            *typa = a;
            *typb = b;
            return 1;
        }
    }
    return 0;
}

int acpi_init(void) {
    have_acpi = 0;
    s5_ok = s1_ok = reset_ok = 0;

    struct acpi_rsdp *rsdp = find_rsdp();
    if (!rsdp) return 0;

    /* read RSDT */
    struct acpi_sdt_header *rsdt =
        (struct acpi_sdt_header *)map_table(rsdp->rsdt_address, sizeof(struct acpi_sdt_header));
    map_table(rsdp->rsdt_address, rsdt->length);
    if (!sig_eq(rsdt->signature, "RSDT", 4)) return 0;

    /* RSDT is followed by an array of 32-bit table pointers */
    uint32_t entries = (rsdt->length - sizeof(struct acpi_sdt_header)) / 4;
    uint32_t *table_ptrs = (uint32_t *)((uint8_t *)rsdt + sizeof(struct acpi_sdt_header));

    struct acpi_fadt *fadt = 0;
    for (uint32_t i = 0; i < entries; i++) {
        struct acpi_sdt_header *h =
            (struct acpi_sdt_header *)map_table(table_ptrs[i], sizeof(struct acpi_sdt_header));
        if (sig_eq(h->signature, "FACP", 4)) {       /* FADT's signature is "FACP" */
            map_table(table_ptrs[i], h->length);
            fadt = (struct acpi_fadt *)h;
            break;
        }
    }
    if (!fadt) return 0;

    have_acpi = 1;
    pm1a_cnt = fadt->pm1a_cnt_blk;
    pm1b_cnt = fadt->pm1b_cnt_blk;
    /* the PM1 enable register sits in the upper half of the PM1 event block.
       evt_len covers both status + enable, so enable = evt_blk + evt_len/2. */
    if (fadt->pm1a_evt_blk && fadt->pm1_evt_len)
        pm1a_en = fadt->pm1a_evt_blk + (fadt->pm1_evt_len / 2);
    if (fadt->pm1b_evt_blk && fadt->pm1_evt_len)
        pm1b_en = fadt->pm1b_evt_blk + (fadt->pm1_evt_len / 2);

    /* reset register (only if it's a system I/O port we can write) */
    if (fadt->header.length >= 129 && fadt->reset_reg_address != 0) {
        reset_space = fadt->reset_reg_space;
        reset_addr  = fadt->reset_reg_address;
        reset_val   = fadt->reset_value;
        reset_ok = 1;
    }

    /* scan the DSDT for _S5_ and _S1_ */
    if (fadt->dsdt) {
        struct acpi_sdt_header *dsdt_h =
            (struct acpi_sdt_header *)map_table(fadt->dsdt, sizeof(struct acpi_sdt_header));
        map_table(fadt->dsdt, dsdt_h->length);
        const uint8_t *dsdt = (const uint8_t *)dsdt_h;
        uint32_t dlen = dsdt_h->length;

        s5_ok = dsdt_find_sx(dsdt, dlen, '5', &s5_typa, &s5_typb);
        s1_ok = dsdt_find_sx(dsdt, dlen, '1', &s1_typa, &s1_typb);
    }

    return 1;
}

int acpi_can_shutdown(void) { return have_acpi && s5_ok && pm1a_cnt; }
int acpi_can_s1(void)       { return have_acpi && s1_ok && pm1a_cnt; }
int acpi_can_reset(void)    { return have_acpi && reset_ok; }

void acpi_shutdown(void) {
    if (!acpi_can_shutdown()) return;
    __asm__ volatile ("cli");
    /* write SLP_TYPa | SLP_EN to PM1a control, and PM1b if present */
    outw((uint16_t)pm1a_cnt, (uint16_t)((s5_typa << 10) | SLP_EN));
    if (pm1b_cnt)
        outw((uint16_t)pm1b_cnt, (uint16_t)((s5_typb << 10) | SLP_EN));
    /* if we got here, it didn't power off */
    for (;;) __asm__ volatile ("hlt");
}

#define PM1_PWRBTN_EN  (1u << 8)   /* power button enable (in PM1 enable reg) */
#define PM1_PWRBTN_STS (1u << 8)   /* power button status (in PM1 status reg) */

int acpi_sleep_s1(void) {
    if (!acpi_can_s1()) return -1;

    /* Arm the POWER BUTTON as a wake source: set PWRBTN_EN in the PM1 enable
       register. Without an armed wake event, S1 is a one-way trip (the machine
       sleeps and never resumes). With PWRBTN_EN set, a brief power-button press
       generates a WAKE event (resume) instead of the default hard power action,
       so you press the power button to wake the machine. (Keyboard wake would
       need GPE/_PRW parsing - an AML-interpreter job for later; the power
       button is the standard, reliable, no-AML wake source.) */
    if (pm1a_en) {
        uint16_t en = inw((uint16_t)pm1a_en);
        en |= PM1_PWRBTN_EN;
        outw((uint16_t)pm1a_en, en);
    }
    if (pm1b_en) {
        uint16_t en = inw((uint16_t)pm1b_en);
        en |= PM1_PWRBTN_EN;
        outw((uint16_t)pm1b_en, en);
    }

    /* clear any stale power-button status so a previous press doesn't
       immediately satisfy the wake (status bits are write-1-to-clear). */
    /* (status reg is the LOWER half of the event block = cnt-independent;
        we conservatively clear via the control-adjacent status if known) */

    __asm__ volatile ("sti");   /* allow the wake event to be serviced */

    /* enter S1: write SLP_TYP(S1) | SLP_EN to PM1 control */
    outw((uint16_t)pm1a_cnt, (uint16_t)((s1_typa << 10) | SLP_EN));
    if (pm1b_cnt)
        outw((uint16_t)pm1b_cnt, (uint16_t)((s1_typb << 10) | SLP_EN));

    return 0;   /* resumed after a wake event */
}

int acpi_reboot(void) {
    if (!acpi_can_reset()) return -1;
    __asm__ volatile ("cli");
    /* reset register may be in system I/O space (space id 1) or memory (0) */
    if (reset_space == 1) {
        outb((uint16_t)reset_addr, reset_val);
    } else if (reset_space == 0) {
        /* the reset register may be memory-mapped; map its page first (same
           higher-half reason as the RSDP scan above) */
        map_table((uint32_t)reset_addr, 1);
        *(volatile uint8_t *)(uint32_t)reset_addr = reset_val;
    }
    /* may take a moment; spin briefly */
    for (volatile int i = 0; i < 100000000; i++) { }
    return -1;   /* didn't reset */
}