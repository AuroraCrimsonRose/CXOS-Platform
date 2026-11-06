// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86/apic.c */
/* Aurora Tejeda / CATX Systems */
/* Local APIC + I/O APIC. See apic.h for what this replaces and what it does not. */

#include "apic.h"
#include "arch/evtimer.h"
#include "arch/x86/cpufeat.h"
#include "platform/pc/pic.h"
#include "arch/x86/msr.h"
#include "arch/cpu_ops.h"
#include "arch/paging.h"
#include "acpi.h"
#include "logging.h"
#include "drivers/iowin.h"
#include "madt.h"
#include "cpu/percpu.h"   /* CPU_MAX */

/* ---- local APIC registers (MMIO, 16-byte spaced) ------------------------ */
#define LAPIC_ID        0x020
#define LAPIC_VERSION   0x030
#define LAPIC_TPR       0x080   /* task priority - must be 0 to accept anything */
#define LAPIC_EOI       0x0B0
#define LAPIC_SVR       0x0F0   /* spurious interrupt vector                   */
#define LAPIC_LINT0     0x350
#define LAPIC_LINT1     0x360

#define SVR_ENABLE      (1u << 8)
#define LVT_MASKED      (1u << 16)

#define IA32_APIC_BASE  0x1B
#define APIC_BASE_ENABLE (1u << 11)

/* The spurious vector must have its low four bits set on some older parts, and
   0xFF satisfies that everywhere. It is never dispatched to a handler. */
#define SPURIOUS_VECTOR 0xFF

/* ---- I/O APIC ----------------------------------------------------------- */
#define IOAPIC_REGSEL   0x00
#define IOAPIC_IOWIN    0x10
#define IOAPIC_REG_ID   0x00
#define IOAPIC_REG_VER  0x01
#define IOAPIC_REG_RED  0x10    /* redirection entry n at 0x10 + 2n */

#define RED_MASKED      (1u << 16)
#define RED_LEVEL       (1u << 15)
#define RED_ACTIVE_LOW  (1u << 13)

