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
 * The area can be on an IDE, SATA or USB disk; the rig has no other disk
 * than the stick it boots from. The USB drivers are polled already, but a
 * panic can land in the middle of a USB transaction, leaving the controller
 * and the stick each mid-command, so the save first does the Bulk-Only reset
 * recovery when one is needed (disk_panic_reset), and the record says so.
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

/* Just before that deliberate panic: on a USB disk, leave a transfer to the
   area's disk half done (disk_test_interrupt), so the test covers the save
   recovering from one. Nothing on other disks. */
void crashlog_test_interrupt(void);

/* The line a save puts in the record when it had to reset the disk first;
   `cxk verify-crashlog --usb` looks for it. */
#define CRASHLOG_RESET_LINE "crash log: the disk was mid-transfer; reset before this save"

/* Did this boot copy a previous boot's record to /System/crash.log? 1 with the
   text's length and CRC-32 (xcrl_crc32), so the self-test can read the file
   back against them; 0 if not. */
int crashlog_recovered(uint32_t *len, uint32_t *crc);

/* For the self-test: where the area is. 0, or -1 if there is none. */
int crashlog_area(uint8_t *disk, uint64_t *lba, uint64_t *sectors);

/* The whole system log, as text, into the crash partition after the crash
   record (XCRL_BOOTLOG_LBA), for `cxk rig bootlog`: how a machine with no
   serial port gets its log to another. Interrupts off while it writes.
   DISK_OK, or the disk error. crashlog_bootlog_stale says whether anything
   was logged since the last save. */
int crashlog_save_bootlog(void);
int crashlog_bootlog_stale(void);

/* Write a record of `kind` with `len` bytes of `text` to the area: the text,
   then the header, then a flush. A disk_err code. `text` must hold whole
   sectors (zero past `len`). Shared by the panic and the self-test, so the
   test exercises the same writes. */
int crashlog_write_record(uint16_t kind, const char *text, uint32_t len);

/* Zero the header, so the area reads as empty. A disk_err code. */
int crashlog_clear(void);

/* ---- user faults ----
   A user process that faults is ended, not a panic, so the crash log would
   hold nothing; on the rig the screen was the only record. These save them.
   arm:    on once the self-tests are done (their faults are deliberate).
   note:   from the fault handler: one line about the fault into a static
           buffer. No disk I/O, no allocation.
   save:   from the faulted process's own thread, once it is out of ring 3:
           write an XCRL_KIND_FAULT record of every fault noted so far and
           the system log's tail. Nothing when nothing is pending.
   saved:  how many saves were written. forget: drop the noted faults (the
           self-test, after its own). */
void     crashlog_faults_arm(int on);
void     crashlog_note_fault(const char *line);
void     crashlog_save_faults(void);
uint32_t crashlog_faults_saved(void);
void     crashlog_faults_forget(void);

#endif
