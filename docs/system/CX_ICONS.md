# Icons
### CX Design Spec: Aurora Tejeda / CATX Systems

> **Status: DECIDED 2026-11-11, nothing built.** An open set for the UI's
> symbols, CXOS's own drawing for what makes it recognisable, and one icon
> format of our own that the OS reads. Built with the toolkit (roadmap 2.8a
> and 2.8a′).

---

## 1. Decisions

| Decision | Reason |
|---|---|
| **UI symbols come from an open set: Phosphor** (MIT), vendored unmodified under `vendor/`. | A toolkit needs hundreds of symbols, drawn alike; that is a specialist's years, as with fonts (`CX_FONTS.md`). Phosphor has six styles (thin, light, regular, bold, fill, duotone), so an icon's weight can follow the text's (`CX_FONTS.md` §3 item 5, weight as an axis). Other sets may ship as choices. |
| **Icon licences: permissive only** (MIT, ISC, Apache-2.0, CC0), each with its row in `THIRD-PARTY-NOTICES.md`, Apache's NOTICE carried with it. | Fonts are OFL-only; icons are mostly not OFL. Share-alike sets (GPL Papirus, CC-BY-SA Adwaita, LGPL Breeze) are left out. |
| **CXOS draws its own:** the logo, the system apps' icons, the document and folder shapes, cursors, and the few dozen icons seen all the time. | Where the system's look lives, and a bounded amount of drawing. |
| **No third-party brand logos as file icons** (Python's, Rust's and the like). | They are trademarks whatever an icon set's licence says. |
| **The OS reads only our own format,** never SVG. | SVG is a large parsing surface (scripts, external references, filters). The host converts a safe subset with `cxk`; the OS reads fixed records through a checker, the trust boundary, as for fonts. |

## 2. The format

- **Symbolic icons** (the UI's symbols): single-colour outlines, tinted by
  the theme. They are XFNT v2 glyphs named instead of numbered, so they share
  the font rasteriser, its weight axis, its glyph cache and its checker. A
  Phosphor style is a master or an instance, as a font's weights are.
- **Colour icons** (apps, file types, folders): a short list of filled
  layers, each a solid colour or a linear or radial gradient. Strokes are
  expanded into fills on the host, so the OS only fills.
- **Small sizes:** an icon may carry a hand-tuned 16 px version beside the
  scalable one; the scalable one is used everywhere else.
- **Named by meaning** (`document-save`, not a file name), with a name and a
  description for the accessibility tree (roadmap 2.8a, goal 4).
  Freedesktop's icon naming spec is the precedent for the names.
- **Survey first:** Haiku's HVIF, its own compact vector icon format
  (`src/libs/icon`), before the colour layer format is fixed.

## 3. File types

- **A template, generated on the host:** one document shape drawn for CXOS, a
  colour per category (document, image, audio, video, archive, code, font,
  disk image, program), the category's Phosphor glyph on it (Phosphor has a
  family of file glyphs), and the extension (`PDF`, `RS`) drawn at runtime in
  the UI face. A new type is a row in a table; every extension works; the
  label is sharp at any size.
- **Previews** replace the icon at larger sizes for images, video, fonts and
  documents, made by the isolated decoders (roadmap Phase B, media): no
  thumbnail is parsed inside the file manager.
- **A signed program shows its trust:** a `.xuex` or `.xsex` carries a badge
  for what verification found (`PROC_TRUST_PLATFORM`, `_PUBLISHER`,
  `_UNVERIFIED`, `_UNSIGNED`), so an unsigned program never looks like a
  trusted one.
- **Folders:** CXOS's folder shape, with a Phosphor glyph on the known
  folders (Documents, Pictures, ...; roadmap Phase 3, the known-folders API).
- **A file's type comes from the type registry** (Phase 3: CXFS v3
  attributes, then the extension). No privileged process reads a file's
  contents to guess it.

## 4. Order

1. Symbolic icons with 2.8a′ (the toolkit needs them; they cost the font
   rasteriser plus a name table): Phosphor converted, the checker, sabotages
   that feed it bad files.
2. Colour icons and CXOS's own drawing with the first system apps: the
   layer format, the document and folder template, the program trust badge.
3. File-type lookup and previews with Phase 3's registry and Phase B's
   decoders.
