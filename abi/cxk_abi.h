// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* abi/cxk_abi.h: GENERATED from abi/kernel.xfil (cx.kernel) by `cxk abi generate`.
 * Do not edit it: change abi/kernel.xfil and regenerate. `cxk check-abi` fails while
 * this file differs from what the .xfil generates. */

#ifndef CXK_ABI_H
#define CXK_ABI_H

#include <stdint.h>

/*
 * The ABI hash: SHA-256 of abi/kernel.xfil without its comments
 * (docs/kernel/CX_SYSCALLS_IPC.md sec 5, 9.5). Every CXEX image records the
 * hash it was built against and the loader refuses one that differs, so a
 * program never runs against numbers or layouts it was not compiled for.
 */
#define CXK_ABI_HASH { 0xfe, 0x55, 0x06, 0xf4, 0xc8, 0xe7, 0xf8, 0xbf, 0x25, 0xac, 0x98, 0x88, 0x3d, 0xde, 0x11, 0xa8, 0xb0, 0x9c, 0xc0, 0x88, 0xae, 0x74, 0x85, 0x6c, 0x9e, 0x8b, 0xfb, 0xba, 0xdd, 0x11, 0x36, 0x92 }

/*
 * CXK ABI v1 - the PUBLIC contract shared by the kernel, the .xoex executive,
 * and .xuex apps (and, later, the X toolchain). Syscall numbers, error codes,
 * and shared call structures: kernel and user code compile against the SAME
 * definitions, so they can never drift.
 *
 * Calling convention (see docs/CXK_ABI_v1):
 *   eax = syscall number   ebx,ecx,edx,esi,edi = args 1..5   -> int 0x80
 *   result in eax: >= 0 success (call-specific), < 0 a negative error code.
 *   all GP registers except eax are preserved across the call.
 *   a u64 or i64 argument takes two registers on x86-32, low half first, and
 *   one on x86-64; sizes and offsets are u64 (docs/kernel/CX_SYSCALLS_IPC.md
 *   sec 5). A 64-bit result comes back through a pointer argument.
 */

/*
 * The version of THIS contract: the syscall numbers, struct layouts and
 * capability meanings below. It is stamped into every CXEX image's abi_version
 * so a kernel can refuse one built against a different contract.
 *
 * It is 2 because docs/x86_32/kernel/CX_ABI.md has declared the live contract v2 since
 * it was written - what was missing was anywhere in the CODE that said so, which
 * is how every image came to be stamped 1 while asking to be refused on a
 * mismatch. Declared in versions.json as `cxex-abi` and checked against this
 * line; bump both, and the DevKit's stamp, in the one commit that changes the
 * contract.
 */
#define CXK_ABI_VERSION          28

/*
 * ---- syscall numbers ----
 * lifecycle 0x00-0x0F (no capability required)
 * ebx = exit code; does not return. Ends the process: each of its threads
 * ends at its next safe point, as SYS_PROCESS_KILL would end them, and the
 * process ends PROC_EXITED with this code. SYS_THREAD_EXIT ends one thread.
 */
#define SYS_EXIT                 0x00
#define SYS_YIELD                0x01           /* cooperatively yield the CPU */
#define SYS_GETPID               0x02           /* -> caller pid */
#define SYS_GETUID               0x03           /* -> caller owning uid (0 = SYSTEM) */
#define SYS_CLOCK                0x04           /* ebx = *clock_info -> 0, or -E_* */
#define SYS_SLEEP                0x05           /* ebx = ms; blocks this thread, then returns 0 */
#define SYS_ARGS                 0x06           /* ebx = *args_info -> 0, or -E_* */

/*
 * ebx = *u64 -> 0, with the monotonic clock in it: nanoseconds since boot.
 * It never goes back and never jumps; every deadline is a time on it.
 */
#define SYS_CLOCK_MONOTONIC      0x07

/*
 * ebx = level -> 0. The calling thread's own priority (PRIO_*). It runs at
 * that, or higher while it serves a call from a thread above it. Lowering is
 * never refused. E_OUT_OF_RANGE: not PRIO_MIN to PRIO_MAX. E_ACCESS_DENIED:
 * above its job's ceiling (job_limits.priority), or PRIO_REALTIME and up
 * without GRANT_REALTIME.
 */
#define SYS_THREAD_SET_PRIORITY  0x08

/*
 * -> the calling thread's own priority in bits 0-7, and in bits 8-15 the one
 * it runs at: higher while it serves a call from a thread above it.
 */
#define SYS_THREAD_PRIORITY      0x09

/*
 * ebx = slot -> the handle its launcher put in that slot (spawn_args.handles),
 * as it was placed at the start; closing it does not clear the slot.
 * E_NOT_FOUND: nothing was put there. E_OUT_OF_RANGE: SPAWN_HANDLES_MAX or more.
 */
#define SYS_ARGS_HANDLE          0x0A

/*
 * ebx = *thread_create_args -> a thread handle with HRIGHT_WAIT,
 * HRIGHT_INSPECT and HRIGHT_DUPLICATE. A new thread in the caller's process,
 * at PRIO_NORMAL (or its job's ceiling), starting at `entry` with arg0 in
 * rdi and arg1 in rsi, on a stack the kernel maps for it: stack_size bytes
 * above an unmapped guard page, charged like a mapping, which the thread
 * cannot unmap and which goes when the thread ends. rsp starts 8 below the
 * top, where a return address would be, and that word is 0: an entry that
 * returns faults, so a thread ends with SYS_THREAD_EXIT. Unprivileged; the
 * job's thread limit and the memory quota are the bounds.
 * E_INVALID_ARGS: flags or reserved not 0, or an entry outside the user half.
 * E_OUT_OF_RANGE: a stack size outside THREAD_USTACK_MIN to _MAX.
 * E_NO_RESOURCES: no thread to spare. E_NO_MEMORY: no room for the stack.
 */
#define SYS_THREAD_CREATE        0x0B

/*
 * ebx = exit code; does not return. Ends the calling thread: its handles see
 * SIGNAL_THREAD_ENDED, with THR_EXITED and this code. The last thread of a
 * process to end this way ends it PROC_EXITED with its code.
 */
#define SYS_THREAD_EXIT          0x0C

/*
 * IPC + handles 0x10-0x1F
 * ebx = *ipc_call_args, whose handle is a send handle (HRIGHT_SEND) -> the
 * reply's length, once the server answers. Calls queue on the endpoint, oldest
 * first. E_PEER_CLOSED: the endpoint's last receive handle closed, or the
 * call's reply token closed unanswered. E_CANCELED: the caller is being killed.
 * The request's handles (ipc_handle) leave the caller's table when the call
 * is queued, all of them, or none if the call is refused; a queued call that
 * then fails closes them. The reply's handles land in the caller's table and
 * their values in reply_handles, then 0s; E_NO_RESOURCES if the table has no
 * room for them all (the reply is lost).
 */
#define SYS_IPC_CALL             0x10

/*
 * ebx = *ipc_recv_args -> the message's length, once one waits: the oldest
 * call, or a kernel notice first. A call comes with a reply token in
 * ipc_msg_info.reply, and its handles in the receiver's table, their values
 * at `handles`. A message longer than `cap`, or with more handles than
 * `handle_cap`, is E_BUFFER_TOO_SMALL and stays, with its length and handle
 * count in ipc_msg_info. E_NO_RESOURCES: no room in the table; it stays.
 */
#define SYS_IPC_RECV             0x11

/*
 * ebx = *ipc_reply_args -> 0, and the token is closed. E_PEER_CLOSED: the
 * caller has gone; the token is closed too, and the handles stay. A reply
 * longer than the caller's ipc_msg_info.reply_cap, or with more handles than
 * its reply_handles, is E_BUFFER_TOO_SMALL, and the token stays. A reply
 * refused for its handles moves none of them and keeps the token.
 */
#define SYS_IPC_REPLY            0x12
#define SYS_EP_CREATE            0x13           /* create an endpoint -> RECV handle (GRANT_ENDPOINT) */

/*
 * ebx = an endpoint handle (HRIGHT_RECV), ecx = badge, edx = rights (a
 * subset of HRIGHT_SEND | HRIGHT_DUPLICATE, HRIGHT_SEND among them) -> a send
 * handle whose calls arrive with that badge (ipc_msg_info.badge). A copy of
 * it keeps the badge.
 */
#define SYS_EP_MINT              0x14
#define SYS_HANDLE_CLOSE         0x16           /* release a handle */

/* ebx = handle, ecx = rights (a subset of its own; needs HRIGHT_DUPLICATE) -> new handle */
#define SYS_HANDLE_DUPLICATE     0x17
#define SYS_OBJECT_INFO          0x18           /* ebx = *object_info_args -> bytes written */

/*
 * ebx = handle (HRIGHT_WAIT), ecx = signals, edx = deadline on the
 * monotonic clock (SYS_CLOCK_MONOTONIC; one already passed, 0 among them,
 * only looks; DEADLINE_NEVER, or later, has none), esi = *u32 for the object's signals
 * as last seen, or 0 -> 0 once any of `signals` is set, or E_TIMED_OUT
 */
#define SYS_OBJECT_WAIT          0x19

