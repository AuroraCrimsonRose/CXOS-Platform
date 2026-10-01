# CX DevKit

Host-side toolchain for the CX ecosystem: the X Native compiler, the `cxk` CLI, the CXEX
format and signing libraries, and CXEX Studio.

## Start here

- **[Architecture, Vision & Roadmap](CX_DEVKIT_DESIGN.md)** — the main design document.
  Artifact taxonomy, key authority and signing, Studio design, the phased roadmap, and the
  known cross-repo ABI hazard (§5.2).
- **[README](../../devkit/README.md)** — project layout, the `cxk` command set, and the compile
  pipeline as it actually runs.
- **[Review Response & Hardening Plan](../planning/HARDENING_PLAN.md)** — what the 2026-10-01 reviews
  found, checked against the code, and the phased plan answering them. Tests are moving to
  xUnit (`CXEX.Tests`), and platform scripts to `cxk` commands.
- **[Security Review](../reviews/2026-10-01/DEVKIT_SECURITY_REVIEW.md)** and **[Engineering Review](../reviews/2026-10-01/DEVKIT_ENGINEERING_REVIEW.md)**
  — the reviews themselves, kept as written.

## API reference

Browse the **API** tab for the auto-generated reference over `CXEX.Build`, `CXEX.Disk`,
`CXEX.FileSystem`, `CXEX.Lang`, `CXEX.Core` and the rest. Anything written as a `///` XML doc
comment in source appears there on the next build.

## The language and the ABI live in CXK

X is specified on the kernel side, because the kernel owns the ABI it compiles against:

| Document | Covers |
|---|---|
| `docs/language/CX_X_CORE_LANG.md` | X core language spec (v0.2); the route to self-hosting |
| `docs/kernel/CX_ABI.md` | Syscall numbers, capabilities, IPC, handles |
| `docs/formats/CX_EXTENSION_SYSTEM.md` | CXEX format, file-type system, code signing |
| `docs/kernel/PROCESS_MODEL.md` | Ring 3, scheduling, preemption, address spaces |
