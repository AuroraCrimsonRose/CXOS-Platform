# File-type icons

Icons for CX file types, used by host integration (Explorer file associations)
and by Studio. `source/` holds the PNG masters; the `.ico` files are built from
them.

| Icon | Type | Status |
|---|---|---|
| `XKEX` | Kernel executable | current |
| `XBEX` | Boot-stage executable | current |
| `XOEX` | OS executable | current |
| `XKCO` | Kernel configuration (X Data) | current |
| `XBCO` | Boot configuration | specified, not built |
| `XBPT` | Boot partition table | current |
| `XKPK` / `XKSK` | Platform public / private key | current |
| `XCDL`, `XCSL` | Libraries (dynamic / static) | specified, not built |
| `XCCO` | Compiled configuration | named in `CX_FILE_STRUCTURE.md` only |
| `XCEX` | **Retired.** `.xcex` was replaced by `.xuex`, `.xsex`, `.xoex` | keep only to recognise old files |
| `XKPT` | Not in the current taxonomy | review |
| `XFILE` | Generic CX file | current |

**Missing**, for types that exist today: `XUEX` (user program), `XSEX`
(service), `XUPK`/`XUSK` (publisher key pair), `XOSV` (service descriptor) and
`XFXN` (X source; `editors/vscode/icons/xfxn.png` can serve as its master).
The templates in `source/` (`X###_TEMPLATE.png`, `X#EX.png`, `X#CO.png`,
`X#PT.png`) are the starting points for these.

The taxonomy is in `docs/formats/CX_EXTENSION_SYSTEM.md` and
`docs/formats/CX_EXTENSION_NAMING.md`.
