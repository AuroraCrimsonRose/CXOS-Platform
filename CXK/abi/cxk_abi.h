/* /CXK/abi/cxk_abi.h */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/*
 * CXK ABI v1 - the PUBLIC contract shared by the kernel, the .xoex executive,
 * and .xcex apps (and, later, the X toolchain). This is the single source of
 * truth for syscall numbers, error codes, and shared call structures: kernel
 * and user code compile against the SAME definitions, so they can never drift.
 *
 * Calling convention (see docs/CXK_ABI_v1):
 *   eax = syscall number   ebx,ecx,edx,esi,edi = args 1..5   -> int 0x80
 *   result in eax: >= 0 success (call-specific), < 0 a negative error code.
 *   all GP registers except eax are preserved across the call.
 *   64-bit values are passed by pointer to a struct, never split across regs.
 */

#ifndef CXK_ABI_H
#define CXK_ABI_H

#include <stdint.h>

/* ---- syscall numbers ---- */
/* lifecycle 0x00-0x0F (no capability required) */
#define SYS_EXIT          0x00   /* ebx = exit code; does not return */
#define SYS_YIELD         0x01   /* cooperatively yield the CPU */
#define SYS_GETPID        0x02   /* -> caller pid */
#define SYS_GETUID        0x03   /* -> caller owning uid (0 = SYSTEM) */
/* IPC + handles 0x10-0x1F */
#define SYS_IPC_CALL      0x10   /* ebx = *ipc_call_args -> reply length (blocks) */
#define SYS_IPC_RECV      0x11   /* ebx = *ipc_recv_args -> request length (blocks) */
#define SYS_IPC_REPLY     0x12   /* ebx = *ipc_reply_args -> 0 */
#define SYS_EP_CREATE     0x13   /* create an endpoint -> RECV handle (GRANT_ENDPOINT) */
#define SYS_HANDLE_CLOSE  0x16   /* release a handle */
/* input 0x20-0x2F (unprivileged: reading your own keystrokes) */
#define SYS_INPUT_READ    0x20   /* ebx = flags (0=block, 1=nonblocking) -> char, 0 if none */
#define SYS_MOUSE_READ    0x21   /* ebx = *mouse_state -> 1 if a mouse is present */
/* console 0x30-0x3F (privileged: GRANT_CONSOLE) */
#define SYS_CONSOLE_WRITE 0x30   /* ebx = buf, ecx = len (0 = bounded NUL-scan) */
/* network 0x50-0x5F (privileged: GRANT_NET) */
#define SYS_NET_OP        0x50   /* ebx = *net_op_args (GRANT_NET) -> op-specific */
/* framebuffer 0x40-0x4F (privileged: GRANT_FRAMEBUFFER) */
#define SYS_FB_OP         0x40   /* ebx = *fb_op_args (GRANT_FRAMEBUFFER) -> op-specific */
/* process 0x70-0x7F (privileged) */
/* files 0x60-0x6F (privileged: GRANT_DISK) */
#define SYS_FILE_OP       0x60   /* ebx = *file_op_args (GRANT_DISK) -> op-specific */

#define SYS_SPAWN         0x70   /* ebx = *spawn_args (GRANT_SPAWN) -> pid */
#define SYS_EXEC_PATH     0x72   /* ebx = path, ecx = *spawn_args (GRANT_SPAWN) -> pid
                                    (image/image_len in the struct are ignored) */
#define SYS_POWER         0x71   /* ebx = POWER_* op (GRANT_POWER); reboot does not return */
#define POWER_REBOOT      0
#define POWER_SHUTDOWN    1      /* ACPI S5 soft-off */
#define POWER_SLEEP       2      /* ecx = ms; 0 = S1/C1 until a keypress */

/* ---- error codes (negative; returned in eax) ---- */
#define E_OK       0
#define E_PERM   (-1)    /* capability denied */
#define E_INVAL  (-2)    /* bad argument */
#define E_FAULT  (-3)    /* bad/unmapped user pointer */
#define E_NOENT  (-4)    /* no such object */
#define E_NOMEM  (-5)    /* out of memory */
#define E_BADF   (-6)    /* bad handle */
#define E_AGAIN  (-7)    /* would block / no message ready */
#define E_RANGE  (-8)    /* message too large / buffer too small */
#define E_NOSYS  (-9)    /* unknown syscall number */
#define E_IO     (-10)   /* disk or filesystem I/O failure */
#define E_EXIST  (-11)   /* the name is already taken */
#define E_ISDIR  (-12)   /* expected a file, got a directory */
#define E_NOTDIR (-13)   /* expected a directory, got a file */

