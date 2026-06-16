# CXK - x86_32 Kernel

**Version 0.0.1.0**
A hobby x86 kernel by Aurora Tejeda / CATX SYSTEMS LLC.

## Overview

**CXK** (the CXOS Kernel) is a 32-bit x86 operating-system kernel written from
scratch - the core of the CXOS operating system. It pairs with a custom
bootloader and is built to learn and explore OS development on real hardware.
It brings up the machine into a graphical framebuffer console with an
interactive shell, and includes drivers for storage, USB, power management, and
networking.

CXK is the kernel; CXOS is the broader operating system it forms the core of.
It targets AMD AM3+ era hardware and newer, and runs on bare metal as well as in
QEMU and Bochs.

## Features

### Boot & Core
- Custom 16-bit bootloader -> protected mode -> 32-bit C kernel
- Self-sizing kernel loader (no fixed sector-count limit)
- Interrupt Descriptor Table (IDT), PIC remapping, PIT timer (1000 Hz)
- Physical memory manager, 2-level paging, kernel heap
- FPU / SSE support
- Graphics-aware kernel panic with stack trace

### Display
- VBE framebuffer console (8/16/32 bpp) with VGA text-mode fallback
- Dual fonts (8x16 / 8x8), adjustable text size
- Batched, scroll-aware rendering for fast output

### Storage
- ATA PIO driver (28-bit LBA)
- AHCI SATA driver (48-bit LBA, DMA) - validated on real drives
- Unified disk registry (HDD/SSD/USB naming, by-id and by-name access)
- CXFS filesystem

### USB
- OHCI host controller driver
- Device enumeration (control transfers, descriptors)
- Mass storage class (Bulk-Only Transport + SCSI) - readable/writable USB disks

### Power (ACPI)
- ACPI table parsing (RSDP / FADT / DSDT scan)
- Shutdown (S5), reboot (ACPI reset), low-power idle (C1), S1 detection

### Networking (in progress)
- Intel e1000 NIC driver (PCI, DMA descriptor rings)
- Ethernet framing + ARP
- Static IP / mask / gateway / DNS configuration
- (IP / ICMP / UDP and beyond: under development)

### Shell
Interactive command shell with command history and line editing. Includes
commands for the filesystem, disks, PCI, USB, networking, power, and more
(type `help` for the full list).

## Toolchain

- GCC i686-ELF cross compiler
- NASM
- CMake (NMake Makefiles generator)

## Building

From the project root:

```
CXK\tools\build.bat        REM incremental build
CXK\tools\clean.bat        REM clean rebuild
```

Output images are written to `CXK/dist/CXK_x86_32/`.

## Running

```
CXK\tools\run_qemu.bat         REM QEMU (i440FX) - default
CXK\tools\run_qemu_ahci.bat    REM QEMU (q35 + AHCI) - storage testing
CXK\tools\run_bochs.bat        REM Bochs
```

## Platform

- x86 (32-bit / protected mode)
- AMD AM3+ era hardware and newer
- Runs on bare metal, QEMU, and Bochs

## License / Ownership

(c) Aurora Tejeda / CATX SYSTEMS LLC.