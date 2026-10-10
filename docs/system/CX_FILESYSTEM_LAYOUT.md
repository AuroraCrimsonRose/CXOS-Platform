# CXOS Filesystem Layout

**CATX Systems — Aurora Tejeda**
Status: **PROPOSAL — for review.** Nothing depends on this yet.

Where things live on a mounted CXFS volume. `CXFS_FILESYSTEM.md` defines the
on-disk *format*; this defines the *tree* built on top of it. Until now the
only documented path was `/System`, named in `CX_EXTENSION_SYSTEM.md` §10 as
the protected area holding `.xkpk`, and the install path created it and put
`Boot.xoex` in it. Everything else got invented per caller, which is how two
callers end up disagreeing.

---

## 1. The tree

```
/
├── System/                 SYSTEM, 0755 — the OS. Only SYSTEM writes here.
│   ├── Programs/           system programs (.xuex)
│   ├── Kernel/             kernel images — see §4
│   ├── Boot/               boot chain    — see §4
│   ├── Drivers/            .xkdr / .xklo, when module loading lands
│   ├── Fonts/              console bitmap fonts (.xfnt)
│   └── Temp/               scratch belonging to the OS
│
├── Shared/                 SYSTEM, 0775 — shared between users, user-writable
│   ├── Programs/           programs anyone may install
│   ├── Documents/
│   ├── Fonts/              user-installed console fonts (.xfnt)
│   ├── Pictures/
│   ├── Audio/
│   └── Videos/
│
├── User/                   SYSTEM, 0755
│   └── <username>/         owned by that UID, 0700
│       ├── Documents/
│       └── …               the user's own; nothing imposed beyond the first
│
├── Temp/                   SYSTEM, 0777 — user-facing scratch
│
└── Drives/                 SYSTEM, 0755 — other drives attach here (§3)
```

Capitalised to match `/System`, which already exists on disk. CXFS names are
case-insensitive and case-preserving, so `cd /system/programs` finds it either
way; capitalisation only decides what `ls` prints.

**`/System` vs `/Shared` is a split by protection, not by trust.** Every
runnable image is signed by the same key — `exec_path` refuses anything else
wherever it sits — so no directory here confers the right to run. What differs
is *who may write*: `/System` is the OS and only SYSTEM changes it, `/Shared`
is where users put things for each other, and CXFS models that directly with
`owner_uid` and the permission bits. Signing governs what may **run**;
permissions govern what may be **placed**.

**Program search order is `/System/Programs`, then `/Shared/Programs`** — never
the reverse. Both are signed, so neither is untrusted, but a user-writable
directory searched first could shadow a system program invisibly, and "signed"
does not mean "the one the user meant".

`/System/Temp` and `/Temp` are deliberately separate: the OS should not have to
compete for space, or contend for names, with whatever a user program is doing.

**Font search order is `/System/Fonts`, then `/Shared/Fonts`** — never the
reverse, and for the reason given above for programs. `/System/Fonts` is the
OS's own and only SYSTEM writes it; `/Shared/Fonts` is group-writable, where a
user installs their own — which the graphical shell will want far more than the
text console does. Neither is signed, because a font is not executable, so the
difference between them is entirely *who may write*, and a user-writable
directory searched first could shadow a system font invisibly. A font decides
what every kernel message **looks like**, which is what makes that worth
guarding rather than a matter of taste.

The search stops at the first directory holding the **name**. A broken font in
`/System/Fonts` is an error, not a reason to fall through and quietly use a
different font of the same name from `/Shared`.

