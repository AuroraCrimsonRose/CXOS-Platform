// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/hwreport.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The processor's part of the hardware report (platform.h,
 * platform_hw_report; docs/hardware/CX_HW_REPORT.md): what the CPU says it
 * is and how its interrupt controller came up, as lines of the report's form
 * (diag/hwfmt.h). The platform calls it between its own sections. On x86:
 * "cpu", "cpuid" (raw leaves), "hypervisor" and "apic" (arch/x86/cpureport.c).
 */

#ifndef ARCH_HWREPORT_H
#define ARCH_HWREPORT_H

#include "diag/hwfmt.h"

void arch_hw_report(struct hw_out *o);

#endif
