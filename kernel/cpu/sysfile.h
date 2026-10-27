// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/sysfile.h */
/* Aurora Tejeda / CATX Systems */
/*
 * SYS_FILE_OP - the filesystem as ring 3 sees it (ABI v3).
 *
 * Until now CXFS was a complete filesystem no program could reach: the kernel
 * had create, read, write, delete, rename, directories, permissions and
 * advisory locking, and the syscall table had console, input, framebuffer,
 * network, spawn and IPC but nothing for files. GRANT_DISK was defined in
 * cxk_abi.h and referenced by no syscall at all. This fills that slot.
 *
 * Everything here is one syscall taking a *file_op_args, the shape every
 * family had before roadmap step 6a2c split them (this one is next).
 *
 * An open file is a KOBJ_FILE object in the calling process's handle table, so
 * SYS_HANDLE_CLOSE releases one as readily as FILE_OP_CLOSE, and a process that
 * exits without closing has its open files released by the scheduler's reaper.
 */

#ifndef SYSFILE_H
#define SYSFILE_H

#include "cxk_abi.h"
#include <stdint.h>

/* SYS_FILE_OP handler (called after the GRANT_DISK check). */
int sys_file_op(uint32_t ua);

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
