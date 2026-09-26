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