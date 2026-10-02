# Third-party notices

Software included in CXOS that was written by someone else, with the licence it
arrives under. Those licences are **not** superseded by
[LICENSE.md](LICENSE.md); each component keeps its own.

## Current inventory

| Component | Upstream | Licence | Where | Modified |
|---|---|---|---|---|
| **Terminus Font** | [terminus-font.sourceforge.net](http://terminus-font.sourceforge.net/) — © 2020 Dimitar Toshkov Zhekov | `OFL-1.1` ([LICENSES/OFL-1.1.txt](LICENSES/OFL-1.1.txt)) | glyph data in `kernel/lib/gfx/font.c`; upstream licence kept at `assets/fonts/terminus/OFL.txt` | **Yes** — glyphs converted to C arrays at 8×16 and 8×8 |
| **1-bit Pixel Icons** by Nikoichu | [nikoichu.itch.io/1-bit-pixel-icons](https://nikoichu.itch.io/1-bit-pixel-icons) | `CC0-1.0` | `assets/icons/` (1,511 files); declared in `assets/icons/CC0.md` | No |

### Terminus Font — obligations that actually bind

This one carries conditions, so they are written out rather than left to the
licence file:

1. **`kernel/lib/gfx/font.c` is `OFL-1.1`, not the kernel's licence.** A Modified
   Version of Font Software stays under the OFL, so this one file is carved out
   of the PolyForm tier. The OFL explicitly permits bundling inside software
   under any licence, so this is a carve-out and not a conflict.
2. **"Terminus Font" is a Reserved Font Name.** This derivative must not be
   presented under it. The arrays are `font_default_8x16` / `font_default_8x8`
   and nothing user-facing says Terminus.
3. **The licence must travel with distribution — including binaries.** The kernel
   image contains these glyphs, so `OFL-1.1.txt` has to ship with the disk image,
   not only with the source.
4. **It may not be sold on its own.** It is not; it is bundled.

### How this was missed

This file previously read *"None. Every line in this repository is original
work."* That was wrong when it was written. The OFL text and `assets/icons/CC0.md`
were both already in the tree, and the claim was made from memory of the codebase
rather than from looking. It surfaced during an audit of SPDX header coverage —
`assets/fonts/terminus/OFL.txt` showed up as an unmarked file and was the thread
worth pulling.

The roadmap will add far more: it plans to **port** zlib, FAT32, codecs, SSH, an
HTTP server and a SQL server, and to **embed** an existing browser engine rather
than write one ([docs/planning/CX_ROADMAP.md](docs/planning/CX_ROADMAP.md)).

## Adding a component

This file is the inventory, so nothing arrives without an entry. When you port
or vendor something:

1. **Check the licence before writing any code against it.** A GPL component
   cannot be linked into the PolyForm-licensed tier, and the question is far
   cheaper to answer first than after a week of porting. Permissive licences
   (MIT, BSD, zlib, Apache 2.0) and file-level copyleft (MPL 2.0) are usually
   workable; strong copyleft usually is not.
2. **Put it under `vendor/`**, not inside a tier directory, so the boundary is
   visible in the tree rather than only recorded here.
3. **Add a row below**, with the upstream licence text copied into
   `licenses/` — a link is not a licence notice; most licences require the text
   to travel with the distribution.
4. **Record any local modifications.** Several licences require changed files to
   say they were changed.

| Component | Version | Upstream | Licence | Licence text | Modified |
|---|---|---|---|---|---|
| *(none yet)* | | | | | |

## Why this file exists before it has any content

The previous licence claimed CXOS was "owned exclusively" by its author. That is
true today and stops being true the first time a port lands — at which point the
claim is not merely stale but wrong, and wrong in a way that misrepresents
someone else's work as this project's. Having the inventory in place first means
the claim is corrected by the act of adding a row, rather than remembered later.
