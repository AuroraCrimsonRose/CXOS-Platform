// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* kernel/cpu/systable.h: GENERATED from abi/kernel.xfil (cx.kernel) by `cxk abi generate`.
 * Do not edit it: change abi/kernel.xfil and regenerate. `cxk check-abi` fails while
 * this file differs from what the .xfil generates. */

#ifndef CXK_SYSTABLE_H
#define CXK_SYSTABLE_H

#include <stdint.h>

/* The handlers, one per syscall, named after it (SYS_FILE_OP: syscall_file_op).
 * Each is given its arguments as the interface declares them. A pointer arrives
 * as the user address it is, a uint32_t; the handler checks and copies it. */

void syscall_exit(int32_t code);                /* SYS_EXIT */
void syscall_yield(void);                       /* SYS_YIELD */
int syscall_getpid(void);                       /* SYS_GETPID */
int syscall_getuid(void);                       /* SYS_GETUID */
int syscall_clock(uint32_t c);                  /* SYS_CLOCK */
int syscall_sleep(uint32_t ms);                 /* SYS_SLEEP */
int syscall_args(uint32_t a);                   /* SYS_ARGS */
int syscall_ipc_call(uint32_t a);               /* SYS_IPC_CALL */
int syscall_ipc_recv(uint32_t a);               /* SYS_IPC_RECV */
int syscall_ipc_reply(uint32_t a);              /* SYS_IPC_REPLY */
int syscall_ep_create(void);                    /* SYS_EP_CREATE */
int syscall_handle_close(int32_t h);            /* SYS_HANDLE_CLOSE */
int syscall_handle_duplicate(int32_t h, uint32_t rights); /* SYS_HANDLE_DUPLICATE */
int syscall_object_info(uint32_t a);            /* SYS_OBJECT_INFO */
int syscall_input_read(void);                   /* SYS_INPUT_READ */
int syscall_mouse_read(uint32_t m);             /* SYS_MOUSE_READ */
int syscall_input_poll(void);                   /* SYS_INPUT_POLL */
int syscall_console_write(uint32_t buf, uint32_t len); /* SYS_CONSOLE_WRITE */
int syscall_log_read(uint32_t i, uint32_t e);   /* SYS_LOG_READ */
int syscall_net_op(uint32_t a);                 /* SYS_NET_OP */
int syscall_fb_info(uint32_t g);                /* SYS_FB_INFO */
int syscall_fb_clear(uint32_t color);           /* SYS_FB_CLEAR */
int syscall_fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color); /* SYS_FB_FILL_RECT */
int syscall_fb_put_pixel(uint32_t x, uint32_t y, uint32_t color); /* SYS_FB_PUT_PIXEL */
int syscall_fb_draw_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t color); /* SYS_FB_DRAW_LINE */
int syscall_fb_draw_text(uint32_t x, uint32_t y, uint32_t text, uint32_t fg, uint32_t bg); /* SYS_FB_DRAW_TEXT */
int syscall_fb_set_font(uint32_t name);         /* SYS_FB_SET_FONT */
int syscall_file_op(uint32_t a);                /* SYS_FILE_OP */
int syscall_spawn(uint32_t a);                  /* SYS_SPAWN */
int syscall_exec_path(uint32_t path, uint32_t a); /* SYS_EXEC_PATH */
int syscall_vm_map(uint32_t a);                 /* SYS_VM_MAP */
int syscall_vm_unmap(uint32_t addr, uint32_t length); /* SYS_VM_UNMAP */
int syscall_vm_info(uint32_t u);                /* SYS_VM_INFO */
int syscall_power_reboot(void);                 /* SYS_POWER_REBOOT */
int syscall_power_shutdown(void);               /* SYS_POWER_SHUTDOWN */
int syscall_power_sleep(uint32_t ms);           /* SYS_POWER_SLEEP */

/* Registers a syscall carries after its number, and the table's slots: one per
 * number up to the largest. syscall_dispatch refuses a number past the end
 * before looking it up, and one that lands in a hole finds no decoder. */
#define SYSCALL_ARGS            5
#define SYSCALL_TABLE_SIZE      147
#define SYSCALL_COUNT           36

/* A decoder takes the raw argument registers, gives each its declared type,
 * and calls the handler; a handler that returns nothing answers 0. */
typedef int (*syscall_decoder)(const uint32_t *args);
extern const syscall_decoder syscall_table[SYSCALL_TABLE_SIZE];

#endif
