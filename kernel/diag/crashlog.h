// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/diag/crashlog.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The crash log (docs/kernel/CX_KERNEL_PLAN.md section 8, roadmap step 4).
 *
 * A panic writes what it put on the screen, and the tail of the system log,
 * to the CRASH partition, and the next boot copies it to /System/crash.log.
 * RAM is not trusted to carry it across: a BIOS may clear or test memory
 * while it starts.
 *
 * The panic path takes no interrupts, allocates nothing and touches no
 * filesystem. The text is gathered in a static buffer as the panic screen is
 * drawn, and written with the disk drivers' polled transfers, whose waits are
 * bounded by a spin count when interrupts are off (timer.h). Which disk and
 * where are settled at boot, so the panic does not search.
 *
 * Only IDE and SATA disks carry the area for now. A USB disk (the rig boots
 * from a stick) needs a polled path through its host controller that does not
 * exist yet; such a disk is passed over, and the boot says so.
 */

#ifndef DIAG_CRASHLOG_H
#define DIAG_CRASHLOG_H

#include <stdint.h>

/* At boot, once the disks are registered and the system volume is mounted:
   find the CRASH partition, copy a previous boot's record to
   /System/crash.log and clear it, and take note of a test request. */
void crashlog_boot(void);

/* The panic screen's characters, as it draws them. Kept until the buffer is
   full; later characters are dropped. */
void crashlog_putc(char c);

/* At panic: add the system log's tail and write the record. 0 when written,
   negative when not (no area, a disk error, or a panic inside this one). */
int crashlog_panic_save(void);

/* Did `cxk verify-crashlog` ask this boot to panic on purpose? */
int crashlog_test_requested(void);

/* Did this boot copy a previous boot's record to /System/crash.log? 1 with the
   text's length and CRC-32 (xcrl_crc32), so the self-test can read the file
   back against them; 0 if not. */
int crashlog_recovered(uint32_t *len, uint32_t *crc);

/* For the self-test: where the area is. 0, or -1 if there is none. */
int crashlog_area(uint8_t *disk, uint64_t *lba, uint64_t *sectors);

/* Write a record of `kind` with `len` bytes of `text` to the area: the text,
   then the header, then a flush. A disk_err code. `text` must hold whole
   sectors (zero past `len`). Shared by the panic and the self-test, so the
   test exercises the same writes. */
int crashlog_write_record(uint16_t kind, const char *text, uint32_t len);

/* Zero the header, so the area reads as empty. A disk_err code. */
int crashlog_clear(void);

#endif
