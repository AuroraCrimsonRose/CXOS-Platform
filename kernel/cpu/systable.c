// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* kernel/cpu/systable.c: GENERATED from abi/kernel.xfil (cx.kernel) by `cxk abi generate`.
 * Do not edit it: change abi/kernel.xfil and regenerate. `cxk check-abi` fails while
 * this file differs from what the .xfil generates. */

#include "systable.h"
#include "cxk_abi.h"   /* the SYS_* numbers */

static int decode_exit(const uintptr_t *a) { syscall_exit((int32_t)a[0]); return 0; }
static int decode_yield(const uintptr_t *a) { (void)a; syscall_yield(); return 0; }
static int decode_getpid(const uintptr_t *a) { (void)a; return syscall_getpid(); }
static int decode_getuid(const uintptr_t *a) { (void)a; return syscall_getuid(); }
static int decode_clock(const uintptr_t *a) { return syscall_clock((uint64_t)a[0]); }
static int decode_sleep(const uintptr_t *a) { return syscall_sleep((uint32_t)a[0]); }
static int decode_args(const uintptr_t *a) { return syscall_args((uint64_t)a[0]); }
static int decode_clock_monotonic(const uintptr_t *a) { return syscall_clock_monotonic((uint64_t)a[0]); }
static int decode_thread_set_priority(const uintptr_t *a) { return syscall_thread_set_priority((uint32_t)a[0]); }
static int decode_thread_priority(const uintptr_t *a) { (void)a; return syscall_thread_priority(); }
static int decode_args_handle(const uintptr_t *a) { return syscall_args_handle((uint32_t)a[0]); }
static int decode_ipc_call(const uintptr_t *a) { return syscall_ipc_call((uint64_t)a[0]); }
static int decode_ipc_recv(const uintptr_t *a) { return syscall_ipc_recv((uint64_t)a[0]); }
static int decode_ipc_reply(const uintptr_t *a) { return syscall_ipc_reply((uint64_t)a[0]); }
static int decode_ep_create(const uintptr_t *a) { (void)a; return syscall_ep_create(); }
static int decode_ep_mint(const uintptr_t *a) {
#if SYSCALL_PAIRS
    return syscall_ep_mint((int32_t)a[0], (uint64_t)((uint64_t)a[1] | (uint64_t)a[2] << 32), (uint32_t)a[3]);
#else
    return syscall_ep_mint((int32_t)a[0], (uint64_t)a[1], (uint32_t)a[2]);
#endif
}
static int decode_handle_close(const uintptr_t *a) { return syscall_handle_close((int32_t)a[0]); }
static int decode_handle_duplicate(const uintptr_t *a) { return syscall_handle_duplicate((int32_t)a[0], (uint32_t)a[1]); }
static int decode_object_info(const uintptr_t *a) { return syscall_object_info((uint64_t)a[0]); }
static int decode_object_wait(const uintptr_t *a) {
#if SYSCALL_PAIRS
    return syscall_object_wait((int32_t)a[0], (uint32_t)a[1], (uint64_t)((uint64_t)a[2] | (uint64_t)a[3] << 32), (uint64_t)a[4]);
#else
    return syscall_object_wait((int32_t)a[0], (uint32_t)a[1], (uint64_t)a[2], (uint64_t)a[3]);
#endif
}
static int decode_timer_create(const uintptr_t *a) {
#if SYSCALL_PAIRS
    return syscall_timer_create((uint64_t)((uint64_t)a[0] | (uint64_t)a[1] << 32));
#else
    return syscall_timer_create((uint64_t)a[0]);
#endif
}
static int decode_timer_set(const uintptr_t *a) {
#if SYSCALL_PAIRS
    return syscall_timer_set((int32_t)a[0], (uint64_t)((uint64_t)a[1] | (uint64_t)a[2] << 32));
#else
    return syscall_timer_set((int32_t)a[0], (uint64_t)a[1]);
#endif
}
static int decode_timer_cancel(const uintptr_t *a) { return syscall_timer_cancel((int32_t)a[0]); }
static int decode_notify_create(const uintptr_t *a) { (void)a; return syscall_notify_create(); }
static int decode_notify_mint(const uintptr_t *a) {
#if SYSCALL_PAIRS
    return syscall_notify_mint((int32_t)a[0], (uint64_t)((uint64_t)a[1] | (uint64_t)a[2] << 32), (uint32_t)a[3]);
#else
    return syscall_notify_mint((int32_t)a[0], (uint64_t)a[1], (uint32_t)a[2]);
#endif
}
static int decode_notify_signal(const uintptr_t *a) {
#if SYSCALL_PAIRS
    return syscall_notify_signal((int32_t)a[0], (uint64_t)((uint64_t)a[1] | (uint64_t)a[2] << 32));
#else
    return syscall_notify_signal((int32_t)a[0], (uint64_t)a[1]);
#endif
}
static int decode_notify_take(const uintptr_t *a) { return syscall_notify_take((int32_t)a[0], (uint64_t)a[1]); }
static int decode_queue_create(const uintptr_t *a) { (void)a; return syscall_queue_create(); }
static int decode_queue_register(const uintptr_t *a) {
#if SYSCALL_PAIRS
    return syscall_queue_register((int32_t)a[0], (int32_t)a[1], (uint32_t)a[2], (uint64_t)((uint64_t)a[3] | (uint64_t)a[4] << 32));
#else
    return syscall_queue_register((int32_t)a[0], (int32_t)a[1], (uint32_t)a[2], (uint64_t)a[3]);
#endif
}
static int decode_queue_arm(const uintptr_t *a) {
#if SYSCALL_PAIRS
    return syscall_queue_arm((int32_t)a[0], (uint64_t)((uint64_t)a[1] | (uint64_t)a[2] << 32));
#else
    return syscall_queue_arm((int32_t)a[0], (uint64_t)a[1]);
#endif
}
static int decode_queue_unregister(const uintptr_t *a) {
#if SYSCALL_PAIRS
    return syscall_queue_unregister((int32_t)a[0], (uint64_t)((uint64_t)a[1] | (uint64_t)a[2] << 32));
#else
    return syscall_queue_unregister((int32_t)a[0], (uint64_t)a[1]);
#endif
}
static int decode_queue_wait(const uintptr_t *a) {
#if SYSCALL_PAIRS
    return syscall_queue_wait((int32_t)a[0], (uint64_t)a[1], (uint32_t)a[2], (uint64_t)((uint64_t)a[3] | (uint64_t)a[4] << 32));
#else
    return syscall_queue_wait((int32_t)a[0], (uint64_t)a[1], (uint32_t)a[2], (uint64_t)a[3]);
#endif
}
static int decode_input_read(const uintptr_t *a) { (void)a; return syscall_input_read(); }
static int decode_mouse_read(const uintptr_t *a) { return syscall_mouse_read((uint64_t)a[0]); }
static int decode_input_poll(const uintptr_t *a) { (void)a; return syscall_input_poll(); }
static int decode_console_write(const uintptr_t *a) {
#if SYSCALL_PAIRS
    return syscall_console_write((uint64_t)a[0], (uint64_t)((uint64_t)a[1] | (uint64_t)a[2] << 32));
#else
    return syscall_console_write((uint64_t)a[0], (uint64_t)a[1]);
#endif
}
static int decode_log_read(const uintptr_t *a) { return syscall_log_read((uint32_t)a[0], (uint64_t)a[1]); }
static int decode_net_status(const uintptr_t *a) { (void)a; return syscall_net_status(); }
static int decode_net_mac(const uintptr_t *a) {
#if SYSCALL_PAIRS
    return syscall_net_mac((uint64_t)a[0], (uint64_t)((uint64_t)a[1] | (uint64_t)a[2] << 32));
#else
    return syscall_net_mac((uint64_t)a[0], (uint64_t)a[1]);
#endif
}
static int decode_net_get_config(const uintptr_t *a) { return syscall_net_get_config((uint64_t)a[0]); }
static int decode_net_set_ip(const uintptr_t *a) { return syscall_net_set_ip((uint32_t)a[0]); }
static int decode_net_set_mask(const uintptr_t *a) { return syscall_net_set_mask((uint32_t)a[0]); }
static int decode_net_set_gateway(const uintptr_t *a) { return syscall_net_set_gateway((uint32_t)a[0]); }
static int decode_net_set_dns(const uintptr_t *a) { return syscall_net_set_dns((uint32_t)a[0]); }
static int decode_net_ping(const uintptr_t *a) { return syscall_net_ping((uint32_t)a[0], (uint32_t)a[1], (uint64_t)a[2]); }
static int decode_net_send(const uintptr_t *a) {
#if SYSCALL_PAIRS
    return syscall_net_send((uint64_t)a[0], (uint64_t)((uint64_t)a[1] | (uint64_t)a[2] << 32));
#else
    return syscall_net_send((uint64_t)a[0], (uint64_t)a[1]);
#endif
}
static int decode_net_recv(const uintptr_t *a) {
#if SYSCALL_PAIRS
    return syscall_net_recv((uint64_t)a[0], (uint64_t)((uint64_t)a[1] | (uint64_t)a[2] << 32));
#else
    return syscall_net_recv((uint64_t)a[0], (uint64_t)a[1]);
#endif
}
static int decode_fb_info(const uintptr_t *a) { return syscall_fb_info((uint64_t)a[0]); }
static int decode_fb_clear(const uintptr_t *a) { return syscall_fb_clear((uint32_t)a[0]); }
static int decode_fb_fill_rect(const uintptr_t *a) { return syscall_fb_fill_rect((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2], (uint32_t)a[3], (uint32_t)a[4]); }
static int decode_fb_put_pixel(const uintptr_t *a) { return syscall_fb_put_pixel((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2]); }
static int decode_fb_draw_line(const uintptr_t *a) { return syscall_fb_draw_line((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2], (uint32_t)a[3], (uint32_t)a[4]); }
static int decode_fb_draw_text(const uintptr_t *a) { return syscall_fb_draw_text((uint32_t)a[0], (uint32_t)a[1], (uint64_t)a[2], (uint32_t)a[3], (uint32_t)a[4]); }
static int decode_fb_set_font(const uintptr_t *a) { return syscall_fb_set_font((uint64_t)a[0]); }
static int decode_spawn(const uintptr_t *a) { return syscall_spawn((uint64_t)a[0]); }
static int decode_exec_path(const uintptr_t *a) { return syscall_exec_path((uint64_t)a[0], (uint64_t)a[1]); }
static int decode_process_kill(const uintptr_t *a) { return syscall_process_kill((int32_t)a[0]); }
static int decode_process_stop(const uintptr_t *a) { return syscall_process_stop((int32_t)a[0], (uint32_t)a[1]); }
static int decode_process_stop_endpoint(const uintptr_t *a) { return syscall_process_stop_endpoint((int32_t)a[0]); }
static int decode_job_create(const uintptr_t *a) { return syscall_job_create((int32_t)a[0], (uint64_t)a[1]); }
static int decode_job_set_limits(const uintptr_t *a) { return syscall_job_set_limits((int32_t)a[0], (uint64_t)a[1]); }
static int decode_job_kill(const uintptr_t *a) { return syscall_job_kill((int32_t)a[0]); }
static int decode_vm_map(const uintptr_t *a) { return syscall_vm_map((uint64_t)a[0]); }
static int decode_vm_unmap(const uintptr_t *a) {
#if SYSCALL_PAIRS
    return syscall_vm_unmap((uint64_t)((uint64_t)a[0] | (uint64_t)a[1] << 32), (uint64_t)((uint64_t)a[2] | (uint64_t)a[3] << 32));
#else
    return syscall_vm_unmap((uint64_t)a[0], (uint64_t)a[1]);
#endif
}
static int decode_vm_info(const uintptr_t *a) { return syscall_vm_info((uint64_t)a[0]); }
static int decode_mem_create(const uintptr_t *a) {
#if SYSCALL_PAIRS
    return syscall_mem_create((uint64_t)((uint64_t)a[0] | (uint64_t)a[1] << 32), (uint32_t)a[2]);
#else
    return syscall_mem_create((uint64_t)a[0], (uint32_t)a[1]);
#endif
}
static int decode_mem_seal(const uintptr_t *a) { return syscall_mem_seal((int32_t)a[0], (uint32_t)a[1]); }
static int decode_power_reboot(const uintptr_t *a) { (void)a; return syscall_power_reboot(); }
static int decode_power_shutdown(const uintptr_t *a) { (void)a; return syscall_power_shutdown(); }
static int decode_power_sleep(const uintptr_t *a) { return syscall_power_sleep((uint32_t)a[0]); }
static int decode_file_open(const uintptr_t *a) { return syscall_file_open((uint64_t)a[0], (uint32_t)a[1]); }
static int decode_file_close(const uintptr_t *a) { return syscall_file_close((int32_t)a[0]); }
static int decode_file_read(const uintptr_t *a) {
#if SYSCALL_PAIRS
    return syscall_file_read((int32_t)a[0], (uint64_t)a[1], (uint64_t)((uint64_t)a[2] | (uint64_t)a[3] << 32));
#else
    return syscall_file_read((int32_t)a[0], (uint64_t)a[1], (uint64_t)a[2]);
#endif
}
static int decode_file_write(const uintptr_t *a) {
#if SYSCALL_PAIRS
    return syscall_file_write((int32_t)a[0], (uint64_t)a[1], (uint64_t)((uint64_t)a[2] | (uint64_t)a[3] << 32));
#else
    return syscall_file_write((int32_t)a[0], (uint64_t)a[1], (uint64_t)a[2]);
#endif
}
static int decode_file_seek(const uintptr_t *a) {
#if SYSCALL_PAIRS
    return syscall_file_seek((int32_t)a[0], (int64_t)((uint64_t)a[1] | (uint64_t)a[2] << 32), (uint32_t)a[3], (uint64_t)a[4]);
#else
    return syscall_file_seek((int32_t)a[0], (int64_t)a[1], (uint32_t)a[2], (uint64_t)a[3]);
#endif
}
static int decode_file_tell(const uintptr_t *a) { return syscall_file_tell((int32_t)a[0], (uint64_t)a[1]); }
static int decode_file_truncate(const uintptr_t *a) {
#if SYSCALL_PAIRS
    return syscall_file_truncate((int32_t)a[0], (uint64_t)((uint64_t)a[1] | (uint64_t)a[2] << 32));
#else
    return syscall_file_truncate((int32_t)a[0], (uint64_t)a[1]);
#endif
}
static int decode_file_stat(const uintptr_t *a) { return syscall_file_stat((uint64_t)a[0], (uint64_t)a[1]); }
static int decode_file_fstat(const uintptr_t *a) { return syscall_file_fstat((int32_t)a[0], (uint64_t)a[1]); }
static int decode_file_readdir(const uintptr_t *a) { return syscall_file_readdir((uint64_t)a[0], (uint32_t)a[1], (uint64_t)a[2]); }
static int decode_file_mkdir(const uintptr_t *a) { return syscall_file_mkdir((uint64_t)a[0]); }
static int decode_file_unlink(const uintptr_t *a) { return syscall_file_unlink((uint64_t)a[0]); }
static int decode_file_rename(const uintptr_t *a) { return syscall_file_rename((uint64_t)a[0], (uint64_t)a[1]); }
static int decode_file_chdir(const uintptr_t *a) { return syscall_file_chdir((uint64_t)a[0]); }
static int decode_file_getcwd(const uintptr_t *a) {
#if SYSCALL_PAIRS
    return syscall_file_getcwd((uint64_t)a[0], (uint64_t)((uint64_t)a[1] | (uint64_t)a[2] << 32));
#else
    return syscall_file_getcwd((uint64_t)a[0], (uint64_t)a[1]);
#endif
}
static int decode_file_sync(const uintptr_t *a) { (void)a; return syscall_file_sync(); }
static int decode_file_lock(const uintptr_t *a) { return syscall_file_lock((int32_t)a[0], (uint64_t)a[1]); }
static int decode_file_unlock(const uintptr_t *a) {
#if SYSCALL_PAIRS
    return syscall_file_unlock((int32_t)a[0], (uint64_t)((uint64_t)a[1] | (uint64_t)a[2] << 32), (uint64_t)((uint64_t)a[3] | (uint64_t)a[4] << 32));
#else
    return syscall_file_unlock((int32_t)a[0], (uint64_t)a[1], (uint64_t)a[2]);
#endif
}
static int decode_pipe_create(const uintptr_t *a) { return syscall_pipe_create((uint32_t)a[0], (uint64_t)a[1]); }

