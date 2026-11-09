// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/drivers/input/inputdev.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The keyboard and the pointer as a handle, held by one process at a time:
 * the display server (docs/system/CX_DISPLAY.md section 3, roadmap 2.4).
 */

#ifndef CXK_INPUTDEV_H
#define CXK_INPUTDEV_H

#include <stdint.h>

void input_sys_init(void);

/* A key from a keyboard: 1 if the handle's holder took it, 0 if there is no
   holder and the caller keeps it. Interrupts may be off. */
int  input_post_key(unsigned char c);

/* The pointer is at (x, y) with these buttons. Nothing if there is no holder.
   A movement replaces one still unread with the same buttons, so a slow
   reader sees where the pointer is rather than everywhere it went. */
void input_post_pointer(int32_t x, int32_t y, uint32_t buttons);

/* The secure attention combination. Goes to the holder if there is one, and
   otherwise nowhere; it is never a key. */
void input_post_attention(void);

/* Does anybody hold the handle? The calls that read input the old way are
   refused while one does. */
int  input_held(void);

int  sys_input_open(void);
int  sys_input_events(int32_t h, uint64_t ev, uint32_t count);

#endif