/*
 * ebx = slack in ns -> a timer handle with HRIGHT_WAIT, HRIGHT_ARM and
 * HRIGHT_DUPLICATE. A timer fires once at a deadline on the monotonic clock:
 * SIGNAL_TIMER_FIRED, at the deadline or up to `slack` after it, with
 * whatever else is due then (0: at the deadline). It starts disarmed, and
 * lasts while a handle on it or a wait on it does.
 */
#define SYS_TIMER_CREATE         0x1A

/*
 * ebx = timer handle (HRIGHT_ARM), ecx = deadline -> 0. Clears
 * SIGNAL_TIMER_FIRED and arms it for `deadline`, in place of any deadline it
 * had; one already passed fires it at once. DEADLINE_NEVER or later is
 * E_INVALID_ARGS: cancel it instead.
 */
#define SYS_TIMER_SET            0x1B

/*
 * ebx = timer handle (HRIGHT_ARM) -> 0. Disarms it and clears
 * SIGNAL_TIMER_FIRED.
 */
#define SYS_TIMER_CANCEL         0x1C

/*
 * Notifications: a 64-bit word that signals OR into, so signals before a
 * take combine and a signaller never blocks. SIGNAL_NOTIFY_PENDING is set
 * while any bit is; wait for it with SYS_OBJECT_WAIT or a queue.
 * -> a notification handle with HRIGHT_WAIT, HRIGHT_SIGNAL, HRIGHT_RECV and
 * HRIGHT_DUPLICATE, no bits set. Unprivileged.
 */
#define SYS_NOTIFY_CREATE        0x15

/*
 * ebx = notification handle (HRIGHT_RECV), ecx = bits (not 0), edx = rights
 * (a subset of HRIGHT_SIGNAL | HRIGHT_DUPLICATE, HRIGHT_SIGNAL among them)
 * -> a signaller handle that can set those bits and no others. A copy of it
 * keeps them. It holds the notification but no handle on it.
 */
#define SYS_NOTIFY_MINT          0x1D

/*
 * ebx = a notification or signaller handle (HRIGHT_SIGNAL), ecx = bits (not
 * 0) -> 0. ORs them into the word and sets SIGNAL_NOTIFY_PENDING. A
 * signaller may set only its own bits (E_ACCESS_DENIED). E_PEER_CLOSED: the
 * notification's last handle has closed, so nobody can take them.
 */
#define SYS_NOTIFY_SIGNAL        0x1E

/*
 * ebx = notification handle (HRIGHT_RECV), ecx = *u64 -> 0, with the bits
 * set since the last take in it (0 if none); the word and
 * SIGNAL_NOTIFY_PENDING are cleared. Does not wait. A bad address takes
 * nothing (E_BAD_ADDRESS).
 */
#define SYS_NOTIFY_TAKE          0x1F

/*
 * ---- handles ----
 * A handle is a positive number, never 0 and never below 256, and a closed
 * handle's number is refused rather than naming whatever comes next. Its
 * rights say what its holder may do with the object, and they only shrink:
 * SYS_HANDLE_DUPLICATE can drop rights, never add one. Each object type
 * defines its own (kernel/object/htab.h).
 */
#define HRIGHT_SEND              0x1u           /* send handle: ipc_call through it */

/*
 * endpoint: ipc_recv on it, SYS_EP_MINT from it; file: FOPEN_READ, FOPEN_WRITE;
 * notification: SYS_NOTIFY_TAKE, SYS_NOTIFY_MINT
 */
#define HRIGHT_RECV              0x2u

/* notification and signaller: SYS_NOTIFY_SIGNAL */
#define HRIGHT_SIGNAL            0x1u

/*
 * reply token: ipc_reply through it. The only right a token has: it cannot
 * be copied, so a call is answered once.
 */
#define HRIGHT_REPLY             0x1u

/*
 * process: OBJINFO_PROCESS, how it is and how it ended; job: OBJINFO_JOB;
 * thread: OBJINFO_THREAD
 */
#define HRIGHT_INSPECT           0x1u
#define HRIGHT_KILL              0x2u           /* process: SYS_PROCESS_KILL; job: SYS_JOB_KILL */

/*
 * process: SYS_PROCESS_STOP. Handed out freely, unlike HRIGHT_KILL: a stop
 * asks first, and a program that answers it ends the way it chooses.
 */
#define HRIGHT_STOP              0x4u

/* job: spawn a process into it (spawn_args.job), make a job under it */
#define HRIGHT_ASSIGN            0x4u
#define HRIGHT_SET_LIMITS        0x8u           /* job: SYS_JOB_SET_LIMITS */
#define HRIGHT_ARM               0x1u           /* timer: SYS_TIMER_SET, SYS_TIMER_CANCEL */

/* queue: SYS_QUEUE_REGISTER, SYS_QUEUE_ARM, SYS_QUEUE_UNREGISTER */
#define HRIGHT_REGISTER          0x1u

/*
 * memory object: SYS_VM_MAP it readable; with HRIGHT_MAP_WRITE writable, with
 * HRIGHT_MAP_EXEC executable. OBJINFO_MEM is HRIGHT_INSPECT (0x1).
 */
#define HRIGHT_MAP               0x2u
#define HRIGHT_MAP_WRITE         0x4u
#define HRIGHT_MAP_EXEC          0x8u

/* memory object: SYS_MEM_SEAL */
#define HRIGHT_SEAL              0x10u

/*
 * pipe: SYS_FILE_READ on its read end, SYS_FILE_WRITE on its write end. The
 * bits are FOPEN_READ's and FOPEN_WRITE's, which a file's handle carries as
 * its rights, so one check serves both kinds of stream.
 */
#define HRIGHT_READ              0x1u
#define HRIGHT_WRITE             0x2u

/*
 * Generic rights, which mean the same on every type, count down from the top
 * bit; a type's own count up from the bottom. A handle without
 * HRIGHT_DUPLICATE cannot be copied, and a copy has it only if asked for.
 */
#define HRIGHT_DUPLICATE         0x80000000u

/*
 * SYS_OBJECT_WAIT on it. Every type takes it; a type with no signals refuses
 * the wait itself (E_NOT_SUPPORTED).
 */
#define HRIGHT_WAIT              0x40000000u

/*
 * ---- signals (SYS_OBJECT_WAIT) ----
 * Each type defines its own, from the bottom bit, and asking for one it does
 * not define is E_INVALID_ARGS. Once set, a signal named here stays set,
 * except where its type says what clears it.
 */
#define SIGNAL_PROCESS_ENDED     0x1u           /* process: its last thread has gone */

/*
 * job: SYS_JOB_KILL reached it (on it or a job above it), and every
 * process in it and below has ended
 */
#define SIGNAL_JOB_ENDED         0x1u

/* timer: its deadline has come. SYS_TIMER_SET and SYS_TIMER_CANCEL clear it. */
#define SIGNAL_TIMER_FIRED       0x1u

/*
 * queue: a key is ready. Cleared when a wait takes the last one, or the last
 * ready one is unregistered.
 */
#define SIGNAL_QUEUE_READY       0x1u

/*
 * endpoint: a call or a kernel notice waits for ipc_recv, or the endpoint
 * has closed (ipc_recv then fails at once). Cleared when the last is
 * received or withdrawn.
 */
#define SIGNAL_EP_READABLE       0x1u

/* notification: a bit is set. SYS_NOTIFY_TAKE clears it. */
#define SIGNAL_NOTIFY_PENDING    0x1u

/*
 * pipe, read end: bytes wait, or the write end has gone (a read then gives
 * 0). Cleared when a read empties the ring.
 */
#define SIGNAL_PIPE_READABLE     0x1u

/*
 * pipe, write end: PIPE_ATOMIC bytes fit, or the read end has gone (a write
 * then fails at once). Cleared when a write leaves less room.
 */
#define SIGNAL_PIPE_WRITABLE     0x2u

/* pipe, either end: the other end's last handle has closed */
#define SIGNAL_PIPE_PEER_CLOSED  0x4u

/* thread: it has ended, by SYS_THREAD_EXIT or with its process */
#define SIGNAL_THREAD_ENDED      0x1u

/*
 * A deadline that never comes (SYS_OBJECT_WAIT): a wait with no limit.
 * Deadlines are nanoseconds on the monotonic clock, so this is 292 years
 * after boot.
 */
#define DEADLINE_NEVER           0x7FFFFFFFFFFFFFFFu

/*
 * queues 0x60-0x6F (unprivileged: the handles are the authority)
 * A queue waits for many objects at once (docs/kernel/CX_KERNEL_PLAN.md
 * section 5). Each registration names an object, the signals wanted from it
 * and a key the caller chooses; when one of those signals is set, the key
 * becomes ready, once. A wait returns the ready keys and nothing else, and a
 * key does not come back until SYS_QUEUE_ARM re-arms it.
 * -> a queue handle with HRIGHT_WAIT, HRIGHT_REGISTER and HRIGHT_DUPLICATE.
 */
#define SYS_QUEUE_CREATE         0x60

/*
 * ebx = queue handle (HRIGHT_REGISTER), ecx = the object's handle
 * (HRIGHT_WAIT), edx = signals, esi = key -> 0. Registers it armed; a signal
 * already set makes it ready at once. The registration holds the object as a
 * handle does, until SYS_QUEUE_UNREGISTER or the queue goes. A key in use is
 * E_ALREADY_EXISTS; a queue is not registered on a queue (E_WRONG_TYPE);
 * past QUEUE_REGISTER_MAX is E_NO_RESOURCES.
 */
