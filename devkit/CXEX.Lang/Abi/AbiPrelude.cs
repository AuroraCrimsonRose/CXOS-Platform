// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
namespace CXEX.Lang.Abi;

/// <summary>
/// The X prelude (abi.x): the CXK ABI (abi/cxk_abi.h) as X sees it. Prepended to
/// every compilation so X programs stay in lockstep with the kernel.
///
/// <para><b>Constants and structures are generated</b> from the interface this
/// assembly was built with (<see cref="AbiModel"/>, DevKit engineering review §7).
/// They used to be a hand-kept copy of a header in another repository. That copy
/// drifted at least once: the kernel gained SYS_MOUSE_READ and struct mouse_state,
/// this file did not, and the userland stopped compiling. It also left whole
/// families out, so <c>os/std/net.xfxn</c> kept a third copy of NET_OP_*. Since the
/// repositories merged (D6) the header is in reach, and nothing about the ABI is
/// copied by hand any more.</para>
///
/// <para><b>Generated</b> since roadmap step 6a1, from <c>abi/kernel.xfil</c>: the
/// constants, the structures, and one typed __syscall wrapper per syscall that
/// declares one (<see cref="XfilEmit"/>). <b>Written by hand:</b> <see cref="Notes"/>
/// and <see cref="Helpers"/> (everything built on the wrappers). They name the
/// generated declarations and declare none of their own. <c>cxk check-abi</c> compares the header on disk with the
/// one built in, field types included (<see cref="AbiSync"/>).</para>
///
/// <para>Every function in the prelude is compiled into every program, so their
/// order is part of each program's bytes.</para>
/// </summary>
public static class AbiPrelude
{
    public const string FileName = "abi.x";

    /// <summary>The whole prelude: the generated declarations, the hand-written notes, the generated wrappers, the hand-written helpers.</summary>
    public static string Generate() => Banner + AbiModel.Embedded.EmitX() + "\n" + Notes + XfilEmit.XWrappers(AbiModel.EmbeddedFile) + Helpers;

    private const string Banner = """
// abi.x - the CXK ABI (abi/kernel.xfil) for X.
// Constants, structures and the syscall wrappers are GENERATED from the
// interface cxk was built with: change it, rebuild the DevKit, and they follow.
// The notes and the helpers built on the wrappers are written by hand.

""";

    /// <summary>Hand-written: how to use the calls. No code.</summary>
    private const string Notes = """
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

// ---- memory (SYS_VM_MAP, SYS_VM_UNMAP, SYS_VM_INFO; unprivileged, bounded by a per-process quota) ----
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

""";

    /// <summary>Hand-written: the calls built on the wrappers.</summary>
    private const string Helpers = """
// ---- helpers ----
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

// Map `len` bytes of anonymous read/write memory. Returns the address, or 0.
// The mapping is zero-filled - a frame off the free list may hold another
// process's memory, so the kernel clears it before you ever see it.
fn mem_map(len: u32) -> u32 {
    let a: vm_map_args;
    a.object = MOBJ_ANON;
    a.offset_lo = 0; a.offset_hi = 0;
    a.length = len; a.addr = 0; a.granted = 0;
    a.prot = MPROT_READ | MPROT_WRITE; a.flags = 0;
    if (vm_map(&a) != E_OK) { return 0; }
    return a.addr;
}

// Release a whole mapping, by the address mem_map returned. Whole or not at
// all: a partial release would have to split the region in two, which could
// fail for want of a slot while trying to give memory BACK.
fn mem_unmap(addr: u32, len: u32) -> i32 { return vm_unmap(addr, len); }

// How many bytes this process may hold at once, and how many it holds now.
fn mem_quota() -> u32 {
    let u: vm_usage;
    if (vm_info(&u) != E_OK) { return 0; }
    return u.quota;
}
fn mem_used() -> u32 {
    let u: vm_usage;
    if (vm_info(&u) != E_OK) { return 0; }
    return u.mapped;
}

// milliseconds since boot, for code that only wants to measure an interval
fn ticks_ms() -> u32 {
    let c: clock_info;
    if (clock_read(&c) != E_OK) { return 0; }
    return c.ticks;
}
""";
}
