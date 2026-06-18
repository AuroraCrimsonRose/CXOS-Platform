# CXK

**CATX Kernel**

A capabilities and execution kernel for x86 systems.

CXK provides memory management, process execution, scheduling, privilege separation, storage access, networking, trust infrastructure, and executable loading services for the broader CX ecosystem.

The project is designed around modularity, verifiable execution, and reusable system capabilities. Rather than coupling operating system functionality directly into the kernel, CXK exposes foundational services that higher-level CX operating environments can build upon.

---

## Current Status

CXK is currently under active development.

The v5 architecture focuses on preserving proven subsystems from previous releases while modernizing the boot chain, executable loading infrastructure, and trust model.

Current development targets include:

- XKEX executable loading
- XBEX boot-stage execution
- CXFS integration
- Signed executable verification
- Ring 3 process execution
- Capability-oriented kernel services

---

## Core Concepts

### Capabilities

CXK is designed as a capabilities and execution kernel.

Core kernel services provide:

- Memory management
- Process scheduling
- User-mode execution
- Storage access
- Network access
- Power management
- Cryptographic verification
- Executable loading

These services form the foundation for future CX operating environments.

### Execution

Execution is a first-class concept within CXK.

The kernel currently supports:

- Ring 0 execution
- Ring 3 execution
- Cooperative multitasking
- Preemptive multitasking
- Kernel threads
- User processes
- Context switching

### Trust

CXK includes cryptographic infrastructure used to support signed executable deployment.

Current components include:

- SHA-256 implementation
- Public/private key tooling
- Signature verification infrastructure
- XKEX signing workflow

---

## Architecture

```text
BIOS
 └─ boot.asm
      ↓
   stage2.xbex
      ↓
   kernel.xkex
      ↓
   CXK
      ├─ Memory Management
      ├─ Process Scheduling
      ├─ Capability Services
      ├─ Storage
      ├─ Networking
      ├─ Power Management
      └─ Executable Loading
```

---

## Major Components

### CPU

- GDT
- IDT
- PIC
- FPU support
- Ring 3 transitions
- Context switching
- User-mode execution

### Memory Management

- Physical memory manager
- Paging
- Kernel heap
- Dynamic allocation

### Process Model

- Kernel threads
- User processes
- Cooperative scheduling
- Preemptive scheduling
- Privilege separation

### Storage

- ATA
- AHCI
- Disk abstraction layer

### Filesystem

- CXFS filesystem

### Networking

- Intel e1000 support
- ARP
- IPv4 foundation
- ICMP Echo (Ping)

### Power

- ACPI support
- Shutdown
- Reboot
- CPU idle support

### Video

- VGA text mode
- Framebuffer support
- VBE initialization

---

## CATX Format Ecosystem

CXK uses a structured CATX format naming convention.

Format names follow:

```text
X + Domain + Type
```

Examples:

| Format | Description |
|----------|----------|
| XKEX | Kernel Executable |
| XKPK | Kernel Public Key |
| XKSK | Kernel Secret Key |
| XBSG | Boot Signature |
| XPMF | Package Manifest |

This naming system allows tooling and developers to identify broad file purpose directly from the extension.

See:

- docs/CX_EXTENSION_NAMING.md
- docs/CX_EXTENSION_SYSTEM.md

for additional details.

---

## v5 Goals

The primary objective of v5 is to modernize executable loading and deployment while preserving proven subsystems.

### New

- XBEX stage-2 boot execution
- XKEX loading infrastructure
- CXEX runtime loader
- Signed executable workflow

### Ported From Previous Versions

- Memory management
- Scheduler
- Ring 3 execution
- ATA and AHCI
- Networking
- Power management
- CXFS

### Future

- UEFI boot support
- Expanded networking stack
- Additional CX formats
- Enhanced CXFS features
- User-space executable ecosystem

---

## Building

Requirements:

- NASM
- CMake
- i686-elf GCC toolchain
- QEMU
- Bochs (optional)

Build:

```bat
tools\build.bat
```

Build signed image:

```bat
tools\build_signed.bat
```

Run:

```bat
tools\run_qemu.bat
```

AHCI test:

```bat
tools\run_qemu_ahci.bat
```

---

## Documentation

- docs/PROCESS_MODEL.md
- docs/CXFS_FILESYSTEM.md
- docs/CX_EXTENSION_SYSTEM.md
- docs/CX_EXTENSION_NAMING.md
- docs/BIOS_ERROR_CODES.md

---

## License

See LICENSE.md.

---

© Aurora Tejeda / CATX SYSTEMS LLC