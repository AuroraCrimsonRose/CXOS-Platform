# CXK Kernel Contracts — Failure Tiers & Architecture Locality
### CXK Reference — Aurora Tejeda / CATX Systems

Two rules every kernel change is held to, from the kernel engineering review
(§13, §14). The ABI structures' own rules are in `CX_ABI.md` §13. The scheduler
modes, the esp0 invariant and boot order are in `PROCESS_MODEL.md` §5.1, §8 and
§13.

---

## 1. Failure tiers

Every failure belongs to exactly one tier. The tier decides what happens, and a
failure must never be handled at a tier above its own. A bad request that panics
the kernel is a denial of service. A broken invariant that is logged and
ignored is worse: the kernel carries on running on state it can no longer
vouch for.

| Tier | Meaning | Mechanism | Examples |
|---|---|---|---|
| **Request** | One call cannot be done. Nothing else is affected. | Return a negative `E_*` (`CX_ABI.md` §4), or a driver's own negative code. The caller decides. | A bad pointer, an absent capability, a full endpoint table, an image that fails verification, a disk read error |
| **Process** | One process cannot continue. | The process ends. A ring-3 fault reaches `usermode_fault`, which logs and `thread_exit`s. Its resources are reaped as on a normal exit. | A user page fault, an invalid opcode in ring 3, a quota-exhausted spawn of the argument page |
| **Subsystem** | A device or service is unavailable, and the system runs without it. | Log `SEV_WARN` or `SEV_ERR` once, at init, naming what is lost. Callers then get request-tier errors (`DISK_ERR_NO_DEVICE`). The call is written as `(void)f()` with the reason, where the discard is the decision. | No ACPI tables (no clean shutdown), no APIC (the 8259 instead), no NIC, an AHCI port the disk registry refuses |
| **Kernel** | An internal invariant is broken. Continuing would run on state the kernel cannot vouch for. | `kpanic(why)` (`cpu/int/idt.h`), or a CPU exception in ring 0. Both draw the panic screen with a stack trace and the recent log, save `crash.log` when a volume is mounted, and halt. | A ring-0 page fault, a kernel stack overflow (the double fault task), thread 0 without an esp0 stack |

**Choosing the tier.** Ask what the failure says about the kernel's own state:

- **Nothing:** the world refused something, whether a user, a device or a disk.
  It is a request or subsystem failure, however unexpected.
- **Something is wrong with code the kernel trusts:** a structure is
  inconsistent, or a "cannot happen" happened. It is kernel-tier. Recovery
  code for an impossible state has never been run, so it cannot be trusted
  either.
- **When unsure:** allocation failure at boot, for a size that cannot fail on
  any machine that got this far, means the allocator is broken (kernel tier).
  The same allocation failing at run time, on behalf of a request, is request
  tier.

**What changed on 2026-10-16, writing this down:**
- **There was no kernel tier for software.** The panic screen was reachable
  only through a CPU exception, so a violated invariant could be ignored or
  downgraded to a WARN and nothing else. `kpanic` now exists, and the first
  invariant moved up to it is thread 0's esp0 stack (`kmain`). It was a WARN,
  even though the self-tests then enter ring 3 relying on it.
- **A process-tier failure sounded like a kernel-tier one.** `isr_handler`
  played the panic tone before handing a ring-3 fault to the process handler,
  so every crashing user program beeped like a kernel panic. The tone now
  follows the check.
- **The degrade that stays a WARN:** if `kmain` cannot get a guarded stack for
  itself, it keeps running on the boot stack. That is a subsystem-tier loss of
  overflow protection, and the system is otherwise sound.

The error-path audit (hardening plan, engineering §9) is the request and
subsystem half of this: every discarded return value was triaged, and each
deliberate discard is written as `(void)` with its reason.

---

## 2. Architecture locality

CXK targets x86-32, deliberately. The rule is not "be portable". It is that an
x86 assumption lives where its name says it does, so that a port, or just a
reader, can find all of them.

**Where architecture-specific code belongs:**

| Place | Holds |
|---|---|
| `kernel/cpu/` | GDT/TSS, IDT and exceptions, context switch, ring transitions, the local and I/O APIC |
| `kernel/memman/paging.*`, `addr_space.*`, `kstack.*` | Two-level 4 KB paging, CR3, the higher-half layout |
| `kernel/kernel.asm`, `kernel/linker.ld`, `boot/` | The entry stub, the link address, the BIOS boot chain |
| `kernel/drivers/` | PC devices: port I/O, PCI configuration mechanism #1, legacy IRQs. These are x86 *platform* drivers by definition. |
| `abi/cxk_abi.h` | The user/kernel boundary's widths, asserted (`CX_ABI.md` §13) |

**Named constants, defined once:**

| Fact | Name | Defined in |
|---|---|---|
| The page size | `PAGE_SIZE` | `memman/paging.h` (`PMM_PAGE_SIZE` in `pmm.h` is the same value for the frame allocator) |
| The higher-half base | `KERNEL_VBASE` | `memman/paging.h`. **The one C definition.** Nine `.c` files used to each `#define` their own copy; they now include it. `kernel.asm` (`KERNEL_VBASE equ`) and `linker.ld` restate it because they cannot include a C header, and those are the only two. |
| The VGA text buffer | `KERNEL_VBASE + 0xB8000` | `vga.c`, `console.c` and the panic writer in `idt.c`. All three used to spell the base as a literal `0xC0000000`. |
| Pointer and `int` width at the ABI | asserted 4 bytes | `abi/cxk_abi.h` |

**Known exceptions, recorded rather than hidden:**
- `kernel/lib/audio/speaker.c` is a PIT/port-0x61 driver living in `lib/`. It
  belongs under `drivers/`.
- `kernel/lib/math/kmath.c` uses an SSE instruction (`sqrtsd`). It is not in
  the build, and neither is `cpu/fpu.c`, which would enable SSE. FP stays off
  until context switches save FPU state (`fpu.c` explains). Wiring it in is a
  `cpu/` concern that happens to have a `lib/` caller.
- Drivers step DMA mappings by a literal `0x1000` in several places. Where that
  is the CPU page size it should be `PAGE_SIZE`. Where it is a device's own unit
  (EHCI and OHCI buffer pages are 4 KB by their specifications) it is correct as
  a literal and should say which.

**What a port would touch, in order:** `abi/cxk_abi.h`'s width asserts (they
fire first and name the assumption), then `cpu/`, then `memman/`, then the
entry stub and linker script. Nothing in `lib/` except the two exceptions above,
and no filesystem or IPC code.
