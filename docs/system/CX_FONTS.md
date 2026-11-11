# Fonts and character sets
### CX Design Spec: Aurora Tejeda / CATX Systems

> **Status: DECIDED 2026-10-16; the console part is built.** CXOS uses
> published fonts and standard character sets. It does not draw its own fonts.

---

## 1. Decisions

| Decision | Reason |
|---|---|
| **Published fonts only.** The drawn `cx_term` fonts (`term_8`, `term_16`) are removed. | A font is years of work by specialists; a drawn one was a standing source of unfinished glyphs (`term_16` never reached a shippable state). |
| **Standard character sets only.** Every byte or codepoint means what a published standard says. | Text written on CXOS reads the same anywhere else, and nothing in the font defines its own codepage. |
| **Licence: SIL OFL 1.1.** | The standard licence for open fonts. It allows bundling with any software, PolyForm and MIT included; it requires the licence to travel with the font and forbids selling the font on its own. `LICENSES/OFL-1.1.txt` is already in the repository. |
| **A converted or subset font never carries the original's name.** | Under the OFL a format conversion or a subset is a Modified Version, and a Modified Version may not use a Reserved Font Name. Terminus reserves "Terminus Font", so the console font is named "Console". |
| **Fonts are vendored unmodified under `vendor/` and converted at build time.** | The repository's third-party rule (`THIRD-PARTY-NOTICES.md`); the converted file can never drift from its source. |

## 2. The console (built)

- **Font:** Terminus 4.49.1 by Dimitar Toshkov Zhekov, `vendor/terminus/ter-u16n.bdf`,
  8×16. Chosen because it was designed for consoles, distinguishes `0`/`O` and
  `1`/`l`/`I`, ships as BDF (no rasteriser needed), and covers 1,356 characters
  including every one of the sets below. The kernel's compiled-in fallback
  font is already Terminus-derived (`THIRD-PARTY-NOTICES.md`).
- **Character set:** IBM code page 437. Bytes 0x20–0x7E are ASCII; 0x80–0xFF
  are CP437's letters, box drawing, shading and symbols, in CP437's own order.
  It is the repertoire the console had before, now in the standard order.
- **Pipeline:** `cxk font build <bdf> --charset cp437 --face Console` →
  `console_16.xfnt`, staged at `/System/Fonts`. Strict: a font missing any
  character of the set fails the build with the codepoints named. The CP437
  table is tested against .NET's own CP437 decoder.

## 3. Next

1. **UTF-8 console.** The console decodes UTF-8 and draws by codepoint
   through the font's codepoint table, so every one of Terminus's 1,356
   characters is reachable, not only CP437's 223.
2. **Fallback for the rest of Unicode:** GNU Unifont (OFL-1.1, dual-licensed
   with the GPL; CXOS takes it under the OFL), which has a glyph for every
   character of the Basic Multilingual Plane at 8×16 and 16×16.
3. **Larger console sizes** for high-resolution screens: Terminus's 12×24 and
   16×32 BDFs, once XFNT's 8-pixel width limit is raised.
4. **The GUI:** Noto Sans, Noto Serif and Noto Sans Mono (OFL-1.1), the family
   with the widest script coverage, drawn by an outline rasteriser in the
   display server (`system/CX_DISPLAY.md`). Which rasteriser is decided under
   the Build / Port / Embed rule when the display server is built.
   **2026-11-11 (roadmap 2.8a′):** built, as XFNT v2 outlines. TrueType is
   converted on the host by `cxk font build`, and the OS reads only our own
   fixed-record format, never a TrueType file. Where the rasteriser runs
   (the display server, as above, or the client toolkit, since windows are
   client-drawn) is decided in 2.8a.
