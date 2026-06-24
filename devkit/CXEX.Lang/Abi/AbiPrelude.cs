namespace CXEX.Lang.Abi;

/// <summary>
/// Generates the X prelude (abi.x) from the frozen CXK ABI (abi/cxk_abi.h): the
/// SYS_* numbers, E_* error codes, the shared arg structs, and typed __syscall
/// wrappers. Compiling against this keeps X programs in lockstep with the kernel -
/// one source of truth across kernel, C, .NET, and X. v0.1 emits the frozen v1
/// values directly; a later version can parse cxk_abi.h to stay automatic.
/// </summary>
public static class AbiPrelude
{
    public const string FileName = "abi.x";

    public static string Generate() => """
// abi.x - GENERATED from cxk_abi.h (CXK ABI v1). Do not edit by hand.
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
const SYS_SPAWN:         u32 = 0x70;

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
struct spawn_args     { image: *u8, image_len: u32, name: *u8, broker_endpoint: i32 }

// ---- typed syscall wrappers ----
fn exit(code: i32) -> void { __syscall(SYS_EXIT, code as u32, 0, 0, 0, 0); }
fn yield_() -> void        { __syscall(SYS_YIELD, 0, 0, 0, 0, 0); }
fn getpid() -> i32         { return __syscall(SYS_GETPID, 0, 0, 0, 0, 0); }
fn getuid() -> i32         { return __syscall(SYS_GETUID, 0, 0, 0, 0, 0); }

fn console_write(buf: *u8, len: u32) -> i32 { return __syscall(SYS_CONSOLE_WRITE, buf as u32, len, 0, 0, 0); }
fn ep_create() -> i32                        { return __syscall(SYS_EP_CREATE, 0, 0, 0, 0, 0); }
fn handle_close(h: i32) -> i32               { return __syscall(SYS_HANDLE_CLOSE, h as u32, 0, 0, 0, 0); }

fn ipc_call(a: *ipc_call_args) -> i32   { return __syscall(SYS_IPC_CALL, a as u32, 0, 0, 0, 0); }
fn ipc_recv(a: *ipc_recv_args) -> i32   { return __syscall(SYS_IPC_RECV, a as u32, 0, 0, 0, 0); }
fn ipc_reply(a: *ipc_reply_args) -> i32 { return __syscall(SYS_IPC_REPLY, a as u32, 0, 0, 0, 0); }
fn spawn(a: *spawn_args) -> i32         { return __syscall(SYS_SPAWN, a as u32, 0, 0, 0, 0); }
""";
}