namespace CXEX.Lang.Abi;

/// <summary>
/// The X prelude (abi.x) mirroring the CXK ABI (abi/cxk_abi.h): the SYS_* numbers,
/// E_* error codes, the shared arg structs, and typed __syscall wrappers. Prepended
/// to every compilation so X programs stay in lockstep with the kernel.
///
/// <para><b>This is hand-maintained, not generated.</b> It is a copy of a header that
/// lives in a different repository, so it drifts silently unless someone updates both.
/// It has already drifted once: the kernel gained SYS_MOUSE_READ and struct mouse_state,
/// this file did not, and gui.xfxn referenced both - so the compiler could not build the
/// userland. <b>If you change cxk_abi.h, change this in the same pass.</b></para>
///
/// <para>Run <c>cxk check-abi</c> to verify the two agree; see
/// <see cref="AbiSync"/> for what it compares and the family rule it applies.
/// Generating this file from the header is the eventual fix, and needs the two
/// repositories visible to each other at build time.</para>
/// </summary>
public static class AbiPrelude
{
    public const string FileName = "abi.x";

    public static string Generate() => """
// abi.x - mirrors cxk_abi.h (CXK ABI v2). HAND-MAINTAINED: nothing generates this.
// If you add a syscall or ABI struct to cxk_abi.h, add it here too, then run
// `cxk check-abi` to confirm the two still agree.
// The only effect in X is __syscall; these wrappers name the kernel's calls.

// ---- syscall numbers ----
const SYS_EXIT:          u32 = 0x00;
const SYS_YIELD:         u32 = 0x01;
const SYS_GETPID:        u32 = 0x02;
const SYS_GETUID:        u32 = 0x03;
const SYS_IPC_CALL:      u32 = 0x10;
const SYS_IPC_RECV:      u32 = 0x11;
const SYS_IPC_REPLY:     u32 = 0x12;
const SYS_EP_CREATE:     u32 = 0x13;
const SYS_HANDLE_CLOSE:  u32 = 0x16;
const SYS_CONSOLE_WRITE: u32 = 0x30;
const SYS_INPUT_READ:    u32 = 0x20;
const SYS_MOUSE_READ:    u32 = 0x21;
const SYS_FB_OP:         u32 = 0x40;
const SYS_NET_OP:        u32 = 0x50;
const SYS_FILE_OP:       u32 = 0x60;
const SYS_SPAWN:         u32 = 0x70;
const SYS_POWER:         u32 = 0x71;
const POWER_REBOOT:      u32 = 0;
const POWER_SHUTDOWN:    u32 = 1;
const POWER_SLEEP:       u32 = 2;

// ---- error codes (returned in the syscall result, negative) ----
const E_OK:    i32 = 0;
const E_PERM:  i32 = -1;
const E_INVAL: i32 = -2;
const E_FAULT: i32 = -3;
const E_NOENT: i32 = -4;
const E_NOMEM: i32 = -5;
const E_BADF:  i32 = -6;
const E_AGAIN: i32 = -7;
const E_RANGE: i32 = -8;
const E_NOSYS: i32 = -9;
const E_IO:    i32 = -10;
const E_EXIST: i32 = -11;
const E_ISDIR: i32 = -12;
const E_NOTDIR: i32 = -13;

// ---- shared call structures (layout matches the kernel) ----
struct ipc_call_args  { ep_handle: i32, req: *u8, req_len: u32, reply: *u8, reply_cap: u32 }
struct ipc_recv_args  { ep_handle: i32, buf: *u8, cap: u32, sender: *i32 }
struct ipc_reply_args { ep_handle: i32, data: *u8, len: u32 }
struct spawn_args     { image: *u8, image_len: u32, name: *u8, broker_endpoint: i32, caps: u32 }
const FB_OP_INFO: u32 = 0;
const FB_OP_CLEAR: u32 = 1;
const FB_OP_FILL_RECT: u32 = 2;
const FB_OP_PUT_PIXEL: u32 = 3;
const FB_OP_DRAW_LINE: u32 = 4;
const FB_OP_DRAW_TEXT: u32 = 5;
struct fb_op_args { op: u32, x: u32, y: u32, w: u32, h: u32, color: u32, color2: u32, text: *u8, out: *u32 }

// ---- network (SYS_NET_OP; CAP_NET) ----
struct net_op_args { op: u32, ip: u32, data: *u8, len: u32, out: *u32 }

// ---- files (SYS_FILE_OP; CAP_DISK) ----
// CXFS is 64-bit throughout; X has no 64-bit integer, so every offset and size
// here is 32-bit signed. read/write return a byte count, seek returns the new
// offset, negative is an E_* code. A user process can therefore address a file
// up to 2 GB. An open handle lives in the process handle table, so
// handle_close() releases one just as file_close() does.
const FILE_OP_OPEN:    u32 = 0;
const FILE_OP_CLOSE:   u32 = 1;
const FILE_OP_READ:    u32 = 2;
const FILE_OP_WRITE:   u32 = 3;
const FILE_OP_SEEK:    u32 = 4;
const FILE_OP_TELL:    u32 = 5;
const FILE_OP_TRUNC:   u32 = 6;
const FILE_OP_STAT:    u32 = 7;
const FILE_OP_FSTAT:   u32 = 8;
const FILE_OP_READDIR: u32 = 9;
const FILE_OP_MKDIR:   u32 = 10;
const FILE_OP_UNLINK:  u32 = 11;
const FILE_OP_RENAME:  u32 = 12;
const FILE_OP_CHDIR:   u32 = 13;
const FILE_OP_GETCWD:  u32 = 14;

const FOPEN_READ:   u32 = 0x01;
const FOPEN_WRITE:  u32 = 0x02;
const FOPEN_CREATE: u32 = 0x04;
const FOPEN_TRUNC:  u32 = 0x08;
const FOPEN_APPEND: u32 = 0x10;

const FSEEK_SET: u32 = 0;
const FSEEK_CUR: u32 = 1;
const FSEEK_END: u32 = 2;

const FTYPE_FILE: u32 = 1;
const FTYPE_DIR:  u32 = 2;

const FILE_NAME_MAX: u32 = 64;
const FILE_PATH_MAX: u32 = 256;

struct file_op_args { op: u32, handle: i32, path: *u8, path2: *u8, data: *u8, len: u32, off: i32, flags: u32 }
struct file_stat { id: u32, kind: u32, size: u32, permissions: u32, owner_uid: u32, created: u32, modified: u32, accessed: u32, name: [64]u8 }

// ---- pointer input (SYS_MOUSE_READ) ----
// Absolute position, already clamped to the screen by the kernel driver, so
// userspace never sees relative deltas. `seq` increments on every state change:
// compare it with the previous read to detect movement without diffing x/y.
// Unprivileged, like keyboard input.
struct mouse_state { x: i32, y: i32, buttons: u32, seq: u32 }

// ---- typed syscall wrappers ----
fn exit(code: i32) -> void { __syscall(SYS_EXIT, code as u32, 0, 0, 0, 0); }
fn yield_() -> void        { __syscall(SYS_YIELD, 0, 0, 0, 0, 0); }
fn getpid() -> i32         { return __syscall(SYS_GETPID, 0, 0, 0, 0, 0); }
fn getuid() -> i32         { return __syscall(SYS_GETUID, 0, 0, 0, 0, 0); }

fn console_write(buf: *u8, len: u32) -> i32 { return __syscall(SYS_CONSOLE_WRITE, buf as u32, len, 0, 0, 0); }
fn input_read() -> i32                       { return __syscall(SYS_INPUT_READ, 0, 0, 0, 0, 0); }        // block for a key
fn input_poll() -> i32                       { return __syscall(SYS_INPUT_READ, 1, 0, 0, 0, 0); }        // 0 if none
fn mouse_read(m: *mouse_state) -> i32        { return __syscall(SYS_MOUSE_READ, m as u32, 0, 0, 0, 0); } // 1 if a mouse is present
fn fb_op(a: *fb_op_args) -> i32         { return __syscall(SYS_FB_OP, a as u32, 0, 0, 0, 0); }
fn net_op(a: *net_op_args) -> i32       { return __syscall(SYS_NET_OP, a as u32, 0, 0, 0, 0); }
fn file_op(a: *file_op_args) -> i32     { return __syscall(SYS_FILE_OP, a as u32, 0, 0, 0, 0); }
fn reboot() -> void                          { __syscall(SYS_POWER, POWER_REBOOT, 0, 0, 0, 0); }              // does not return
fn shutdown() -> void                        { __syscall(SYS_POWER, POWER_SHUTDOWN, 0, 0, 0, 0); }            // does not return
fn sleep(ms: u32) -> i32                     { return __syscall(SYS_POWER, POWER_SLEEP, ms, 0, 0, 0); }
fn ep_create() -> i32                        { return __syscall(SYS_EP_CREATE, 0, 0, 0, 0, 0); }
fn handle_close(h: i32) -> i32               { return __syscall(SYS_HANDLE_CLOSE, h as u32, 0, 0, 0, 0); }

fn ipc_call(a: *ipc_call_args) -> i32   { return __syscall(SYS_IPC_CALL, a as u32, 0, 0, 0, 0); }
fn ipc_recv(a: *ipc_recv_args) -> i32   { return __syscall(SYS_IPC_RECV, a as u32, 0, 0, 0, 0); }
fn ipc_reply(a: *ipc_reply_args) -> i32 { return __syscall(SYS_IPC_REPLY, a as u32, 0, 0, 0, 0); }
fn spawn(a: *spawn_args) -> i32         { return __syscall(SYS_SPAWN, a as u32, 0, 0, 0, 0); }
""";
}