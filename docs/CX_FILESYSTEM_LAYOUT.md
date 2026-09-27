# CXOS Filesystem Layout

**CATX Systems LLC — Aurora Tejeda**
Status: **PROPOSAL — for review.** Nothing depends on this yet.

Where things live on a mounted CXFS volume. `CXFS_FILESYSTEM.md` defines the
on-disk *format*; this defines the *tree* built on top of it. Until now the
only documented path was `/System`, named in `CX_EXTENSION_SYSTEM.md` §10 as
the protected area holding `.xkpk`, and the install path created it and put
`Boot.xoex` in it. Everything else was unwritten, so it got invented per
caller — which is exactly how two callers end up disagreeing.

---

## 1. What CXFS makes cheap, and what it makes expensive

The layout is shaped by four properties of the filesystem underneath it. They
are not incidental; a layout that ignores them is wrong here even if it looks
familiar from elsewhere.

- **Directory membership is derived, not stored.** A directory has no data
  blocks; `cxfs_list_dir` and `cxfs_find_in_dir` scan the *whole manifest* for
  entries whose `parent_id` matches. So the cost of a lookup is the size of the
  volume, not the size of the directory. **Depth is what costs**, because
  resolving an N-component path is N manifest scans. Shallow beats tidy.
- **The manifest is a fixed 1024 entries** (`CXFS_MAX_ENTRIES`). Every
  directory permanently spends one of them. A deep FHS-style tree would burn a
  noticeable fraction of the volume's total file count on empty structure.
- **Depth is capped at 16** by `cxfs_path_of`, which walks up to the root
  through a 16-level name buffer. A path deeper than that cannot be printed.
- **There are no symlinks and no hard links.** A file is in exactly one place.
  Aliases like `/bin -> /usr/bin` cannot be expressed, so the layout must be
  right the first time rather than papered over later.

One more, from the trust model rather than the filesystem: **everything
runnable is signed by the same key.** There is no third-party or
locally-compiled tier that is trusted less than the shipped one — `exec_path`
refuses anything the kernel's embedded key did not sign, wherever it sits. So
no directory in this layout may be read as conferring trust; where a program
sits says who may replace it, never whether it may run. See §2 `/Shared`.

---

## 2. The tree

```
/
├── System/      SYSTEM, 0755 — the OS and system programs
├── Shared/      SYSTEM, 0775 — shared between users; programs, common files
├── User/        SYSTEM, 0755 — one directory per human user
├── Temp/        SYSTEM, 0777 — scratch
└── Volumes/     SYSTEM, 0755 — where other disks attach (see section 3)
```

Capitalised to match `/System`, which already exists on disk. CXFS names are
case-insensitive and case-preserving, so `cd /system` finds it either way; the
capitalisation only decides what `ls` prints.

### `/System`

The OS and the programs that are part of it. SYSTEM-owned, not user-writable,
and the unit an update replaces wholesale.

| Path | Holds |
|------|-------|
| `/System/Boot.xoex` | the executive the kernel launches (`cxk_launch_executive`) |
| `/System/*.xcex` | system programs |
| `/System/Drivers/` | `.xkdr` / `.xklo`, when module loading lands |

`Drivers/` is listed but **not created until there is a driver to put in it.**
An empty directory costs a manifest entry and tells a reader something exists
that does not.

### `/Shared`

Files shared between users, including programs, with none of `/System`'s
protection. Group-writable rather than SYSTEM-only, so a user can install
something here without being SYSTEM.

This is a split by **protection**, not by trust, and that distinction is what
makes it correct here. An earlier draft of this document argued for a single
program directory on the grounds that every runnable image is signed by the
same key, so a `/bin` versus `/usr/local/bin` provenance tier would be
meaningless. That argument holds — and misses the point. The real difference
between these two directories is *who may write to them*, which CXFS models
directly with `owner_uid` and the permission bits. `/System` is the OS and only
SYSTEM changes it; `/Shared` is where users put things for each other. Signing
still governs what may *run*; permissions govern what may be *placed*.

**Search order is `/System` first, then `/Shared`.** A user-writable directory
must not be able to shadow a system program: if `/Shared` were searched first,
dropping `/Shared/edit.xcex` would silently replace the system `edit` for
everyone. Both are signed, so neither is untrusted — but "signed" does not mean
"the one the user meant", and the shadowing would be invisible.

