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
static int decode_net_status(const uint32_t *a) { (void)a; return syscall_net_status(); }
static int decode_net_mac(const uint32_t *a) { return syscall_net_mac(a[0], a[1]); }
static int decode_net_get_config(const uint32_t *a) { return syscall_net_get_config(a[0]); }
static int decode_net_set_ip(const uint32_t *a) { return syscall_net_set_ip(a[0]); }
static int decode_net_set_mask(const uint32_t *a) { return syscall_net_set_mask(a[0]); }
static int decode_net_set_gateway(const uint32_t *a) { return syscall_net_set_gateway(a[0]); }
static int decode_net_set_dns(const uint32_t *a) { return syscall_net_set_dns(a[0]); }
static int decode_net_ping(const uint32_t *a) { return syscall_net_ping(a[0], a[1], a[2]); }
static int decode_net_send(const uint32_t *a) { return syscall_net_send(a[0], a[1]); }
static int decode_net_recv(const uint32_t *a) { return syscall_net_recv(a[0], a[1]); }
static int decode_fb_info(const uint32_t *a) { return syscall_fb_info(a[0]); }
static int decode_fb_clear(const uint32_t *a) { return syscall_fb_clear(a[0]); }
static int decode_fb_fill_rect(const uint32_t *a) { return syscall_fb_fill_rect(a[0], a[1], a[2], a[3], a[4]); }
static int decode_fb_put_pixel(const uint32_t *a) { return syscall_fb_put_pixel(a[0], a[1], a[2]); }
static int decode_fb_draw_line(const uint32_t *a) { return syscall_fb_draw_line(a[0], a[1], a[2], a[3], a[4]); }
static int decode_fb_draw_text(const uint32_t *a) { return syscall_fb_draw_text(a[0], a[1], a[2], a[3], a[4]); }
static int decode_fb_set_font(const uint32_t *a) { return syscall_fb_set_font(a[0]); }
static int decode_spawn(const uint32_t *a) { return syscall_spawn(a[0]); }
static int decode_exec_path(const uint32_t *a) { return syscall_exec_path(a[0], a[1]); }
static int decode_vm_map(const uint32_t *a) { return syscall_vm_map(a[0]); }
static int decode_vm_unmap(const uint32_t *a) { return syscall_vm_unmap(a[0], a[1]); }
static int decode_vm_info(const uint32_t *a) { return syscall_vm_info(a[0]); }
static int decode_power_reboot(const uint32_t *a) { (void)a; return syscall_power_reboot(); }
static int decode_power_shutdown(const uint32_t *a) { (void)a; return syscall_power_shutdown(); }
static int decode_power_sleep(const uint32_t *a) { return syscall_power_sleep(a[0]); }
static int decode_file_open(const uint32_t *a) { return syscall_file_open(a[0], a[1]); }
static int decode_file_close(const uint32_t *a) { return syscall_file_close((int32_t)a[0]); }
static int decode_file_read(const uint32_t *a) { return syscall_file_read((int32_t)a[0], a[1], a[2]); }
static int decode_file_write(const uint32_t *a) { return syscall_file_write((int32_t)a[0], a[1], a[2]); }
static int decode_file_seek(const uint32_t *a) { return syscall_file_seek((int32_t)a[0], (int32_t)a[1], a[2]); }
static int decode_file_tell(const uint32_t *a) { return syscall_file_tell((int32_t)a[0]); }
static int decode_file_truncate(const uint32_t *a) { return syscall_file_truncate((int32_t)a[0], (int32_t)a[1]); }
static int decode_file_stat(const uint32_t *a) { return syscall_file_stat(a[0], a[1]); }
static int decode_file_fstat(const uint32_t *a) { return syscall_file_fstat((int32_t)a[0], a[1]); }
static int decode_file_readdir(const uint32_t *a) { return syscall_file_readdir(a[0], a[1], a[2]); }
static int decode_file_mkdir(const uint32_t *a) { return syscall_file_mkdir(a[0]); }
static int decode_file_unlink(const uint32_t *a) { return syscall_file_unlink(a[0]); }
static int decode_file_rename(const uint32_t *a) { return syscall_file_rename(a[0], a[1]); }
static int decode_file_chdir(const uint32_t *a) { return syscall_file_chdir(a[0]); }
static int decode_file_getcwd(const uint32_t *a) { return syscall_file_getcwd(a[0], a[1]); }
static int decode_file_sync(const uint32_t *a) { (void)a; return syscall_file_sync(); }
static int decode_file_lock(const uint32_t *a) { return syscall_file_lock((int32_t)a[0], (int32_t)a[1], a[2], a[3]); }
static int decode_file_unlock(const uint32_t *a) { return syscall_file_unlock((int32_t)a[0], (int32_t)a[1], a[2]); }

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
    [SYS_FB_INFO] = decode_fb_info,
    [SYS_FB_CLEAR] = decode_fb_clear,
    [SYS_FB_FILL_RECT] = decode_fb_fill_rect,
    [SYS_FB_PUT_PIXEL] = decode_fb_put_pixel,
    [SYS_FB_DRAW_LINE] = decode_fb_draw_line,
    [SYS_FB_DRAW_TEXT] = decode_fb_draw_text,
    [SYS_FB_SET_FONT] = decode_fb_set_font,
    [SYS_NET_STATUS] = decode_net_status,
    [SYS_NET_MAC] = decode_net_mac,
    [SYS_NET_GET_CONFIG] = decode_net_get_config,
    [SYS_NET_SET_IP] = decode_net_set_ip,
    [SYS_NET_SET_MASK] = decode_net_set_mask,
    [SYS_NET_SET_GATEWAY] = decode_net_set_gateway,
    [SYS_NET_SET_DNS] = decode_net_set_dns,
    [SYS_NET_PING] = decode_net_ping,
    [SYS_NET_SEND] = decode_net_send,
    [SYS_NET_RECV] = decode_net_recv,
    [SYS_SPAWN] = decode_spawn,
    [SYS_EXEC_PATH] = decode_exec_path,
    [SYS_VM_MAP] = decode_vm_map,
    [SYS_VM_UNMAP] = decode_vm_unmap,
    [SYS_VM_INFO] = decode_vm_info,
    [SYS_POWER_REBOOT] = decode_power_reboot,
    [SYS_POWER_SHUTDOWN] = decode_power_shutdown,
    [SYS_POWER_SLEEP] = decode_power_sleep,
    [SYS_FILE_OPEN] = decode_file_open,
    [SYS_FILE_CLOSE] = decode_file_close,
    [SYS_FILE_READ] = decode_file_read,
    [SYS_FILE_WRITE] = decode_file_write,
    [SYS_FILE_SEEK] = decode_file_seek,
    [SYS_FILE_TELL] = decode_file_tell,
    [SYS_FILE_TRUNCATE] = decode_file_truncate,
    [SYS_FILE_STAT] = decode_file_stat,
    [SYS_FILE_FSTAT] = decode_file_fstat,
    [SYS_FILE_READDIR] = decode_file_readdir,
    [SYS_FILE_MKDIR] = decode_file_mkdir,
    [SYS_FILE_UNLINK] = decode_file_unlink,
    [SYS_FILE_RENAME] = decode_file_rename,
    [SYS_FILE_CHDIR] = decode_file_chdir,
    [SYS_FILE_GETCWD] = decode_file_getcwd,
    [SYS_FILE_SYNC] = decode_file_sync,
    [SYS_FILE_LOCK] = decode_file_lock,
    [SYS_FILE_UNLOCK] = decode_file_unlock,
};
