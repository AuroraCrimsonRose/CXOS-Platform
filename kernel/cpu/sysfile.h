// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/sysfile.h */
/* Aurora Tejeda / CATX Systems */
/*
 * SYS_FILE_* - the filesystem as ring 3 sees it (ABI v3).
 *
 * Until now CXFS was a complete filesystem no program could reach: the kernel
 * had create, read, write, delete, rename, directories, permissions and
 * advisory locking, and the syscall table had console, input, framebuffer,
 * network, spawn and IPC but nothing for files. GRANT_DISK was defined in
 * cxk_abi.h and referenced by no syscall at all. This fills that slot.
 *
 * One function per syscall since roadmap step 6a2c (ABI 9), its arguments in
 * registers; until then one SYS_FILE_OP took a *file_op_args. Pointers arrive
 * as the user addresses they are, and each function checks and copies its own.
 *
 * An open file is a KOBJ_FILE object in the calling process's handle table, so
 * SYS_HANDLE_CLOSE releases one as readily as SYS_FILE_CLOSE, and a process
 * that exits without closing has its open files released by the scheduler's
 * reaper.
 */

#ifndef SYSFILE_H
#define SYSFILE_H

#include "cxk_abi.h"
#include <stdint.h>

/* The SYS_FILE_* handlers, each called after the GRANT_DISK check; the ABI
   comments in abi/kernel.xfil give each one's arguments and answer. */
int sys_file_open(uint32_t path, uint32_t flags);
int sys_file_close(int32_t h);
int sys_file_read(int32_t h, uint32_t buf, uint32_t n);
int sys_file_write(int32_t h, uint32_t buf, uint32_t n);
int sys_file_seek(int32_t h, int32_t off, uint32_t origin);
int sys_file_tell(int32_t h);
int sys_file_truncate(int32_t h, int32_t size);
int sys_file_stat(uint32_t path, uint32_t st);
int sys_file_fstat(int32_t h, uint32_t st);
int sys_file_readdir(uint32_t path, uint32_t index, uint32_t st);
int sys_file_mkdir(uint32_t path);
int sys_file_unlink(uint32_t path);
int sys_file_rename(uint32_t path, uint32_t newname);
int sys_file_chdir(uint32_t path);
int sys_file_getcwd(uint32_t buf, uint32_t cap);
int sys_file_sync(void);
int sys_file_lock(int32_t h, int32_t start, uint32_t len, uint32_t mode);
int sys_file_unlock(int32_t h, int32_t start, uint32_t len);

/* register the open-file object type; call once during init. */
void sysfile_init(void);

/* how many open files are in use - for the self-tests to prove handles are
   actually being released rather than leaked. */
int sysfile_open_count(void);

/* 1 if this CXFS entry was unlinked while open: its name is gone to every
   lookup, and the entry itself goes at its last close. Anything else that
   resolves paths for a program (exec_path) must treat it as absent too. */
int sysfile_entry_doomed(uint32_t id);

#endif
