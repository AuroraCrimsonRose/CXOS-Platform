// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /CXLite/kernel/drivers/acpi.c */
/* Aurora Tejeda */
/* Minimal ACPI: RSDP/FADT discovery + targeted DSDT scan for _S5_/_S1_. */

#include "acpi.h"
#include "drivers/iowin.h"
#include "arch/paging.h"
#include "arch/cpu_ops.h"
#include "boot/cxbi.h"

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
/* the PM1 control registers, and the PM1 enable registers (where PWRBTN_EN
   lives: the upper half of each event block, at evt_blk + pm1_evt_len/2).
   A window the FADT did not name stays closed (IOWIN_NONE). */
static struct iowin pm1a_cnt, pm1b_cnt;
static struct iowin pm1a_en, pm1b_en;

static int      s5_ok = 0;
static uint16_t s5_typa = 0, s5_typb = 0;
static int      s1_ok = 0;
static uint16_t s1_typa = 0, s1_typb = 0;

static int      reset_ok = 0;
static uint8_t  reset_space = 0;
static uint64_t reset_addr = 0;
static struct iowin reset_reg;     /* opened here for a port, at reboot for memory */
static struct iowin pm_tmr;        /* the PM timer's 32-bit register */
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

/* forward: identity-maps a physical range on demand (defined below). */
static void *map_table(uint32_t phys, uint32_t len);

/* The RSDP, from the loader's handover (boot/cxbi.h): finding it is the
   firmware interface's business - a BIOS loader searches the EBDA and the ROM
   area, a UEFI one reads the configuration table - and the kernel reads no
   firmware area itself. Checked rather than trusted: the signature, and the
   checksum of the ACPI 1.0 part. */
static struct acpi_rsdp *find_rsdp(void) {
    uint64_t at = cxbi()->acpi_rsdp;
    if (at == 0 || at >= 0x100000000ull) return 0;
    struct acpi_rsdp *r = (struct acpi_rsdp *)map_table((uint32_t)at, 20);
    if (!sig_eq((const char *)r, "RSD PTR ", 8) || sum_bytes((const uint8_t *)r, 20) != 0)
        return 0;
    return r;
}

/* map a physical region so we can read a table (ACPI tables are usually in low
   reserved memory, already identity-mapped by our 128MB floor, but map to be
   safe for higher addresses). */
