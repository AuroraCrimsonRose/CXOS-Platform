// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/object/kpipe.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Pipes (KOBJ_PIPE; docs/kernel/CX_KERNEL_PLAN.md section 7: "a pair of
 * handles over a bounded ring. Writing with no reader returns an error, not
 * a signal").
 *
 * A pipe is two objects, its read end and its write end, made together over
 * one ring of bytes (XNU's `struct pipepair`, bsd/kern/sys_pipe.c). Each end
 * counts its own handles, so the last handle on one end closing is what the
 * other end sees: a reader finds the end of the stream once the ring is
 * empty, and a writer gets E_PEER_CLOSED (XNU and Serenity `FIFO::write`
 * return EPIPE; Haiku sends SIGPIPE as well, which CXOS has no signals for).
 *
 * A read takes what is there, at least one byte, and blocks only while the
 * ring is empty. A write blocks until it has put everything; one of at most
 * PIPE_ATOMIC bytes goes in whole, never split around another writer's
 * (POSIX PIPE_BUF, 512 in XNU's syslimits.h). Signals follow the same
 * rules, so a thread waiting through a queue reads or writes without
 * blocking: the read end is SIGNAL_PIPE_READABLE while bytes wait or the
 * writer has gone, the write end SIGNAL_PIPE_WRITABLE while PIPE_ATOMIC
 * bytes fit or the reader has gone (XNU `pipe_select`), and each end
 * SIGNAL_PIPE_PEER_CLOSED once the other's last handle has closed.
 *
 * The ring is charged to the creator's job for as long as either end lasts.
 * Bytes cross the ring boundary through a PIPE_ATOMIC bounce buffer on the
 * caller's stack, so no user page is touched under the wait lock, which is
 * what guards the ring.
 */
#ifndef CXK_KPIPE_H
#define CXK_KPIPE_H

#include <stdint.h>
#include "object/kobj.h"

struct job;
struct kpipe;

struct kpipe_end {
    struct kobj   hdr;    /* first; KOBJ_PIPE */
    struct kpipe *p;      /* the pair this end is half of */
    uint8_t       write;  /* 1: the write end */
};

struct kpipe {
    struct kpipe_end rd, wr;
    uint8_t   *ring;
    uint32_t   size;      /* bytes the ring holds */
    uint32_t   head;      /* next byte to read */
    uint32_t   count;     /* bytes waiting; under the wait lock, as is the rest */
    uint8_t    rd_closed; /* the read end's last handle has closed */
    uint8_t    wr_closed; /* the write end's */
    atomic_u32 ends;      /* ends not yet destroyed: the last frees the pair */
    struct job *job;      /* charged `size`; a reference */
};

void kpipe_sys_init(void);

/* A new empty pipe of `size` bytes (PIPE_SIZE_MIN..PIPE_SIZE_MAX, checked by
   the caller), its ring charged to `job`, with one reference on each end for
   the caller. 0 with *rc set: E_NO_MEMORY, or the job's refusal. */
struct kpipe *kpipe_create(uint32_t size, struct job *job, int *rc);

/* Read at most `n` bytes into user memory at `ubuf` (checked writable by the
   caller): blocks while the ring is empty and the writer is there. -> bytes
   read, 0 at the end of the stream (the ring empty and the writer gone),
   E_BAD_ADDRESS, or E_CANCELED when the caller is being killed. */
int kpipe_read(struct kpipe_end *e, uint64_t ubuf, uint32_t n);

/* Write `n` bytes from user memory at `ubuf`: blocks until all are in the
   ring. -> n; E_PEER_CLOSED when the reader has gone before any byte went in
   (after some, the count that did); E_BAD_ADDRESS; E_CANCELED. */
int kpipe_write(struct kpipe_end *e, uint64_t ubuf, uint32_t n);

/* Bytes waiting in the ring now, for the self-tests. */
uint32_t kpipe_count(const struct kpipe *p);

#endif
