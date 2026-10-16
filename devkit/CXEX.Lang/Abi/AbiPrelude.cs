// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
﻿namespace CXEX.Lang.Abi;

/// <summary>
/// The X prelude (abi.x): the CXK ABI (abi/cxk_abi.h) as X sees it. Prepended to
/// every compilation so X programs stay in lockstep with the kernel.
///
/// <para><b>Constants and structures are generated</b> from the header this
/// assembly was built with (<see cref="AbiModel"/>, DevKit engineering review §7).
/// They used to be a hand-kept copy of a header in another repository. That copy
/// drifted at least once: the kernel gained SYS_MOUSE_READ and struct mouse_state,
/// this file did not, and the userland stopped compiling. It also left whole
/// families out, so <c>os/std/net.xfxn</c> kept a third copy of NET_OP_*. Since the
/// repositories merged (D6) the header is in reach, and nothing about the ABI is
/// copied by hand any more.</para>
///
/// <para><b>Written by hand:</b> <see cref="Wrappers"/>, the typed __syscall
/// wrappers and the notes on using them. They name the generated declarations
/// and declare none of their own. <c>cxk check-abi</c> compares the header on
/// disk with the one built in, field types included (<see cref="AbiSync"/>).</para>
/// </summary>
public static class AbiPrelude
{
    public const string FileName = "abi.x";

    /// <summary>The whole prelude: the generated declarations, then the hand-written wrappers.</summary>
    public static string Generate() => Banner + AbiModel.Embedded.EmitX() + "\n" + Wrappers;

    private const string Banner = """
// abi.x - the CXK ABI (abi/cxk_abi.h) for X.
// Constants and structures are GENERATED from the header cxk was built
// against: change the header, rebuild the DevKit, and they follow. Below them,
// written by hand, are the __syscall wrappers that name the kernel's calls.

""";

    /// <summary>Hand-written: the wrappers and their notes. No constants or structures; those are generated.</summary>
    private const string Wrappers = """
// The only effect in X is __syscall; these wrappers name the kernel's calls.

// ---- clock and sleep (SYS_CLOCK, SYS_SLEEP; unprivileged) ----
// ticks is milliseconds since boot and monotonic - use it to measure an
// interval. epoch is seconds since 1970 from the RTC, or 0 when the RTC could
// not be read - use it to stamp something, and check it before trusting it.
// sleep_ms blocks THIS process only; it is not the ACPI machine sleep.


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

// the SEND handle on this program's broker endpoint, installed by a launcher
// that gave it one, or -1. Handles are never small numbers, so ask.
fn broker_handle() -> i32 {
    let ai: args_info;
    if (args_read(&ai) != E_OK) { return 0 - 1; }
    return ai.broker;
}

// ---- network (SYS_NET_OP; GRANT_NET) ----

// ---- capabilities ----
// A process's authority, as a bitmask: what it is ALLOWED TO DO. GRANT_ because
// each bit is a grant, handed over when the process was started and never wider
// than what the thing that started it held. Mirrored here so a userspace
// spawner can name what it is handing a child: spawn and exec_path attenuate the requested
// set against the caller's own, so you pass a subset of your authority and can
// never amplify. Previously userspace had to redeclare these locally, which
// cxk check-abi reports as a third copy of the ABI with no link to the header.

// ---- files (SYS_FILE_OP; GRANT_DISK) ----
// CXFS is 64-bit throughout; every offset and size here is 32-bit signed, which
// caps what a user process can address at 2 GB. X DOES now have i64 and u64 -
// that was the blocker and it is gone - so widening this is a deliberate ABI
// change rather than a language limit, and has not been made yet. read/write return a byte count, seek returns the new
// offset, negative is an E_* code. A user process can therefore address a file
// up to 2 GB. An open handle lives in the process handle table, so
// handle_close() releases one just as file_close() does.







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





// ---- pointer input (SYS_MOUSE_READ) ----
// Absolute position, already clamped to the screen by the kernel driver, so
// userspace never sees relative deltas. `seq` increments on every state change:
// compare it with the previous read to detect movement without diffing x/y.
// Unprivileged, like keyboard input.

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
// a second handle on the same object with `rights`, a subset of h's own
fn handle_duplicate(h: i32, rights: u32) -> i32 { return __syscall(SYS_HANDLE_DUPLICATE, h as u32, rights, 0, 0, 0); }
fn object_info(a: *object_info_args) -> i32 { return __syscall(SYS_OBJECT_INFO, a as u32, 0, 0, 0, 0); }

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