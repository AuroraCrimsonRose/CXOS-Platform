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
