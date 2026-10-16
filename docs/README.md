# CXOS documentation (x86-64 line)

This branch, `x86_64-DEV`, is where CXOS is going: x86-64 first, then
everything in the roadmap. Its documents start fresh from the design reviews of
2026-10-16. The documents of the x86-32 line are kept, frozen, in
[`x86_32/`](x86_32/README.md); they describe the inherited code until each area
is reworked, and their content is ported here as needed.

Start with the roadmap.

## Planning: `planning/`

| Document | Covers |
|---|---|
| [CX_ROADMAP.md](planning/CX_ROADMAP.md) | The plan: principles, phases, every decision and why |
| [HARDENING_PLAN.md](planning/HARDENING_PLAN.md) | The response to the 2026-10-01 reviews (carried over, still live) |
| [VERSIONING_AND_RELEASE.md](planning/VERSIONING_AND_RELEASE.md) | `versions.json` and how a release is cut (carried over, still live) |
| [TRANSITIONAL_CODE.md](planning/TRANSITIONAL_CODE.md) | Code on its way somewhere (carried over, still live) |

## Hardware: `hardware/`

| Document | Covers |
|---|---|
| [CX_TARGET_HARDWARE.md](hardware/CX_TARGET_HARDWARE.md) | The test rig, and what it decides |

## Kernel: `kernel/`

| Document | Covers |
|---|---|
| [CX_KERNEL_PLAN.md](kernel/CX_KERNEL_PLAN.md) | The kernel base, objects and handles, processes and threads, waiting, IPC, scheduling |
| [CX_SYSCALLS_IPC.md](kernel/CX_SYSCALLS_IPC.md) | Syscalls, calling convention, ABI rules, errors, the system page, `.xidl`, IPC messages |
| [CX_DRIVERS.md](kernel/CX_DRIVERS.md) | The device tree, matching, user-space drivers, device classes |

## Boot: `boot/`

| Document | Covers |
|---|---|
| [CX_BOOT.md](boot/CX_BOOT.md) | One loader for BIOS and UEFI that starts a 64-bit kernel |

## System: `system/`

| Document | Covers |
|---|---|
| [CX_DISPLAY.md](system/CX_DISPLAY.md) | The display server, compositor, animations, trusted UI, GPU path |
| [CX_AUDIO.md](system/CX_AUDIO.md) | The audio graph, timing, drivers, privacy |
| [CX_NETWORK.md](system/CX_NETWORK.md) | The network stack as a user-space service, per-app permission, connect by name, TLS |
| [CX_FILESYSTEM.md](system/CX_FILESYSTEM.md) | The layout, the VFS layer, CXFS v3, foreign filesystems in user space, one cache |

## Language and formats

| Document | Covers |
|---|---|
| [language/CX_X_PLAN.md](language/CX_X_PLAN.md) | What X needs next, in order |
| [formats/CX_FILE_TYPES.md](formats/CX_FILE_TYPES.md) | File types: what changes from the x86-32 scheme |

## DevKit: `devkit/`

| Document | Covers |
|---|---|
| [CX_DEVKIT_DESIGN.md](devkit/CX_DEVKIT_DESIGN.md) | DevKit architecture (carried over) |
| [index.md](devkit/index.md) | Home of the DevKit API reference, built with DocFX |

## Reviews: `reviews/`

External reviews, kept as written. Their findings are tracked in the hardening
plan.

| Review | |
|---|---|
| [2026-10-01/KERNEL_SECURITY_REVIEW.md](reviews/2026-10-01/KERNEL_SECURITY_REVIEW.md) | Kernel security |
| [2026-10-01/KERNEL_ENGINEERING_REVIEW.md](reviews/2026-10-01/KERNEL_ENGINEERING_REVIEW.md) | Kernel engineering |
| [2026-10-01/DEVKIT_SECURITY_REVIEW.md](reviews/2026-10-01/DEVKIT_SECURITY_REVIEW.md) | DevKit security |
| [2026-10-01/DEVKIT_ENGINEERING_REVIEW.md](reviews/2026-10-01/DEVKIT_ENGINEERING_REVIEW.md) | DevKit engineering |
| [2026-10-09/PLATFORM_SECURITY_REVIEW.md](reviews/2026-10-09/PLATFORM_SECURITY_REVIEW.md) | Platform security |
