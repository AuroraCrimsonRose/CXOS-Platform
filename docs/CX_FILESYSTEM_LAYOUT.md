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
refuses anything the kernel's embedded key did not sign, wherever it sits. The
Unix split between `/bin` and `/usr/local/bin` encodes a trust and provenance
distinction that does not exist here, so reproducing it would be cargo cult.

---

## 2. The tree

```
/
├── System/      SYSTEM, 0755 — the OS itself
├── Programs/    SYSTEM, 0755 — every runnable program
├── Users/       SYSTEM, 0755 — one directory per human user
└── Temp/        SYSTEM, 0777 — scratch
```

Four top-level directories, each one manifest entry. Capitalised to match
`/System`, which already exists.

### `/System`

The OS. Owned by SYSTEM, not writable by a user, and the unit an update
replaces wholesale.

| Path | Holds |
|------|-------|
| `/System/Boot.xoex` | the executive the kernel launches (`cxk_launch_executive`) |
| `/System/*.xkpk` | the trusted public key, *if* §10.4's on-disk anchor is ever implemented — see §4 |
| `/System/Drivers/` | `.xkdr` / `.xklo`, when module loading lands |

`Drivers/` is listed but **not created until there is a driver to put in it.**
An empty directory costs a manifest entry and tells a reader something exists
that does not.

### `/Programs`

Every runnable `.xcex`. This is the search path: a bare `run foo` resolves
`/Programs/foo.xcex`.

One directory rather than a system/installed split, because the signing model
gives no meaning to the distinction — see §1. If a provenance tier ever does
appear (a second trusted key, or user-signed builds), *that* is when a split
earns its place, and it should be a sibling (`/Programs.local`) rather than a
nested one, to keep resolution one scan deep.

### `/Users`

One directory per human user, named by account name, owned by that UID, mode
`0700`. `uid.h` establishes that UID 0 is SYSTEM and humans are UID ≥ 1, and
notes the account layer does not exist yet — so until it does, there are no
entries here and the directory itself is the placeholder for the convention.

A user's own files live directly in their directory. No `Documents/`,
`Desktop/` and so on imposed from above: those are the user's to create, and
pre-creating them would spend manifest entries on someone else's taste.

### `/Temp`

Scratch, world-writable. Not preserved across boots by policy, though nothing
currently clears it — noted so the first thing that depends on it knows the
guarantee is aspirational.

---

## 3. Rules

1. **A program is only runnable if signed.** Location grants nothing:
   `/Programs/x.xcex` and `/Temp/x.xcex` are equally subject to verification.
   The layout is organisation, never authority.
2. **Depth ≤ 4 from the root**, well inside the 16-level cap, so paths stay
   printable and resolution stays cheap.
3. **Create on need, not on principle.** A directory in this document is not
   created until something is put in it.
4. **`/System` is SYSTEM's.** A user process writing there requires UID 0,
   which the CXFS permission check already enforces.

---

## 4. Known divergence: the trust anchor

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
