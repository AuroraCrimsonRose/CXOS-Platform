// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using CXEX.Lang.IR;

namespace CXEX.Lang.Sema;

/* The names X treats as operations rather than functions. Like __syscall they
 * are calls by name: no keyword, no node of their own, so the parser and the
 * tree know nothing of them and each stage asks here.
 *
 * The atomics (roadmap 1.5b1) work on a 4- or 8-byte integer through a
 * pointer, sequentially consistent, and return the value that was there
 * before (a store returns nothing):
 *
 *   __atomic_load(p)            *p
 *   __atomic_store(p, v)        *p = v
 *   __atomic_xchg(p, v)         *p = v
 *   __atomic_add(p, v)          *p = *p + v, wrapping
 *   __atomic_cas(p, e, n)       *p = n if *p == e; it worked if the result is e
 */
public static class Intrinsics
{
    public const string Syscall = "__syscall";

    /* The operation an atomic's name stands for, or null. */
    public static IrAtomicOp? Atomic(string name) => name switch
    {
        "__atomic_load" => IrAtomicOp.Load,
        "__atomic_store" => IrAtomicOp.Store,
        "__atomic_xchg" => IrAtomicOp.Xchg,
        "__atomic_add" => IrAtomicOp.Add,
        "__atomic_cas" => IrAtomicOp.Cas,
        _ => null,
    };

    /* Arguments, the pointer among them. */
    public static int Arity(IrAtomicOp op) => op switch
    {
        IrAtomicOp.Load => 1,
        IrAtomicOp.Cas => 3,
        _ => 2,
    };

    public static bool IsIntrinsic(string name) => name == Syscall || Atomic(name) != null;
}
