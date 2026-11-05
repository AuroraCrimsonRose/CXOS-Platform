// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/platform/pc/hwreport.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The hardware report (roadmap 1.8a, docs/hardware/CX_HW_REPORT.md): what
 * this PC says it is, in the fixed form of diag/hwfmt.h, so a boot under
 * QEMU, one under Hyper-V and one on the rig can be put side by side. The
 * loader's memory map, CPUID's raw leaves, every ACPI table the RSDT and the
 * XSDT name with its checksum judged, the MADT entry by entry, the PCI
 * functions, and the clocks: the TSC as the tick measured it and as the ACPI
 * PM timer measures it, which should agree.
 *
 * Read again from the firmware, not taken from the drivers: acpi.c reads only
 * the RSDT and judges no table's checksum, and the report is where that
 * would show.
 */

#ifndef PLATFORM_PC_HWREPORT_H
#define PLATFORM_PC_HWREPORT_H

#include <stdint.h>

/* platform_hw_report (platform.h) builds the report (about 50 ms of it is
   the PM-timer measurement), prints it to serial, and saves it to the crash
   area. These say what it did, for the self-test (ktest_pc.c). */

/* The report this boot built, and its length; 0 before platform_hw_report. */
const char *hwreport_text(uint32_t *len);

/* The TSC's rate as the PM timer measured it in the report: Hz, or 0 when
   there was no PM timer or it did not move. */
uint64_t hwreport_tsc_pm_hz(void);

/* What saving the report gave: DISK_OK, or the disk error. */
int hwreport_saved(void);

#endif
