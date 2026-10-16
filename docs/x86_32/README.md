# x86-32 documents (inherited, frozen)

These are the documents of the x86-32 line, as they stood when `x86_64-DEV`
branched from `x86_32_DEV` on 2026-10-16. They describe the code this branch
inherited, so they stay accurate for that code until it is replaced.

**They are not updated here.** Where a new document under `docs/` covers the
same ground, the new one is the plan and this one is history. Content is ported
into the new documents as each area is reworked, never edited in place.

Still the reference for code that has not been reworked yet:

| Document | Reference for |
|---|---|
| `kernel/CX_ABI.md` | the syscall contract as built (v2, 32-bit) |
| `kernel/PROCESS_MODEL.md` | ring 3, scheduling, address spaces as built |
| `kernel/KERNEL_CONTRACTS.md` | failure tiers; where x86 assumptions may live |
| `system/CXFS_FILESYSTEM.md` | the CXFS v2 on-disk format |
| `system/CX_FILESYSTEM_LAYOUT.md` | the current tree (superseded by `system/CX_FILESYSTEM.md`) |
| `formats/CX_EXTENSION_SYSTEM.md` | CXEX, the extension scheme, code signing |
| `formats/CX_EXTENSION_NAMING.md` | the `x` + domain + type formula |
| `formats/CX_FILE_STRUCTURE.md` | CXEX section by section |
| `formats/CX_KEY_FORMAT.md` | keys, the CXSG signature block, the trust chain |
| `language/CX_X_CORE_LANG.md` | X as built (superseded as a plan by `language/CX_X_PLAN.md`) |
| `language/CX_X_DATA.md` | X Data, unchanged and still authoritative |
| `planning/CX_ROADMAP.md` | the x86-32 roadmap (superseded by `planning/CX_ROADMAP.md`) |
| `planning/history/` | the v5 port plan (historical) |
