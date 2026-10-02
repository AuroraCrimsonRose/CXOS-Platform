# Third-party notices

Software included in CXOS that was written by someone else, with the licence it
arrives under. Those licences are **not** superseded by
[LICENSE.md](LICENSE.md); each component keeps its own.

## Current inventory

**None.** Every line in this repository is original work by Aurora Tejeda
(CATX Systems). CXOS has no third-party source, no vendored libraries and no
bundled binaries.

That is unusual and it will not last: the roadmap plans to **port** zlib, FAT32,
codecs, SSH, an HTTP server and a SQL server, and to **embed** an existing
browser engine rather than write one
([docs/planning/CX_ROADMAP.md](docs/planning/CX_ROADMAP.md)).

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
