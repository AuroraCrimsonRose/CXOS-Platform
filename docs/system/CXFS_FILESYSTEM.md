# CXFS — CX File System (v2 Specification)

**CATX Systems — Aurora Tejeda**
On-disk format specification for CXFS version 2.

CXFS is the native filesystem of CXOS. This document defines the **v2** on-disk
layout: the byte-exact structures a conforming reader/writer must produce and
interpret. v2 is a clean break from v1 (no migration path is provided or
required); existing v1 volumes must be reformatted.

---

## 1. Design goals (v2)

- **4 KB block size**, page-aligned, so a filesystem block equals one CPU page
  (clean for future memory-mapped files). The block size is **stored in the
  superblock**, not hardcoded — a future large-volume profile (**CXFSX**) is the
  same format with a larger block-size value, not a new filesystem.
- **Partition-aware.** All block I/O goes through a **base LBA offset** so a
  CXFS volume may live inside a partition (GPT now, others later) rather than
  owning the whole disk. `base_lba = 0` means whole-disk.
- **64-bit disk addressing** for placement (a volume can sit anywhere on a large
  disk), with **32-bit internal block numbers** — a practical ceiling of
  **16 TB** per volume at 4 KB blocks, keeping on-disk structures compact.
- **Ownership, permissions, timestamps, and advisory locking** as first-class
  per-entry metadata (the fields v1 reserved are now defined and used).
- **No backward compatibility with v1.** The version field gates this.

---

## 2. Units and conventions

