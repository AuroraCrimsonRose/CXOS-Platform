# Third-party notices

Software included in CXOS that was written by someone else, with the licence it
arrives under. Those licences are **not** superseded by
[LICENSE.md](LICENSE.md); each component keeps its own.

## Current inventory

**None.** No third-party source, no vendored libraries, no bundled assets.

This time the claim was arrived at by looking rather than by remembering, and by
removing what did not survive the look.

| Was | Licence | What happened |
|---|---|---|
| **1-bit Pixel Icons** by Nikoichu, 1,495 PNGs in `assets/icons/` | `CC0-1.0` | **Removed 2026-10-02.** CC0 imposed nothing, so this was housekeeping rather than compliance — but an unused third-party set is still a thing to explain to every future reader. |
| CX file-type icons, `assets/icons/filetypes/` and its `source/` masters | original work | **Removed 2026-10-02** at the author's direction, to be redrawn. They were not third-party; they went with the directory. |

### The fonts are not third-party components — but read this

`kernel/lib/gfx/font.c` is **not** in the table above, and the reasoning is worth
recording because it was briefly entered there in error.

The glyph array was produced from a **bitmap PNG of a rendered font**, put
through a **Python converter written for the job**. No font file — no BDF, PSF or
TTF — was copied. The converter has since been lost, so the conversion cannot be
re-run or independently audited; only its output survives, as `font.c`. The 8×16
shapes are Terminus Font, and `assets/fonts/terminus/OFL.txt` is kept as a record
of where they came from rather than as a licence CXOS operates under.

**Why that distinction matters.** US copyright excludes *"typeface as such"*
(37 CFR § 202.1(e)). What can be protected is **font software** — the program or
file — and none was taken. A raster of rendered glyphs is the typeface's
appearance, not the file that produced it.

**Stated without spin:** this was a *mechanical conversion of the whole glyph
set*, not a handful of shapes redrawn by eye. That is a closer reproduction of
the typeface than selective transcription would be. It does not change which
thing was copied — appearance, not software — but it is the fact a lawyer should
hear first, not the one to bury.

**This is a judgement, not a settled fact.** It is on the list for the lawyer,
and the consequences if it is decided the other way are specific: `font.c`
becomes `OFL-1.1` attributed to Dimitar Toshkov Zhekov, the Reserved Font Name
clause constrains how it may be named, and the OFL text must ship with
**binaries** as well as source. None of that is onerous — the OFL is permissive
and expressly allows bundling inside software under any licence — which is worth
knowing, because it means the downside of being wrong here is attribution and a
file carve-out, not a blocked release.

### Closed: the 8×8 glyphs are gone

`font_default_8x8` came from a **different** font — described as permissive,
needing no attribution and usable commercially, but the name was no longer
remembered, so the claim could not be checked. It was compared against the
public-domain `font8x8_basic` by Daniel Hepper and was not that: `'B'` matched
byte for byte after bit reversal while `'A'`, `'M'` and `'0'` did not, which is
what two unrelated 8×8 fonts sharing obvious letterforms look like.

**Removed 2026-10-02**, along with its accessor. Nothing in the tree ever called
it — only the 8×16 array is read, through `fb.c` — so it was simultaneously the
only asset whose provenance could not be demonstrated and the only one that cost
nothing to delete. An unidentifiable asset that is also unused is not a question
worth keeping open.

The 8×16 Terminus-derived array **stays**. It is load-bearing rather than
decorative: Terminus distinguishes `0` from `O` and `1` from `l` from `I`, which
a console printing hex dumps, paths and key fingerprints actually depends on. A
hand-drawn replacement is intended in time, and would retire the provenance
question above entirely — but only once it makes those same distinctions.

### How the earlier claim went wrong

This file previously read *"None. Every line in this repository is original
work."* The icons made that false, and `assets/icons/CC0.md` was already in the
tree — the claim was written from memory of the codebase rather than by looking
at it. It surfaced during an audit of SPDX header coverage, where
`assets/fonts/terminus/OFL.txt` appeared as an unmarked file and was worth
pulling on.

The correction then overshot in the other direction, marking `font.c` as
OFL-licensed work of another author. Both errors came from the same habit:
deciding a provenance question without asking the person who wrote the code.

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