/* ---- program arguments ----
   A launcher hands over a flat blob: the argument strings, NUL-terminated, one
   after another. The kernel copies it into the new address space as a single
   page at USER_ARGS_BASE, laid out as

       u32  argc
       u32  off[argc]        byte offsets from the base of the page
       ...  the strings, NUL-terminated

   The page is always mapped, so a program with no arguments reads argc == 0
   rather than faulting, and it is writable, so a program may chew on its own
   argument strings in place. Offsets rather than pointers because the blob is
   built in one address space and read in another; a launcher never hands a
   child a pointer into its own memory. */
#define USER_ARGS_BASE   0xBFFFF000u  /* the page just above the user stack */
#define USER_ARGS_MAX    4096u        /* one page, header included */
#define USER_ARGS_MAXC   64u          /* most arguments one program can be given */

/* ---- shared call structures ---- */
struct spawn_args {
    const void *image;            /* the app's CXEX bytes (in the caller's space) */
    uint32_t    image_len;
    const char *name;             /* optional; not dereferenced in v1 */
    int         broker_endpoint;  /* a RECV endpoint handle the caller owns */
    uint32_t    caps;             /* requested caps for the child; kernel masks against
                                     the spawner's own caps (attenuation, never amplify).
                                     0 = capability-less app. */
    const char *args;             /* argc NUL-terminated strings back to back, or NULL */
    uint32_t    args_len;         /* total bytes of that blob, terminators included */
};

/* IPC is synchronous: ipc_call blocks the caller until the owner ipc_replies.
   Messages are bounded (<= one page) and copied through the kernel. The message
   schema is defined by the executive; the kernel just moves bytes. */
struct ipc_call_args {
    int         ep_handle;   /* a SEND endpoint handle (the broker channel) */
    const void *req;         /* request bytes */
    uint32_t    req_len;
    void       *reply;       /* buffer the reply is copied into */
    uint32_t    reply_cap;
};
struct ipc_recv_args {
    int       ep_handle;     /* a RECV endpoint handle (owner side) */
    void     *buf;           /* request copied here */
    uint32_t  cap;
    int      *sender;        /* set to the caller's pid (may be 0) */
};
struct ipc_reply_args {
    int         ep_handle;   /* the RECV endpoint just received on */
    const void *data;        /* reply bytes */
    uint32_t    len;
};


/* ---- framebuffer ops (SYS_FB_OP; GRANT_FRAMEBUFFER) ---- */
enum {
    FB_OP_INFO      = 0,   /* -> out[0..3] = width,height,bpp,pitch */
    FB_OP_CLEAR     = 1,   /* color */
    FB_OP_FILL_RECT = 2,   /* x,y,w,h,color */
    FB_OP_PUT_PIXEL = 3,   /* x,y,color */
    FB_OP_DRAW_LINE = 4,   /* x,y=(x0,y0) w,h=(x1,y1) color */
    FB_OP_DRAW_TEXT = 5,   /* x,y,color(fg),color2(bg),text */
};
struct fb_op_args {
    uint32_t    op;
    uint32_t    x, y;
    uint32_t    w, h;
    uint32_t    color;       /* 0x00RRGGBB (fg for text) */
    uint32_t    color2;      /* bg for DRAW_TEXT */
    const char *text;        /* DRAW_TEXT: NUL-terminated */
    uint32_t   *out;         /* INFO: caller buffer >= 4 u32 */
};

/* ---- capabilities (for spawn_args.caps) ----
   A process's authority, as a bitmask: what it is ALLOWED TO DO. One bit per
   class of privileged primitive, checked by the syscall dispatcher before the
   call runs. GRANT_ because each bit is a grant - handed over when the process
   was started, and never wider than what the thing that started it held.
   Values MUST match kernel/cpu/caps.h. Guarded so including both is harmless. */
#ifndef GRANT_CONSOLE
#define GRANT_CONSOLE     0x0001u
#define GRANT_MEM         0x0002u
#define GRANT_DISK        0x0004u
#define GRANT_NET         0x0008u
#define GRANT_SPAWN       0x0010u
#define GRANT_POWER       0x0020u
#define GRANT_ENDPOINT    0x0040u
#define GRANT_IOPORT      0x0080u
#define GRANT_FRAMEBUFFER 0x0100u
#endif
#ifndef GRANT_OS_BASELINE
#define GRANT_OS_BASELINE (GRANT_CONSOLE | GRANT_MEM | GRANT_DISK | GRANT_SPAWN | GRANT_POWER | GRANT_ENDPOINT | GRANT_FRAMEBUFFER | GRANT_NET)
#endif


/* ---- network (SYS_NET_OP; GRANT_NET) ----
   IPv4 addresses cross the ABI as a packed u32 in NETWORK byte order
   (a.b.c.d -> (a<<24)|(b<<16)|(c<<8)|d) so userspace needs no array type. */