### `/User`

One directory per human user, named by account name, owned by that UID, mode
`0700`. `uid.h` establishes UID 0 as SYSTEM and humans as UID >= 1, and notes
the account layer does not exist yet — so there are no entries here until it
does, and the directory is the placeholder for the convention.

A user's files live directly in their directory. No `Documents/`, `Desktop/`
and so on imposed from above: those are the user's to create, and pre-creating
them spends manifest entries on someone else's taste.

### `/Temp`

Scratch, world-writable. Not preserved across boots by policy, though nothing
currently clears it — noted so the first thing that relies on it knows the
guarantee is aspirational.

---

## 3. Other disks

**Extra volumes attach at `/Volumes/<label>`**, where the label is the
partition name (12 characters, `struct partition.name`) or, failing that, the
disk's assigned name (`HDD0`, `EXT-CDROM0`, `struct disk.name`).

Why a directory in the tree rather than a second syntax like `HDD1:/path`: one
mount point costs one extra path component, which is one extra manifest scan,
and nothing else in the system has to learn a new way to spell a path. Every
existing caller, `cxfs_resolve` included, keeps working unchanged. A volume's
own contents are its business — this layout describes the boot volume and
imposes nothing on a data disk.

### 3.1 What this needs first, honestly

**CXFS cannot currently mount two volumes at once.** Every piece of mount state
in `cxfs.c` is a single static: one `sb`, one `mounted`, one `cxfs_id`, one
`bitmap_cache`, one `fs_base_lba`, one `fs_sectors_per_block`. `cxfs_mount_at`
replaces them, so mounting a second volume unmounts the first. `/Volumes` is
therefore a destination, not a description of anything that works today.

Three things stand between here and there, in increasing order of how invasive
they are:

1. **Per-volume state.** The six statics become a table of mounted volumes.
   Mechanical.
2. **Crossing a mount point.** `cxfs_resolve` has to notice that an entry is a
   mount root and continue the walk in another volume. Contained, since
   resolution is already one function.
3. **An entry id must become `(volume, id)`.** This is the one with reach.
   Today `cxfs_resolve` returns a bare `int`, `struct thread.cwd` is a bare
   `uint32_t`, `file_stat.id` is a `uint32_t`, and the open-file table holds a
   bare entry id. Every one of those silently means "id on *the* volume". They
   all have to carry which volume, and `file_stat.id` is ABI, so userspace sees
   the change too.

Until that work is done there is a cheaper thing that already works:
`cxfs_set_id()` switches which volume is the active one, wholesale. A `mount`
command built on it would behave like a DOS drive letter — one volume visible
at a time, switched explicitly — which is honest about the limitation and
commits to no path syntax that step 3 would have to undo.

---

## 4. Rules

1. **A program is only runnable if signed.** Location grants nothing:
   `/System/x.xcex` and `/Temp/x.xcex` are equally subject to verification.
   The layout is organisation, never authority.
2. **Depth ≤ 4 from the root**, well inside the 16-level cap, so paths stay
   printable and resolution stays cheap.
3. **Create on need, not on principle.** A directory in this document is not
   created until something is put in it.
4. **`/System` is SYSTEM's.** A user process writing there requires UID 0,
   which the CXFS permission check already enforces.
5. **Program search order is `/System`, then `/Shared`** — never the reverse,
   so a user-writable directory cannot shadow a system program.

---

## 5. Known divergence: the trust anchor

`CX_EXTENSION_SYSTEM.md` §10.4 describes the kernel embedding the *SHA-256
fingerprint* of the trusted public key and reading `/System/<key>.xkpk` at
startup, accepting it only if the hash matches. §10.6 lists "the `/System`
protected area to hold `.xkpk`" as a dependency of signing.

**That is not what the code does.** `trusted_key.c` embeds the entire 272-byte
public key into the kernel image, and nothing ever reads a `.xkpk` from disk;
`cxex_verify.c` hashes the *embedded* key and compares that against the
signature's fingerprint. The effect is stronger than documented — there is no
key on writable storage to swap in the first place — but it means `/System`
holds no key today, and the dependency §10.6 lists was never needed.

Recorded here rather than silently designed around: either the doc should be
corrected to match the implementation, or the on-disk key should be added if
it was wanted for key rotation. This layout assumes the former.
