// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* abi/cxk_abi.h: GENERATED from abi/kernel.xfil (cx.kernel) by `cxk abi generate`.
 * Do not edit it: change abi/kernel.xfil and regenerate. `cxk check-abi` fails while
 * this file differs from what the .xfil generates. */

#ifndef CXK_ABI_H
#define CXK_ABI_H

#include <stdint.h>

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
 *   64-bit values are passed by pointer to a struct, never split across regs.
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
#define CXK_ABI_VERSION          6

/*
 * ---- syscall numbers ----
 * lifecycle 0x00-0x0F (no capability required)
 */
#define SYS_EXIT                 0x00           /* ebx = exit code; does not return */
#define SYS_YIELD                0x01           /* cooperatively yield the CPU */
#define SYS_GETPID               0x02           /* -> caller pid */
#define SYS_GETUID               0x03           /* -> caller owning uid (0 = SYSTEM) */
#define SYS_CLOCK                0x04           /* ebx = *clock_info -> 0, or -E_* */
#define SYS_SLEEP                0x05           /* ebx = ms; blocks this thread, then returns 0 */
#define SYS_ARGS                 0x06           /* ebx = *args_info -> 0, or -E_* */

/* IPC + handles 0x10-0x1F */
#define SYS_IPC_CALL             0x10           /* ebx = *ipc_call_args -> reply length (blocks) */
#define SYS_IPC_RECV             0x11           /* ebx = *ipc_recv_args -> request length (blocks) */
#define SYS_IPC_REPLY            0x12           /* ebx = *ipc_reply_args -> 0 */
#define SYS_EP_CREATE            0x13           /* create an endpoint -> RECV handle (GRANT_ENDPOINT) */
#define SYS_HANDLE_CLOSE         0x16           /* release a handle */

/* ebx = handle, ecx = rights (a subset of its own; needs HRIGHT_DUPLICATE) -> new handle */
#define SYS_HANDLE_DUPLICATE     0x17
#define SYS_OBJECT_INFO          0x18           /* ebx = *object_info_args -> bytes written */

/*
 * ---- handles ----
 * A handle is a positive number, never 0 and never below 256, and a closed
 * handle's number is refused rather than naming whatever comes next. Its
 * rights say what its holder may do with the object, and they only shrink:
 * SYS_HANDLE_DUPLICATE can drop rights, never add one. Each object type
 * defines its own (kernel/object/htab.h).
 */
#define HRIGHT_SEND              0x1u           /* endpoint: ipc_call through it */

/* endpoint: ipc_recv and ipc_reply on it; file: FOPEN_READ, FOPEN_WRITE */
#define HRIGHT_RECV              0x2u

/*
 * Generic rights, which mean the same on every type, count down from the top
 * bit; a type's own count up from the bottom. A handle without
 * HRIGHT_DUPLICATE cannot be copied, and a copy has it only if asked for.
 */
#define HRIGHT_DUPLICATE         0x80000000u

/* input 0x20-0x2F (unprivileged: reading your own keystrokes) */
#define SYS_INPUT_READ           0x20           /* block until a key -> char */
#define SYS_MOUSE_READ           0x21           /* ebx = *mouse_state -> 1 if a mouse is present */
#define SYS_INPUT_POLL           0x22           /* -> char, 0 if none waiting */

/* console 0x30-0x3F (privileged: GRANT_CONSOLE) */
#define SYS_CONSOLE_WRITE        0x30           /* ebx = buf, ecx = len (0 = bounded NUL-scan) */

/* ebx = index (0 = oldest kept), ecx = *log_entry -> 1 filled, 0 past the newest */
#define SYS_LOG_READ             0x31

/* network 0x50-0x5F (privileged: GRANT_NET) */
#define SYS_NET_OP               0x50           /* ebx = *net_op_args (GRANT_NET) -> op-specific */

/* framebuffer 0x40-0x4F (privileged: GRANT_FRAMEBUFFER) */
#define SYS_FB_OP                0x40           /* ebx = *fb_op_args (GRANT_FRAMEBUFFER) -> op-specific */

