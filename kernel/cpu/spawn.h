// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/spawn.h */
/* Aurora Tejeda / CATX Systems */
/* ABI v1 process launch. proc_start is the one path that turns a CXEX image into
   a ring-3 scheduler thread (own space + esp0 + caps). cxex_exec and sys_spawn
   both use it. struct spawn_args lives in the shared public ABI header. */

#ifndef SPAWN_H
#define SPAWN_H

#include "cxk_abi.h"
#include <stdint.h>

struct endpoint;   /* cpu/ipc.h */

/* create a ring-3 thread from a CXEX image readable in the current space.
   caps = its capabilities; broker = an endpoint to give it a SEND handle on,
   derived from broker_grant (or NULL for a root executive). args/args_len are the argument blob described in
   cxk_abi.h, also read in the current space; pass NULL/0 for none. If `handle`
   is given, the calling process gets a handle on the new one (HRIGHT_WAIT,
   HRIGHT_INSPECT, HRIGHT_DUPLICATE) in *handle, or nothing starts. Returns pid,
   or a negative ABI error. */
int proc_start(const void *image, uint32_t image_len, uint32_t caps,
               struct endpoint *broker, uint32_t broker_grant,
               const char *args, uint32_t args_len, int32_t *handle);

/* SYS_SPAWN handler (called after the GRANT_SPAWN check). Returns a handle on
   the new process, or a negative ABI error. */
int sys_spawn(uint64_t ua);

/* SYS_EXEC_PATH handler (called after the GRANT_SPAWN check). Reads the CXEX at
   `upath` from CXFS inside the kernel and runs it verified; the image fields of
   *ua are ignored. Returns a handle on the new process, or a negative ABI error. */
int sys_exec_path(uint64_t upath, uint64_t ua);

#endif