# Third-party notices

Software included in CXOS that was written by someone else, with the licence it
arrives under. Those licences are not superseded by [LICENSE.md](LICENSE.md).
Each component keeps its own.

## Current inventory

**None.** No third-party source, no vendored libraries, no bundled assets.

| Was | Licence | What happened |
|---|---|---|
| 1-bit Pixel Icons by Nikoichu, 1,495 PNGs in `assets/icons/` | `CC0-1.0` | Removed 2026-10-02. CC0 imposed nothing, so this was housekeeping rather than compliance. An unused third-party set is still something to explain to every future reader. |
| CX file-type icons, `assets/icons/filetypes/` and its `source/` masters | original work | Removed 2026-10-02 at the author's direction, to be redrawn. They were not third-party; they went with the directory. |

## The console font

`kernel/lib/gfx/font.c` is not in the table above. The reasoning is recorded here
because it was briefly entered there in error.

The glyph array was produced from a bitmap PNG of a rendered font, put through a
Python converter written for the job. No font file was copied: no BDF, no PSF, no
TTF. The converter has since been lost, so the conversion cannot be re-run or
audited. Only its output survives, as `font.c`. The shapes are Terminus Font, and
`assets/fonts/terminus/OFL.txt` is kept as a record of where they came from
rather than as a licence CXOS operates under.

US copyright excludes "typeface as such" (37 CFR § 202.1(e)). What it protects is
font software, meaning the program or the file, and none was taken. A raster of
rendered glyphs is the typeface's appearance, not the file that produced it.

Stated without spin: this was a mechanical conversion of the whole glyph set, not
a handful of shapes redrawn by eye. That is a closer reproduction than selective
transcription would be. It does not change which thing was copied, appearance
rather than software, but it is the fact a lawyer should hear first.

**This is a judgement, not a settled fact**, and it is on the list for legal
review. If it is decided the other way, the consequences are specific: `font.c`
becomes `OFL-1.1` attributed to Dimitar Toshkov Zhekov, the Reserved Font Name
clause constrains how it may be named, and the OFL text must ship with binaries
as well as source. None of that is onerous. The OFL is permissive and expressly
allows bundling inside software under any licence, so the downside of being wrong
is attribution and a one-file carve-out, not a blocked release.

The font stays because it earns its place. Terminus distinguishes `0` from `O`
and `1` from `l` from `I`, which a console printing hex dumps, paths and key
fingerprints depends on. A hand-drawn replacement is intended in time and would
retire this question entirely, but only once it makes the same distinctions.

### The 8×8 glyphs, removed

`font_default_8x8` came from a different font, described as permissive and
needing no attribution, but the name was no longer remembered and the claim could
not be checked. It was compared against the public-domain `font8x8_basic` by
Daniel Hepper and was not that: `'B'` matched byte for byte after bit reversal
while `'A'`, `'M'` and `'0'` did not, which is what two unrelated 8×8 fonts
sharing obvious letterforms look like.

Removed 2026-10-02 along with its accessor. Nothing in the tree ever called it.
Only the 8×16 array is read, through `fb.c`. It was at once the only asset whose
provenance could not be demonstrated and the only one that cost nothing to
delete.

## How the earlier claim went wrong

This file once read "None. Every line in this repository is original work." The
icons made that false, and `assets/icons/CC0.md` was already in the tree. The
claim was written from memory of the codebase rather than by looking at it.

It surfaced during an audit of SPDX header coverage, where
`assets/fonts/terminus/OFL.txt` appeared as an unmarked file and was worth
pulling on.

The correction then overshot the other way, marking `font.c` as OFL-licensed work
of another author. Both errors came from the same habit: deciding a provenance
question without asking the person who wrote the code.

## Adding a component

This file is the inventory, so nothing arrives without an entry. When you port or
vendor something:

1. **Check the licence before writing any code against it.** A GPL component
   cannot be linked into the PolyForm-licensed tier, and the question is far
   cheaper to answer first than after a week of porting. Permissive licences
   (MIT, BSD, zlib, Apache 2.0) and file-level copyleft (MPL 2.0) are usually
   workable. Strong copyleft usually is not.
2. **Put it under `vendor/`**, not inside a tier directory, so the boundary is
   visible in the tree rather than only recorded here.
3. **Add a row above**, with the upstream licence text copied into `LICENSES/`.
   A link is not a licence notice; most licences require the text to travel with
   the distribution.
4. **Record any local modifications.** Several licences require changed files to
   say they were changed.

The roadmap will add far more. It plans to port zlib, FAT32, codecs, SSH, an HTTP
server and a SQL server, and to embed an existing browser engine rather than
write one ([docs/planning/CX_ROADMAP.md](docs/planning/CX_ROADMAP.md)).