#define SYS_QUEUE_REGISTER       0x61

/*
 * ebx = queue handle (HRIGHT_REGISTER), ecx = key -> 0. Arms a registration
 * whose key a wait returned; a signal already set makes it ready at once.
 * One armed or ready already is left as it is. E_NOT_FOUND: no such key.
 */
#define SYS_QUEUE_ARM            0x62

/*
 * ebx = queue handle (HRIGHT_REGISTER), ecx = key -> 0. Removes it, ready
 * or not, and lets go of its object. E_NOT_FOUND: no such key.
 */
#define SYS_QUEUE_UNREGISTER     0x63

/*
 * ebx = queue handle (HRIGHT_WAIT), ecx = *queue_event[count], edx = count
 * (at least 1; at most QUEUE_EVENTS_MAX are written), esi = deadline as
 * SYS_OBJECT_WAIT takes it -> the number of events written, oldest ready
 * first, or E_TIMED_OUT. Each key returned is ready no longer and waits to be
 * re-armed.
 */
#define SYS_QUEUE_WAIT           0x64

/* input 0x20-0x2F (unprivileged: reading your own keystrokes) */
#define SYS_INPUT_READ           0x20           /* block until a key -> char */
#define SYS_MOUSE_READ           0x21           /* ebx = *mouse_state -> 1 if a mouse is present */
#define SYS_INPUT_POLL           0x22           /* -> char, 0 if none waiting */

/*
 * console 0x30-0x3F (privileged: GRANT_CONSOLE)
 * buf, len (0 = a NUL-scan of at most 4096 bytes); a len of 2^32 or more is
 * E_OUT_OF_RANGE -> bytes written
 */
#define SYS_CONSOLE_WRITE        0x30

/* ebx = index (0 = oldest kept), ecx = *log_entry -> 1 filled, 0 past the newest */
#define SYS_LOG_READ             0x31

/*
 * network 0x50-0x5F (privileged: GRANT_NET). IPv4 addresses cross the ABI as
 * a packed u32 in NETWORK byte order (a.b.c.d -> (a<<24)|(b<<16)|(c<<8)|d),
 * so userspace needs no array type.
 */
#define SYS_NET_STATUS           0x50           /* -> 1 if the interface is up, 0 if not */
#define SYS_NET_MAC              0x51           /* len >= 6 -> 6, the MAC in buf */
#define SYS_NET_GET_CONFIG       0x52           /* ebx = *net_ip_config -> 0, filled in */

/*
 * Each stores the address as given, unchecked -> 0. SET_DNS sets the primary
 * server and keeps the secondary.
 */
#define SYS_NET_SET_IP           0x53
#define SYS_NET_SET_MASK         0x54
#define SYS_NET_SET_GATEWAY      0x55
#define SYS_NET_SET_DNS          0x56

/*
 * One ICMP echo, waiting up to a second -> 1 on a reply (*rtt = the round
 * trip in MICROSECONDS; rtt may be 0), 0 on a timeout. E_INVALID_ARGS for an
 * address that cannot answer: 0.0.0.0, the all-ones broadcast, and the local
 * subnet's network and broadcast addresses.
 */
#define SYS_NET_PING             0x57
#define SYS_NET_SEND             0x58           /* a raw Ethernet frame, 1-1514 bytes */

/* -> bytes received, 0 if none waiting; a cap past 65535 is taken as 65535 */
#define SYS_NET_RECV             0x59

/*
 * framebuffer 0x40-0x4F (privileged: GRANT_FRAMEBUFFER). Colours are 0x00RRGGBB;
 * the kernel converts them to the active mode. Each answers E_NOT_FOUND when
 * there is no framebuffer (VGA text mode).
 */
#define SYS_FB_INFO              0x40           /* ebx = *fb_geometry -> 0, filled in */
#define SYS_FB_CLEAR             0x41
#define SYS_FB_FILL_RECT         0x42
#define SYS_FB_PUT_PIXEL         0x43
#define SYS_FB_DRAW_LINE         0x44

/* text NUL-terminated, at most 4096 drawn -> characters drawn */
#define SYS_FB_DRAW_TEXT         0x45

/*
 * Install a console font by NAME, from /System/Fonts/<name>.xfnt.
 *
 * A name, not a path, on purpose. The kernel composes the path and reads
 * the file itself, so there is no user buffer holding the font between
 * being validated and being installed - the same reason SYS_EXEC_PATH
 * reads the image rather than taking the caller's copy - and a capability
 * to draw on the screen does not become a capability to make the kernel
 * read an arbitrary path. Names carrying '/' or '.' are refused.
 *
 * The font is validated as an XFNT before anything is installed; a bad one
 * leaves the current font working. Returns 0, or E_INVALID_ARGS / E_NOT_FOUND.
 */
#define SYS_FB_SET_FONT          0x46

/*
 * process 0x70-0x7F (spawning is privileged; kill, stop and jobs need only the
 * handle's right). Both return a process handle with HRIGHT_WAIT,
 * HRIGHT_INSPECT, HRIGHT_KILL, HRIGHT_STOP and HRIGHT_DUPLICATE, not a pid: the process is freed once it has ended and
 * the last handle on it is closed, and nobody reaps it. Its pid is
 * OBJINFO_PROCESS.
 */
#define SYS_SPAWN                0x70           /* ebx = *spawn_args (GRANT_SPAWN) -> process handle */

/*
 * ebx = path, ecx = *spawn_args (GRANT_SPAWN) -> process handle (image/image_len
 * in the struct are ignored)
 */
#define SYS_EXEC_PATH            0x72

/*
 * ebx = process handle (HRIGHT_KILL; the right is the authority, no grant is
 * needed) -> 0. Each of its threads ends at its next safe point: on its way
 * back to ring 3, or woken from any wait (an IPC call or receive, a file
 * lock, the keyboard, a sleep, an object wait). It ends PROC_KILLED, with the
 * caller's pid, unless it had already noted another way out. A process that
 * has ended already is 0 and unchanged. Returns before it has ended: wait for
 * SIGNAL_PROCESS_ENDED.
 */
#define SYS_PROCESS_KILL         0x71

/*
 * ebx = process handle (HRIGHT_STOP), ecx = deadline in ms (at most
 * SLEEP_MAX_MS) -> 0. Asks the process to end: a stop_notice is left on the
 * endpoint it registered (SYS_PROCESS_STOP_ENDPOINT), and if it has not
 * ended when the deadline passes, it is ended as a kill would end it,
 * PROC_STOPPED with the caller's pid. A process that answers by exiting
 * ends PROC_EXITED with its own code. With no endpoint registered, or one
 * whose last handle has closed, nothing can ask it, and it is ended at
 * once. A second stop may bring the deadline closer, never later. A process
 * that has ended already is 0 and unchanged. Returns before it has ended.
 */
#define SYS_PROCESS_STOP         0x73

/*
 * ebx = an endpoint handle with HRIGHT_RECV, or 0 for none -> 0. Where the
 * caller's process is asked to stop: the next ipc_recv on it returns a
 * stop_notice, from sender IPC_SENDER_KERNEL, before any call waiting
 * there. It replaces any endpoint registered before and lasts until the
 * process ends.
 */
#define SYS_PROCESS_STOP_ENDPOINT 0x74

/*
 * Jobs: a group of processes under one memory and thread budget, ended
 * together. Every process is in one, for life: the job its spawn named
 * (spawn_args.job), or its spawner's. A charge is made to the job and every
 * job above it, and refused if any would pass its limit (E_NO_MEMORY,
 * E_NO_RESOURCES), so a job can never hold more than its parent allows.
 * Memory is what SYS_VM_INFO counts for each process (its image, stack,
 * argument and system pages, and its mappings), on top of the process's own
 * quota; threads are its processes' threads until each is gone.
 *
 * ebx = the parent: a job handle with HRIGHT_ASSIGN, or 0 for the caller's
 * own job; ecx = *job_limits, or 0 for none of its own -> a handle on the new
 * job with HRIGHT_WAIT, HRIGHT_INSPECT, HRIGHT_KILL, HRIGHT_ASSIGN,
 * HRIGHT_SET_LIMITS and HRIGHT_DUPLICATE. No grant: a job only divides what
 * its parent has. E_CANCELED under a killed job, E_OUT_OF_RANGE past
 * JOB_DEPTH_MAX. A job lasts while a handle on it, a process in it or a job
 * under it does.
 */
#define SYS_JOB_CREATE           0x75

/*
 * ebx = job handle (HRIGHT_SET_LIMITS), ecx = *job_limits -> 0. A limit
 * below what is in use takes nothing back; it refuses the next charge.
 */
#define SYS_JOB_SET_LIMITS       0x76

/*
 * ebx = job handle (HRIGHT_KILL) -> 0. Ends every process in the job and
 * below it as SYS_PROCESS_KILL would (PROC_KILLED, the caller's pid), and
 * closes them all: no process or job is made in them again (E_CANCELED).
 * Returns before they have ended: wait for SIGNAL_JOB_ENDED.
 */