const syscall_decoder syscall_table[SYSCALL_TABLE_SIZE] = {
    [SYS_EXIT] = decode_exit,
    [SYS_YIELD] = decode_yield,
    [SYS_GETPID] = decode_getpid,
    [SYS_GETUID] = decode_getuid,
    [SYS_CLOCK] = decode_clock,
    [SYS_SLEEP] = decode_sleep,
    [SYS_ARGS] = decode_args,
    [SYS_CLOCK_MONOTONIC] = decode_clock_monotonic,
    [SYS_THREAD_SET_PRIORITY] = decode_thread_set_priority,
    [SYS_THREAD_PRIORITY] = decode_thread_priority,
    [SYS_ARGS_HANDLE] = decode_args_handle,
    [SYS_IPC_CALL] = decode_ipc_call,
    [SYS_IPC_RECV] = decode_ipc_recv,
    [SYS_IPC_REPLY] = decode_ipc_reply,
    [SYS_EP_CREATE] = decode_ep_create,
    [SYS_EP_MINT] = decode_ep_mint,
    [SYS_NOTIFY_CREATE] = decode_notify_create,
    [SYS_HANDLE_CLOSE] = decode_handle_close,
    [SYS_HANDLE_DUPLICATE] = decode_handle_duplicate,
    [SYS_OBJECT_INFO] = decode_object_info,
    [SYS_OBJECT_WAIT] = decode_object_wait,
    [SYS_TIMER_CREATE] = decode_timer_create,
    [SYS_TIMER_SET] = decode_timer_set,
    [SYS_TIMER_CANCEL] = decode_timer_cancel,
    [SYS_NOTIFY_MINT] = decode_notify_mint,
    [SYS_NOTIFY_SIGNAL] = decode_notify_signal,
    [SYS_NOTIFY_TAKE] = decode_notify_take,
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
    [SYS_QUEUE_CREATE] = decode_queue_create,
    [SYS_QUEUE_REGISTER] = decode_queue_register,
    [SYS_QUEUE_ARM] = decode_queue_arm,
    [SYS_QUEUE_UNREGISTER] = decode_queue_unregister,
    [SYS_QUEUE_WAIT] = decode_queue_wait,
    [SYS_SPAWN] = decode_spawn,
    [SYS_PROCESS_KILL] = decode_process_kill,
    [SYS_EXEC_PATH] = decode_exec_path,
    [SYS_PROCESS_STOP] = decode_process_stop,
    [SYS_PROCESS_STOP_ENDPOINT] = decode_process_stop_endpoint,
    [SYS_JOB_CREATE] = decode_job_create,
    [SYS_JOB_SET_LIMITS] = decode_job_set_limits,
    [SYS_JOB_KILL] = decode_job_kill,
    [SYS_VM_MAP] = decode_vm_map,
    [SYS_VM_UNMAP] = decode_vm_unmap,
    [SYS_VM_INFO] = decode_vm_info,
    [SYS_MEM_CREATE] = decode_mem_create,
    [SYS_MEM_SEAL] = decode_mem_seal,
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
    [SYS_PIPE_CREATE] = decode_pipe_create,
};