/* ---- MADT --------------------------------------------------------------- */
struct madt_header {
    uint8_t  sig[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    uint8_t  oem_id[6];
    uint8_t  oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
    uint32_t lapic_address;
    uint32_t flags;             /* bit 0: the legacy 8259 pair is present */
} __attribute__((packed));

struct madt_entry {
    uint8_t type;
    uint8_t length;
} __attribute__((packed));

#define MADT_IOAPIC        1
#define MADT_ISO           2    /* interrupt source override */
#define MADT_LAPIC_ADDR_OVR 5

/* The local APIC's registers end at 0x3F0; the I/O APIC is a select register
   at 0x00 and a data window at 0x10. */
static struct iowin lapic;
static struct iowin ioapic;
static int ioapic_found = 0;
static uint32_t ioapic_gsi_base = 0;
static unsigned ioapic_entries = 0;
static int lapic_up = 0;
static int apic_up = 0;

static struct madt_cpu        madt_cpu_list[CPU_MAX];
static uint32_t               madt_cpu_n;
static struct madt_cpu_counts madt_cpu_seen;

/* ISA IRQ -> global system interrupt. Identity unless the MADT says otherwise,
   and it very often does: IRQ 0 is routinely rerouted to GSI 2. Ignoring these
   overrides gives a machine whose timer interrupt never arrives. */
static uint32_t iso_gsi[16];
static uint16_t iso_flags[16];

static inline uint32_t lapic_rd(uint32_t reg) {
    return iowin_read32(&lapic, reg);
}
static inline void lapic_wr(uint32_t reg, uint32_t v) {
    iowin_write32(&lapic, reg, v);
}

static uint32_t ioapic_rd(uint32_t reg) {
    iowin_write32(&ioapic, IOAPIC_REGSEL, reg);
    return iowin_read32(&ioapic, IOAPIC_IOWIN);
}
static void ioapic_wr(uint32_t reg, uint32_t v) {
    iowin_write32(&ioapic, IOAPIC_REGSEL, reg);
    iowin_write32(&ioapic, IOAPIC_IOWIN, v);
}

void lapic_eoi(void) {
    /* Writing zero is the architectural acknowledge. Anything else is
       undefined, and on some parts faults. */
    if (lapic_up) lapic_wr(LAPIC_EOI, 0);
}

uint32_t lapic_id(void) {
    return lapic_up ? (lapic_rd(LAPIC_ID) >> 24) : 0;
}

int apic_active(void)  { return apic_up; }
int lapic_active(void) { return lapic_up; }
int irq_msi_available(void) { return lapic_up; }

uint32_t irq_msi_address(void) {
    /* 0xFEE00000 | (destination APIC id << 12). Fixed delivery to this CPU. */
    return 0xFEE00000u | (lapic_id() << 12);
}

uint32_t irq_msi_data(uint8_t vector) {
    /* Fixed delivery mode, edge triggered: the vector alone. */
    return vector;
}

/* ---- MADT walk ----------------------------------------------------------- */
static void parse_madt(struct madt_header *madt) {
    for (int i = 0; i < 16; i++) { iso_gsi[i] = (uint32_t)i; iso_flags[i] = 0; }
    madt_cpu_n = madt_cpus((const uint8_t *)madt, madt->length,
                           madt_cpu_list, CPU_MAX, &madt_cpu_seen);

    uint8_t *p = (uint8_t *)madt + sizeof(struct madt_header);
    uint8_t *end = (uint8_t *)madt + madt->length;

    while (p + 2 <= end) {
        struct madt_entry *e = (struct madt_entry *)p;
        if (e->length < 2) break;              /* malformed: stop, do not spin */
        if (p + e->length > end) break;

        if (e->type == MADT_IOAPIC && !ioapic_found) {
            uint32_t addr = *(uint32_t *)(p + 4);
            ioapic_gsi_base = *(uint32_t *)(p + 8);
            volatile void *v = paging_map_phys(addr, 0x20, VM_WRITE | VM_UNCACHED);
            ioapic_found = v && iowin_mmio(&ioapic, v, 0x20) == 0;
        } else if (e->type == MADT_ISO) {
            uint8_t source = p[3];
            uint32_t gsi = *(uint32_t *)(p + 4);
            uint16_t flags = *(uint16_t *)(p + 8);
            if (source < 16) { iso_gsi[source] = gsi; iso_flags[source] = flags; }
        } else if (e->type == MADT_LAPIC_ADDR_OVR) {
            /* A 64-bit override. A 32-bit kernel can only use it if the high
               half is zero, which it is on every real machine. */
            uint64_t addr = *(uint64_t *)(p + 4);
            if ((addr >> 32) == 0 && addr) {
                volatile void *v = paging_map_phys((paddr_t)addr, 0x400, VM_WRITE | VM_UNCACHED);
                if (v) (void)iowin_mmio(&lapic, v, 0x400);
            }
        }
        p += e->length;
    }
}

/* Program one I/O APIC redirection entry for a legacy ISA IRQ. */
static void route_irq(int irq, uint8_t vector, int masked) {
    uint32_t gsi = iso_gsi[irq];
    if (gsi < ioapic_gsi_base) return;
    uint32_t idx = gsi - ioapic_gsi_base;
    if (idx >= ioapic_entries) return;

    uint32_t low = vector;
    if (masked) low |= RED_MASKED;
    /* The MADT's polarity and trigger bits: 0b11 = active low, 0b11 in the
       trigger field = level. ISA defaults are active-high edge, which is what
       the zero case means. */
    uint16_t f = iso_flags[irq];
    if ((f & 0x3) == 3) low |= RED_ACTIVE_LOW;
    if (((f >> 2) & 0x3) == 3) low |= RED_LEVEL;

    ioapic_wr(IOAPIC_REG_RED + idx * 2 + 1, lapic_id() << 24);   /* destination */
    ioapic_wr(IOAPIC_REG_RED + idx * 2, low);                    /* unmasked */
}

void irq_mask(int irq) {
    if (irq < 0 || irq > 15) return;
    if (!apic_up) { pic_mask(irq); return; }
    uint32_t gsi = iso_gsi[irq];
    if (gsi < ioapic_gsi_base) return;
    uint32_t idx = gsi - ioapic_gsi_base;
    if (idx >= ioapic_entries) return;
    ioapic_wr(IOAPIC_REG_RED + idx * 2,
              ioapic_rd(IOAPIC_REG_RED + idx * 2) | RED_MASKED);
}

void irq_unmask(int irq) {
    if (irq < 0 || irq > 15) return;
    if (!apic_up) { pic_unmask(irq); return; }
    uint32_t gsi = iso_gsi[irq];
    if (gsi < ioapic_gsi_base) return;
    uint32_t idx = gsi - ioapic_gsi_base;
    if (idx >= ioapic_entries) return;
    ioapic_wr(IOAPIC_REG_RED + idx * 2,
              ioapic_rd(IOAPIC_REG_RED + idx * 2) & ~RED_MASKED);
}

int irq_controller_init(void) {
    lapic_up = 0;
    apic_up = 0;

    if (!cpu_has(CPU_APIC)) {     /* the feature table, read at boot */
        klog("APIC", SEV_INFO, "no local APIC on this CPU - staying on the 8259");
        return 0;
    }

    /* The base address lives in an MSR, and bit 11 is a global enable that
       firmware can leave clear. Set it rather than assuming. */
    uint64_t base_msr = rdmsr(IA32_APIC_BASE);
    uint32_t base = (uint32_t)(base_msr & 0xFFFFF000u);
    if (!base) base = 0xFEE00000u;
    wrmsr(IA32_APIC_BASE, (base_msr | APIC_BASE_ENABLE));

    volatile void *v = paging_map_phys(base, 0x400, VM_WRITE | VM_UNCACHED);
    if (v) (void)iowin_mmio(&lapic, v, 0x400);

    struct madt_header *madt = (struct madt_header *)acpi_find_table("APIC");
    if (madt) parse_madt(madt);       /* may relocate lapic via an override */

    /* Accept every priority. A non-zero task priority silently drops
       interrupts below it, which looks exactly like a device that never fires. */
    lapic_wr(LAPIC_TPR, 0);
    /* Software-enable, and give the spurious vector somewhere harmless to go. */
    lapic_wr(LAPIC_SVR, SVR_ENABLE | SPURIOUS_VECTOR);
    lapic_up = 1;

    klog_u32("APIC", SEV_OK, "local APIC online, id ", lapic_id(), LOG_COLOR_VALUE, "");

    if (!madt) {
        klog("APIC", SEV_INFO, "no MADT - local APIC only, IRQs stay on the 8259");
        return 0;
    }
    if (!ioapic_found) {
        klog("APIC", SEV_INFO, "MADT describes no I/O APIC - IRQs stay on the 8259");
        return 0;
    }

    ioapic_entries = ((ioapic_rd(IOAPIC_REG_VER) >> 16) & 0xFF) + 1;

    /* Take the 8259's CURRENT mask state before silencing it, and carry it over.
       The PIC comes up with every line masked and each driver unmasks the one
       it handles, so mirroring that is what keeps the set of live lines the
       same across the handover.
     *
       Routing everything unmasked instead - which is the obvious thing to
       write - enables lines that have no handler at all. A device asserting one
       is then never serviced, so it keeps asserting: the handler runs, finds
       nobody, acknowledges, and is immediately re-entered. That storm starves
       everything else, and it presents as an intermittent hang partway through
       boot rather than as an interrupt problem. */
    /* Mask interrupts for the whole handover. The timer is firing at 1000 Hz
       while this runs (kmain enables interrupts long before irq_controller_init), so
       every step below is racing it. See the apic_up note further down for
       what that race actually cost. */
    uint32_t saved_flags = arch_irq_save();

    uint16_t masks = pic_get_masks();

    /* Mask the 8259 completely BEFORE routing anything through the I/O APIC.
       Both controllers are wired to the same lines, and leaving the PIC live
       means every interrupt arrives twice - once acknowledged, once not. */
    pic_mask_all();

    /* Legacy IRQs keep their existing vectors (32 + irq), so the IDT and every
       installed handler are untouched.
     *
     * Two routing hazards, and the first one silently kills the timer:
     *
     * A source override moves an IRQ onto some other GSI - IRQ 0 onto GSI 2 is
     * near universal. That GSI is then ALSO the identity mapping of a different
     * IRQ, so routing every IRQ in order has the later one overwrite the
     * earlier: IRQ 2 lands on redirection entry 2 and replaces the timer's
     * vector with its own. The timer interrupt then never arrives, the
     * scheduler never preempts, and the boot stops somewhere later with no
     * indication that interrupts are the problem.
     *
     * So an overridden GSI is claimed, and an identity mapping is not allowed
     * to take it. IRQ 2 is skipped outright: it is the 8259 cascade line and
     * means nothing once the I/O APIC is delivering. */
    uint8_t gsi_claimed[64];
    for (int i = 0; i < 64; i++) gsi_claimed[i] = 0;
    for (int irq = 0; irq < 16; irq++)
        if (iso_gsi[irq] != (uint32_t)irq && iso_gsi[irq] < 64)
            gsi_claimed[iso_gsi[irq]] = (uint8_t)(irq + 1);

    /* Declare the I/O APIC in charge BEFORE routing anything onto it.
     *
     * irq_eoi() picks its controller with apic_active(), which reads this flag.
     * Setting it after the routing loop left a window that the timer walked
     * into on roughly one boot in three: route_irq(0, ...) puts IRQ 0 live on
     * the I/O APIC on the FIRST iteration, but until the loop finished and the
     * flag went up, irq_eoi still sent the acknowledgement to the 8259. So an
     * interrupt delivered by the I/O APIC was acknowledged at the PIC, the
     * local APIC's in-service bit for vector 32 was never cleared, and nothing
     * of equal or lower priority was ever delivered again.
     *
     * The timer stopped, permanently and silently. The boot carried on until
     * something actually needed preemption - the scheduler self-test - and
     * then waited forever for threads that could never be scheduled. The
     * comment on irq_eoi() in idt.c describes this exact failure; the window
     * here is how it happened.
     *
     * Safe to set early: the 8259 is already fully masked, and interrupts are
     * off for the duration, so nothing can be delivered by either controller
     * until the restore below. */
    apic_up = 1;

    for (int irq = 0; irq < 16; irq++) {
        if (irq == 2) continue;                      /* cascade - not a real line */
        uint32_t gsi = iso_gsi[irq];
        if (gsi == (uint32_t)irq && gsi < 64 &&
            gsi_claimed[gsi] && gsi_claimed[gsi] != (uint8_t)(irq + 1))
            continue;                                /* an override owns this GSI */
        route_irq(irq, (uint8_t)(32 + irq), (masks >> irq) & 1);
    }

    arch_irq_restore(saved_flags);

    klog_u32("APIC", SEV_OK, "I/O APIC online, redirection entries: ",
             ioapic_entries, LOG_COLOR_VALUE, "");
    return 1;
}

/* ---- arch/evtimer.h: the TSC and the local APIC's timer ------------------
 *
 * One-shot mode, divided by 16: the count runs down at the APIC's clock over
 * 16 and interrupts once at zero. timer.c measures that rate against the
 * clock. 16 is a compromise between reach and grain: on QEMU's 1 GHz APIC
 * clock a step is 16 ns and the longest count 68 s; at the 100-200 MHz of a
 * real bus, 80-160 ns and minutes. TSC-deadline mode would program the time
 * itself, but the rig's Piledriver does not have it (AMD never shipped it),
 * so the count is the one path. */
#define LAPIC_LVT_TIMER   0x320
#define LAPIC_TIMER_INIT  0x380
#define LAPIC_TIMER_CUR   0x390
#define LAPIC_TIMER_DIV   0x3E0
#define TIMER_DIV_16      0x3

static irq_handler_t lapic_timer_fn;

int arch_cycles_constant(void) {
    if (!cpu_has(CPU_TSC)) return 0;
    return cpu_has(CPU_INVTSC) ? 2 : 1;
}

/* Hyper-V's HV_X64_MSR_TSC_FREQUENCY (0x40000022), read only where CPUID
   0x40000003 grants it: EAX bit 11 (AccessFrequencyMsrs) and EDX bit 8 (the
   frequency MSRs are there). Linux's ms_hyperv_init_platform checks the same
   two bits before taking it (from memory: Linux is not an indexed tree).
   XNU's tsc_init (osfmk/i386/tsc.c) takes a hypervisor's stated rate over a
   measurement the same way, from leaf 0x40000010, which Hyper-V lacks. */
#define HV_SIG_EBX  0x7263694du    /* "Micr" */
#define HV_SIG_ECX  0x666f736fu    /* "osof" */
#define HV_SIG_EDX  0x76482074u    /* "t Hv" */
#define HV_IFACE    0x31237648u    /* "Hv#1" */
#define HV_MSR_TSC_FREQUENCY 0x40000022u

uint64_t arch_cycles_stated_hz(void) {
    uint32_t r[4];
    cpuid_read(1, 0, r);
    if (!(r[2] & (1u << 31))) return 0;              /* no hypervisor */
    cpuid_read(0x40000000u, 0, r);
    if (r[0] < 0x40000003u || r[1] != HV_SIG_EBX || r[2] != HV_SIG_ECX || r[3] != HV_SIG_EDX)
        return 0;
    cpuid_read(0x40000001u, 0, r);
    if (r[0] != HV_IFACE) return 0;
    cpuid_read(0x40000003u, 0, r);
    if (!(r[0] & (1u << 11)) || !(r[3] & (1u << 8))) return 0;
    return rdmsr(HV_MSR_TSC_FREQUENCY);
}

int arch_evtimer_init(irq_handler_t on_fire) {
    if (!lapic_up) return -1;
    lapic_timer_fn = on_fire;
    lapic_wr(LAPIC_TIMER_INIT, 0);
    lapic_wr(LAPIC_TIMER_DIV, TIMER_DIV_16);
    lapic_wr(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR);   /* one-shot, unmasked */
    return 0;
}

void arch_evtimer_start(uint32_t count) {
    if (lapic_up) lapic_wr(LAPIC_TIMER_INIT, count);
}

uint32_t arch_evtimer_left(void) {
    return lapic_up ? lapic_rd(LAPIC_TIMER_CUR) : 0;
}

void lapic_timer_interrupt(struct registers *r) {
    if (lapic_timer_fn) lapic_timer_fn(r);
}

/* ---- 1.9d: the other CPUs ------------------------------------------------
 *
 * The interrupt command register: the destination's ID in the high word's
 * top byte (xAPIC: 8 bits, so madt.c keeps none past 0xFE), the delivery
 * mode and vector in the low word, whose write sends. Delivery status (bit
 * 12) stays set until the target's local APIC has accepted the IPI. INIT,
 * then STARTUP with the page the AP starts at, as the Intel SDM's MP
 * initialisation sequence has it (vol. 3A 8.4.4) and Haiku's loader and
 * XNU send them (smp_boot_other_cpus; i386_start_cpu). */
#define LAPIC_ESR       0x280
#define LAPIC_ICR_LO    0x300
#define LAPIC_ICR_HI    0x310
#define ICR_FIXED       0x000u
#define ICR_INIT        0x500u
#define ICR_STARTUP     0x600u
#define ICR_PENDING     (1u << 12)
#define ICR_ASSERT      (1u << 14)
#define ICR_LEVEL       (1u << 15)
/* A poll gives up after this many reads: an MMIO read exits to the host
   under Hyper-V (a few microseconds), so this is well under a second
   there and nothing on hardware, where it clears in a few hundred cycles. */
#define ICR_POLLS       100000u

uint32_t apic_cpus(const struct madt_cpu **list, const struct madt_cpu_counts **counts) {
    if (list) *list = madt_cpu_list;
    if (counts) *counts = &madt_cpu_seen;
    return madt_cpu_n;
}

/* Interrupts off from the destination's write to the delivery: an
   interrupt that sent an IPI of its own between the two writes (1.9e1: a
   shootdown from a handler) would leave this one aimed at its CPU. */
static int icr_send(uint32_t apic_id, uint32_t low) {
    if (!lapic_up) return -1;
    uint32_t f = arch_irq_save();
    lapic_wr(LAPIC_ESR, 0);
    lapic_wr(LAPIC_ICR_HI, apic_id << 24);
    lapic_wr(LAPIC_ICR_LO, low);
    int rc = -1;
    for (uint32_t i = 0; i < ICR_POLLS && rc; i++)
        if (!(lapic_rd(LAPIC_ICR_LO) & ICR_PENDING)) rc = 0;
    arch_irq_restore(f);
    return rc;
}

int lapic_send_init(uint32_t apic_id) {
    /* Asserted, then de-asserted: a Pentium 4 and later ignore the second,
       an older local APIC needs it to leave the INIT state (Haiku sends both). */
    if (icr_send(apic_id, ICR_INIT | ICR_LEVEL | ICR_ASSERT)) return -1;
    return icr_send(apic_id, ICR_INIT | ICR_LEVEL);
}

int lapic_send_startup(uint32_t apic_id, uint32_t page) {
    return icr_send(apic_id, ICR_STARTUP | (page & 0xFF));
}

int lapic_send_fixed(uint32_t apic_id, uint8_t vector) {
    return icr_send(apic_id, ICR_FIXED | ICR_ASSERT | vector);
}

/* An AP's own local APIC, as irq_controller_init and arch_evtimer_init set
   the boot CPU's: the global enable in the MSR, every priority accepted,
   software-enabled with the spurious vector, and the timer divided by 16 on
   its vector but stopped (count 0) and masked, as nothing on an AP runs it
   until 1.9e. The registers are at the same address on every CPU, each
   seeing its own. */
void lapic_ap_init(void) {
    uint64_t base_msr = rdmsr(IA32_APIC_BASE);
    wrmsr(IA32_APIC_BASE, base_msr | APIC_BASE_ENABLE);
    lapic_wr(LAPIC_TPR, 0);
    lapic_wr(LAPIC_SVR, SVR_ENABLE | SPURIOUS_VECTOR);
    lapic_wr(LAPIC_TIMER_INIT, 0);
    lapic_wr(LAPIC_TIMER_DIV, TIMER_DIV_16);
    lapic_wr(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR | LVT_MASKED);
}

uint32_t lapic_svr(void)       { return lapic_rd(LAPIC_SVR); }
uint32_t lapic_tpr(void)       { return lapic_rd(LAPIC_TPR); }
uint32_t lapic_lvt_timer(void) { return lapic_rd(LAPIC_LVT_TIMER); }

/* Unmask the timer for one count on the calling CPU (its self-test, smp.c),
   or mask it again. */
void lapic_timer_oneshot(uint32_t count, int unmask) {
    lapic_wr(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR | (unmask ? 0 : LVT_MASKED));
    lapic_wr(LAPIC_TIMER_INIT, count);
}