/* files 0x60-0x6F (privileged: GRANT_DISK) */
#define SYS_FILE_OP              0x60           /* ebx = *file_op_args (GRANT_DISK) -> op-specific */

/* process 0x70-0x7F (privileged) */
#define SYS_SPAWN                0x70           /* ebx = *spawn_args (GRANT_SPAWN) -> pid */

/*
 * ebx = path, ecx = *spawn_args (GRANT_SPAWN) -> pid (image/image_len in the
 * struct are ignored)
 */
#define SYS_EXEC_PATH            0x72

/* memory 0x80-0x8F (unprivileged, quota-bounded) */
#define SYS_VM_MAP               0x80           /* ebx = *vm_map_args -> 0, with addr and granted filled in */

/* ebx = addr, ecx = length (0 = the whole mapping) -> 0 */
#define SYS_VM_UNMAP             0x81
#define SYS_VM_INFO              0x82           /* ebx = *vm_usage -> 0, with quota and mapped filled in */

/* power 0x90-0x9F (privileged: GRANT_POWER). Each returns only if refused. */
#define SYS_POWER_REBOOT         0x90
#define SYS_POWER_SHUTDOWN       0x91           /* ACPI S5 soft-off */

/* ebx = ms: idle the CPU (C1) that long, or until a key; 0 = until a key -> 0 */
#define SYS_POWER_SLEEP          0x92

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
#define E_DEADLOCK               (-24)          /* waiting would never end (FILE_OP_LOCK) */
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
 * What SYS_ARGS hands back: where this program's arguments are, and how many.
 * `base` points at the argument page described above - the kernel's choice,
 * not a constant the program is expected to know.
 */
struct args_info {
    uint32_t count;                             /* argc */
    uint32_t base;                              /* address of the argument page in THIS process */
    int32_t broker;                             /* a SEND handle on the launcher's broker endpoint, or -1 */
};

