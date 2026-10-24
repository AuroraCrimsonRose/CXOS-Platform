// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_64/early_con.h */
/* Aurora Tejeda / CATX Systems */
/* The x86-64 kernel's first output: COM1, polled, no buffer, no lock. It is
   what step 5b has before the generic kernel (klog, the serial driver behind
   an iowin) runs on x86-64; lines keep klog's shape, so the serial log reads,
   and cxk verify-boot judges it, the same way. */

#ifndef ARCH_X86_64_EARLY_CON_H
#define ARCH_X86_64_EARLY_CON_H

#include <stdint.h>

#define EARLY_OK    "OK  "
#define EARLY_FAIL  "FAIL"
#define EARLY_INFO  "INFO"

void early_con_init(void);
void early_putc(char c);
void early_puts(const char *s);
void early_hex(uint64_t v);         /* 0x and 16 digits */
void early_dec(uint64_t v);

/* "[TAG     ] - [SEV ] - " with no newline, so a value can follow; then
   early_puts("\n"). early_log is that plus the message and the newline. */
void early_log_start(const char *tag, const char *sev);
void early_log(const char *tag, const char *sev, const char *msg);

#endif
