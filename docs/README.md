# CXOS Platform documentation

## Kernel — `kernel/`

| Document | Covers |
|---|---|
| [CX_ABI.md](kernel/CX_ABI.md) | The syscall and capability contract (v2): numbers, capabilities, IPC, handles |
| [PROCESS_MODEL.md](kernel/PROCESS_MODEL.md) | Ring 3, scheduling, preemption, per-process address spaces |

## System — `system/`

| Document | Covers |
|---|---|
| [CXFS_FILESYSTEM.md](system/CXFS_FILESYSTEM.md) | The CXFS filesystem |
| [CX_FILESYSTEM_LAYOUT.md](system/CX_FILESYSTEM_LAYOUT.md) | Where things live on a CXOS disk (a proposal) |

## Formats — `formats/`

| Document | Covers |
|---|---|
| [CX_EXTENSION_SYSTEM.md](formats/CX_EXTENSION_SYSTEM.md) | The CXEX executable format, the file-type system, code signing |
| [CX_EXTENSION_NAMING.md](formats/CX_EXTENSION_NAMING.md) | The `X + Domain + Type` naming formula |
| [CX_FILE_STRUCTURE.md](formats/CX_FILE_STRUCTURE.md) | CXEX file structure, section by section |

## Language — `language/`

| Document | Covers |
|---|---|
| [CX_X_CORE_LANG.md](language/CX_X_CORE_LANG.md) | The X core language, and the route to self-hosting (§10) |
| [CX_X_DATA.md](language/CX_X_DATA.md) | X Data, the configuration and descriptor format |

## DevKit — `devkit/`

| Document | Covers |
|---|---|
| [CX_DEVKIT_DESIGN.md](devkit/CX_DEVKIT_DESIGN.md) | DevKit architecture, artifact taxonomy, key authority and signing, Studio, roadmap |
| [index.md](devkit/index.md) | Home page of the DevKit API reference, built with DocFX (`docfx.json` beside it) |

The tool reference is [`devkit/README.md`](../devkit/README.md) at the DevKit's root.

## Planning — `planning/`

| Document | Covers |
|---|---|
| [CX_ROADMAP.md](planning/CX_ROADMAP.md) | Where CXOS is going, and in what order |
| [HARDENING_PLAN.md](planning/HARDENING_PLAN.md) | The response to the 2026-10-01 reviews: decisions, every finding's status, the phased checklist |
| [VERSIONING_AND_RELEASE.md](planning/VERSIONING_AND_RELEASE.md) | `versions.json`, what every component and format version means, and how a release is cut |
| [history/V5_PORTING_MANIFEST.md](planning/history/V5_PORTING_MANIFEST.md) | The v5 port plan (complete; historical) |

## Reviews — `reviews/`

External reviews, kept as written. Their findings are tracked in the hardening plan.

| Review | |
|---|---|
| [2026-10-01/KERNEL_SECURITY_REVIEW.md](reviews/2026-10-01/KERNEL_SECURITY_REVIEW.md) | Kernel security |
| [2026-10-01/KERNEL_ENGINEERING_REVIEW.md](reviews/2026-10-01/KERNEL_ENGINEERING_REVIEW.md) | Kernel engineering |
| [2026-10-01/DEVKIT_SECURITY_REVIEW.md](reviews/2026-10-01/DEVKIT_SECURITY_REVIEW.md) | DevKit security |
| [2026-10-01/DEVKIT_ENGINEERING_REVIEW.md](reviews/2026-10-01/DEVKIT_ENGINEERING_REVIEW.md) | DevKit engineering |
