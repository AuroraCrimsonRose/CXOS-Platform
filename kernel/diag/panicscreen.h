// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/diag/panicscreen.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The panic screen (roadmap Phase 2 step 3a): a headline and advice anyone can
 * read, the stop code, the crash number, a short named trace, and a QR code
 * holding the essentials when the screen has room for one.
 *
 * Static buffers only, no locks, no allocation: arch/x86_64/idt.c calls this
 * after panic_begin has taken the screen, possibly on the double-fault stack.
 */
#ifndef DIAG_PANICSCREEN_H
#define DIAG_PANICSCREEN_H

#include "diag/panicrep.h"

/* Fills r->log from the system log and draws the report. No framebuffer: does
   nothing, and the VGA text the panic printed stands alone. */
void panic_screen_show(struct panic_report *r);

/* One more line under the report: whether the crash log was saved. */
void panic_screen_status(const char *line);

/* Walks the frame-pointer chain from `rbp`, keeping return addresses in the
   kernel's text; every frame is checked to be mapped before it is read, so a
   wild pointer ends the trace rather than faulting the panic. Returns how
   many were written to `pcs`. (arch/x86_64/idt.c) */
unsigned panic_stack_walk(uint64_t rbp, uint64_t *pcs, unsigned max);

/* The crash test (cxk verify-crashlog): make the panic have to fight for the
   screen, as it would over a display server still drawing on another CPU. */
void panic_test_prepare(void);

#endif
