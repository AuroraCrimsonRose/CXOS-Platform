// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/ktest_races.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The races (roadmap Phase 1, step 6c): each ordering that one CPU almost
 * never produces by itself, made to happen at a race point
 * (diag/testhook.h) and its outcome checked. The calls come from ring 3,
 * as a process makes them (usermode_run_calls).
 */

#ifndef KTEST_RACES_H
#define KTEST_RACES_H

/* A futex word unmapped after the wait faulted it in and before it reads
   it: E_BAD_ADDRESS, no waiter left queued, the key's reference on the
   memory object dropped (nothing left behind). */
int ktest_futex_unmapped(void);

/* A waiter whose deadline has come, woken before it looks again: it
   returns E_OK, and the wake counted it. */
int ktest_futex_deadline(void);

/* One memory object mapped in two processes at different addresses: a
   waiter in one is woken through the other's mapping, and not through a
   mapping of another object at the same offset. */
int ktest_futex_shared(void);

/* A token closed by another thread of its server while the reply reads
   its bytes: a readable reply still reaches the caller; an unreadable one
   fails it with E_PEER_CLOSED rather than leaving it waiting. */
int ktest_ipc_reply_close(void);

/* A call of IPC_MSG_MAX bytes and its reply of IPC_MSG_MAX, both whole; a
   receive one byte short is told the length, a reply one byte long is
   refused. */
int ktest_ipc_msg_max(void);

/* An endpoint closed and freed while a call it gave out is still being
   answered: the call stops naming it, and a new endpoint in its slot lifts
   nobody through that call. */
int ktest_ipc_from_detach(void);

/* A server thread that took a call and passed its token on ends: the call
   no longer names it as its server, and the token still answers it. */
int ktest_ipc_thread_exit(void);

/* A queue's only event taken by another waiter between this waiter's wake
   and its look: it waits again and times out, never returns 0 events. */
int ktest_queue_taken(void);

/* A queue waiter killed while blocked, and one killed at the point where it
   was woken but not yet looking: each leaves the wait at once, not at its
   deadline, and leaves no waiter. */
int ktest_queue_kill(void);

#endif