enum {
    NET_OP_STATUS  = 0,   /* -> 1 if the interface is up, 0 if not */
    NET_OP_MAC     = 1,   /* data = 6-byte buffer for the MAC */
    NET_OP_GET_IP  = 2,   /* -> out[0..3] = ip, mask, gateway, dns1 */
    NET_OP_SET_IP  = 3,   /* ip = address, len selects which field (0=ip 1=mask 2=gw 3=dns) */
    NET_OP_PING    = 4,   /* ip = destination, len = sequence -> out[0] = rtt ms */
    NET_OP_SEND    = 5,   /* data/len = a raw Ethernet frame */
    NET_OP_RECV    = 6,   /* data/len = buffer -> bytes received, 0 if none */
};
struct net_op_args {
    uint32_t  op;
    uint32_t  ip;        /* packed IPv4, network order */
    void     *data;      /* frame or MAC buffer */
    uint32_t  len;       /* buffer length, or a small selector/sequence */
    uint32_t *out;       /* results (>= 4 u32 for GET_IP) */
};


/* ---- path execution (SYS_EXEC_PATH; GRANT_SPAWN) ----
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

/* ---- files (SYS_FILE_OP; GRANT_DISK) ----
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
enum {
    FILE_OP_OPEN    = 0,   /* path, flags=FOPEN_* -> handle */
    FILE_OP_CLOSE   = 1,   /* handle */
    FILE_OP_READ    = 2,   /* handle, data, len -> bytes read (0 at EOF) */
    FILE_OP_WRITE   = 3,   /* handle, data, len -> bytes written */
    FILE_OP_SEEK    = 4,   /* handle, off, flags=FSEEK_* -> the new offset */
    FILE_OP_TELL    = 5,   /* handle -> the current offset */
    FILE_OP_TRUNC   = 6,   /* handle, off = the new size */
    FILE_OP_STAT    = 7,   /* path, data = *file_stat */
    FILE_OP_FSTAT   = 8,   /* handle, data = *file_stat */
    FILE_OP_READDIR = 9,   /* path = dir, len = index, data = *file_stat
                              -> 1 on a hit, 0 once the index is past the end */
    FILE_OP_MKDIR   = 10,  /* path */
    FILE_OP_UNLINK  = 11,  /* path (a directory must be empty) */
    FILE_OP_RENAME  = 12,  /* path = existing, path2 = the new name */
    FILE_OP_CHDIR   = 13,  /* path */
    FILE_OP_GETCWD  = 14,  /* data = buffer, len = capacity -> length written */
};

/* FILE_OP_OPEN flags. One of FOPEN_READ / FOPEN_WRITE is required. */
#define FOPEN_READ    0x01
#define FOPEN_WRITE   0x02
#define FOPEN_CREATE  0x04   /* create the file when it does not exist */
#define FOPEN_TRUNC   0x08   /* cut an existing file to zero length on open */
#define FOPEN_APPEND  0x10   /* every write goes to the current end of file */

/* FILE_OP_SEEK origin */
#define FSEEK_SET 0
#define FSEEK_CUR 1
#define FSEEK_END 2

/* entry kinds reported by file_stat.type */
#define FTYPE_FILE 1
#define FTYPE_DIR  2

#define FILE_NAME_MAX 64     /* must equal CXFS_NAME_LEN */
#define FILE_PATH_MAX 256    /* longest path a syscall will copy in */

struct file_op_args {
    uint32_t    op;
    int32_t     handle;
    const char *path;
    const char *path2;       /* RENAME: the new name */
    void       *data;        /* READ/WRITE buffer, or *file_stat */
    uint32_t    len;         /* buffer length, or READDIR index */
    int32_t     off;         /* SEEK offset (signed), TRUNC size */
    uint32_t    flags;       /* FOPEN_* or FSEEK_* */
};

/* Timestamps are unsigned epoch seconds truncated to 32 bits, which runs out in
   2106; CXFS keeps the full 64 bits on disk. */
struct file_stat {
    uint32_t id;
    uint32_t kind;           /* FTYPE_* ("type" is a reserved word in X, so the
                                ABI side cannot call it that) */
    uint32_t size;
    uint32_t permissions;
    uint32_t owner_uid;
    uint32_t created;
    uint32_t modified;
    uint32_t accessed;
    char     name[FILE_NAME_MAX];
};


/* ---- pointer input (SYS_MOUSE_READ) ----
   Absolute cursor position maintained by the kernel driver, already clamped to
   the screen, so userspace never sees raw relative deltas. `seq` increments on
   every state change - compare it against the previous read to detect movement
   without diffing coordinates. Unprivileged, like keyboard input. */
struct mouse_state {
    int32_t  x, y;
    uint32_t buttons;   /* bit 0 left, bit 1 right, bit 2 middle */
    uint32_t seq;
};

#endif
