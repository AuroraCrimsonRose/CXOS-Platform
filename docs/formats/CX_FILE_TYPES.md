# File types: what changes
### CX Design Spec: Aurora Tejeda / CATX Systems

> **Status: PLAN.** The scheme as built is in
> `docs/x86_32/formats/CX_EXTENSION_SYSTEM.md`, `CX_EXTENSION_NAMING.md`,
> `CX_FILE_STRUCTURE.md` and `CX_KEY_FORMAT.md`, all still the reference. This
> document records what changes and why.

---

## 1. Kept

The extension is a hint and the header decides what a file is. Windows
decides what runs by extension, which is how `invoice.pdf.exe` deceives people;
CXOS does not. The ownership ladder (`.xkex`, `.xbex`, `.xoex`, `.xsex`,
`.xuex`) and the `x` + domain + type formula stay.

## 2. Changes

1. **The UI shows the real type and the signer**, read from the header, never
   just the name.
2. **A type registry with a hierarchy**, like macOS's type identifiers:
   `.xuex` is a kind of CXEX, which is a kind of executable. Packages declare
   the types they handle; "open with" choices live in each user's layered
   settings. Foreign files are identified by content (Haiku's sniffer rules),
   not by name.
3. **Standard formats keep standard extensions.** `.png` stays `.png`. The `x`
   codes are only for CX's own formats; the UI shows friendly names instead of
   four-letter codes.
4. **One header prefix for every binary CX format** (magic, type, version,
   size, flags), so any tool can identify any CX file. X Data documents gain an
   optional schema line so they can be validated.
5. **The CXEX architecture code gains x86-64**, and the loader refuses an image
   for another architecture.
6. **Attributes in CXFS v3:** an origin marker on downloaded files, and indexed
   attributes for live queries (`system/CX_FILESYSTEM.md` §4).
