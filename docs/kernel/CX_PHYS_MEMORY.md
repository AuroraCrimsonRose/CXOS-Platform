# Reaching physical memory
### CX Design Spec: Aurora Tejeda / CATX Systems

> **Status: 1.9c done (2026-11-06): every touch of a frame by its physical
> address goes through `memman/physmap.h`, checked in test-hook builds; the
> kernel stacks' slots locked; file locks on an `ocache`**
> (`planning/CX_ROADMAP.md`, Phase 1 step 9).

On x86-64 every byte of RAM is already mapped, at `DIRECT_MAP_BASE + phys`,
so touching a frame costs nothing and needs no rules. That is what makes it
dangerous before SMP (1.9d): code that computes `DIRECT_MAP_BASE + p` by
hand has no place to put a per-CPU slot, a TLB flush or a bounds check when
one is needed, and no way to be found when the map changes. 1.9c puts every
such touch behind one interface whose rules hold on one CPU today and on
eight tomorrow.

## 1. The interface

`kernel/memman/physmap.h`:

| Call | For |
|---|---|
| `phys_map(phys)` / `phys_unmap(at)` | a short look at one frame: copy into it, read a word, fill it |
| `phys_zero(frame)` | clear a frame (a page off the free list holds what its last owner left) |
| `phys_map_lasting(phys, len)` | a range kept mapped for good (the page database) |
| `phys_map_depth()` | how many of this CPU's mappings are open |

The arch side (`arch/paging.h`) gives `paging_phys_slot(cpu, slot, frame)`,
`paging_phys_slot_clear` and `paging_map_ram`; on x86-64 each is the direct
map. A 32-bit or ARM port without a direct map of all RAM gives each CPU
`PHYS_MAP_DEPTH` page-table slots instead, and nothing above changes.
`RAM_REACH` (`paging_defs.h`) is how far the kernel can reach.

Device registers are not RAM and are not here: they go through the physical
window (`paging_map_phys`, `PHYS_WINDOW_*`), uncached.

## 2. The rules

Each a panic, or with the checker armed (`physmap_test_arm`, test hooks
only) a counted failure:

- **Mappings nest, last in first out**, up to `PHYS_MAP_DEPTH` (8) per CPU:
  an unmap of anything but the newest (`PM_NOT_LAST`), an unmap with none
  open (`PM_NONE_OPEN`), and a ninth (`PM_DEPTH`) are each caught.
- **Interrupts are off from map to unmap**; the unmap restores what the map
  found. A slot is the CPU's, so nothing else may run on it meanwhile.
- **No frame past `RAM_REACH`** (`PM_REACH`).
- **No thread switch with one open** (`PM_SWITCH`): `switch_to` asks
  `phys_assert_none`, beside the lock checker's own question.

After a panic (`stopped`) mapping still works, through slot 0 with no list,
so the crash path can read what it needs.

How the reference systems do it, and what was taken:

- Serenity `Kernel/Memory/MemoryManager.cpp` `quickmap_page` /
  `unquickmap_page`: one slot per CPU behind a per-CPU lock taken with
  interrupts off. Taken: interrupts off for the whole mapping. Not taken:
  one slot only; the loader maps the page it fills while the image's own
  pages are mapped, so mappings must nest.
- Haiku `x86_physical_page_mapper_large_memory.cpp` `GetPageCurrentCPU` /
  `PutPageCurrentCPU`: a queue of slots per CPU, the caller pinned to it;
  `generic_vm_physical_page_ops.cpp` builds `memset_physical` on it.
  Taken: several slots per CPU, and `phys_zero` as the built-in fill.
- WRK `base/ntos/mm/hypermap.c` `MiMapPageInHyperSpace`: returns holding
  the hyperspace lock (raised IRQL), released by the unmap. Taken: the
  map is a critical section that the unmap ends.
- XNU `osfmk/i386/pmap.h` `PHYSMAP_PTOV_check`: XNU keeps a direct map
  like this one and panics on an address past it. Taken: `RAM_REACH`.

## 3. Free pages are poisoned (test hooks)

`pmm_alloc` does not zero. In test-hook builds a freed frame is filled with
`PMM_POISON` (`0xA5A5A5A5A5A5A5A5`, `pmm.h`), once the buddy accepts the
free (a refused one is still someone's) and under the page database's lock,
so a frame handed on unzeroed shows on every boot rather than when its last
owner happened to leave something. The first boot with it found one:
`spawn.c` mapped a new process's 64 user stack pages without clearing them,
so ring 3 started on the last owner's data. They are cleared now, and an X
self-test reads the stack's bottom 16 pages.

## 4. The allocation paths

Every allocation path is on `ocache` or the locked heap, or holds its own
lock:

| Path | Before | Now |
|---|---|---|
| `memman/kstack.c` slots | no lock | `slots_lock` (`LR_KSTACK`, after `LR_DMA`): a slot claimed (`SLOT_BUSY`) and given back under it, pages mapped and freed outside it |
| `cpu/sysfile.c` file locks | a static array of 64 | an `ocache` (`file-lock`), capped at `MAX_FILE_LOCKS` |
| `memman/dma.c` regions | locked (`LR_DMA`) | - |

Not allocators, so left: the PCI, USB and disk tables (filled at boot),
the ARP cache and the syslog ring (caches and rings that overwrite).
The file system layer as a whole, and `sysfile`'s graveyard and lock
waiters, left for 1.9e, are locked since 1.9e3 (the storage lock and
`files_lock`, `kernel/CX_SMP.md` §4b).

## 5. Enforced by the build

`tools/cmake/check_arch_boundary.cmake` refuses `DIRECT_MAP_`,
`PHYS_WINDOW_`, `paging_phys_slot` and `paging_map_ram` outside
`kernel/arch/` and `kernel/test/`, but in `memman/physmap.c`. Two uses of
the boot's identity window at `KERNEL_VBASE` stay: the boot info page
(`cxbi.c`) and the VGA text buffer (`platform/pc/vga.c`,
`arch/x86_64/idt.c`'s panic screen), both reached before
the page database exists; they go when the boot window does.

## 6. Tests

- `hooks: physical memory is mapped by the rules` (`ktest_hooks.c`): nesting
  and the frame's offset kept, interrupts off while mapped and restored
  after, each of the five failures caught and leaving nothing open,
  `phys_zero`, a freed frame all `PMM_POISON`, a refused free of a page of
  the kernel's image leaving it as it was.
- `hooks: kernel stacks take their lock`: an alloc and a free each take
  the kstack lock twice (under a test lock of the same rank, two `LO_ORDER`
  failures each).
- `advisory locks follow the open file`, steps 58 to 61: a lock is one
  object, an allocation failure refuses the lock and keeps nothing, the cap
  holds, an unlock gives them back.
- `x: memory: a new process's stack is zeroed`.

Each seen red under sabotage: `tests/sabotage/1.9c.sab`.
