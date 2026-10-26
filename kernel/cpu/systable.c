// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* kernel/cpu/systable.c: GENERATED from abi/kernel.xfil (cx.kernel) by `cxk abi generate`.
 * Do not edit it: change abi/kernel.xfil and regenerate. `cxk check-abi` fails while
 * this file differs from what the .xfil generates. */

#include "systable.h"
#include "cxk_abi.h"   /* the SYS_* numbers */

static int decode_exit(const uint32_t *a) { syscall_exit((int32_t)a[0]); return 0; }
static int decode_yield(const uint32_t *a) { (void)a; syscall_yield(); return 0; }
static int decode_getpid(const uint32_t *a) { (void)a; return syscall_getpid(); }
static int decode_getuid(const uint32_t *a) { (void)a; return syscall_getuid(); }
static int decode_clock(const uint32_t *a) { return syscall_clock(a[0]); }
static int decode_sleep(const uint32_t *a) { return syscall_sleep(a[0]); }
static int decode_args(const uint32_t *a) { return syscall_args(a[0]); }
static int decode_ipc_call(const uint32_t *a) { return syscall_ipc_call(a[0]); }
static int decode_ipc_recv(const uint32_t *a) { return syscall_ipc_recv(a[0]); }
static int decode_ipc_reply(const uint32_t *a) { return syscall_ipc_reply(a[0]); }
static int decode_ep_create(const uint32_t *a) { (void)a; return syscall_ep_create(); }
static int decode_handle_close(const uint32_t *a) { return syscall_handle_close((int32_t)a[0]); }
static int decode_handle_duplicate(const uint32_t *a) { return syscall_handle_duplicate((int32_t)a[0], a[1]); }
static int decode_object_info(const uint32_t *a) { return syscall_object_info(a[0]); }
static int decode_input_read(const uint32_t *a) { (void)a; return syscall_input_read(); }
static int decode_mouse_read(const uint32_t *a) { return syscall_mouse_read(a[0]); }
static int decode_input_poll(const uint32_t *a) { (void)a; return syscall_input_poll(); }
static int decode_console_write(const uint32_t *a) { return syscall_console_write(a[0], a[1]); }
static int decode_log_read(const uint32_t *a) { return syscall_log_read(a[0], a[1]); }
static int decode_net_op(const uint32_t *a) { return syscall_net_op(a[0]); }
static int decode_fb_op(const uint32_t *a) { return syscall_fb_op(a[0]); }
static int decode_file_op(const uint32_t *a) { return syscall_file_op(a[0]); }
static int decode_spawn(const uint32_t *a) { return syscall_spawn(a[0]); }
static int decode_exec_path(const uint32_t *a) { return syscall_exec_path(a[0], a[1]); }
static int decode_vm_map(const uint32_t *a) { return syscall_vm_map(a[0]); }
static int decode_vm_unmap(const uint32_t *a) { return syscall_vm_unmap(a[0], a[1]); }
static int decode_vm_info(const uint32_t *a) { return syscall_vm_info(a[0]); }
static int decode_power_reboot(const uint32_t *a) { (void)a; return syscall_power_reboot(); }
static int decode_power_shutdown(const uint32_t *a) { (void)a; return syscall_power_shutdown(); }
static int decode_power_sleep(const uint32_t *a) { return syscall_power_sleep(a[0]); }

const syscall_decoder syscall_table[SYSCALL_TABLE_SIZE] = {
    [SYS_EXIT] = decode_exit,
    [SYS_YIELD] = decode_yield,
    [SYS_GETPID] = decode_getpid,
    [SYS_GETUID] = decode_getuid,
    [SYS_CLOCK] = decode_clock,
    [SYS_SLEEP] = decode_sleep,
    [SYS_ARGS] = decode_args,
    [SYS_IPC_CALL] = decode_ipc_call,
    [SYS_IPC_RECV] = decode_ipc_recv,
    [SYS_IPC_REPLY] = decode_ipc_reply,
    [SYS_EP_CREATE] = decode_ep_create,
    [SYS_HANDLE_CLOSE] = decode_handle_close,
    [SYS_HANDLE_DUPLICATE] = decode_handle_duplicate,
    [SYS_OBJECT_INFO] = decode_object_info,
    [SYS_INPUT_READ] = decode_input_read,
    [SYS_MOUSE_READ] = decode_mouse_read,
    [SYS_INPUT_POLL] = decode_input_poll,
    [SYS_CONSOLE_WRITE] = decode_console_write,
    [SYS_LOG_READ] = decode_log_read,
    [SYS_FB_OP] = decode_fb_op,
    [SYS_NET_OP] = decode_net_op,
    [SYS_FILE_OP] = decode_file_op,
    [SYS_SPAWN] = decode_spawn,
    [SYS_EXEC_PATH] = decode_exec_path,
    [SYS_VM_MAP] = decode_vm_map,
    [SYS_VM_UNMAP] = decode_vm_unmap,
    [SYS_VM_INFO] = decode_vm_info,
    [SYS_POWER_REBOOT] = decode_power_reboot,
    [SYS_POWER_SHUTDOWN] = decode_power_shutdown,
    [SYS_POWER_SLEEP] = decode_power_sleep,
};
