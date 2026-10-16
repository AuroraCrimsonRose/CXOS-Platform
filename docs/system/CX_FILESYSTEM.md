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

## 6. Where CXOS aims to be better

1. **Foreign filesystems run in user space.** CXFS and the RAM filesystem stay
   in the kernel. FAT32, exFAT, ISO 9660, and reading NTFS or ext4 run as
   services behind the VFS, because a USB stick from outside is hostile input
   and a crafted disk image is a known way into a kernel. macOS moved FAT and
   exFAT to user space (FSKit) for this reason; Fuchsia and Redox run every
   filesystem there.
2. **One cache.** File data is cached in memory objects, so reading a file and
   mapping it see the same pages (Haiku `cache/file_cache.cpp` and
   `vnode_store.cpp`; NT's cache manager; XNU's unified buffer cache). A
   user-space filesystem fills pages through a pager interface, so a cached
   read never visits its process.
3. **Asynchronous I/O.** Requests queue with completions through the wait
   queue; the AHCI driver keeps up to 32 commands in flight (NCQ).
4. **Safe save.** Create an unnamed file in a directory, write it, then put it
   in place in one atomic step that survives a crash. `std`'s save uses it, so
   a crash never leaves a half-written document or settings file.
5. **Change notifications and a change journal.** Watch handles report changes
   through the wait queue (inotify, kqueue, FSEvents), and each volume keeps a
   persistent journal of changes (NTFS's USN journal, `base/fs/ntfs/usnsup.c`;
   macOS FSEvents), so search indexing and backup catch up after a reboot
   without rescanning.
6. **Crash-consistency testing.** A test run records every block write, then
   every prefix of that record, and every reordering the write barriers allow,
   is mounted and checked (CrashMonkey, Linux dm-log-writes). This is the
   "prove it by breaking it" rule applied to the disk, and it is the tool for
   the class of failure behind the intermittent install bug.
7. **Instant copies.** Copy-on-write lets a copy share blocks until either side
   changes (APFS `clonefile`, Linux reflink).
8. **`/System` checked as it is read.** The signed system layers carry a hash
   tree, so every block is verified when read, not only at install (dm-verity,
   APFS's signed system volume).
9. **Background scrub** of the checksums, so a failing disk is found before the
   data is needed.
10. **Rights on file handles:** read, write, append-only, execute, map. A
    directory handle can never reach above itself: no `..` out of it, and no
    symbolic link that escapes (Capsicum, Linux `RESOLVE_BENEATH`).
11. **Quotas** per user and per app folder, charged to job budgets.
12. **Encryption** later: per-user keys from the password, a key per file
    (APFS, Linux fscrypt). The rig has no TPM.

## 7. Order

1. VFS layer and RAM filesystem; directory handles with rights.
2. Known-folders API; safe save.
3. One cache in memory objects; asynchronous I/O.
4. CXFS v3, with the crash-consistency harness built alongside it; change
   notifications and the change journal; instant copies.
5. Packages, layers, `/Local`, per-app folders, the picker, layered settings;
   the hash tree for `/System`.
6. Foreign filesystems in user space: FAT32 first (Phase B), on the VFS.
7. Later: quotas, encryption, background scrub.

## 8. Names

Decided 2026-10-16: **case-insensitive and case-preserving**, as on Windows
and macOS. A name is stored exactly as typed and compared after Unicode
normalisation and case folding, so `Report.txt`, `report.TXT` and the same
name typed with a composed or a decomposed accent are one file (APFS behaves
this way). Names must be valid UTF-8, with no control characters and no `/`.
The directory index stores the folded form beside the name, so a lookup costs
no more than a case-sensitive one.

## 9. CXFS in the kernel: the risk, and the answer

A filesystem in the kernel parses whatever is on the disk. If an attacker can
write that disk, the kernel parses the attacker's data with full privilege.
How an attacker could write it:

| Path | Answer |
|---|---|
| A CXFS disk or image from elsewhere: a USB stick, a downloaded image, a second drive | **Only the boot disk's own volumes mount in the kernel.** Any other CXFS volume mounts through the user-space instance of the same CXFS code (§6.1), as a foreign filesystem would. |
| A program writing raw sectors under the filesystem | **No program gets raw access to a mounted volume.** `GRANT_DISK` today is all or nothing; it becomes a handle to one unmounted device, given only to disk tools. |
| Someone with the machine off: edits the disk, then boots it | **`/System` is verified before it is parsed:** the hash tree (§6.8) is checked against the signed root, so tampered metadata is rejected rather than interpreted. Writable volumes get **authenticated metadata** with encryption (§6.12): without the key, an edit fails the check before any parsing. Until then, physical tampering with the data volume is a stated gap (`security/CX_SECURITY.md` §2). |
| A bug in CXFS itself | **Fuzzed on the host:** CXFS builds for the DevKit, and every image the crash-consistency harness produces (§6.6) is also mutated and mounted. Every on-disk field is range-checked on read. |

**The VFS keeps the choice open.** A filesystem implements one operations
table and does not know whether it runs in the kernel or as a service. If
CXFS in the kernel ever proves a problem, it moves out without rewriting it.
Fuchsia runs every filesystem as a service; the price is that the filesystem
needed to boot must itself be started from something simpler (Fuchsia's
bootfs), which CXOS's loader can provide.