#define SYS_JOB_KILL             0x77

/* memory 0x80-0x8F (unprivileged, quota-bounded) */
#define SYS_VM_MAP               0x80           /* ebx = *vm_map_args -> 0, with addr and granted filled in */

/* addr, length (0 = the whole mapping) -> 0 */
#define SYS_VM_UNMAP             0x81
#define SYS_VM_INFO              0x82           /* ebx = *vm_usage -> 0, with quota and mapped filled in */

/*
 * ebx/ecx = size in bytes (1 to MMAP_MAX_BYTES, rounded up to a page), edx =
 * flags (0) -> a memory object handle with HRIGHT_MAP, HRIGHT_MAP_WRITE,
 * HRIGHT_MAP_EXEC, HRIGHT_SEAL, HRIGHT_INSPECT and HRIGHT_DUPLICATE. Its pages are zero
 * and arrive on first touch; all of them are committed now, charged to the
 * caller's job (E_NO_MEMORY past its limit or past what memory can promise).
 */
#define SYS_MEM_CREATE           0x83

/*
 * ebx = memory object handle (HRIGHT_SEAL), ecx = seals (MEM_SEAL_*, not 0)
 * -> 0, the seals added for every holder and never taken off. E_BUSY for
 * MEM_SEAL_WRITE while a writable mapping of it exists; E_ACCESS_DENIED once
 * it has MEM_SEAL_SEAL. A receiver reads them with OBJINFO_MEM.
 */
#define SYS_MEM_SEAL             0x84

/* power 0x90-0x9F (privileged: GRANT_POWER). Each returns only if refused. */
#define SYS_POWER_REBOOT         0x90
#define SYS_POWER_SHUTDOWN       0x91           /* ACPI S5 soft-off */

/* ebx = ms: idle the CPU (C1) that long, or until a key; 0 = until a key -> 0 */
#define SYS_POWER_SLEEP          0x92

/*
 * files 0xA0-0xBF (privileged: GRANT_DISK). 32 numbers: the 18 calls did not
 * fit 0x60-0x6F, now the queues'. Paths, offsets and handles: see "files" below.
 * flags = FOPEN_*, one of FOPEN_READ / FOPEN_WRITE required -> a handle
 */
#define SYS_FILE_OPEN            0xA0
#define SYS_FILE_CLOSE           0xA1

/*
 * -> bytes read, 0 at end of file. A short count is normal at the end, so a
 * reader loops until it gets 0; a call moves at most 2^31 - 1 bytes.
 *
 * READ and WRITE take a pipe end too (SYS_PIPE_CREATE), and then need no
 * GRANT_DISK: the handle's HRIGHT_READ or HRIGHT_WRITE is the authority. A
 * pipe read blocks while the pipe is empty and returns what is there; 0 is
 * the end of the stream. A pipe write blocks until every byte is in;
 * E_PEER_CLOSED once the read end has gone (after some bytes, their count).
 */
#define SYS_FILE_READ            0xA2
#define SYS_FILE_WRITE           0xA3           /* -> bytes written */

/*
 * origin = FSEEK_* -> 0, the new offset in *pos (pos may be 0). Past the end
 * is allowed and allocates nothing until a write reaches it; before the start
 * is E_OUT_OF_RANGE.
 */
#define SYS_FILE_SEEK            0xA4
#define SYS_FILE_TELL            0xA5           /* -> 0, the current offset in *pos */

/* cut (or extend) the file to exactly size bytes */
#define SYS_FILE_TRUNCATE        0xA6
#define SYS_FILE_STAT            0xA7
#define SYS_FILE_FSTAT           0xA8

/*
 * entry number index of the directory at path, into st -> 1 on a hit, 0 once
 * the index is past the end
 */
#define SYS_FILE_READDIR         0xA9
#define SYS_FILE_MKDIR           0xAA
#define SYS_FILE_UNLINK          0xAB           /* a directory must be empty */

/* newname is a bare name, not a path: the entry stays in its directory */
#define SYS_FILE_RENAME          0xAC
#define SYS_FILE_CHDIR           0xAD

/* the working directory into buf, NUL-terminated -> its length */
#define SYS_FILE_GETCWD          0xAE

/*
 * Commit everything written so far to stable media -> 0, or E_IO when the
 * device cannot confirm it.
 *
 * Separate from WRITE on purpose: a sync per write costs a device cache
 * flush on every byte, so it is the caller's decision - as fsync,
 * FlushFileBuffers and VNOP_FSYNC all are. Anything that must survive a
 * power cut writes, syncs, and only then treats the result as committed.
 */
#define SYS_FILE_SYNC            0xAF

/*
 * Advisory byte-range lock, described by a file_lock_args: len bytes from
 * start (len 0 runs to the end of the file however it grows, so start 0 len 0
 * is the whole file), mode = FLOCK_SHARED or FLOCK_EXCLUSIVE, optionally |
 * FLOCK_WAIT. A structure because two u64s and a mode are six registers on
 * x86-32.
 *
 * A lock belongs to the OPEN FILE (the handle), not to the process and not
 * to the file on disk - SerenityOS's model. Shared ranges overlap freely;
 * an exclusive range overlaps nothing another open file holds, a second
 * open in the same process included. A handle's own locks never conflict,
 * so locking the same range again converts it. Closing the handle releases
 * them all, however it goes (SYS_FILE_CLOSE, SYS_HANDLE_CLOSE, exit).
 *
 * A conflict is E_BUSY, or with FLOCK_WAIT the call sleeps until the
 * conflicting locks are released - except when that could never happen (a
 * conflicting lock is the caller's own, or its holder is waiting, directly
 * or through others, on one of the caller's), which is E_DEADLOCK.
 *
 * Advisory: only another LOCK is refused. Opening, reading, writing and
 * unlinking a locked file all still work, as they do on every Unix. Nothing
 * is written to disk.
 */
#define SYS_FILE_LOCK            0xB0

/*
 * Drop this handle's locks lying wholly inside the range (start 0 len 0: all
 * of them). Ranges are not split - unlock what was locked. E_OK even when
 * nothing was held.
 */
#define SYS_FILE_UNLOCK          0xB1

/*
 * pipes 0xC0-0xCF (unprivileged: the handles are the authority; the ring is
 * charged to the caller's job). Bytes move through SYS_FILE_READ and
 * SYS_FILE_WRITE, so a program reads a pipe as it reads a file.
 * ebx = size in bytes (0: PIPE_SIZE_DEFAULT; else PIPE_SIZE_MIN to
 * PIPE_SIZE_MAX, E_OUT_OF_RANGE), ecx = *i32[2] -> 0, the read end's handle
 * in [0] (HRIGHT_READ) and the write end's in [1] (HRIGHT_WRITE), each with
 * HRIGHT_WAIT and HRIGHT_DUPLICATE. Nothing is made unless both are written.
 */
#define SYS_PIPE_CREATE          0xC0

/*
 * ---- error codes (negative; returned in eax) ----
 * One list, shared by syscalls and services (docs/kernel/CX_SYSCALLS_IPC.md sec 6).
 */
#define E_OK                     0
#define E_INTERNAL               (-1)           /* a kernel bug; never expected */
#define E_NOT_SUPPORTED          (-2)           /* no such operation, or not for this object */
#define E_INVALID_ARGS           (-3)           /* malformed arguments, unknown flags, bad alignment */
#define E_BAD_ADDRESS            (-4)           /* user memory not mapped or not accessible */
#define E_BAD_HANDLE             (-5)           /* not a live handle in this process */
#define E_WRONG_TYPE             (-6)           /* the handle names the wrong kind of object */
#define E_ACCESS_DENIED          (-7)           /* the handle lacks a right, or policy forbids it */
#define E_NO_MEMORY              (-8)           /* no memory, or the commit quota refused it */
#define E_NO_RESOURCES           (-9)           /* a table or budget is full */
#define E_OUT_OF_RANGE           (-10)          /* an offset or size beyond the object or a limit */
#define E_BUFFER_TOO_SMALL       (-11)          /* the output buffer is too small */
#define E_TIMED_OUT              (-12)          /* the deadline passed */
#define E_CANCELED               (-13)          /* the wait or call was canceled */
#define E_PEER_CLOSED            (-14)          /* the other end is gone */
#define E_SHOULD_WAIT            (-15)          /* nothing ready now; wait and retry */
#define E_BUSY                   (-16)          /* in use; try later */
#define E_ALREADY_EXISTS         (-17)          /* the name is already taken */
#define E_NOT_FOUND              (-18)          /* no such name or object */
#define E_NOT_DIR                (-19)          /* expected a directory, got a file */
#define E_IS_DIR                 (-20)          /* expected a file, got a directory */
#define E_NO_SPACE               (-21)          /* the volume is full */
#define E_IO                     (-22)          /* disk or filesystem I/O failure */
#define E_CORRUPT                (-23)          /* on-disk data failed a check */
#define E_DEADLOCK               (-24)          /* waiting would never end (SYS_FILE_LOCK) */
#define E_PROTOCOL               (-25)          /* an IPC message did not match its interface */

