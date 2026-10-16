# Filesystems and the system tree
### CX Design Spec: Aurora Tejeda / CATX Systems

> **Status: DESIGN.** Replaces the layout proposal in
> `docs/x86_32/system/CX_FILESYSTEM_LAYOUT.md` as the plan; that document and
> `docs/x86_32/system/CXFS_FILESYSTEM.md` still describe what is built.

---

## 1. Inherited

- **The tree:** `/System`, `/Shared`, `/User/<name>`, `/Temp`,
  `/Drives/<drive>/<volume>`. Already good: readable names, other drives
  inside the tree, location never grants the right to run, system search paths
  before user-writable ones.
- **Tied to CXFS.** About 45 calls to `cxfs_*` across `sysfile.c`,
  `install.c`, `kconfig.c` and `spawn.c`; an open file is a raw CXFS entry id.
- **CXFS v2 limits:** 1024 entries per volume in total, 8 extents per file,
  directories found by scanning the whole manifest, 63-byte names, nothing
  found on crash consistency.
- `/Shared/Config` is writable by every program, so any program can rewrite
  another's settings (the layout document's own warning).

## 2. How the others do it

- **Linux:** one app spread over `/usr/bin`, `/usr/lib`, `/usr/share`,
  `/etc`, `/var` and dotfiles; `/etc` accumulates hand edits.
- **Windows:** Program Files, three kinds of AppData, ProgramData and the
  registry; uninstalling leaves debris.
- **macOS:** apps are bundles; the system volume is signed and read-only;
  sandboxed apps get a container folder.
- **Haiku:** each package is an image mounted read-only and merged into the
  system tree (`packagefs`); apps ask `find_directory` (`FindDirectory.h`)
  where things are.

## 3. The VFS layer

- A mount table, an operations table per filesystem type, path lookup that
  crosses mount points, and open files keyed by mount and node.
- Locks and deferred delete move up into it, so every filesystem has them.
- **A RAM filesystem** proves two filesystems work before FAT32, and holds the
  shared `/Temp`.
- **Directory handles:** open relative to a directory handle (`openat`,
  Capsicum, Redox), so a process can be given one directory instead of the
  whole disk. Today `GRANT_DISK` is all or nothing.
- Filesystems without permissions (FAT32) get an owner and mode from the mount.
- Trust does not depend on CXFS: programs are trusted by signature.

## 4. CXFS v3

An index per directory (B-tree); far more entries; unlimited extents;
**copy-on-write** updates, consistent after a crash and giving snapshots;
**checksums** on data and metadata; **attributes**, including an origin marker
on downloaded files, indexed for live queries as in Haiku's BFS.

## 5. The tree

```
/System        read-only signed layers: the OS, never written while running
/Local         machine-wide state that changes: setting overrides, installed
               packages, logs, services
/User/<name>/
    Apps/<app>/Config  Data  Cache  Temp     one folder per app
    Documents/ ...                            the user's own
/Drives/<drive>/<volume>
/Temp          shared scratch, on the RAM filesystem
```

1. **An app is one signed package**, mounted read-only. Install adds a file;
   uninstall deletes it.
2. **`/System` is a stack of signed read-only layers.** An update stacks a
   layer; rollback removes it, tied to the loader's automatic rollback
   (`boot/CX_BOOT.md`). Nothing writes `/System` while the system runs.
3. **Each app sees only its own folder** (`<app>` is signer plus name), through
   a directory handle. Other files come through a picker drawn by the trusted
   UI, which hands over a handle to just the chosen file. This retires
   `/Shared/Config`.
4. **Config, data and cache mean something:** reset clears `Config`; freeing
   space clears every `Cache`; backups skip caches; uninstall asks about data.
5. **Settings in layers, as X Data:** signed defaults in `/System`, machine
   overrides in `/Local`, user overrides; the difference from the defaults is
   always visible and reversible.
6. **No hard-coded paths:** a known-folders API in `std`, now.
7. **Temporary files per user and per app**; no shared world-writable scratch
   for programs.
8. **Devices stay out of the filesystem.** They are handles from the device
   tree; system information comes from APIs.
9. **The kernel lives in its system layer** once the loader can read CXFS.

## 6. Order

1. VFS layer and RAM filesystem; directory handles.
2. Known-folders API.
3. CXFS v3.
4. Packages, layers, `/Local`, per-app folders, the picker, layered settings.
5. FAT32 (Phase B) on the VFS.
