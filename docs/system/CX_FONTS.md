# Fonts and character sets
### CX Design Spec: Aurora Tejeda / CATX Systems

> **Status: DECIDED 2026-10-16; the console part is built; XFNT v2 outlines
> built on the host (2026-11-25, §4).** CXOS uses
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
   fixed-record format, never a TrueType file. **Where the rasteriser runs:
   the client toolkit** (decided 2.8a, `system/CX_TOOLKIT.md` §2), since
   windows are client-drawn; displayd links the same library for its frames
   and trusted screens.
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

## 4. XFNT v2, the outline format (built 2026-11-25, roadmap 2.8a′1)

The host (`cxk font build <font>.ttf --face "CX Sans"`) writes it; the OS
reads it (2.8a′2). Code: `devkit/CXEX.Build/Fonts/XfntOutline.cs` (writer
and checker), `OutlineConvert.cs` (TrueType to v2), `TrueTypeFont.cs` (the
host's TrueType reader), `OutlineRaster.cs` (the oracle rasteriser). Tests:
`CXEX.Tests/Unit/XfntOutlineTests.cs`.

**The OS side** (2.8a′2): `os/std/outline.xfxn`, the format and its check, which needs nothing from the system (so the host builds it too, for the rasteriser's fact), and `os/std/fontfile.xfxn`, which loads a face. `of_check` makes the host
checker's comparisons in its order and names the rule a file broke
(`OF_W_*`); `of_load` takes a name, never a path, and reads only
`/System/Fonts/<name>.xfnt` (a-z, 0-9, `-`, `_`; 14 at most, XSTG's
32-byte staged names). The image carries CX Sans and CX Mono, regular,
bold, italic and bold italic (`cxsans`, `cxsans-b`, `-i`, `-bi`;
`cxmono...`), converted at build time. The x tests
(`os/apps/outline_tests.xfxn`) break the host tests' small face the same
42 ways; `XfntOutlineTests` reads those bytes and mutations out of the X
file and holds them to the C# writer and checker.

**No offsets.** Every section's place follows from the header's four
counts, every index is checked against a running sum, and the file is
exactly as long as the counts say. All fields are little-endian.

| Offset | Field |
|---|---|
| 0 | `u32` magic `XFNT` · `u16` version 2 · `u8` kind 2 (outline) · `u8` flags (1 italic, 2 monospace) |
| 8 | `u16` em, always 4096 · `u16` weight 1-1000 |
| 12 | `i16` ascender > 0, descender ≤ 0, line gap ≥ 0, cap height, x height, underline position, underline thickness · `u16` 0 |
| 28 | `u32` glyphs (1-65535), map entries, contours, points · `u32` 0 |
| 48 | face name: 16 bytes of printable ASCII, NUL padded |

Then, in this order:

1. **Map**, 8 bytes each: `u32` codepoint, `u32` glyph. Strictly ascending;
   no surrogates, nothing past U+10FFFF; every glyph in the font. A
   codepoint not in the map draws glyph 0.
2. **Glyphs**, 16 bytes: `u16` advance; `i16` box xmin, ymin, xmax, ymax;
   `u16` contour count; `u32` first contour (the running sum). The box is
   the points' own (the control box), all zero for a glyph with no contours.
3. **Contours**, 8 bytes: `u32` first point (the running sum), `u32` count
   ≥ 2.
4. **Points**, 4 bytes: `i16` x, y, in em units, y up. Every point lies in
   its glyph's box; every coordinate, box and advance within ±4 em.
5. **On-curve flags**, a byte per point, 0 or 1, zero-padded to 4 bytes.
   Each contour starts on the curve and never has two off-curve points
   together, so every segment is a line or one quadratic. (The converter
   writes TrueType's implied midpoints out.)

Caps: 2^22 contours, 2^24 points. Not carried yet: kerning (a pair table, a
later step), ligatures (§3 item 5), icon names (2.8a′5).

**The converter** (host only): reads `glyf` TrueType (CFF is refused for
now: no default face needs it), picks `cmap` 3/10 or 0/4-6 format 12 over
3/1 or 0/x format 4, flattens composites (offsets, the three scale forms,
SCALED_COMPONENT_OFFSET, ROUND_XY_TO_GRID, USE_MY_METRICS; point matching
refused), cuts the font to glyph 0 and what `cmap` reaches, rescales to the
4096 em and rounds once, drops contours that draw nothing, reads ascender
and descender from OS/2 when USE_TYPO_METRICS is set. Every font it writes
is read back through the checker. A damaged source fails as bad data,
never as a crash (a seeded damage test: `CXEX_TEST_SEED` tries others).

### 4.1 The rasteriser (built 2026-11-25, roadmap 2.8a′3)

Two copies, held to each other pixel for pixel: the oracle
(`devkit/CXEX.Build/Fonts/OutlineRaster.cs`) and the OS's
(`os/std/raster.xfxn`, `rs_draw`). The Differential fact `RasterDiff`
runs `os/xc/rastdump.xfxn` on the host against `cxk font raster` (a line a
glyph and size: box, advance, an FNV-1a of the coverage): every glyph of
the image's eight faces at 1-48 ppem and a sample at 96, 300 and 1024, and
faces generated from the seed (points at ±4 em, hairlines, slivers,
overlaps, degenerate curves, the cap's edge), then the same through the
cache. The X copy refuses a size past 1-1024 (`E_INVALID_ARGS`) and a box of
more than 2^22 cells, (width + 1) · height (`E_NO_MEMORY`; 64 MiB of them;
no face in the image comes near it), and the oracle's verb reports the
same.

**The cache** (`rs_cache`): one face at one size, 256 slots, open
addressing on the glyph (Fibonacci hashing, the top 8 bits), and a budget
of coverage bytes. A miss draws and keeps; at three quarters full, or past
the budget, everything is let go and the glyph kept alone. A bitmap it
hands out is valid until the next `rs_cache_get`. An LRU is not worth its
cost until a′4 measures text drawing.


Integers only, and every division is of non-negative numbers:

1. **Scale** (ppem 1-1024): an em coordinate becomes 26.6 pixels as
   `((v + 16384) * ppem + 32 >> 6) - 256 * ppem`, which is
   `floor((v * ppem + 32) / 64)`. The bias keeps the shift's operand
   non-negative, so X needs no signed shift. Y is negated (y down).
2. **Box**: left = floor(xmin / 64 px), right = ceil(xmax), top =
   floor(-ymax), bottom = ceil(-ymin); a zero width or height becomes one.
   Every point is shifted by (left, top) · 64, so all are ≥ 0.
3. **Curves**: dev = max(|p0 − 2p1 + p2|) over x and y; n is the least of
   1-16 with dev ≤ 32n² (an eighth of a pixel at most); point i is
   `((n−i)²p0 + 2i(n−i)p1 + i²p2 + n²/2) / n²`, the last exactly p2.
4. **Lines** are cut at each pixel row, then each column. With the ends
   ordered y-up (sign −1 if swapped), x at height y is
   `a.x ± (y − a.y)·|b.x − a.x| / (b.y − a.y)`, and y at a column edge
   the same way round. Each piece adds to its cell (column
   floor(min x / 64), at most `width`, the spare column): cover += sign·dy,
   area += sign·dy·(fx0 + fx1), fx relative to the cell.
5. **Sweep** each row left to right: cover accumulates; a pixel is
   `|cover·128 − area| >> 5`, clamped to 255 (non-zero winding by magnitude,
   as FreeType's default).
6. **Advance**: Scale(advance), in 26.6.

The method is FreeType's `smooth` area-and-cover accumulation (from memory;
FreeType is not on disk). SerenityOS's `Userland/Libraries/LibGfx/EdgeFlagPathRasterizer.cpp`
(sampled edge flags) was the other candidate: exact only to its sample
count, which an oracle would have to reproduce too.

### 4.2 The toolkit's font handle (built 2026-11-26, roadmap 2.8a′4)

A request is a **family, a weight, italic or not, and a size** (§3 item 5).
`of_open` (`os/std/fontfile.xfxn`) picks the family's file: `<family>`,
`-b`, `-i`, `-bi` (weights 550 and up are bold); when that file is missing
it falls back to the upright face of the same weight, and then to regular.
`tk_font` (`os/std/toolkit_font.xfxn`) holds a face at a size given in
logical units. `tk_font_scale` turns that into device pixels for a ui's
scale, `ppem = (size · q + 2) / 4`, and keeps a glyph cache (§4.1) for
that size only.

- **Metrics** are whole device pixels. `asc = ceil(ascender)`, and
  `line = asc + ceil(−descender)`. The underline position and thickness
  are each at least 1 pixel.
- **Advances** are 26.6 values. They are summed and rounded only where a
  glyph is placed (`(p + 32) >> 6`), so a run of text has the same width
  however it is split. There is no kerning yet.
- **Unseen characters** take no room and draw nothing. These are the bidi
  controls that UAX #9 removes, plus the zero-width characters, U+FEFF
  and U+00AD. Without this the face's glyph 0 would draw a box for them.
- **Sharing.** A display client has no `GRANT_DISK`, so it cannot read
  /System/Fonts itself. displayd loads the regular and bold faces of the
  configured family and passes each memory object to every client it
  starts, in spawn slots 6 and 7 with `MAP|INSPECT` rights. The client maps
  the object read-only and runs `of_check` on it as it would on a file
  (`of_map`). The server can still change a face after the client has
  checked it, which is the same trust the client already gives the
  server's bitmap font.

Sizes in the toolkit: body text is 13, headings are 18 bold (Aurora,
2026-11-26). The x tests (`os/apps/font_tests.xfxn`, 8000–8399) hold drawn
text to `rs_draw` pixel for pixel, never to the cache.