5. **A choice of faces, proportional and monospace** (Aurora, 2026-11-11).
   All OFL-1.1, each licence read when it is vendored (reserved names
   included: a converted face that reserves its name ships under one of
   ours). Regular, Bold, Italic and Bold Italic first; where a family
   publishes only a variable font, the converter instantiates those four on
   the host.

   **The defaults (Aurora, 2026-11-11), replacing item 4's Noto:**

   | Role | Default |
   |---|---|
   | Primary system UI | IBM Plex Sans |
   | Headings and branding (the system's own titles; not for body text: no italics, weights 300-700 only) | Space Grotesk |
   | Terminal and developer tools | IBM Plex Mono (no ligatures) |
   | Accessibility alternative (one setting swaps the UI face, and the terminal's to Atkinson Hyperlegible Mono) | Atkinson Hyperlegible Next |
   | Documents (serif; to confirm) | IBM Plex Serif |

   **Every face here, defaults and choices, ships in our own format,** XFNT
   (v2 for outlines): no TTF or OTF file is in the image, and nothing in the
   OS reads one. A font a user adds later is converted too: on a host by
   `cxk font build`, or on CXOS by a converter running as its own process
   with no grants, whose output is accepted only if the XFNT checker passes
   it. The checker, not the converter, is the trust boundary.

   **One file per family** (Aurora, 2026-11-11). Size is free (outlines).
   Weight is a variable axis: one set of outlines plus each master's
   changes, blended by the OS to any weight the file declares. Width and
   optical size can be further axes when a source has them. The converter
   checks the masters are compatible (same points, same contour order) and
   stores every point's change explicitly, so the OS only multiplies and adds
   and never infers untouched points as TrueType's `gvar` makes a reader do.
   The OS checks again: equal point counts across masters, axis values
   clamped to the declared range, each change bounded so no blended point
   leaves the glyph's box. Italic is a second design in the same file, each
   design variable in weight, since a true italic is drawn, not slanted; a
   computed slant is a fallback, off by default. A family published only as
   separate fixed instances whose outlines do not match is stored as those
   instances in the one file, and a request takes the nearest. A request is
   family, weight, italic and size; the font picker shows a family with a
   weight slider and an italic switch. The glyph cache rounds weights to
   steps (25). Kerning is the default master's at first, blended later.
   The C# oracle blends as the X rasteriser does.

   **Shipped as choices, many of them:** the user picks any face for each
   role.

   | Role | Choices |
   |---|---|
   | UI | Noto Sans, Inter, Source Sans 3, Open Sans |
   | Serif | Noto Serif, Source Serif 4 |
   | Monospace | Noto Sans Mono, Fira Code, JetBrains Mono, Cascadia Code, Source Code Pro |
   | Same metrics as the commercial fonts (documents made elsewhere) | Liberation Sans, Serif, Mono (Arial, Times New Roman, Courier New); Carlito (Calibri); Caladea (Cambria) |
   | Bitmap | Terminus (the console, built); GNU Unifont (item 2) |
   | Other scripts | Noto's families, and Plex's own (Arabic, Hebrew, Devanagari, Thai, KR, JP): fallback where the chosen face has no glyph |

   Plex reserves its name, I believe: if so the converted faces ship under
   ours ("CX Sans", "CX Mono" or the like), credited in the notices; check
   every face's reserved names when it is vendored. The terminal draws the
   box-drawing and block characters (U+2500-259F) itself, whatever the
   face, since a monospace face may lack them and drawn ones join exactly.

   Not shipped: Arial, Helvetica, Times New Roman, Segoe UI, Calibri,
   Consolas and the like, which are licensed for their vendors' systems
   only. Roboto and DejaVu are not OFL and would need decision 3 widened.
   The image carries the defaults and Terminus always, and as many of the
   choices as its size allows (measured in 2.8a′); whatever does not fit
   becomes a package once Phase 3 has them.
   Needed with it: faces found by family, weight and style in
   `/System/Fonts`, the toolkit's font handle asking by those and a size,
   and the user's default UI, document and terminal faces as a setting.
   The console keeps Terminus; the terminal app defaults to IBM Plex Mono
   and offers the other monospace faces and Terminus. **Ligatures** (Fira Code's, JetBrains Mono's and Cascadia Code's
   `->`, `!=`) need a sequence-to-glyph table carried in XFNT v2 and a
   lookup when text is laid out: a step after 2.8a′; without it those faces
   still draw, unjoined.