/*
 * ---- program arguments ----
 * A launcher hands over a flat blob: the argument strings, NUL-terminated, one
 * after another. The kernel copies it into the new address space as a single
 * page at USER_ARGS_BASE, laid out as
 *
 *     u32  argc
 *     u32  off[argc]        byte offsets from the base of the page
 *     ...  the strings, NUL-terminated
 *
 * The page is always mapped, so a program with no arguments reads argc == 0
 * rather than faulting, and it is writable, so a program may chew on its own
 * argument strings in place. Offsets rather than pointers because the blob is
 * built in one address space and read in another; a launcher never hands a
 * child a pointer into its own memory.
 */

/*
 * WHERE the page is is the KERNEL's business, and a program must ask.
 *
 * This address is a 32-bit x86 one. On another architecture the user address
 * space is laid out differently and this number means nothing - so a program
 * that hardcoded it would fault on the first argument it read, and the port
 * would be a userland port rather than a kernel port. SYS_ARGS exists so that
 * the one arch-specific fact here stays on the kernel's side of the line.
 *
 * USER_ARGS_BASE is still in this header because the kernel needs it and
 * because a reader deserves to know where the page actually goes. It is not
 * for userspace to use.
 */
#define USER_ARGS_BASE           0xBFFFF000u    /* x86-32: the page just above the user stack */
#define USER_ARGS_MAX            4096u          /* one page, header included */
#define USER_ARGS_MAXC           64u            /* most arguments one program can be given */

/*
 * ---- the system page ----
 * The kernel maps one read-only page of code into every process and hands
 * its base to the program at entry: in esi on x86-32, rsi on x86-64 (the
 * second argument register, as Fuchsia passes its vDSO). Where the page is
 * is the kernel's choice, and a program never assumes it.
 *
 * The page holds one stub per syscall, SYSPAGE_STUB_SIZE bytes apart: the
 * stub for number n starts at base + n * SYSPAGE_STUB_SIZE. A program loads
 * the arguments as for the gate (ebx, ecx, edx, esi, edi; rdi, rsi, rdx,
 * r10, r8 on x86-64) and calls the stub, which puts n in eax itself, enters
 * the kernel and returns with the result in eax. A number with no syscall
 * has no stub: its slot is int3 throughout, so calling it faults.
 */
#define SYSPAGE_STUB_SIZE        16u            /* bytes per stub; 256 of them fill the page */

/*
 * What SYS_ARGS hands back: where this program's arguments are, and how many.
 * `base` points at the argument page described above - the kernel's choice,
 * not a constant the program is expected to know.
 */
struct args_info {
    uint64_t base;                              /* address of the argument page in THIS process */
    uint32_t count;                             /* argc */
    int32_t broker;                             /* a SEND handle on the launcher's broker endpoint, or -1 */
};

/*
 * ---- shared call structures ----
 * The u64 fields come first in each, so none needs padding before it; padding
 * at the end is a field named `reserved`, which must be 0 (E_INVALID_ARGS).
 * A user address is a u64 on every target (ABI 12; until then a u32, which
 * held only while the user half kept x86-32's layout below 4 GB).
 */
struct spawn_args {
    uint64_t image_len;                         /* bytes at image */
    uint64_t args_len;                          /* total bytes of the args blob, terminators included */
    uint64_t image;                             /* the app's CXEX bytes (in the caller's space) */
    uint64_t name;                              /* optional; not dereferenced in v1 */
    uint64_t args;                              /* argc NUL-terminated strings back to back, or NULL */
    uint64_t handles;                           /* spawn_handle[handle_count], or 0 with a count of 0 */
    int32_t broker_endpoint;                    /* a RECV endpoint handle the caller owns */
    /*
     * requested caps for the child; kernel masks against the spawner's own
     * caps (attenuation, never amplify). 0 = capability-less app.
     */
    uint32_t caps;
    /*
     * the job to run it in: a handle with HRIGHT_ASSIGN, or 0 for the
     * caller's own (SYS_JOB_CREATE)
     */
    int32_t job;
    uint32_t handle_count;                      /* at most SPAWN_HANDLES_MAX */
};

/*
 * ---- handles named at spawn ----
 * A child holds the handles its launcher names and no others: nothing is
 * inherited (XNU's posix_spawn file actions under POSIX_SPAWN_CLOEXEC_DEFAULT,
 * bsd/kern/kern_exec.c exec_handle_file_actions). Each entry is moved or
 * copied exactly as in an IPC message (ipc_handle), and goes in a slot, the
 * number the child asks for with SYS_ARGS_HANDLE, as PSFA_DUP2 names the
 * child's descriptor. The list is read and its slots checked before
 * anything else; the handles are taken only once the image has been
 * verified and the process made, so a refused spawn leaves every one where
 * it was. A slot named twice is E_INVALID_ARGS, one past the last
 * E_OUT_OF_RANGE, and so is a count past SPAWN_HANDLES_MAX; a bad handle,
 * op or right is refused as ipc_call refuses it.
 */
#define SPAWN_HANDLES_MAX        16
#define ARGS_STDIN               0              /* the slot of standard input */
#define ARGS_STDOUT              1              /* standard output */
#define ARGS_STDERR              2              /* standard error */

struct spawn_handle {
    int32_t handle;
    uint32_t op;                                /* IPC_HANDLE_MOVE or IPC_HANDLE_COPY */
    uint32_t rights;                            /* a subset of the handle's, or IPC_RIGHTS_SAME */
    uint32_t slot;                              /* below SPAWN_HANDLES_MAX; one handle per slot */
};

/*
 * ---- clock and sleep (SYS_CLOCK, SYS_SLEEP; unprivileged) ----
 *
 * Neither needs a capability. Reading the clock discloses nothing a process
 * could not time for itself, and waiting is the opposite of a privilege - a
 * sleeping process is one not using the machine. SYS_POWER_SLEEP is a
 * different thing entirely: it idles the whole CPU, every thread with it, and
 * is rightly behind GRANT_POWER. SYS_SLEEP blocks one thread and leaves the
 * rest of the system running.
 *
 * Two clocks, because they answer different questions:
 *
 *   ticks    milliseconds since boot, from the 1000 Hz timer. Monotonic. Use
 *            it to measure an interval - it cannot jump, and it does not care
 *            whether the machine knows what year it is.
 *   epoch    seconds since 1970, from the RTC. Use it to stamp a file or to
 *            decide that it is 3am. 0 means the RTC could not be read, so a
 *            caller that needs a real date must check rather than assume.
 *
 * A scheduler wants both: `ticks` to decide that 30 seconds have passed, and
 * `epoch` to record when something ran.
 *
 * SYS_CLOCK_MONOTONIC reads the clock `ticks` comes from in nanoseconds and
 * 64 bits, which never wrap; every deadline (SYS_OBJECT_WAIT, SYS_TIMER_SET)
 * is a time on it. It advances one tick at a time today: a deadline is met at
 * the first tick at or after it.
 */
struct clock_info {
    uint32_t ticks;                             /* ms since boot, monotonic; wraps after ~49 days */
    uint32_t epoch;                             /* seconds since 1970 from the RTC, or 0 if unavailable */
};

/*
 * SYS_SLEEP bounds. A sleep is a promise to come back, so it is capped: an
 * unbounded one is indistinguishable from a hang, and nothing in the system
 * needs to wait longer than an hour in a single call.
 */
#define SLEEP_MAX_MS             3600000u       /* one hour */

/*
 * IPC: ipc_call blocks the caller until the server answers its reply token,
 * in whatever order the server answers its calls. A message or a reply is at
 * most IPC_MSG_MAX bytes, copied through a kernel buffer charged to the
 * caller's job while the call lasts. The message schema is the interface's;
 * the kernel just moves bytes.
 */
#define IPC_MSG_MAX              65536

/*
 * SYS_OBJECT_INFO: what one of the caller's handles names. OBJINFO_BASIC
 * needs no right and no grant: it describes only what the caller already
 * holds. Any other topic needs the right it names.
 */
#define OBJINFO_BASIC            1              /* -> struct object_basic */
#define OBJINFO_PROCESS          2              /* a process (HRIGHT_INSPECT) -> struct process_info */
#define OBJINFO_JOB              3              /* a job (HRIGHT_INSPECT) -> struct job_info */
#define OBJINFO_MEM              4              /* a memory object (HRIGHT_INSPECT) -> struct mem_info */
#define OBJINFO_THREAD           5              /* a thread (HRIGHT_INSPECT) -> struct thread_info */
#define OBJ_TYPE_ENDPOINT        1
#define OBJ_TYPE_FILE            2
#define OBJ_TYPE_PROCESS         4              /* 3 is the kernel self-tests' own */
#define OBJ_TYPE_JOB             5
#define OBJ_TYPE_TIMER           6
#define OBJ_TYPE_QUEUE           7
#define OBJ_TYPE_SEND            8              /* a send handle's object: an endpoint and a badge */
#define OBJ_TYPE_REPLY           9              /* a reply token */
#define OBJ_TYPE_NOTIFY          10             /* a notification */
#define OBJ_TYPE_SIGNALLER       11             /* a signaller: a notification and the bits it may set */
#define OBJ_TYPE_MEM             12             /* a memory object */
#define OBJ_TYPE_PIPE            13             /* a pipe end, read or write */
#define OBJ_TYPE_THREAD          14             /* a thread (SYS_THREAD_CREATE) */

