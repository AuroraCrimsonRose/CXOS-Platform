# CXFS — The CXOS Filesystem (v1 Standard)
### CXK Reference — Aurora Tejeda / CATX SYSTEMS LLC

This document specifies the on-disk format of **CXFS version 1**, the native
filesystem of CXK. It is intended as a reference for anyone reading or writing
CXFS volumes.

> **Status:** v1 is implemented and in use. Several entry fields (ownership,
> group, permissions, locks) are **reserved** in v1 — they exist on disk but
> are zeroed and not enforced until v2, once CXK has a user/process model.

---

## 1. Design Overview

CXFS is a **manifest-based** filesystem. The core idea:

- The on-disk **manifest** — a flat table of fixed-size entries — is the single
  source of truth for everything on the volume.
- The directory tree is **derived**, not stored as nested structures: each entry
  records its `parent_id`, and the hierarchy is reconstructed from those links.
- **Moving** a file or directory changes only its `parent_id`. The file's data
  blocks never move.

File contents are stored using **extents** (contiguous runs of blocks) rather
than per-block linked lists, which keeps small files fast and simple.

Names are **case-preserving but case-insensitive**: `Readme.txt` and
`readme.txt` are the same name, but the original casing is retained.

---

## 2. Volume Layout

A CXFS volume is divided into 512-byte blocks, laid out in this order:

```
+-----------+  block 0
| Superblock|              master header (one block)
+-----------+  bitmap_start
| Bitmap    |              allocation bitmap (bitmap_blocks blocks)
+-----------+  manifest_start
| Manifest  |              entry table (manifest_blocks blocks)
+-----------+  data_start
| Data      |              file content blocks (extents allocate from here)
|   ...     |
+-----------+
```

The superblock records the start block and size of each region, so a reader
locates everything from block 0.

| Constant | Value | Meaning |
|----------|-------|---------|
| Magic | `0x43584653` (`"CXFS"`) | identifies a CXFS volume |
| Version | `1` | this specification |
| Block size | `512` bytes | fixed in v1 |
| Name length | `64` bytes | max filename, NUL-padded |
| Max extents | `4` | per file in v1 |

---

## 3. Superblock (block 0)

The superblock is exactly one 512-byte block. All multi-byte integers are
little-endian (native x86).

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0  | 4 | `magic` | `0x43584653` (`"CXFS"`) |
| 4  | 2 | `version` | format version (`1`) |
| 6  | 2 | `block_size` | bytes per block (`512`) |
| 8  | 4 | `total_blocks` | total blocks on the volume |
| 12 | 4 | `bitmap_start` | first block of the allocation bitmap |
| 16 | 4 | `bitmap_blocks` | blocks occupied by the bitmap |
| 20 | 4 | `manifest_start` | first block of the manifest table |
| 24 | 4 | `manifest_blocks` | blocks occupied by the manifest |
| 28 | 4 | `manifest_count` | maximum number of manifest entries |
| 32 | 4 | `data_start` | first data block |
| 36 | 4 | `reserved_blocks` | system-reserved data blocks (full-disk safety) |
| 40 | 4 | `root_id` | manifest id of the root directory |
| 44 | 468 | `pad` | zero padding to fill the block |

To **mount** a volume: read block 0, verify `magic` and `version`.

---

## 4. Allocation Bitmap

A bitmap of data-block usage, starting at `bitmap_start`. Each bit represents one
data block: `1` = allocated, `0` = free. Block `0` (the superblock) is never a
valid data block, so a returned data-block number of `0` always means
"allocation failed."

`reserved_blocks` data blocks are held back as a safety margin so the volume
cannot be filled completely.

---

## 5. Manifest Entries

The manifest is an array of fixed **128-byte** entries beginning at
`manifest_start`. An entry's `id` is its index in this array. Entry types:

| Type | Value | Meaning |
|------|-------|---------|
| `CXFS_TYPE_FREE` | 0 | unused slot |
| `CXFS_TYPE_FILE` | 1 | regular file |
| `CXFS_TYPE_DIR`  | 2 | directory |

### Entry structure (128 bytes)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0   | 4  | `id` | this entry's id (its index in the table) |
| 4   | 4  | `parent_id` | id of the containing directory (move = change this) |
| 8   | 1  | `type` | `CXFS_TYPE_*` |
| 9   | 1  | `flags` | reserved misc flags |
| 10  | 2  | `pad0` | padding |
| 12  | 64 | `name` | filename, case-preserved, NUL-padded |
| 76  | 4  | `size` | file size in bytes |
| 80  | 16 | `extent_start[4]` | start block of each of up to 4 extents |
| 96  | 16 | `extent_len[4]` | length (in blocks) of each extent |
| 112 | 4  | `owner_uid` | **reserved (v2)** — 0 = SYSTEM for now |
| 116 | 4  | `group_id` | **reserved (v2)** |
| 120 | 2  | `permissions` | **reserved (v2)** |
| 122 | 1  | `lock_state` | **reserved (v2)** — 0 = unlocked |
| 123 | 1  | `reserved_pad` | reserved |
| 124 | 4  | `pad1` | padding to 128 bytes |

The **root directory** is the entry whose id equals the superblock's `root_id`;
its `parent_id` refers to itself (it has no parent).

---

## 6. File Content (Extents)

A file's data is stored as up to **4 extents**, each a contiguous run of data
blocks described by an `(extent_start, extent_len)` pair. To read a file, walk
its extents in order until `size` bytes have been consumed. Unused extents have
`extent_len = 0`.

Because v1 caps a file at 4 extents, a file that cannot be represented in 4
contiguous runs (severe fragmentation, or simply too large) will fail to write.
This is a known v1 limitation; later versions may add indirect extents or a
larger extent count.

---

## 7. Names

- Maximum 64 bytes, NUL-padded.
- Case-insensitive for lookups, but the original casing is preserved on disk.
- Spaces are normalized to underscores (`_`) on creation.
- `/` and control characters are illegal; an empty name is invalid.

---

## 8. Path Resolution

Paths may be **absolute** (`/a/b/c`) or **relative** to a current directory.
`.` (current) and `..` (parent) are supported. Resolution walks the manifest by
matching each path component against the children of the current directory
(entries whose `parent_id` equals the current directory's id).

---

## 9. Reserved for v2

These exist on disk now (zeroed) and will be enforced once CXK has a
user/process model:

- `owner_uid`, `group_id` — ownership
- `permissions` — access control
- `lock_state` — file locking

Until then, all entries are owned by SYSTEM (uid 0), unlocked, and unrestricted.

---

## 10. Implementation Notes

- CXFS operates on a block device through the unified **disk registry**, so a
  CXFS volume can live on any registered disk (ATA, AHCI, or USB), selected by
  disk id at runtime.
- The default target is ATA drive 1 (the filesystem disk) in emulators; on real
  hardware the volume may be on another channel and is selected at runtime.
- All integers are little-endian.

---

*CXFS v1 — the native filesystem of CXK. This document describes the v1 on-disk
format. Reserved fields are part of the format but inert until v2.*