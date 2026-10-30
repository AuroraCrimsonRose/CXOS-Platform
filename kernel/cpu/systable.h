// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* kernel/cpu/systable.h: GENERATED from abi/kernel.xfil (cx.kernel) by `cxk abi generate`.
 * Do not edit it: change abi/kernel.xfil and regenerate. `cxk check-abi` fails while
 * this file differs from what the .xfil generates. */

#ifndef CXK_SYSTABLE_H
#define CXK_SYSTABLE_H

#include <stdint.h>

/* The handlers, one per syscall, named after it (SYS_FILE_READ: syscall_file_read).
 * Each is given its arguments as the interface declares them. A pointer arrives
 * as the user address it is, a uint64_t; the handler checks and copies it. A
 * u64 or i64 arrives whole: x86-32 carries it in two registers, low then high,
 * and its decoder joins them; x86-64 carries it in one. */

void syscall_exit(int32_t code);                /* SYS_EXIT */
void syscall_yield(void);                       /* SYS_YIELD */
int syscall_getpid(void);                       /* SYS_GETPID */
int syscall_getuid(void);                       /* SYS_GETUID */
int syscall_clock(uint64_t c);                  /* SYS_CLOCK */
int syscall_sleep(uint32_t ms);                 /* SYS_SLEEP */
int syscall_args(uint64_t a);                   /* SYS_ARGS */
int syscall_clock_monotonic(uint64_t now);      /* SYS_CLOCK_MONOTONIC */
int syscall_ipc_call(uint64_t a);               /* SYS_IPC_CALL */
int syscall_ipc_recv(uint64_t a);               /* SYS_IPC_RECV */
int syscall_ipc_reply(uint64_t a);              /* SYS_IPC_REPLY */
int syscall_ep_create(void);                    /* SYS_EP_CREATE */
int syscall_ep_mint(int32_t h, uint64_t badge, uint32_t rights); /* SYS_EP_MINT */
int syscall_handle_close(int32_t h);            /* SYS_HANDLE_CLOSE */
int syscall_handle_duplicate(int32_t h, uint32_t rights); /* SYS_HANDLE_DUPLICATE */
int syscall_object_info(uint64_t a);            /* SYS_OBJECT_INFO */
int syscall_object_wait(int32_t h, uint32_t signals, uint64_t deadline, uint64_t seen); /* SYS_OBJECT_WAIT */
int syscall_timer_create(uint64_t slack);       /* SYS_TIMER_CREATE */
int syscall_timer_set(int32_t h, uint64_t deadline); /* SYS_TIMER_SET */
int syscall_timer_cancel(int32_t h);            /* SYS_TIMER_CANCEL */
int syscall_notify_create(void);                /* SYS_NOTIFY_CREATE */
int syscall_notify_mint(int32_t h, uint64_t bits, uint32_t rights); /* SYS_NOTIFY_MINT */
int syscall_notify_signal(int32_t h, uint64_t bits); /* SYS_NOTIFY_SIGNAL */
int syscall_notify_take(int32_t h, uint64_t bits); /* SYS_NOTIFY_TAKE */
int syscall_queue_create(void);                 /* SYS_QUEUE_CREATE */
int syscall_queue_register(int32_t q, int32_t h, uint32_t signals, uint64_t key); /* SYS_QUEUE_REGISTER */
int syscall_queue_arm(int32_t q, uint64_t key); /* SYS_QUEUE_ARM */
int syscall_queue_unregister(int32_t q, uint64_t key); /* SYS_QUEUE_UNREGISTER */
int syscall_queue_wait(int32_t q, uint64_t events, uint32_t count, uint64_t deadline); /* SYS_QUEUE_WAIT */
int syscall_input_read(void);                   /* SYS_INPUT_READ */
int syscall_mouse_read(uint64_t m);             /* SYS_MOUSE_READ */
int syscall_input_poll(void);                   /* SYS_INPUT_POLL */
int syscall_console_write(uint64_t buf, uint64_t len); /* SYS_CONSOLE_WRITE */
int syscall_log_read(uint32_t i, uint64_t e);   /* SYS_LOG_READ */
int syscall_net_status(void);                   /* SYS_NET_STATUS */
int syscall_net_mac(uint64_t buf, uint64_t len); /* SYS_NET_MAC */
int syscall_net_get_config(uint64_t c);         /* SYS_NET_GET_CONFIG */
int syscall_net_set_ip(uint32_t addr);          /* SYS_NET_SET_IP */
int syscall_net_set_mask(uint32_t addr);        /* SYS_NET_SET_MASK */
int syscall_net_set_gateway(uint32_t addr);     /* SYS_NET_SET_GATEWAY */
int syscall_net_set_dns(uint32_t addr);         /* SYS_NET_SET_DNS */
int syscall_net_ping(uint32_t dst, uint32_t seq, uint64_t rtt); /* SYS_NET_PING */
int syscall_net_send(uint64_t frame, uint64_t len); /* SYS_NET_SEND */
int syscall_net_recv(uint64_t buf, uint64_t cap); /* SYS_NET_RECV */
int syscall_fb_info(uint64_t g);                /* SYS_FB_INFO */
int syscall_fb_clear(uint32_t color);           /* SYS_FB_CLEAR */
int syscall_fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color); /* SYS_FB_FILL_RECT */
int syscall_fb_put_pixel(uint32_t x, uint32_t y, uint32_t color); /* SYS_FB_PUT_PIXEL */
int syscall_fb_draw_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t color); /* SYS_FB_DRAW_LINE */
int syscall_fb_draw_text(uint32_t x, uint32_t y, uint64_t text, uint32_t fg, uint32_t bg); /* SYS_FB_DRAW_TEXT */
int syscall_fb_set_font(uint64_t name);         /* SYS_FB_SET_FONT */
int syscall_spawn(uint64_t a);                  /* SYS_SPAWN */
int syscall_exec_path(uint64_t path, uint64_t a); /* SYS_EXEC_PATH */
int syscall_process_kill(int32_t h);            /* SYS_PROCESS_KILL */
int syscall_process_stop(int32_t h, uint32_t deadline_ms); /* SYS_PROCESS_STOP */
int syscall_process_stop_endpoint(int32_t h);   /* SYS_PROCESS_STOP_ENDPOINT */
int syscall_job_create(int32_t parent, uint64_t limits); /* SYS_JOB_CREATE */
int syscall_job_set_limits(int32_t h, uint64_t limits); /* SYS_JOB_SET_LIMITS */
int syscall_job_kill(int32_t h);                /* SYS_JOB_KILL */
int syscall_vm_map(uint64_t a);                 /* SYS_VM_MAP */
int syscall_vm_unmap(uint64_t addr, uint64_t length); /* SYS_VM_UNMAP */
int syscall_vm_info(uint64_t u);                /* SYS_VM_INFO */
int syscall_mem_create(uint64_t size, uint32_t flags); /* SYS_MEM_CREATE */
int syscall_power_reboot(void);                 /* SYS_POWER_REBOOT */
int syscall_power_shutdown(void);               /* SYS_POWER_SHUTDOWN */
int syscall_power_sleep(uint32_t ms);           /* SYS_POWER_SLEEP */
int syscall_file_open(uint64_t path, uint32_t flags); /* SYS_FILE_OPEN */
int syscall_file_close(int32_t h);              /* SYS_FILE_CLOSE */
int syscall_file_read(int32_t h, uint64_t buf, uint64_t n); /* SYS_FILE_READ */
int syscall_file_write(int32_t h, uint64_t buf, uint64_t n); /* SYS_FILE_WRITE */
int syscall_file_seek(int32_t h, int64_t off, uint32_t origin, uint64_t pos); /* SYS_FILE_SEEK */
int syscall_file_tell(int32_t h, uint64_t pos); /* SYS_FILE_TELL */
int syscall_file_truncate(int32_t h, uint64_t size); /* SYS_FILE_TRUNCATE */
int syscall_file_stat(uint64_t path, uint64_t st); /* SYS_FILE_STAT */
int syscall_file_fstat(int32_t h, uint64_t st); /* SYS_FILE_FSTAT */
int syscall_file_readdir(uint64_t path, uint32_t index, uint64_t st); /* SYS_FILE_READDIR */
int syscall_file_mkdir(uint64_t path);          /* SYS_FILE_MKDIR */
int syscall_file_unlink(uint64_t path);         /* SYS_FILE_UNLINK */
int syscall_file_rename(uint64_t path, uint64_t newname); /* SYS_FILE_RENAME */
int syscall_file_chdir(uint64_t path);          /* SYS_FILE_CHDIR */
int syscall_file_getcwd(uint64_t buf, uint64_t cap); /* SYS_FILE_GETCWD */
int syscall_file_sync(void);                    /* SYS_FILE_SYNC */
int syscall_file_lock(int32_t h, uint64_t a);   /* SYS_FILE_LOCK */
int syscall_file_unlock(int32_t h, uint64_t start, uint64_t len); /* SYS_FILE_UNLOCK */

/* Registers a syscall carries after its number, and the table's slots: one per
 * number up to the largest. syscall_dispatch refuses a number past the end
 * before looking it up, and one that lands in a hole finds no decoder. */
#define SYSCALL_ARGS            5
#define SYSCALL_TABLE_SIZE      178
#define SYSCALL_COUNT           84

/* An argument register is as wide as the machine's: 32 bits on x86-32, where a
 * u64 takes a register pair, and 64 on x86-64, where it takes one. */
#if UINTPTR_MAX == 0xFFFFFFFFu
#define SYSCALL_PAIRS           1
#else
#define SYSCALL_PAIRS           0
#endif

/* A decoder takes the raw argument registers, gives each its declared type,
 * and calls the handler; a handler that returns nothing answers 0. */
typedef int (*syscall_decoder)(const uintptr_t *args);
extern const syscall_decoder syscall_table[SYSCALL_TABLE_SIZE];

#endif