/* ---- shared call structures ---- */
struct spawn_args {
    uint32_t image;                             /* the app's CXEX bytes (in the caller's space) */
    uint32_t image_len;
    uint32_t name;                              /* optional; not dereferenced in v1 */
    int32_t broker_endpoint;                    /* a RECV endpoint handle the caller owns */
    /*
     * requested caps for the child; kernel masks against the spawner's own
     * caps (attenuation, never amplify). 0 = capability-less app.
     */
    uint32_t caps;
    uint32_t args;                              /* argc NUL-terminated strings back to back, or NULL */
    uint32_t args_len;                          /* total bytes of that blob, terminators included */
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
 * IPC is synchronous: ipc_call blocks the caller until the owner ipc_replies.
 * Messages are bounded (<= one page) and copied through the kernel. The message
 * schema is defined by the executive; the kernel just moves bytes.
 */

/*
 * SYS_OBJECT_INFO: what one of the caller's handles names. It needs no right
 * and no grant: it describes only what the caller already holds.
 */
#define OBJINFO_BASIC            1              /* -> struct object_basic */
#define OBJ_TYPE_ENDPOINT        1
#define OBJ_TYPE_FILE            2

struct object_info_args {
    int32_t handle;
    uint32_t topic;                             /* OBJINFO_* */
    uint32_t buf;                               /* the topic's structure is written here */
    uint32_t cap;                               /* bytes at buf; smaller than the topic needs is E_BUFFER_TOO_SMALL */
};

struct object_basic {
    uint32_t kind;                              /* OBJ_TYPE_* (not `type`: an X keyword) */
    uint32_t rights;                            /* this handle's */
    uint32_t koid;                              /* the object's kernel id: handles on one object agree */
    uint32_t grant;                             /* this handle's grant id */
    uint32_t parent_grant;                      /* the grant it was derived from; 0 = none */
};

struct ipc_call_args {
    int32_t ep_handle;                          /* a SEND endpoint handle (the broker channel) */
    uint32_t req;                               /* request bytes */
    uint32_t req_len;
    uint32_t reply;                             /* buffer the reply is copied into */
    uint32_t reply_cap;
};

struct ipc_recv_args {
    int32_t ep_handle;                          /* a RECV endpoint handle (owner side) */
    uint32_t buf;                               /* request copied here */
    uint32_t cap;
    uint32_t sender;                            /* set to the caller's pid (may be 0) */
};

struct ipc_reply_args {
    int32_t ep_handle;                          /* the RECV endpoint just received on */
    uint32_t data;                              /* reply bytes */
    uint32_t len;
};

/* ---- framebuffer ops (SYS_FB_OP; GRANT_FRAMEBUFFER) ---- */
enum { /* fb_op */
    FB_OP_INFO = 0,                             /* -> out[0..3] = width,height,bpp,pitch */
    FB_OP_CLEAR = 1,                            /* color */
    FB_OP_FILL_RECT = 2,                        /* x,y,w,h,color */
    FB_OP_PUT_PIXEL = 3,                        /* x,y,color */
    FB_OP_DRAW_LINE = 4,                        /* x,y=(x0,y0) w,h=(x1,y1) color */
    FB_OP_DRAW_TEXT = 5,                        /* x,y,color(fg),color2(bg),text */
    /*
     * Install a console font by NAME, from /System/Fonts/<text>.xfnt.
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
    FB_OP_SET_FONT = 6,                         /* text = font name, no path, no extension */
};

struct fb_op_args {
    uint32_t op;
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;
    uint32_t color;                             /* 0x00RRGGBB (fg for text) */
    uint32_t color2;                            /* bg for DRAW_TEXT */
    uint32_t text;                              /* DRAW_TEXT: NUL-terminated */
    uint32_t out;                               /* INFO: caller buffer >= 4 u32 */
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
#define GRANT_OS_BASELINE        (GRANT_CONSOLE | GRANT_MEM | GRANT_DISK | GRANT_SPAWN | GRANT_POWER | GRANT_ENDPOINT | GRANT_FRAMEBUFFER | GRANT_NET)

/*
 * Not a grant: a restriction, carried in spawn_args.caps's top bit. The child's
 * SYS_CONSOLE_WRITE goes to the kernel log, a line at a time, instead of the
 * screen - the supervisor and its services, which would otherwise print over
 * the shell. Every process the child starts inherits it, asked for or not, so
 * a background process cannot hand the screen to its own children.
 */
#define SPAWN_BACKGROUND         0x80000000u

/*
 * ---- network (SYS_NET_OP; GRANT_NET) ----
 * IPv4 addresses cross the ABI as a packed u32 in NETWORK byte order
 * (a.b.c.d -> (a<<24)|(b<<16)|(c<<8)|d) so userspace needs no array type.
 */
enum { /* net_op */
    NET_OP_STATUS = 0,                          /* -> 1 if the interface is up, 0 if not */
    NET_OP_MAC = 1,                             /* data = 6-byte buffer for the MAC */
    NET_OP_GET_IP = 2,                          /* -> out[0..3] = ip, mask, gateway, dns1 */
    NET_OP_SET_IP = 3,                          /* ip = address, len selects which field (0=ip 1=mask 2=gw 3=dns) */
    NET_OP_PING = 4,                            /* ip = destination, len = sequence -> out[0] = rtt ms */
    NET_OP_SEND = 5,                            /* data/len = a raw Ethernet frame */
    NET_OP_RECV = 6,                            /* data/len = buffer -> bytes received, 0 if none */
};

struct net_op_args {
    uint32_t op;
    uint32_t ip;                                /* packed IPv4, network order */
    uint32_t data;                              /* frame or MAC buffer */
    uint32_t len;                               /* buffer length, or a small selector/sequence */
    uint32_t out;                               /* results (>= 4 u32 for GET_IP) */
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
 * ---- memory (SYS_VM_MAP, SYS_VM_UNMAP, SYS_VM_INFO; unprivileged, bounded by a per-process quota) ----
 *
 * Ask for memory, give it back. This is the only way a ring-3 process obtains
 * storage beyond its image and its stack.
 *
 * It is UNPRIVILEGED and deliberately not behind a grant. A capability every
 * program must hold is not a capability, it is noise - and denying a process
 * the ability to allocate does not make it safe, it makes it broken. The
 * containment is a per-process QUOTA instead: a byte ceiling on how much may
 * be mapped at once, attenuated on spawn exactly as grants are, so a child can
 * never be given a larger ceiling than its parent holds.
 *
 * WHAT A MAPPING IS ADDRESSED BY. A request names an OBJECT, a 64-bit OFFSET
 * into it, and a LENGTH. Today the only object is MOBJ_ANON - anonymous
 * zero-filled memory - and the offset must be 0, so the shape looks like
 * overkill. It is not, and the reason is the one thing a 32-bit pointer can
 * never do:
 *
 *   A pointer here is 32 bits and the kernel occupies 0xC0000000 upward, so a
 *   process sees under 3 GB of flat address space. No single pointer will ever
 *   reach past that; that is the hardware, not a policy. But an OBJECT larger
 *   than 4 GB is perfectly reachable, by mapping a WINDOW of it at a time and
 *   moving the window - the object is addressed by a 64-bit offset, and only
 *   the window is addressed by a pointer. A file, a device aperture, or shared
 *   memory can all be larger than the space that views them.
 *
 * That only works if the offset is 64 bits from the first day. It is one field
 * now and an ABI break later, so it is here now, before anything is written
 * against it. It is carried as two 32-bit halves rather than a uint64_t so the
 * struct's layout is beyond question on either side of the boundary.
 *
 * WHAT COMES BACK. `addr` is where the mapping landed and `granted` is how
 * many bytes it actually covers. Both are outputs, because both are the
 * kernel's to decide: hardware maps in PAGES, and a page is 4 KB here, 16 KB
 * or 64 KB elsewhere. A caller asking in bytes and being TOLD what it got
 * never has to know the page size, which is the whole reason this is a
 * syscall and not a constant.
 *
 * `addr` is also an INPUT when MMAP_HINT is set - a suggested address the
 * kernel may decline. That is what makes a moving window cheap: unmap and
 * remap at the same address and the pointers inside stay valid. It is a hint
 * and not a demand because a process that could dictate its own layout could
 * ask to land on top of something it does not own.
 *
 * PROTECTION, AND AN HONEST LIMIT. `prot` is explicit, and MPROT_WRITE with
 * MPROT_EXEC together is REFUSED. Read-only is genuinely enforced: a page
 * mapped without MPROT_WRITE faults on a store.
 *
 *   Execute is NOT enforced, and cannot be here. A 32-bit non-PAE page table
 *   entry has no no-execute bit - there is nowhere to put "not executable" -
 *   so every readable page is also executable whatever this field says. The
 *   refusal of WRITE|EXEC is therefore a DISCIPLINE today and becomes a
 *   hardware guarantee when this kernel gains PAE or moves to 64-bit, where
 *   the bit exists. It is enforced now so that nothing is ever written that
 *   would break when it starts being real.
 * vm_map_args.prot - what the mapping may be used for.
 */
#define MPROT_NONE               0x00
#define MPROT_READ               0x01
#define MPROT_WRITE              0x02
#define MPROT_EXEC               0x04           /* accepted and recorded; see the honest limit above */

/* vm_map_args.flags */
#define MMAP_HINT                0x01           /* try `addr` first; the kernel may place elsewhere */

/* vm_map_args.object - what the mapping is a view OF. */
#define MOBJ_ANON                0              /* anonymous zero-filled memory; offset must be 0 */

/*
 * The largest single mapping a process may ask for. A bound rather than a
 * limit of the design: it keeps one bad length from walking the region list
 * off the end of the user half, and it is far above anything that has a
 * reason to exist yet. 64 MiB.
 */
#define MMAP_MAX_BYTES           0x04000000u

/* How many separate mappings one process may hold at once. */
#define MMAP_MAX_REGIONS         32

struct vm_map_args {
    uint32_t object;                            /* MOBJ_* (MOBJ_ANON today) */
    uint32_t offset_lo;                         /* 64-bit offset into the object, low half */
    uint32_t offset_hi;                         /* ... and high half; both 0 for MOBJ_ANON */
    uint32_t length;                            /* bytes wanted */
    /* in: a hint if MMAP_HINT; out: where it is */
    uint32_t addr;
    uint32_t granted;                           /* out: bytes actually mapped (length, rounded up to a page) */
    uint32_t prot;                              /* MPROT_* */
    uint32_t flags;                             /* MMAP_* */
};

/* SYS_VM_INFO: this process's memory account. */
struct vm_usage {
    uint32_t quota;                             /* the ceiling, in bytes */
    uint32_t mapped;                            /* how much of it is in use */
};

/*
 * ---- files (SYS_FILE_OP; GRANT_DISK) ----
 *
 * The filesystem as userspace sees it. CXFS itself is 64-bit throughout, but
 * X Native has no 64-bit integer type, so every offset and size crossing this
 * boundary is 32 bits and SIGNED: read/write return a byte count, seek returns
 * the new offset, and a negative return is an E_* code. That caps a file a
 * user process can address at INT32_MAX (2 GB). Raising it means either a
 * 64-bit type in the language or an offset-hi field, and the cap is documented
 * here rather than silently truncating.
 *
 * A handle from FILE_OP_OPEN is an ordinary entry in the process's handle table
 * (abi sec 6), so SYS_HANDLE_CLOSE releases it as well as FILE_OP_CLOSE does,
 * and it is dropped with the rest of the table when the process exits.
 *
 * Paths are resolved against the process's cwd unless they start with '/'.
 */
enum { /* file_op */
    FILE_OP_OPEN = 0,                           /* path, flags=FOPEN_* -> handle */
    FILE_OP_CLOSE = 1,                          /* handle */
    FILE_OP_READ = 2,                           /* handle, data, len -> bytes read (0 at EOF) */
    FILE_OP_WRITE = 3,                          /* handle, data, len -> bytes written */
    FILE_OP_SEEK = 4,                           /* handle, off, flags=FSEEK_* -> the new offset */
    FILE_OP_TELL = 5,                           /* handle -> the current offset */
    FILE_OP_TRUNC = 6,                          /* handle, off = the new size */
    FILE_OP_STAT = 7,                           /* path, data = *file_stat */
    FILE_OP_FSTAT = 8,                          /* handle, data = *file_stat */
    /*
     * path = dir, len = index, data = *file_stat -> 1 on a hit, 0 once the
     * index is past the end
     */
    FILE_OP_READDIR = 9,
    FILE_OP_MKDIR = 10,                         /* path */
    FILE_OP_UNLINK = 11,                        /* path (a directory must be empty) */
    FILE_OP_RENAME = 12,                        /* path = existing, path2 = the new name */
    FILE_OP_CHDIR = 13,                         /* path */
    FILE_OP_GETCWD = 14,                        /* data = buffer, len = capacity -> length written */
    /*
     * Commit everything written so far to stable media. No arguments.
     *
     * Separate from WRITE on purpose: a sync per write costs a device cache
     * flush on every byte, so it is the caller's decision - as fsync,
     * FlushFileBuffers and VNOP_FSYNC all are. Anything that must survive a
     * power cut writes, syncs, and only then treats the result as committed.
     */
    FILE_OP_SYNC = 15,
    /*
     * Advisory byte-range locks: handle, off = start, len = length (0 runs to
     * the end of the file however it grows, so off 0 len 0 is the whole file),
     * flags = FLOCK_SHARED or FLOCK_EXCLUSIVE, optionally | FLOCK_WAIT.
     *
     * A lock belongs to the OPEN FILE (the handle), not to the process and not
     * to the file on disk - SerenityOS's model. Shared ranges overlap freely;
     * an exclusive range overlaps nothing another open file holds, a second
     * open in the same process included. A handle's own locks never conflict,
     * so locking the same range again converts it. Closing the handle releases
     * them all, however it goes (FILE_OP_CLOSE, SYS_HANDLE_CLOSE, exit).
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
    FILE_OP_LOCK = 16,
    /*
     * handle, off, len: drop this handle's locks lying wholly inside the range
     * (off 0 len 0: all of them). Ranges are not split - unlock what was
     * locked. E_OK even when nothing was held.
     */
    FILE_OP_UNLOCK = 17,
};

/* FILE_OP_LOCK modes. Shared locks coexist; an exclusive lock excludes all. */
#define FLOCK_SHARED             1
#define FLOCK_EXCLUSIVE          2
#define FLOCK_WAIT               0x100          /* sleep until the lock can be had */

/* FILE_OP_OPEN flags. One of FOPEN_READ / FOPEN_WRITE is required. */
#define FOPEN_READ               0x01
#define FOPEN_WRITE              0x02
#define FOPEN_CREATE             0x04           /* create the file when it does not exist */
#define FOPEN_TRUNC              0x08           /* cut an existing file to zero length on open */
#define FOPEN_APPEND             0x10           /* every write goes to the current end of file */

/* FILE_OP_SEEK origin */
#define FSEEK_SET                0
#define FSEEK_CUR                1
#define FSEEK_END                2

/* entry kinds reported by file_stat.type */
#define FTYPE_FILE               1
#define FTYPE_DIR                2
#define FILE_NAME_MAX            64             /* must equal CXFS_NAME_LEN */
#define FILE_PATH_MAX            256            /* longest path a syscall will copy in */

struct file_op_args {
    uint32_t op;
    int32_t handle;
    uint32_t path;
    uint32_t path2;                             /* RENAME: the new name */
    uint32_t data;                              /* READ/WRITE buffer, or *file_stat */
    uint32_t len;                               /* buffer length, or READDIR index */
    int32_t off;                                /* SEEK offset (signed), TRUNC size */
    uint32_t flags;                             /* FOPEN_* or FSEEK_* */
};

/*
 * Timestamps are unsigned epoch seconds truncated to 32 bits, which runs out in
 * 2106; CXFS keeps the full 64 bits on disk.
 */
struct file_stat {
    uint32_t id;
    /*
     * FTYPE_* ("type" is a reserved word in X, so the ABI side cannot call it
     * that)
     */
    uint32_t kind;
    uint32_t size;
    uint32_t permissions;
    uint32_t owner_uid;
    uint32_t created;
    uint32_t modified;
    uint32_t accessed;
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

CXK_ABI_STATIC_ASSERT(sizeof(struct args_info) == 12, "ABI layout: args_info");
CXK_ABI_STATIC_ASSERT(sizeof(struct spawn_args) == 28, "ABI layout: spawn_args");
CXK_ABI_STATIC_ASSERT(sizeof(struct clock_info) == 8, "ABI layout: clock_info");
CXK_ABI_STATIC_ASSERT(sizeof(struct object_info_args) == 16, "ABI layout: object_info_args");
CXK_ABI_STATIC_ASSERT(sizeof(struct object_basic) == 20, "ABI layout: object_basic");
CXK_ABI_STATIC_ASSERT(sizeof(struct ipc_call_args) == 20, "ABI layout: ipc_call_args");
CXK_ABI_STATIC_ASSERT(sizeof(struct ipc_recv_args) == 16, "ABI layout: ipc_recv_args");
CXK_ABI_STATIC_ASSERT(sizeof(struct ipc_reply_args) == 12, "ABI layout: ipc_reply_args");
CXK_ABI_STATIC_ASSERT(sizeof(struct fb_op_args) == 36, "ABI layout: fb_op_args");
CXK_ABI_STATIC_ASSERT(sizeof(struct net_op_args) == 20, "ABI layout: net_op_args");
CXK_ABI_STATIC_ASSERT(sizeof(struct vm_map_args) == 32, "ABI layout: vm_map_args");
CXK_ABI_STATIC_ASSERT(sizeof(struct vm_usage) == 8, "ABI layout: vm_usage");
CXK_ABI_STATIC_ASSERT(sizeof(struct file_op_args) == 32, "ABI layout: file_op_args");
CXK_ABI_STATIC_ASSERT(sizeof(struct file_stat) == 96, "ABI layout: file_stat");
CXK_ABI_STATIC_ASSERT(sizeof(struct mouse_state) == 16, "ABI layout: mouse_state");
CXK_ABI_STATIC_ASSERT(sizeof(struct log_entry) == 80, "ABI layout: log_entry");

#endif