`SYS_FB_OP`'s `FB_OP_SET_FONT` takes a **name**, never a path, and composes
`<dir>/<name>.xfnt` itself — a name carrying `/`, `\` or `.` is refused, so the
capability to change the console font cannot become a capability to make the
kernel read an arbitrary file. Dropping `.` is what means `..` needs no case of
its own.

Selecting a font does not survive a reboot yet, and nothing is loaded from here
at boot: the compiled-in 8x16 font is what the console starts with, so a missing
or broken font can never cost you a console to read the error on.

---

## 2. What this costs, measured

Two things about CXFS shape the layout. The first turns out not to matter at
this scale; the second matters more than expected, and is the reason for §2.1.

**Manifest entries are finite but not scarce here.** The manifest is a fixed
1024 entries (`CXFS_MAX_ENTRIES`) and every directory permanently spends one.
The tree above is about 15 directories — **1.5% of the volume's total entry
count**. An earlier draft of this document argued against structure on the
grounds that "speculative directories are not free"; at fifteen of them that
was simply wrong, and the structure costs nothing worth counting.

**Depth is expensive, and more than it looks.** A directory has no data blocks:
`cxfs_find_in_dir` derives membership by scanning the *whole manifest* for
entries with a matching `parent_id`. The manifest is 64 blocks (1024 × 256 B =
256 KB), so **one path component costs up to 64 block reads**, and
`cxfs_resolve` does that per component:

| Path | Components | Worst-case reads |
|------|-----------:|-----------------:|
| `/System/Boot.xoex` | 2 | 128 blocks — **512 KB** |
| `/Shared/Documents/notes.txt` | 3 | 192 blocks — **768 KB** |
| `/User/aurora/Documents/notes.txt` | 4 | 256 blocks — **1 MB** |

That is disk I/O to resolve a single name, and it is **already happening**: the
two-component paths in use today cost half a megabyte each. The deeper tree
makes an existing problem more visible; it does not create it.

`cxfs_path_of` also caps depth at 16 levels, which this layout is nowhere near.

### 2.1 The fix is a manifest cache, not a flatter tree

The driver already solves exactly this problem for the allocation bitmap:
`bitmap_cache` is loaded into RAM once at mount, every test and set happens in
memory, and only changed blocks are written back. The comment on it says the
per-bit disk reads "was making allocation and free-counting take seconds".

The manifest has the same access pattern and none of the cache. 256 KB is an
affordable resident cost — the bitmap cache is already 8 KB, and the target
machine has gigabytes — and it would turn every lookup above into a RAM scan,
making depth nearly free and this layout cheap to use.

**So the sequencing is: cache the manifest first, then adopt this tree.** Not
because the tree is wrong, but because adopting it without the cache would
quadruple an I/O cost that is already too high, and it would look like the
layout's fault.

**Measured, over the self-test suite** (create, resolve, readdir, read, write,
truncate, rename, delete), on the same build with one line changed:

| | Block reads |
|---|---:|
| Manifest uncached | 1728 |
| Manifest cached | **193** |

**9.0x fewer.** The cache landed first, so the tree above is now affordable.

---

## 3. Other drives

A **drive** is hardware — a disk the machine has. A **volume** is a filesystem
*on* a drive, either filling it or sitting in one of its partitions. The tree
says so:

```
/Drives/
├── HDD1/                   one directory per drive, named by the disk registry
│   ├── Data/               a volume on that drive, named by its own label
│   └── Scratch/            a second volume on the same drive
└── EXT-CDROM0/             a drive with no CXFS volume on it: present, empty
```

So a file reads `/Drives/HDD1/Data/notes.txt` — drive, then volume, then path.

**Why two levels and not one.** Hanging every volume directly off a single
directory is one component shorter and breaks the first time two drives each
hold a volume called `Data`: the second has nowhere to go. Naming the drive
first also means an unmountable drive still appears — an empty `/Drives/HDD1`
says "this drive is here and has no CXFS volume on it", which is more use to
someone looking than no entry at all.

A directory in the tree rather than a second syntax like `HDD1:/path`: a mount
costs path components and nothing in the system has to learn a new way to spell
a path. A volume's own contents are its business — this document describes the
boot volume and imposes nothing on a data drive.

**The volume's name comes from the volume**, out of a `label` field in its
superblock, falling back to a generated `VolumeN` for a volume formatted before
that field existed. The label was carved out of the superblock's padding, which
was already zeroed, so an older volume reads an empty label rather than
garbage.

### 3.1 How it works

Three pieces, and none of them is special-cased to `/Drives`:

1. **Per-volume state.** Everything describing one mounted filesystem — disk
   id, base LBA, geometry, superblock, caches — is a `cxfs_volume`, and a
   `vol` pointer selects the one being operated on. Volume 0 is the root
   volume, the one the system booted from, the one `/` means. It is the only
   one given the 256KB manifest cache; the others fall back to reading the
   manifest off the disk, which the cache was always written to allow.

2. **Crossing a mount point.** Any directory can have a volume mounted on it.
   `cross_mount` turns that directory's id into the mounted volume's root
   during a path walk, and `uncross_mount` does the reverse so `..` and
   `cxfs_path_of` can step back out. `/Drives` is where the system happens to
   put them and nothing more.

3. **An entry id carries its volume**: `(volume << 24) | index`. The index is
   a manifest slot, bounded by `manifest_count` — 1024 today — so 24 bits is
   far more room than the format can use.

   This was expected to be an ABI break and is not. The root volume is 0, so
   every root id is numerically identical to the untagged id it was before:
   `file_stat.id` keeps its width and meaning, `0` still means the root, and
   nothing already written down changes value.

   On disk, `id` and `parent_id` inside a `cxfs_entry` stay volume-**local**. A
   disk must not record where it happens to be mounted, or moving it to another
   slot would point every reference on it at the wrong volume. The tag goes on
   in `cxfs_read_entry` and comes off in `cxfs_write_entry`, so that pair is the
   whole boundary.

Cross-volume operations are refused rather than approximated: `cxfs_move` and
`cxfs_write_entry` both reject a parent on another volume, because a parent
pointer is a manifest index and one volume's index 2 has nothing to do with
another's. Moving a file between volumes means copying its blocks and deleting
the original — a caller's job, not the driver's.

---

## 4. Open question: `/System/Kernel` and `/System/Boot`

**The kernel and the boot chain are not files in CXFS today.** They live in a
raw partition: `PART_TYPE_CXBOOT` (0xCB) is documented in `partition.h` as
"raw boot area: stage2 + kernel.xkex", written by `cxk image` at build time.
Stage 2 finds the kernel by LBA, not by path — it has no CXFS reader.

So these two directories cannot hold the *running* kernel without stage 2
learning to read CXFS, which is a substantial change to the earliest and most
constrained part of the boot path. What they can hold, cheaply, is the
**update-staging** copy: a new `kernel.xkex` written to `/System/Kernel/` by an
updater and copied into the boot partition on the next boot.
`CX_EXTENSION_SYSTEM.md` §10.4 gestures at this — it notes signature checking
is "very useful with in-place kernel updates" — so the intent seems to exist.

Which of the two is meant changes what gets built, so it is left open rather
than guessed.

---

## 5. Rules

1. **A program is only runnable if signed.** Location grants nothing:
   `/System/Programs/x.xuex` and `/Temp/x.xuex` are equally subject to
   verification. The layout is organisation, never authority.
2. **Create on need, not on principle.** A directory listed here is not created
   until something goes in it. `Drivers/` waits for a driver.
3. **`/System` is SYSTEM's.** A user process writing there requires UID 0,
   which the CXFS permission check already enforces.
4. **Search `/System/Programs` before `/Shared/Programs`**, never the reverse.
5. **Depth stays ≤ 4 from the root**, well inside `cxfs_path_of`'s 16-level cap.

---

## 6. Known divergence: the trust anchor

`CX_EXTENSION_SYSTEM.md` §10.4 describes the kernel embedding the *SHA-256
fingerprint* of the trusted public key and reading `/System/<key>.xkpk` at
startup, accepting it only if the hash matches. §10.6 lists "the `/System`
protected area to hold `.xkpk`" as a dependency of signing.

**That is not what the code does.** `trusted_key.c` embeds the entire 272-byte
public key into the kernel image, and nothing ever reads a `.xkpk` from disk;
`cxex_verify.c` hashes the *embedded* key and compares it against the
signature's fingerprint. The effect is stronger than documented — there is no
key on writable storage to swap — but it means `/System` holds no key today.

Either the doc should be corrected to match the implementation, or the on-disk
key added if it was wanted for key rotation. This layout assumes the former.