/*
 * Pipe sizes (SYS_PIPE_CREATE). A write of PIPE_ATOMIC bytes or fewer goes
 * into the pipe whole, never split around another writer's (POSIX PIPE_BUF).
 */
#define PIPE_ATOMIC              512
#define PIPE_SIZE_DEFAULT        4096
#define PIPE_SIZE_MIN            512
#define PIPE_SIZE_MAX            65536

/* Registrations one queue holds at most (SYS_QUEUE_REGISTER). */
#define QUEUE_REGISTER_MAX       64

/* Events one SYS_QUEUE_WAIT writes at most; a larger count is not refused. */
#define QUEUE_EVENTS_MAX         16

/* SYS_QUEUE_WAIT: one key that became ready. */
struct queue_event {
    uint64_t key;                               /* as registered */
    uint32_t signals;                           /* the object's signals when the key became ready */
    uint32_t reserved;                          /* 0 */
};

/* Jobs nest at most this deep below the kernel's own (SYS_JOB_CREATE). */
#define JOB_DEPTH_MAX            8

/*
 * SYS_JOB_CREATE, SYS_JOB_SET_LIMITS. 0 is no limit of the job's own; a
 * job above it may still have one.
 */
struct job_limits {
    uint64_t memory;                            /* bytes */
    uint32_t threads;
    /*
     * the highest priority its threads may set themselves (PRIO_*); a
     * thread made in it starts no higher. Takes nothing back.
     */
    uint32_t priority;
};

/*
 * ---- thread priority (SYS_THREAD_SET_PRIORITY) ----
 * The highest level with a thread ready runs; threads at one level take
 * turns. A thread serving a call runs at least at its caller's level, and so
 * on down a chain of calls; a call still queued lifts the threads answering
 * that endpoint's other calls. 0 is no thread's level.
 */
#define PRIO_MIN                 1
#define PRIO_BACKGROUND          2              /* background is 1-3 */
#define PRIO_NORMAL              8              /* normal is 4-11; a new thread's level */
#define PRIO_REALTIME            12             /* real-time is 12-15: GRANT_REALTIME */
#define PRIO_MAX                 15

/*
 * ---- user threads (SYS_THREAD_CREATE) ----
 * A thread's stack, in bytes; a size is rounded up to a page.
 */
#define THREAD_USTACK_DEFAULT    65536
#define THREAD_USTACK_MIN        16384
#define THREAD_USTACK_MAX        1048576

struct thread_create_args {
    uint64_t entry;                             /* where it starts, in the caller's image */
    uint64_t arg0;                              /* in rdi */
    uint64_t arg1;                              /* in rsi */
    uint64_t stack_size;                        /* 0 is THREAD_USTACK_DEFAULT */
    uint32_t flags;                             /* 0 */
    uint32_t reserved;                          /* 0 */
};

/* thread_info.ended: how it ended, once SIGNAL_THREAD_ENDED is set. */
#define THR_RUNNING              0              /* not ended */
#define THR_EXITED               1              /* SYS_THREAD_EXIT; `code` is its argument */

/*
 * with its process: SYS_EXIT, a crash or a kill, in any of its threads
 * (process_info says which)
 */
#define THR_WITH_PROCESS         2

struct thread_info {
    uint32_t tid;                               /* for display; names the thread until it is reaped */
    uint32_t ended;                             /* THR_* */
    int32_t code;                               /* THR_EXITED */
    uint32_t reserved;                          /* 0 */
};

/*
 * OBJINFO_MEM: a memory object's size, how much of it has been touched, and
 * its seals.
 */
struct mem_info {
    uint64_t size;                              /* bytes, a whole number of pages; all of it committed */
    uint64_t resident;                          /* bytes of it that hold a page now */
    uint32_t seals;                             /* MEM_SEAL_* */
    uint32_t reserved;                          /* 0 */
};

struct job_info {
    uint64_t memory_limit;
    uint64_t memory_used;                       /* by its processes and the jobs below it */
    uint64_t memory_peak;                       /* the most memory_used has been */
    uint32_t thread_limit;
    uint32_t threads;                           /* in it and below */
    uint32_t processes;                         /* in it and below, not yet ended */
    uint32_t killed;                            /* 1 once SYS_JOB_KILL has reached it */
    uint32_t priority_limit;                    /* its own job_limits.priority */
    uint32_t reserved;                          /* 0 */
};

/* process_info.ended: how it ended, once SIGNAL_PROCESS_ENDED is set. */
#define PROC_RUNNING             0              /* not ended */
#define PROC_EXITED              1              /* SYS_EXIT; `code` is its argument */

/*
 * a CPU exception in ring 3, or a call from outside the system page;
 * `vector`, `ip`, and for a page fault `addr`
 */
#define PROC_CRASHED             2

/* it never reached ring 3: the loader refused the image, or no memory */
#define PROC_NOT_STARTED         3
#define PROC_KILLED              4              /* SYS_PROCESS_KILL; `killer` is who */

/*
 * SYS_PROCESS_STOP, whose deadline passed (or nothing could ask it); `killer`
 * is who asked
 */
#define PROC_STOPPED             5

struct process_info {
    uint64_t ip;                                /* PROC_CRASHED: where */
    uint64_t addr;                              /* PROC_CRASHED by a page fault: the address it touched */
    uint32_t pid;                               /* for display; it names the process until the record goes */
    uint32_t ended;                             /* PROC_* */
    int32_t code;                               /* PROC_EXITED */
    uint32_t vector;                            /* PROC_CRASHED: the exception, or PROC_VECTOR_REFUSED */
    uint32_t killer;                            /* PROC_KILLED, PROC_STOPPED: the pid that asked, as it was then */
    uint32_t reserved;                          /* 0 */
};

/*
 * process_info.vector: not an exception but a system call from outside the
 * system page, which ends the process the way a fault does
 */
#define PROC_VECTOR_REFUSED      256u

struct object_info_args {
    uint64_t cap;                               /* bytes at buf; smaller than the topic needs is E_BUFFER_TOO_SMALL */
    uint64_t buf;                               /* the topic's structure is written here */
    int32_t handle;
    uint32_t topic;                             /* OBJINFO_* */
};

struct object_basic {
    uint32_t kind;                              /* OBJ_TYPE_* (not `type`: an X keyword) */
    uint32_t rights;                            /* this handle's */
    uint32_t koid;                              /* the object's kernel id: handles on one object agree */
    uint32_t grant;                             /* this handle's grant id */
    uint32_t parent_grant;                      /* the grant it was derived from; 0 = none */
};

/*
 * Handles a message carries, beside its bytes, never inside them: a request
 * or a reply takes at most IPC_HANDLES_MAX. Each is moved, or copied, which
 * needs HRIGHT_DUPLICATE; either way its rights can only shrink (XNU
 * ipc_kmsg_copyin_port_descriptor). A moved handle keeps its grant id; a copy
 * gets a new one, derived from it. All are checked before any is taken: one
 * bad entry refuses the message and leaves every handle where it was.
 * E_NOT_SUPPORTED: the endpoint or reply token the message goes through,
 * which cannot carry itself (Zircon refuses a channel written into itself).
 * Every type travels; an open file takes its locks along (they belong to the
 * open file, not a thread). A handle named twice is E_INVALID_ARGS.
 */
#define IPC_HANDLES_MAX          16
#define IPC_HANDLE_MOVE          1              /* the handle leaves the sender's table */
#define IPC_HANDLE_COPY          2              /* the sender keeps it; needs HRIGHT_DUPLICATE */

/*
 * ipc_handle.rights: the handle's own rights, unchanged. No type defines
 * every bit, so it cannot be a real set.
 */
#define IPC_RIGHTS_SAME          0xFFFFFFFFu

struct ipc_handle {
    int32_t handle;
    uint32_t op;                                /* IPC_HANDLE_MOVE or IPC_HANDLE_COPY */
    uint32_t rights;                            /* a subset of the handle's, or IPC_RIGHTS_SAME */
    uint32_t reserved;                          /* 0 */
};

struct ipc_call_args {
    uint64_t req_len;
    uint64_t reply_cap;
    uint64_t req;                               /* request bytes */
    uint64_t reply;                             /* buffer the reply is copied into */
    uint64_t handles;                           /* ipc_handle[handle_count], sent with the request */
    uint64_t reply_handles;                     /* i32[reply_handle_cap]: the reply's handles, then 0s */
    int32_t ep_handle;                          /* a send handle (HRIGHT_SEND), as the broker channel is */
    uint32_t handle_count;                      /* at most IPC_HANDLES_MAX */
    uint32_t reply_handle_cap;                  /* past IPC_HANDLES_MAX is taken as IPC_HANDLES_MAX */
    uint32_t reserved;
};

struct ipc_recv_args {
    uint64_t cap;
    uint64_t buf;                               /* the message is copied here */
    uint64_t info;                              /* where an ipc_msg_info is written; never 0 */
    uint64_t handles;                           /* i32[handle_cap]: the message's handles are written here */
    int32_t ep_handle;                          /* an endpoint handle (HRIGHT_RECV) */
    uint32_t handle_cap;                        /* past IPC_HANDLES_MAX is taken as IPC_HANDLES_MAX */
};

