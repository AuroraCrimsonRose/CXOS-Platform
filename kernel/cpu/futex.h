// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/futex.h */
/* Aurora Tejeda / CATX Systems */
#ifndef FUTEX_H
#define FUTEX_H

#include <stdint.h>

/* SYS_FUTEX_WAIT: if the 4-byte word at `uaddr` still holds `expected`,
   block until a futex_wake on the same word, `deadline` (on timer_now_ns;
   TIMER_NEVER or later has none) or a kill. E_OK when woken; E_SHOULD_WAIT
   if the word held something else; E_TIMED_OUT; E_CANCELED (killed);
   E_INVALID_ARGS if the word is not 4-byte aligned; E_BAD_ADDRESS if it is
   not readable user memory. */
int futex_wait(uint64_t uaddr, uint32_t expected, uint64_t deadline);

/* SYS_FUTEX_WAKE: wake up to `count` threads waiting on the word at `uaddr`,
   oldest first -> how many were woken, or E_INVALID_ARGS / E_BAD_ADDRESS as
   futex_wait. */
int futex_wake(uint64_t uaddr, uint32_t count);

/* Threads waiting in futex_wait now, for the self-tests. */
uint32_t futex_waiting(void);

#endif
