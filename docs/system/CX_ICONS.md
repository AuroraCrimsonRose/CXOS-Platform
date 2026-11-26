# Icons
### CX Design Spec: Aurora Tejeda / CATX Systems

> **Status: DECIDED 2026-11-11; the set chosen 2026-11-25: IBM's Carbon. Nothing built.** An open set for the UI's
> symbols, CXOS's own drawing for what makes it recognisable, and one icon
> format of our own that the OS reads. Built with the toolkit (roadmap 2.8a
> and 2.8a′).

---

## 1. Decisions

| Decision | Reason |
|---|---|
| **UI symbols come from an open set: IBM's Carbon icons** (Apache-2.0), vendored unmodified under `vendor/` (Aurora, 2026-11-25, after a sheet of Carbon and Phosphor side by side). | A toolkit needs hundreds of symbols, drawn alike; that is a specialist's years, as with fonts (`CX_FONTS.md`). Carbon is drawn to go with IBM Plex, the UI and terminal face, so symbols and text read as one design. Other sets may ship later as choices; never mixed in one theme. |
| **Icon licences: permissive only** (MIT, ISC, Apache-2.0, CC0), each with its row in `THIRD-PARTY-NOTICES.md`, Apache's NOTICE carried with it. | Fonts are OFL-only; icons are mostly not OFL. Share-alike sets (GPL Papirus, CC-BY-SA Adwaita, LGPL Breeze) are left out. |
| **CXOS draws its own:** the logo, the system apps' icons, the document and folder shapes, cursors, and the few dozen icons seen all the time. | Where the system's look lives, and a bounded amount of drawing. |
| **No third-party brand logos as file icons** (Python's, Rust's and the like). | They are trademarks whatever an icon set's licence says. |
| **The OS reads only our own format,** never SVG. | SVG is a large parsing surface (scripts, external references, filters). The host converts a safe subset with `cxk`; the OS reads fixed records through a checker, the trust boundary, as for fonts. |

**Why Carbon over Phosphor** (2026-11-25). Phosphor's six weights were
the case for it; Aurora preferred Carbon's drawings, and what the weights
would have bought is covered otherwise:

- **State** (a set favourite, unread notifications, a locked item): Carbon's
  146 `--filled` variants (`star--filled`, `notification--filled`,
  `locked--filled`, `checkmark--filled`, ...), each its own named glyph.
- **High contrast:** icons drawn a size larger, or emboldened by the
  rasteriser (outlines offset outward, as FreeType's embolden); which one is
  decided with the high-contrast theme's icons (`CX_ACCESSIBILITY.md`).
- **Thin, light and duotone** have no use planned in the toolkit; colour
  icons are CXOS's own (§2).

Carbon is drawn on a 32 px grid in one weight; 68 icons have hand-tuned
16 px versions and 9 have 20 px versions, which ship beside the scalable
one (§2). It is published as SVG only, with cubic curves, so the converter
needs a cubic-to-quadratic step on the host (the same one CFF fonts need).
Apache-2.0's NOTICE travels with it.

## 2. The format

- **Symbolic icons** (the UI's symbols): single-colour outlines, tinted by
  the theme. They are XFNT v2 glyphs named instead of numbered, so they share
  the font rasteriser, its glyph cache and its checker. A filled variant is
  a glyph of its own name.
- **Colour icons** (apps, file types, folders): a short list of filled
  layers, each a solid colour or a linear or radial gradient. Strokes are
  expanded into fills on the host, so the OS only fills.
- **Small sizes:** an icon may carry a hand-tuned 16 px version beside the
  scalable one (Carbon's own, where it has one); the scalable one is used
  everywhere else.
- **Named by meaning** (`document-save`, not a file name), with a name and a
  description for the accessibility tree (roadmap 2.8a, goal 4).
  Freedesktop's icon naming spec is the precedent for the names; a table
  maps each to its Carbon icon (`save`, `trash-can`, ...).
- **Survey first:** Haiku's HVIF, its own compact vector icon format
  (`src/libs/icon`), before the colour layer format is fixed.

## 3. File types

- **A template, generated on the host:** one document shape drawn for CXOS, a
  colour per category (document, image, audio, video, archive, code, font,
  disk image, program), the category's Carbon glyph on it, and the
  extension (`PDF`, `RS`) drawn at runtime in the UI face. A new type is a
  row in a table; every extension works; the label is sharp at any size.
- **System files** get their own drawn icons, not the template: programs
  (`.xuex`, `.xsex`, with the trust badge below), scripts (`.xssh`,
  `.xush`), kernel packages (`.xkpk`), fonts (`.xfnt`), CXFS disk images,
  and the boot and configuration files, each marked as CXOS's own.
- **Placeholders until they are drawn** (Aurora, 2026-11-11): the template
  works on day one with no drawing at all, since the host generates it. A
  plain rounded document shape in the category's colour, the extension in
  the UI face, and a Carbon file glyph; system files use the same with a
  CXOS mark; app icons are a rounded square in a colour derived from the
  app's name with its first letter. Each placeholder is replaced by name,
  so no app or file changes when the real drawing lands.
- **Previews** replace the icon at larger sizes for images, video, fonts and
  documents, made by the isolated decoders (roadmap Phase B, media): no
  thumbnail is parsed inside the file manager.
- **A signed program shows its trust:** a `.xuex` or `.xsex` carries a badge
  for what verification found (`PROC_TRUST_PLATFORM`, `_PUBLISHER`,
  `_UNVERIFIED`, `_UNSIGNED`), so an unsigned program never looks like a
  trusted one.
- **Folders:** CXOS's folder shape, with a Carbon glyph on the known
  folders (Documents, Pictures, ...; roadmap Phase 3, the known-folders API).
- **A file's type comes from the type registry** (Phase 3: CXFS v3
  attributes, then the extension). No privileged process reads a file's
  contents to guess it.

## 4. Order

1. Symbolic icons with 2.8a′5 (the toolkit needs them; they cost the font
   rasteriser plus a name table): Carbon vendored and converted (SVG paths,
   cubics to quadratics, on the host), the name table, the checker,
   sabotages that feed it bad files.
2. Colour icons and CXOS's own drawing with the first system apps: the
   layer format, the document and folder template, the program trust badge.
3. File-type lookup and previews with Phase 3's registry and Phase B's
   decoders.
