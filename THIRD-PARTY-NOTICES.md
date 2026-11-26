# Third-party notices

Software included in CXOS that was written by someone else, with the licence it
arrives under. Those licences are not superseded by [LICENSE.md](LICENSE.md).
Each component keeps its own.

## Current inventory

| Component | Where | Licence | Use |
|---|---|---|---|
| Terminus Font 4.49.1, `ter-u16n.bdf`, by Dimitar Toshkov Zhekov | `vendor/terminus/` | `OFL-1.1` (`vendor/terminus/OFL.txt`, `LICENSES/OFL-1.1.txt`), Reserved Font Name "Terminus Font" | Vendored unmodified since 2026-10-16. The build converts it to `console_16.xfnt`, the 8×16 console font, cut to IBM code page 437. That file is a Modified Version under the OFL (converted and subset), so it is named "Console", never Terminus. Source: terminus-font-4.49.1.tar.gz from SourceForge, SHA-256 `d961c1b781627bf417f9b340693d64fc219e0113ad3a3af1a3424c7aa373ef79`. |
| IBM Plex Sans 1.3.6 and IBM Plex Mono 2.5.0, Regular, Bold, Italic and Bold Italic TrueType (`fonts/complete/ttf/`), by IBM Corp. | `vendor/plex/sans/`, `vendor/plex/mono/` | `OFL-1.1` (`vendor/plex/*/LICENSE.txt`, `LICENSES/OFL-1.1.txt`), Reserved Font Name "Plex" | Vendored unmodified since 2026-11-25 (roadmap 2.8a′1). `cxk font build` converts them to XFNT v2 outlines (rescaled, subset to what `cmap` reaches, hinting dropped): Modified Versions under the OFL, so they ship as "CX Sans" and "CX Mono", never Plex; the converter refuses a name sharing a word of the source's family. Sources: the GitHub releases `@ibm/plex-sans@1.3.6` `ibm-plex-sans.zip`, SHA-256 `a52bd405930040a6bfdb5c8aff67f1b7205538999986e2186d8f8895ca6a5de1`, and `@ibm/plex-mono@2.5.0` `ibm-plex-mono.zip`, SHA-256 `6d23f01257663d8cc49a0d64c22ced630b79e0e2a0ac08a0da86e9a38bbc481c`. |
| QR Code generator library (C), `qrcodegen.c` and `qrcodegen.h`, by Project Nayuki | `vendor/qrcodegen/` | `MIT` (the text is in `vendor/qrcodegen/Readme.markdown`, `LICENSES/MIT.txt`) | Vendored unmodified since 2026-11-08 and compiled into the kernel, where it encodes the panic screen's QR code. The only shims are the headers it includes, `kernel/lib/qr/`. Source: github.com/nayuki/QR-Code-generator, commit `bcfe1b018cec5f1f91b5705eca5bb4cdd34e9f8d`, `c/` and `Readme.markdown`. SHA-256: `qrcodegen.c` `6a2b9cc65176f2345dde260c74b6d352627e8a0a6385d086ae0e9c5d0913c70c`, `qrcodegen.h` `e82df4bff37d18b5863b9e7486fe6bda1b6cda8c3b9ecebfec473907265cb589`, `Readme.markdown` `eea862d8e975c6df197b050e5bd6c93b70c046952ac704b4ee1d213f2393f52a`. |
| Unicode Character Database 18.0.0, the bidirectional files: `extracted/DerivedBidiClass.txt`, `BidiBrackets.txt`, `BidiMirroring.txt`, `BidiTest.txt`, `BidiCharacterTest.txt`, by Unicode, Inc. | `vendor/unicode/` | `Unicode-3.0` (`vendor/unicode/license.txt`, `LICENSES/Unicode-3.0.txt`) | Vendored unmodified since 2026-11-13 (roadmap 2.8a4). `cxk unicode bidi` turns the first three into `os/std/unibidi.xfxn`, the tables of the UAX #9 implementation (`os/std/bidi.xfxn`, our own code); the two test files are Unicode's conformance tests, run against it in the CI container. Source: unicode.org/Public/18.0.0/ucd/. SHA-256: `DerivedBidiClass.txt` `d9e23222522551348ea1ccfbb4f62efbf98982afb95840f8959c08ed992c5607`, `BidiBrackets.txt` `4b3b62e4a14b84ee752808c810c602534921c09a4a1bf78cfbee566d66c125b3`, `BidiMirroring.txt` `cd54810ebf52f0e61a730c8b9cb25975de6c85f6d788a559b416afd548923fd6`, `BidiTest.txt` `9af2f882a4ab50912e388f069a673b94eacd82fa6d07d20a3ff7f3c759e905aa`, `BidiCharacterTest.txt` `045b24d2c8ab066951bd32fe8c6b4de34647f72b5b1c7df0265f24ab53573e01`, `license.txt` (unicode.org/license.txt) `e7a93b009565cfce55919a381437ac4db883e9da2126fa28b91d12732bc53d96`. |
| Unicode CLDR 49, `common/supplemental/plurals.xml`, by Unicode, Inc. | `vendor/cldr/` | `Unicode-3.0` (`vendor/cldr/LICENSE`, the repository's own; its first part restricts the UTS #35 *specification* text, which is not vendored; the data is under Unicode License v3) | Vendored unmodified since 2026-11-13 (roadmap 2.8a4). `cxk strings build` compiles a locale's plural rules from it into that locale's string table, and `cxk strings plurals` writes every locale's rules with CLDR's own samples for the boot test. Source: github.com/unicode-org/cldr, tag `release-49`. SHA-256: `plurals.xml` `69a30c57a621de9c52ec6ca95fe7bf9f06df6f8c13ca71a72e447b532c92cff7`, `LICENSE` `65eb33419e22a297550a811b5b535f5e532f9a7e0114b516c0ea18afd53c3804`. |
| Five photographs from free-images.com: `mountain_dawn.jpg`, `facets.jpg`, `paper_fan_dark.jpg`, `paper_fan.jpg`, `light_trails.jpg` | `assets/image/background/` | Public domain or `CC0-1.0`, as the site states for every image it offers ("Every image is Public Domain or CC0"). The page each one came from was not recorded. | Added 2026-11-18 as the desktop's backgrounds; `mountain_dawn` is background 1, the default. **Not under `vendor/` because they are modified:** renamed, and their metadata stripped (`cxk picture strip`: EXIF with a camera's GPS position, XMP, IPTC), image data byte-for-byte as downloaded. Neither licence forbids either. The build converts them to QOI (`cxk picture build`) for `/System/Backgrounds/1.qoi` to `5.qoi`. |
| ZXing.Net 0.16.11 | NuGet package reference of `devkit/CXEX.Tests` only | `Apache-2.0` | Test only, never part of CXOS or the DevKit: `PanicScreenTests` reads the panic screen's QR code with it. |

Removed on the same day: `assets/fonts/cx_term/` (`term_8`, `term_16`), the
console fonts drawn for CXOS. Not third-party; CXOS now uses published fonts
instead of drawing its own.

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
`vendor/terminus/OFL.txt` (moved from `assets/fonts/terminus/`) records where they came from
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
write one ([docs/x86_32/planning/CX_ROADMAP.md](docs/x86_32/planning/CX_ROADMAP.md)).
