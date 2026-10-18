# Boot
### CX Design Spec: Aurora Tejeda / CATX Systems

> **Status: DESIGN.** One loader for BIOS and UEFI that starts a 64-bit kernel.

---

## 1. Inherited

- **BIOS:** `boot.asm` loads stage 2 from LBA 2 (32 sectors). Stage 2 enables
  A20, gathers the E820 map, sets a VBE mode, and `cxexload.asm` places the
  kernel's sections and jumps. Nothing checks the kernel's signature. Since
  roadmap step 4a the kernel no longer reads its boot data from fixed
  addresses (§4 item 1).
- **UEFI:** `boot/uefi/cxboot.c` is part 1 only: it gathers information into
  CXBI, prints it and stops.
- **The handoff structure is right already.** `abi/cxk_boot.h` uses fixed
  64-bit fields so a 32-bit or 64-bit kernel reads it identically.

## 2. How the others do it

- **Haiku**, the closest model: one loader with a platform layer
  (`boot/platform/{bios_ia32,efi,...}`) and an architecture layer
  (`boot/arch/*`). Its 32-bit BIOS loader starts a 64-bit kernel:
  `bios_ia32/long.cpp` builds 64-bit page tables, widens the handoff
  structure's addresses (`convert_kernel_args`) and enters long mode. Its ELF
  loader takes both classes (`loader/elf.cpp`).
- **Serenity:** GRUB or Limine plus a prekernel (`Kernel/Prekernel`,
  `Kernel/EFIPrekernel`).
- **Windows:** bootmgr then winload; under UEFI the loader's bitness must match
  the firmware's.
- **macOS** (public, not in the tree): boot.efi once chose a 32-bit or 64-bit
  kernel by machine.

On UEFI the firmware fixes the loader's bitness. A BIOS loader can start
either kernel.

## 3. Design: `cxboot`

One loader, two front ends (BIOS, UEFI), one shared core in C.

1. **BIOS is the main path** (the rig is BIOS-only). The BIOS front end goes
   from real mode to long mode itself, as Haiku does.
2. **Check the CPU.** Without long mode or NX, refuse with a clear message.
   There is one kernel, for x86-64.
3. **Hand over through CXBI only.** Both front ends fill `cxk_boot_info`; the
   kernel never reads a fixed address. This step can land first, on 32-bit.
4. **Verify the kernel's signature in the loader.** With Secure Boot (already
   working with CXOS's own keys) the chain is unbroken: firmware, loader,
   kernel, programs. TPM measurement later.
5. **Automatic rollback.** A boot entry is a kernel plus a system layer
   (`system/CX_FILESYSTEM.md`). An entry that fails to reach "boot complete" a
   set number of times is abandoned for the last good one, with no one at the
   keyboard (Android and ChromeOS boot counting).
6. **A boot menu:** safe mode (built-in drivers and the plain framebuffer
   only), verbose, self-test mode, kernel options.
7. **Boot drivers load beside the kernel**, so storage drivers need not be
   built into it.
8. **Firmware entropy** goes into `rng_seed`, for KASLR and the stack
   protector.
9. **A read-only CXFS reader in the loader**, so each entry's kernel lives
   inside its system layer instead of a raw partition. This answers the open
   question in the x86-32 layout document §4.

## 4. Order

1. The kernel reads CXBI; the BIOS stage fills it (32-bit, now). **Done
   2026-10-18** (roadmap step 4a). Stage 2 builds `cxk_boot_info` at physical
   `0x2000` (`boot/cxbi.asm`): the E820 map, gathered straight into it with
   unknown types folded into reserved; the VBE mode with its pixel layout; the
   ACPI RSDP, found and checksummed by the loader (EBDA, then
   `0xE0000`–`0xFFFFF`); CPUID leaf 0 and leaf 1; the firmware kind. Its
   address goes to the kernel in EBX. `kernel/boot/cxbi.c` checks the magic,
   version, size and every map entry's type, refuses the boot otherwise, and
   keeps a copy; the PMM, the framebuffer driver and ACPI read only that. The
   loader fills its old addresses (`0x500`, `0x1C40`) with `0xA5`, and a
   self-test checks the poison, so nothing can go on reading them unnoticed.
2. The shared C core and the BIOS front end into long mode, with the 64-bit
   kernel bring-up.
3. Signature check in the loader.
4. UEFI part 2 on the shared core.
5. The CXFS reader, boot entries, rollback and the menu, with the layered
   system tree.
