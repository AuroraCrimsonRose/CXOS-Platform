// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
﻿namespace CXEX.Lang.Abi;

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
const SYS_CLOCK:         u32 = 0x04;
const SYS_SLEEP:         u32 = 0x05;
const SYS_ARGS:          u32 = 0x06;
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
const SYS_EXEC_PATH:     u32 = 0x72;
const SYS_SPAWN:         u32 = 0x70;
const SYS_POWER:         u32 = 0x71;
const SYS_MEM_OP:        u32 = 0x80;
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
// ---- clock and sleep (SYS_CLOCK, SYS_SLEEP; unprivileged) ----
// ticks is milliseconds since boot and monotonic - use it to measure an
// interval. epoch is seconds since 1970 from the RTC, or 0 when the RTC could
// not be read - use it to stamp something, and check it before trusting it.
// sleep_ms blocks THIS process only; it is not the ACPI machine sleep.
struct clock_info     { ticks: u32, epoch: u32 }
const SLEEP_MAX_MS: u32 = 3600000;

struct ipc_call_args  { ep_handle: i32, req: *u8, req_len: u32, reply: *u8, reply_cap: u32 }
struct ipc_recv_args  { ep_handle: i32, buf: *u8, cap: u32, sender: *i32 }
struct ipc_reply_args { ep_handle: i32, data: *u8, len: u32 }
struct spawn_args     { image: *u8, image_len: u32, name: *u8, broker_endpoint: i32, caps: u32, args: *u8, args_len: u32 }

// ---- program arguments ----
// The launcher hands the kernel a flat blob - the argument strings, one after
// another, each NUL-terminated - and the kernel lays it out as a single page
// in the child: argc, then one offset per argument, then the strings.
//
// WHERE that page is, this code does not know and must not. The address is an
// architecture fact - it is one number on 32-bit x86 and a different one
// anywhere else - so a program that compiled it in would need porting
// alongside the kernel. SYS_ARGS asks, which keeps that fact on the kernel's
// side of the line and this file the same on every machine.
//
// The page is always mapped, so a program with no arguments reads 0 rather
// than faulting.
struct args_info { count: u32, base: u32 }

fn args_read(a: *args_info) -> i32 { return __syscall(SYS_ARGS, a as u32, 0, 0, 0, 0); }

// how many arguments this program was started with
fn arg_count() -> u32 {
    let ai: args_info;
    if (args_read(&ai) != E_OK) { return 0; }
    return ai.count;
}

// argument i, or an empty string when i is out of range - so a caller may read
// an argument it was not given without checking arg_count() first
fn arg_at(i: u32) -> *u8 {
    let ai: args_info;
    if (args_read(&ai) != E_OK) { return ""; }
    if (i >= ai.count)          { return ""; }
    let head: *u32 = ai.base as *u32;
    return (ai.base + head[1 + i]) as *u8;
}
const FB_OP_INFO: u32 = 0;
const FB_OP_CLEAR: u32 = 1;
const FB_OP_FILL_RECT: u32 = 2;
const FB_OP_PUT_PIXEL: u32 = 3;
const FB_OP_DRAW_LINE: u32 = 4;
const FB_OP_DRAW_TEXT: u32 = 5;
const FB_OP_SET_FONT: u32 = 6;
struct fb_op_args { op: u32, x: u32, y: u32, w: u32, h: u32, color: u32, color2: u32, text: *u8, out: *u32 }

// ---- network (SYS_NET_OP; GRANT_NET) ----
struct net_op_args { op: u32, ip: u32, data: *u8, len: u32, out: *u32 }

// ---- capabilities ----
// A process's authority, as a bitmask: what it is ALLOWED TO DO. GRANT_ because
// each bit is a grant, handed over when the process was started and never wider
// than what the thing that started it held. Mirrored here so a userspace
// spawner can name what it is handing a child: spawn and exec_path attenuate the requested
// set against the caller's own, so you pass a subset of your authority and can
// never amplify. Previously userspace had to redeclare these locally, which
// cxk check-abi reports as a third copy of the ABI with no link to the header.
const GRANT_CONSOLE:     u32 = 0x0001;
const GRANT_MEM:         u32 = 0x0002;
const GRANT_DISK:        u32 = 0x0004;
const GRANT_NET:         u32 = 0x0008;
const GRANT_SPAWN:       u32 = 0x0010;
const GRANT_POWER:       u32 = 0x0020;
const GRANT_ENDPOINT:    u32 = 0x0040;
const GRANT_IOPORT:      u32 = 0x0080;
const GRANT_FRAMEBUFFER: u32 = 0x0100;

// ---- files (SYS_FILE_OP; GRANT_DISK) ----
// CXFS is 64-bit throughout; every offset and size here is 32-bit signed, which
// caps what a user process can address at 2 GB. X DOES now have i64 and u64 -
// that was the blocker and it is gone - so widening this is a deliberate ABI
// change rather than a language limit, and has not been made yet. read/write return a byte count, seek returns the new
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
const FILE_OP_SYNC:    u32 = 15;

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