/* What ipc_recv says about the message beside its bytes. */
struct ipc_msg_info {
    uint64_t badge;                             /* the badge of the send handle it came through; 0 for a notice */
    uint64_t len;                               /* its length, also when it did not fit */
    uint64_t reply_cap;                         /* the longest reply the caller takes; 0 for a notice */
    int32_t sender;                             /* the caller's pid, informational; IPC_SENDER_KERNEL for a notice */
    int32_t reply;                              /* the call's reply token (HRIGHT_REPLY); 0 for a notice, or when it did not fit */
    uint32_t handles;                           /* the handles it carries, also when they did not fit */
    uint32_t reply_handles;                     /* the most handles the caller takes back in the reply */
};

/*
 * ipc_msg_info.sender for a message from the kernel itself, not a call: no
 * pid is negative. There is no caller, so it takes no ipc_reply.
 */
#define IPC_SENDER_KERNEL        (-1)

/* stop_notice.kind */
#define NOTICE_STOP              1

/*
 * What ipc_recv delivers on a stop endpoint (SYS_PROCESS_STOP). A cap
 * smaller than it is E_BUFFER_TOO_SMALL, and the notice stays.
 */
struct stop_notice {
    uint32_t kind;                              /* NOTICE_STOP */
    uint32_t stopper;                           /* the pid that asked */
    uint32_t deadline_ms;                       /* how long it gave, from when it asked */
    uint32_t reserved;                          /* 0 */
};

struct ipc_reply_args {
    uint64_t data;                              /* reply bytes */
    uint64_t len;
    uint64_t handles;                           /* ipc_handle[handle_count], sent with the reply */
    int32_t token;                              /* the call's reply token, from ipc_msg_info.reply */
    uint32_t handle_count;                      /* at most ipc_msg_info.reply_handles */
};

/* ---- framebuffer (SYS_FB_INFO) ---- */
struct fb_geometry {
    uint32_t width;
    uint32_t height;
    uint32_t bpp;                               /* 16 or 32 */
    uint32_t pitch;                             /* bytes per scanline */
};

/*
 * ---- capabilities (for spawn_args.caps) ----
 * A process's authority, as a bitmask: what it is ALLOWED TO DO. One bit per
 * class of privileged primitive, checked by the syscall dispatcher before the
 * call runs. GRANT_ because each bit is a grant - handed over when the process
 * was started, and never wider than what the thing that started it held.
 * kernel/cpu/caps.h describes what each one guards.
 */
#define GRANT_CONSOLE            0x0001u

/*
 * reserved, NOT enforced: the SYS_VM_* calls are unprivileged by design - see
 * kernel/cpu/caps.h
 */
#define GRANT_MEM                0x0002u
#define GRANT_DISK               0x0004u
#define GRANT_NET                0x0008u
#define GRANT_SPAWN              0x0010u
#define GRANT_POWER              0x0020u
#define GRANT_ENDPOINT           0x0040u
#define GRANT_IOPORT             0x0080u
#define GRANT_FRAMEBUFFER        0x0100u

/* SYS_THREAD_SET_PRIORITY to PRIO_REALTIME and up */
#define GRANT_REALTIME           0x0200u
#define GRANT_OS_BASELINE        (GRANT_CONSOLE | GRANT_MEM | GRANT_DISK | GRANT_SPAWN | GRANT_POWER | GRANT_ENDPOINT | GRANT_FRAMEBUFFER | GRANT_NET | GRANT_REALTIME)

/*
 * Not a grant: a restriction, carried in spawn_args.caps's top bit. The child's
 * SYS_CONSOLE_WRITE goes to the kernel log, a line at a time, instead of the
 * screen - the supervisor and its services, which would otherwise print over
 * the shell. Every process the child starts inherits it, asked for or not, so
 * a background process cannot hand the screen to its own children.
 */
#define SPAWN_BACKGROUND         0x80000000u

/*
 * ---- network (SYS_NET_GET_CONFIG) ----
 * Each a packed IPv4 address, network order.
 */
struct net_ip_config {
    uint32_t ip;
    uint32_t mask;
    uint32_t gateway;
    uint32_t dns;                               /* the primary server */
};

/*
 * ---- path execution (SYS_EXEC_PATH; GRANT_SPAWN) ----
 *
 * Run the CXEX image at `path`. Takes the same spawn_args as SYS_SPAWN for the
 * name, broker endpoint and requested caps, and IGNORES its image/image_len -
 * the kernel reads the file itself.
 *
 * That is the whole point of having a second call rather than letting a
 * process read a file and hand the bytes to SYS_SPAWN. SYS_SPAWN takes an
 * image already in the caller's memory and does not verify it; only the
 * kernel's own load path checks signatures. Reading the image inside the
 * kernel means the bytes that are verified are exactly the bytes that are
 * loaded, with no window in which the caller could swap them, and no way for
 * ring 3 to introduce code that was never signed.
 *
 * Caps are attenuated against the caller's own set, as with SYS_SPAWN: you may
 * pass a subset of your authority and never amplify.
 */

/*
 * ---- memory (SYS_VM_*, SYS_MEM_CREATE; unprivileged, bounded by a per-process quota and the job) ----
 *
 * Ask for memory, give it back. This is the only way a ring-3 process obtains
 * storage beyond its image and its stack.
 *
 * It is UNPRIVILEGED and deliberately not behind a grant. A capability every
 * program must hold is not a capability, it is noise - and denying a process
 * the ability to allocate does not make it safe, it makes it broken. The
 * containment is a per-process QUOTA on how much may be mapped at once,
 * attenuated on spawn exactly as grants are, and the job's memory budget,
 * which every memory object's commit is charged to.
 *
 * MEMORY OBJECTS (ABI 23). Memory is held by objects; a mapping is a view of
 * one. SYS_MEM_CREATE makes one and returns its handle; `object` 0 in
 * vm_map_args makes a new one for that mapping alone. An object's pages are
 * zero and arrive on FIRST TOUCH, through the page fault, but all of them are
 * COMMITTED when it is made (strict commit): its size is charged to the job
 * and that many physical pages are set aside, so running out is E_NO_MEMORY
 * from the call that asked, never a fault with nothing behind it. There is no
 * overcommit and no OOM killer. An object lives while a handle or a mapping
 * holds it, and its pages go with it.
 *
 * SHARING (ABI 24). An object's handle travels in IPC messages, and its
 * commit stays charged to the job that made it. A receiver that must trust
 * the bytes not to change asks OBJINFO_MEM for MEM_SEAL_WRITE: the sender
 * unmaps its writable views and seals the object (SYS_MEM_SEAL) first.
 *
 * WHAT A MAPPING IS ADDRESSED BY: an object, a 64-bit OFFSET into it (a whole
 * number of pages, and 0 for a new anonymous one), and a LENGTH that must fit
 * inside it. The offset is 64 bits so that an object larger than the window
 * that views it - a file, a device aperture, shared memory - can be mapped a
 * window at a time.
 *
 * WHAT COMES BACK. `addr` is where the mapping landed and `granted` is how
 * many bytes it actually covers. Both are outputs, because both are the
 * kernel's to decide: hardware maps in PAGES, and a page is 4 KB here, 16 KB
 * or 64 KB elsewhere. A caller asking in bytes and being TOLD what it got
 * never has to know the page size.
 *
 * `addr` is also an INPUT when MMAP_HINT is set - a suggested address the
 * kernel may decline. That is what makes a moving window cheap: unmap and
 * remap at the same address and the pointers inside stay valid. It is a hint
 * and not a demand because a process that could dictate its own layout could
 * ask to land on top of something it does not own.
 *
 * PROTECTION. `prot` is explicit, and MPROT_WRITE with MPROT_EXEC together is
 * REFUSED. A page mapped without MPROT_WRITE faults on a store, and one
 * without MPROT_EXEC on an instruction fetch (the no-execute bit). Mapping an
 * object needs HRIGHT_MAP, and HRIGHT_MAP_WRITE or HRIGHT_MAP_EXEC for those.
 */

/* vm_map_args.prot - what the mapping may be used for. */
#define MPROT_NONE               0x00
#define MPROT_READ               0x01
#define MPROT_WRITE              0x02
#define MPROT_EXEC               0x04

/* vm_map_args.flags */
#define MMAP_HINT                0x01           /* try `addr` first; the kernel may place elsewhere */

/*
 * vm_map_args.object: 0 asks for a new anonymous memory object, made for the
 * mapping and gone with it.
 */
#define MOBJ_ANON                0

/*
 * SYS_MEM_SEAL, mem_info.seals. A seal binds every holder of the object, its
 * creator too, so a receiver that finds MEM_SEAL_WRITE knows nobody can
 * change the bytes after it has checked them (Linux memfd seals).
 */
#define MEM_SEAL_WRITE           0x01           /* no writable mapping, now or later */
#define MEM_SEAL_SEAL            0x02           /* no more seals */

/*
 * The largest single mapping, and the largest memory object, a process may
 * ask for. A bound rather than a limit of the design, far above anything that
 * has a reason to exist yet. 64 MiB.
 */
#define MMAP_MAX_BYTES           0x04000000u

/*
 * How many separate mappings one process may hold at once (a balanced tree
 * since ABI 23; it was a fixed array of 32).
 */
#define MMAP_MAX_REGIONS         256