- All multi-byte integers are **little-endian**.
- **Sector** = 512 bytes (the disk's native unit, via the block device layer).
- **Block** = the CXFS allocation unit = `block_size` bytes (4096 in the
  standard profile). One 4 KB block = 8 sectors.
- **Block numbers** are 32-bit, relative to the start of the volume (block 0 is
  the superblock). To reach the disk, a reader computes:

  ```
  disk_lba = base_lba + (block_number * (block_size / 512))
  ```

  where `base_lba` is the volume's partition offset (superblock field).
- **Entry id** = an index into the manifest table. Id 0 is the root directory.
- Structures are `__attribute__((packed))`; no implicit padding beyond the
  explicit pad fields shown.

---

## 3. Volume layout

A CXFS volume is a contiguous run of blocks, in this order:

```
 block 0            : Superblock
 bitmap_start ..    : Allocation bitmap (1 bit per block; 1 = used)
 manifest_start ..  : Manifest table (fixed array of 256-byte entries)
 data_start ..      : File/directory data blocks
```

All four regions are located by superblock fields, so the exact block numbers
are not fixed by this spec — only their order and the superblock that describes
them. Block 0 is always the superblock.

---

## 4. Superblock

Located at volume block 0 (disk LBA `base_lba`). One block in size; the defined
fields occupy the first portion and the remainder is reserved (zero-filled).

| Offset | Size | Field             | Notes                                            |
|-------:|-----:|-------------------|--------------------------------------------------|
| 0x00   | 4    | `magic`           | `0x43584653` (`"CXFS"`)                          |
| 0x04   | 2    | `version`         | `2` for this spec                                |
| 0x06   | 2    | `block_size`      | bytes per block; **4096** standard (authoritative) |
| 0x08   | 8    | `base_lba`        | partition offset in 512-byte sectors; 0 = whole disk |
| 0x10   | 4    | `total_blocks`    | total blocks in the volume                       |
| 0x14   | 4    | `bitmap_start`    | first block of the allocation bitmap             |
| 0x18   | 4    | `bitmap_blocks`   | blocks occupied by the bitmap                    |
| 0x1C   | 4    | `manifest_start`  | first block of the manifest table                |
| 0x20   | 4    | `manifest_blocks` | blocks occupied by the manifest                  |
| 0x24   | 4    | `manifest_count`  | number of entry slots in the manifest            |
| 0x28   | 4    | `data_start`      | first data block                                 |
| 0x2C   | 4    | `reserved_blocks` | data blocks held in reserve (full-disk safety)   |
| 0x30   | 4    | `root_id`         | manifest id of the root directory (always 0)     |
| 0x34   | 4    | `feature_flags`   | bitfield of active v2 features (see §7)          |
| 0x38   | 8    | `created`         | volume creation timestamp                        |
| 0x40   | 8    | `modified`        | last-modified (superblock write) timestamp       |
| 0x48   | 4    | `entry_size`      | bytes per manifest entry; **256** standard       |
| 0x4C   | …    | reserved          | zero-filled to the end of the block              |

Notes:
- `block_size` and `entry_size` are stored so future profiles (CXFSX, or larger
  entries) are described by the volume itself, never assumed by the reader.
- A reader **must** reject a volume whose `version` it does not implement, and
  **must not** write to a volume it cannot fully interpret.

---

## 5. Allocation bitmap

- One bit per block in the volume, LSB-first within each byte.
- Bit value `1` = block in use, `0` = free.
- Blocks for the superblock, bitmap, and manifest are marked used at format
  time. Data allocation never returns block 0.
- Size: `ceil(total_blocks / 8)` bytes, rounded up to whole blocks
  (`bitmap_blocks`).

---

## 6. Manifest entry (256 bytes)

The manifest is a fixed array of `manifest_count` entries, each `entry_size`
(256) bytes. Entry id = array index. A `type` of `CXFS_TYPE_FREE` (0) marks an
unused slot. Root is id 0 (a directory whose `parent_id` is itself).

| Offset | Size | Field            | Notes                                              |
|-------:|-----:|------------------|----------------------------------------------------|
| 0x00   | 4    | `id`             | this entry's id (== its index)                     |
| 0x04   | 4    | `parent_id`      | containing directory's id (move = change this)     |
| 0x08   | 1    | `type`           | 0 free, 1 file, 2 directory                        |
| 0x09   | 1    | `flags`          | misc flags (reserved)                              |
| 0x0A   | 1    | `name_len`       | length of `name` in bytes (0..63)                  |
| 0x0B   | 1    | `reserved0`      | alignment                                          |
| 0x0C   | 64   | `name`           | case-preserved filename, not NUL-required          |
| 0x4C   | 8    | `size`           | file size in bytes (64-bit; dirs use 0)            |
| 0x54   | 32   | `extent_start[8]`| 8 × uint32 extent start block numbers              |
| 0x74   | 32   | `extent_len[8]`  | 8 × uint32 extent lengths (in blocks)              |
| 0x94   | 4    | `owner_uid`      | owning user (0 = SYSTEM; users ≥ 1)                |
| 0x98   | 4    | `group_id`       | owning group (0 = system group)                    |
| 0x9C   | 2    | `permissions`    | permission bits (see §6.2)                         |
| 0x9E   | 1    | `lock_state`     | 0 unlocked, 1 advisory write-lock (see §6.3)       |
| 0x9F   | 1    | `lock_pad`       | alignment                                          |
| 0xA0   | 4    | `lock_owner_pid` | process holding the advisory lock (0 = none)       |
| 0xA4   | 8    | `created`        | creation timestamp                                 |
| 0xAC   | 8    | `modified`       | last-modified timestamp                            |
| 0xB4   | 8    | `accessed`       | last-accessed timestamp                            |
| 0xBC   | 68   | reserved         | zero-filled to 256 bytes (future metadata)         |

With 256-byte entries, **16 entries fit per 4 KB block**.

### 6.1 Extents

File content is stored as up to **8 extents**, each a `(start_block, length)`
run of contiguous blocks. Total capacity per file in v2 is therefore bounded by
8 contiguous runs; larger/fragmented files are a future extension (an indirect
extent block) and are out of scope for v2's first implementation. Directories
store no data via extents in v2 — directory membership is derived by scanning
the manifest for entries whose `parent_id` matches.

### 6.2 Permissions

`permissions` is a 16-bit field. The low 9 bits follow the familiar
owner/group/other × read/write/execute model:

```
 bit  8 7 6   5 4 3   2 1 0
      r w x   r w x   r w x
      owner   group   other
```

Upper bits (9..15) are reserved. **Enforcement** of permissions is a kernel
behavior layered on top of this format; the on-disk bits are defined here, the
checking logic is implemented in the driver. Until the OS has a full
user/session model, SYSTEM (uid 0) bypasses checks.

### 6.3 Advisory locking

`lock_state = 1` marks an **advisory** write lock: a cooperating writer sets it
(recording its pid in `lock_owner_pid`) to signal "in use — don't edit," and
other cooperating writers honor it. It is **advisory**, not enforced by the
hardware or paging — a non-cooperating writer is not prevented from writing. A
reader must treat a lock whose `lock_owner_pid` no longer corresponds to a live
process as stale (the driver clears stale locks). Range locks and richer lock
modes are reserved for a future revision (the field has headroom).

---

## 7. Feature flags

`feature_flags` in the superblock records which optional v2 behaviors a volume
was written with, so a reader can refuse a volume using features it lacks:

| Bit | Name              | Meaning                                       |
|----:|-------------------|-----------------------------------------------|
| 0   | `FEAT_TIMESTAMPS` | entry timestamps are maintained               |
| 1   | `FEAT_PERMS`      | ownership/permission bits are meaningful      |
| 2   | `FEAT_LOCKING`    | advisory locking is in use                    |
| 3   | `FEAT_LARGE_BLK`  | block_size > 4096 (CXFSX profile)             |
| 4.. | reserved          | must be 0                                     |

---

## 8. Timestamps

Timestamps are 64-bit counts of seconds since the CXOS epoch
(**1970-01-01T00:00:00Z**, matching Unix, sourced from the RTC). `created`,
`modified`, and `accessed` are maintained per entry when `FEAT_TIMESTAMPS` is
set. A value of 0 means "unknown / not set."

---

## 9. Capacity summary

| Quantity              | v2 limit                                  |
|-----------------------|-------------------------------------------|
| Block size            | 4 KB standard (field-defined; larger via CXFSX) |
| Internal block number | 32-bit                                    |
| Max volume size       | 2³² × 4 KB = **16 TB** (4 KB profile)     |
| Volume placement      | anywhere in a 64-bit LBA address space    |
| Max file name         | 63 bytes                                  |
| Extents per file      | 8                                         |
| Manifest entry size   | 256 bytes (16 per block)                  |

---

## 10. Differences from v1 (summary)

- Block size 512 → **4096**, and now a superblock field (`block_size`).
- Added `base_lba` (partition offset) and 64-bit volume placement.
- Manifest entry 128 → **256 bytes**; `entry_size` recorded in the superblock.
- File `size` 32-bit → **64-bit**; extents 4 → **8**.
- Activated the v1-reserved metadata: `owner_uid`, `group_id`, `permissions`,
  `lock_state`, plus new `lock_owner_pid` and `created`/`modified`/`accessed`
  timestamps.
- Added `feature_flags` and `entry_size` to the superblock.
- **No migration from v1** — reformat required.

---

## 11. Implementation notes (non-normative)

- The whole implementation - format logic (struct layouts, offset math,
  validation, name normalization) and driver (mount state, block I/O via the
  disk layer, allocation, directory ops) - lives in
  `kernel/drivers/storage/filesys/cxfs.{c,h}`. An earlier revision of this
  document split the format logic into `kernel/lib/format/kcxfs.{c,h}`; that
  file was never written, and `kernel/lib/format/` holds the CXEX container
  code instead.
- All driver block I/O must apply `base_lba` so a volume works identically
  whole-disk or within a partition.
- Disk writes are gated by the kernel's `CXK_ALLOW_DISK_WRITE` build switch;
  formatting is never automatic on a non-CXFS disk.