// ---- memory (SYS_MEM_OP; unprivileged, bounded by a per-process quota) ----
// The only way to get memory beyond your image and stack. Not behind a grant:
// a capability every program must hold is noise, and a process that cannot
// allocate is broken rather than contained. A per-process byte quota bounds it
// instead, attenuated on spawn the way grants are.
//
// A request names an object, a 64-bit offset into it and a length. Only
// MOBJ_ANON exists today and its offset must be 0 - but the offset is 64-bit
// from the start because that is what lets an object LARGER than the address
// space be read through a moving window later. A pointer here is 32 bits and
// always will be; an object does not have to be.
//
// `addr` and `granted` come back because both are the kernel's to decide:
// memory is mapped in pages, and the page size is an architecture fact this
// side of the boundary should never have to know. `granted` is `length`
// rounded up to whatever a page is.
//
// prot is explicit and MPROT_WRITE|MPROT_EXEC together is refused. Read-only
// is genuinely enforced. Execute is NOT - a 32-bit non-PAE page table entry
// has no no-execute bit, so every mapped page is executable whatever is asked.
// The refusal is discipline now so that nothing breaks when PAE or 64-bit
// makes it real.
const MEM_OP_MAP:   u32 = 0;
const MEM_OP_UNMAP: u32 = 1;
const MEM_OP_INFO:  u32 = 2;

const MPROT_NONE:  u32 = 0x00;
const MPROT_READ:  u32 = 0x01;
const MPROT_WRITE: u32 = 0x02;
const MPROT_EXEC:  u32 = 0x04;

const MMAP_HINT: u32 = 0x01;
const MOBJ_ANON: u32 = 0;

const MMAP_MAX_BYTES:   u32 = 67108864;
const MMAP_MAX_REGIONS: u32 = 32;

struct mem_op_args { op: u32, object: u32, offset_lo: u32, offset_hi: u32, length: u32, addr: u32, granted: u32, prot: u32, flags: u32, quota: u32, mapped: u32 }

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
fn mem_op(a: *mem_op_args) -> i32       { return __syscall(SYS_MEM_OP, a as u32, 0, 0, 0, 0); }

// Map `len` bytes of anonymous read/write memory. Returns the address, or 0.
// The mapping is zero-filled - a frame off the free list may hold another
// process's memory, so the kernel clears it before you ever see it.
fn mem_map(len: u32) -> u32 {
    let a: mem_op_args;
    a.op = MEM_OP_MAP; a.object = MOBJ_ANON;
    a.offset_lo = 0; a.offset_hi = 0;
    a.length = len; a.addr = 0; a.granted = 0;
    a.prot = MPROT_READ | MPROT_WRITE; a.flags = 0;
    if (mem_op(&a) != E_OK) { return 0; }
    return a.addr;
}

// Release a whole mapping, by the address mem_map returned. Whole or not at
// all: a partial release would have to split the region in two, which could
// fail for want of a slot while trying to give memory BACK.
fn mem_unmap(addr: u32, len: u32) -> i32 {
    let a: mem_op_args;
    a.op = MEM_OP_UNMAP; a.addr = addr; a.length = len;
    return mem_op(&a);
}

// How many bytes this process may hold at once, and how many it holds now.
fn mem_quota() -> u32 {
    let a: mem_op_args;
    a.op = MEM_OP_INFO;
    if (mem_op(&a) != E_OK) { return 0; }
    return a.quota;
}
fn mem_used() -> u32 {
    let a: mem_op_args;
    a.op = MEM_OP_INFO;
    if (mem_op(&a) != E_OK) { return 0; }
    return a.mapped;
}
fn reboot() -> void                          { __syscall(SYS_POWER, POWER_REBOOT, 0, 0, 0, 0); }              // does not return
fn shutdown() -> void                        { __syscall(SYS_POWER, POWER_SHUTDOWN, 0, 0, 0, 0); }            // does not return
fn sleep(ms: u32) -> i32                     { return __syscall(SYS_POWER, POWER_SLEEP, ms, 0, 0, 0); }
fn ep_create() -> i32                        { return __syscall(SYS_EP_CREATE, 0, 0, 0, 0, 0); }
fn handle_close(h: i32) -> i32               { return __syscall(SYS_HANDLE_CLOSE, h as u32, 0, 0, 0, 0); }

fn ipc_call(a: *ipc_call_args) -> i32   { return __syscall(SYS_IPC_CALL, a as u32, 0, 0, 0, 0); }
fn ipc_recv(a: *ipc_recv_args) -> i32   { return __syscall(SYS_IPC_RECV, a as u32, 0, 0, 0, 0); }
fn ipc_reply(a: *ipc_reply_args) -> i32 { return __syscall(SYS_IPC_REPLY, a as u32, 0, 0, 0, 0); }
fn clock_read(c: *clock_info) -> i32    { return __syscall(SYS_CLOCK, c as u32, 0, 0, 0, 0); }
fn sleep_ms(ms: u32) -> i32             { return __syscall(SYS_SLEEP, ms, 0, 0, 0, 0); }

// milliseconds since boot, for code that only wants to measure an interval
fn ticks_ms() -> u32 {
    let c: clock_info;
    if (clock_read(&c) != E_OK) { return 0; }
    return c.ticks;
}

fn spawn(a: *spawn_args) -> i32         { return __syscall(SYS_SPAWN, a as u32, 0, 0, 0, 0); }
// Run the CXEX at `path`. The kernel reads and verifies the file itself, so the
// image/image_len fields of `a` are ignored. A negative broker_endpoint means
// the child gets no broker handle.
fn exec_path(path: *u8, a: *spawn_args) -> i32 { return __syscall(SYS_EXEC_PATH, path as u32, a as u32, 0, 0, 0); }
""";
}