struct vm_map_args {
    uint64_t offset;                            /* into the object, a whole number of pages; 0 for MOBJ_ANON */
    uint64_t length;                            /* bytes wanted */
    uint64_t granted;                           /* out: bytes actually mapped (length, rounded up to a page) */
    /* in: a hint if MMAP_HINT; out: where it is */
    uint64_t addr;
    uint32_t object;                            /* a memory object handle (HRIGHT_MAP), or MOBJ_ANON */
    uint32_t prot;                              /* MPROT_* */
    uint32_t flags;                             /* MMAP_* */
    uint32_t reserved;
};

/* SYS_VM_INFO: this process's memory account. */
struct vm_usage {
    uint64_t quota;                             /* the ceiling, in bytes */
    uint64_t mapped;                            /* how much of it is in use */
};

/*
 * ---- files (SYS_FILE_*; GRANT_DISK) ----
 *
 * The filesystem as userspace sees it. Offsets, sizes and lock ranges are u64,
 * as CXFS's own are (ABI 10; until then they were 32-bit and signed, which
 * capped a file at 2 GB). An offset runs to 2^63 - 1, so a seek's signed
 * arithmetic cannot wrap; CXFS itself refuses a file past 2^32 blocks long
 * before that. read/write return a byte count in an i32, so one call moves at
 * most 2^31 - 1 bytes, and a negative return is an E_* code. seek and tell
 * give the offset back through a *u64, since a result register is 32 bits on
 * x86-32.
 *
 * A handle from SYS_FILE_OPEN is an ordinary entry in the process's handle table
 * (abi sec 6), so SYS_HANDLE_CLOSE releases it as well as SYS_FILE_CLOSE does,
 * and it is dropped with the rest of the table when the process exits.
 *
 * Paths are resolved against the process's cwd unless they start with '/'.
 */

/* SYS_FILE_LOCK modes. Shared locks coexist; an exclusive lock excludes all. */
#define FLOCK_SHARED             1
#define FLOCK_EXCLUSIVE          2
#define FLOCK_WAIT               0x100          /* sleep until the lock can be had */

/* SYS_FILE_OPEN flags. One of FOPEN_READ / FOPEN_WRITE is required. */
#define FOPEN_READ               0x01
#define FOPEN_WRITE              0x02
#define FOPEN_CREATE             0x04           /* create the file when it does not exist */
#define FOPEN_TRUNC              0x08           /* cut an existing file to zero length on open */
#define FOPEN_APPEND             0x10           /* every write goes to the current end of file */

/* SYS_FILE_SEEK origin */
#define FSEEK_SET                0
#define FSEEK_CUR                1
#define FSEEK_END                2

/* entry kinds reported by file_stat.type */
#define FTYPE_FILE               1
#define FTYPE_DIR                2
#define FILE_NAME_MAX            64             /* must equal CXFS_NAME_LEN */
#define FILE_PATH_MAX            256            /* longest path a syscall will copy in */

/* SYS_FILE_LOCK's range and mode. */
struct file_lock_args {
    uint64_t start;
    uint64_t len;                               /* 0 = to the end of the file, however it grows */
    uint32_t mode;                              /* FLOCK_SHARED or FLOCK_EXCLUSIVE, optionally | FLOCK_WAIT */
    uint32_t reserved;
};

/*
 * Timestamps are unsigned epoch seconds truncated to 32 bits, which runs out in
 * 2106; CXFS keeps the full 64 bits on disk. Time becomes i64 nanoseconds in
 * its own step (docs/kernel/CX_SYSCALLS_IPC.md sec 5).
 */
struct file_stat {
    uint32_t id;
    /*
     * FTYPE_* ("type" is a reserved word in X, so the ABI side cannot call it
     * that)
     */
    uint32_t kind;
    uint64_t size;
    uint32_t permissions;
    uint32_t owner_uid;
    uint32_t created;
    uint32_t modified;
    uint32_t accessed;
    uint32_t reserved;                          /* 0 */
    char name[FILE_NAME_MAX];
};

/*
 * ---- pointer input (SYS_MOUSE_READ) ----
 * Absolute cursor position maintained by the kernel driver, already clamped to
 * the screen, so userspace never sees raw relative deltas. `seq` increments on
 * every state change - compare it against the previous read to detect movement
 * without diffing coordinates. Unprivileged, like keyboard input.
 */
struct mouse_state {
    int32_t x;
    int32_t y;
    uint32_t buttons;                           /* bit 0 left, bit 1 right, bit 2 middle */
    uint32_t seq;
};

/*
 * ---- the kernel log (SYS_LOG_READ; GRANT_CONSOLE) ----
 * One entry of the kernel's log ring, oldest first: every kernel log line and
 * every line a background process wrote. The ring keeps the newest few hundred;
 * `seq` counts every entry ever recorded, so a gap between two reads shows how
 * many were overwritten. The strings are NUL-padded and need not be
 * terminated when full.
 */
#define LOG_SYSTEM_W             12
#define LOG_EVENT_W              48

struct log_entry {
    uint32_t seq;
    uint32_t sev;                               /* 0 INFO, 1 OK, 2 WARN, 3 ERR, 4 FAIL */
    char system[LOG_SYSTEM_W];                  /* the tag: EXEC, KTEST, or the pid as "pid 3" */
    char subsystem[LOG_SYSTEM_W];
    char event[LOG_EVENT_W];
};

/* ---- layout lock ----
 * Every structure above crosses the user/kernel boundary, so its size is the
 * contract, not an accident of the compiler. The generator refuses implicit
 * padding and computed these sizes; the compiler confirms them. No field is a
 * C pointer: a user address is an integer, so the kernel cannot dereference
 * one by accident - it goes through user_copy_in/out. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
  #define CXK_ABI_STATIC_ASSERT(c, m) _Static_assert(c, m)
#else
  #define CXK_ABI_STATIC_ASSERT(c, m) typedef char cxk_abi_assert_##__LINE__[(c) ? 1 : -1]
#endif

CXK_ABI_STATIC_ASSERT(sizeof(struct args_info) == 16, "ABI layout: args_info");
CXK_ABI_STATIC_ASSERT(sizeof(struct spawn_args) == 64, "ABI layout: spawn_args");
CXK_ABI_STATIC_ASSERT(sizeof(struct spawn_handle) == 16, "ABI layout: spawn_handle");
CXK_ABI_STATIC_ASSERT(sizeof(struct clock_info) == 8, "ABI layout: clock_info");
CXK_ABI_STATIC_ASSERT(sizeof(struct queue_event) == 16, "ABI layout: queue_event");
CXK_ABI_STATIC_ASSERT(sizeof(struct job_limits) == 16, "ABI layout: job_limits");
CXK_ABI_STATIC_ASSERT(sizeof(struct thread_create_args) == 40, "ABI layout: thread_create_args");
CXK_ABI_STATIC_ASSERT(sizeof(struct thread_info) == 16, "ABI layout: thread_info");
CXK_ABI_STATIC_ASSERT(sizeof(struct mem_info) == 24, "ABI layout: mem_info");
CXK_ABI_STATIC_ASSERT(sizeof(struct job_info) == 48, "ABI layout: job_info");
CXK_ABI_STATIC_ASSERT(sizeof(struct process_info) == 40, "ABI layout: process_info");
CXK_ABI_STATIC_ASSERT(sizeof(struct object_info_args) == 24, "ABI layout: object_info_args");
CXK_ABI_STATIC_ASSERT(sizeof(struct object_basic) == 20, "ABI layout: object_basic");
CXK_ABI_STATIC_ASSERT(sizeof(struct ipc_handle) == 16, "ABI layout: ipc_handle");
CXK_ABI_STATIC_ASSERT(sizeof(struct ipc_call_args) == 64, "ABI layout: ipc_call_args");
CXK_ABI_STATIC_ASSERT(sizeof(struct ipc_recv_args) == 40, "ABI layout: ipc_recv_args");
CXK_ABI_STATIC_ASSERT(sizeof(struct ipc_msg_info) == 40, "ABI layout: ipc_msg_info");
CXK_ABI_STATIC_ASSERT(sizeof(struct stop_notice) == 16, "ABI layout: stop_notice");
CXK_ABI_STATIC_ASSERT(sizeof(struct ipc_reply_args) == 32, "ABI layout: ipc_reply_args");
CXK_ABI_STATIC_ASSERT(sizeof(struct fb_geometry) == 16, "ABI layout: fb_geometry");
CXK_ABI_STATIC_ASSERT(sizeof(struct net_ip_config) == 16, "ABI layout: net_ip_config");
CXK_ABI_STATIC_ASSERT(sizeof(struct vm_map_args) == 48, "ABI layout: vm_map_args");
CXK_ABI_STATIC_ASSERT(sizeof(struct vm_usage) == 16, "ABI layout: vm_usage");
CXK_ABI_STATIC_ASSERT(sizeof(struct file_lock_args) == 24, "ABI layout: file_lock_args");
CXK_ABI_STATIC_ASSERT(sizeof(struct file_stat) == 104, "ABI layout: file_stat");
CXK_ABI_STATIC_ASSERT(sizeof(struct mouse_state) == 16, "ABI layout: mouse_state");
CXK_ABI_STATIC_ASSERT(sizeof(struct log_entry) == 80, "ABI layout: log_entry");

#endif