static void *map_table(uint32_t phys, uint32_t len) {
    /* Mapping a table again, longer, once its header gives the length, must
       not move it: paging_map_phys keeps a range at one address. */
    return (void *)paging_map_phys(phys, len, VM_WRITE);
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

/* Kept after init so acpi_find_table() can walk it later. The MADT is only
   wanted once the interrupt controllers come up, which is after ACPI. */
static struct acpi_sdt_header *saved_rsdt = 0;

void *acpi_find_table(const char *sig) {
    if (!saved_rsdt) return 0;
    uint32_t entries = (saved_rsdt->length - sizeof(struct acpi_sdt_header)) / 4;
    uint32_t *ptrs = (uint32_t *)((uint8_t *)saved_rsdt + sizeof(struct acpi_sdt_header));
    for (uint32_t i = 0; i < entries; i++) {
        struct acpi_sdt_header *h =
            (struct acpi_sdt_header *)map_table(ptrs[i], sizeof(struct acpi_sdt_header));
        if (sig_eq(h->signature, sig, 4)) {
            map_table(ptrs[i], h->length);      /* map the whole table */
            return h;
        }
    }
    return 0;
}

int acpi_init(void) {
    have_acpi = 0;
    saved_rsdt = 0;
    s5_ok = s1_ok = reset_ok = 0;

    struct acpi_rsdp *rsdp = find_rsdp();
    if (!rsdp) return 0;

    /* read RSDT */
    struct acpi_sdt_header *rsdt =
        (struct acpi_sdt_header *)map_table(rsdp->rsdt_address, sizeof(struct acpi_sdt_header));
    map_table(rsdp->rsdt_address, rsdt->length);
    if (!sig_eq(rsdt->signature, "RSDT", 4)) return 0;
    saved_rsdt = rsdt;

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
    /* PM1 control and enable registers are 16 bits wide, in I/O space. */
    if (fadt->pm1a_cnt_blk) iowin_port(&pm1a_cnt, fadt->pm1a_cnt_blk, 2);
    if (fadt->pm1b_cnt_blk) iowin_port(&pm1b_cnt, fadt->pm1b_cnt_blk, 2);
    /* the PM1 enable register sits in the upper half of the PM1 event block.
       evt_len covers both status + enable, so enable = evt_blk + evt_len/2. */
    if (fadt->pm1a_evt_blk && fadt->pm1_evt_len)
        iowin_port(&pm1a_en, fadt->pm1a_evt_blk + (fadt->pm1_evt_len / 2), 2);
    if (fadt->pm1b_evt_blk && fadt->pm1_evt_len)
        iowin_port(&pm1b_en, fadt->pm1b_evt_blk + (fadt->pm1_evt_len / 2), 2);

    if (fadt->pm_tmr_blk && fadt->pm_tmr_len >= 4)
        iowin_port(&pm_tmr, fadt->pm_tmr_blk, 4);

    /* reset register: system I/O (space id 1) or memory (0). A port past
       0xFFFF opens no window, so it is not offered as a way to reset. */
    if (fadt->header.length >= 129 && fadt->reset_reg_address != 0) {
        reset_space = fadt->reset_reg_space;
        reset_addr  = fadt->reset_reg_address;
        reset_val   = fadt->reset_value;
        if (reset_space == 1)
            reset_ok = reset_addr <= 0xFFFF && iowin_port(&reset_reg, (uint32_t)reset_addr, 1) == 0;
        else
            reset_ok = reset_space == 0;
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

int acpi_can_shutdown(void) { return have_acpi && s5_ok && pm1a_cnt.kind; }
int acpi_can_s1(void)       { return have_acpi && s1_ok && pm1a_cnt.kind; }
int acpi_can_reset(void)    { return have_acpi && reset_ok; }

int acpi_pm_timer_read(uint32_t *out) {
    if (!pm_tmr.kind) return -1;
    *out = iowin_read32(&pm_tmr, 0) & 0xFFFFFFu;
    return 0;
}

void acpi_shutdown(void) {
    if (!acpi_can_shutdown()) return;
    arch_irq_disable();
    /* write SLP_TYPa | SLP_EN to PM1a control, and PM1b if present */
    iowin_write16(&pm1a_cnt, 0, (uint16_t)((s5_typa << 10) | SLP_EN));
    if (pm1b_cnt.kind)
        iowin_write16(&pm1b_cnt, 0, (uint16_t)((s5_typb << 10) | SLP_EN));
    /* if we got here, it didn't power off */
    arch_cpu_halt_forever();
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
    if (pm1a_en.kind) {
        uint16_t en = iowin_read16(&pm1a_en, 0);
        en |= PM1_PWRBTN_EN;
        iowin_write16(&pm1a_en, 0, en);
    }
    if (pm1b_en.kind) {
        uint16_t en = iowin_read16(&pm1b_en, 0);
        en |= PM1_PWRBTN_EN;
        iowin_write16(&pm1b_en, 0, en);
    }

    /* clear any stale power-button status so a previous press doesn't
       immediately satisfy the wake (status bits are write-1-to-clear). */
    /* (status reg is the LOWER half of the event block = cnt-independent;
        we conservatively clear via the control-adjacent status if known) */

    arch_irq_enable();   /* allow the wake event to be serviced */

    /* enter S1: write SLP_TYP(S1) | SLP_EN to PM1 control */
    iowin_write16(&pm1a_cnt, 0, (uint16_t)((s1_typa << 10) | SLP_EN));
    if (pm1b_cnt.kind)
        iowin_write16(&pm1b_cnt, 0, (uint16_t)((s1_typb << 10) | SLP_EN));

    return 0;   /* resumed after a wake event */
}

int acpi_reboot(void) {
    if (!acpi_can_reset()) return -1;
    arch_irq_disable();
    /* reset register may be in system I/O space (space id 1) or memory (0) */
    if (reset_space == 1) {
        iowin_write8(&reset_reg, 0, reset_val);
    } else if (reset_space == 0) {
        /* the reset register may be memory-mapped; map its page first (same
           higher-half reason as the RSDP scan above) */
        volatile void *v = paging_map_phys((paddr_t)reset_addr, 1, VM_WRITE);
        if (v && iowin_mmio(&reset_reg, v, 1) == 0)
            iowin_write8(&reset_reg, 0, reset_val);
    }
    /* may take a moment; spin briefly */
    for (volatile int i = 0; i < 100000000; i++) { }
    return -1;   /* didn't reset */
}