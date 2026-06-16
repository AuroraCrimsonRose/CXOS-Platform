# BIOS Interrupt Error Codes
### CXK Reference — Aurora Tejeda / CATX SYSTEMS LLC

This reference documents the BIOS interrupt error codes the **CXK** bootloader
may encounter and report during early (real-mode) startup, before the kernel
takes over. Error codes are read from `AH` after the carry flag (`CF`) is set.

---

## INT 13h — Disk Services

| Code | Error |
|------|-------|
| `0x00` | Success |
| `0x01` | Bad command / parameter |
| `0x02` | Address mark not found |
| `0x03` | Write protect error |
| `0x04` | Sector not found |
| `0x05` | Reset failed |
| `0x06` | Disk changed / removed |
| `0x07` | Drive parameter activity failed |
| `0x08` | DMA overrun |
| `0x09` | DMA crossed 64KB boundary |
| `0x0A` | Bad sector flag |
| `0x0B` | Bad track flag |
| `0x0C` | Unsupported track / invalid media |
| `0x0D` | Invalid number of sectors on format |
| `0x0E` | Control data address mark detected |
| `0x0F` | DMA arbitration level out of range |
| `0x10` | Uncorrectable ECC / CRC error |
| `0x11` | ECC corrected data error |
| `0x20` | Controller failure |
| `0x31` | No media in drive |
| `0x32` | Incorrect drive type in CMOS |
| `0x40` | Seek operation failed |
| `0x80` | Drive timeout / not ready |
| `0xAA` | Drive not ready |
| `0xB0` | Volume not locked in drive |
| `0xB1` | Volume locked in drive |
| `0xB2` | Volume not removable |
| `0xB3` | Volume in use |
| `0xB4` | Lock count exceeded |
| `0xB5` | Valid eject request failed |
| `0xBB` | Undefined error |
| `0xCC` | Write fault |
| `0xE0` | Status register error |
| `0xFF` | Sense operation failed |

---

## INT 13h — Most Common in QEMU

| Code | Cause |
|------|-------|
| `0x01` | Wrong sector count or command byte |
| `0x04` | Sector beyond disk image size |
| `0x80` | Disk image not attached or wrong format |
| `0xBB` | Generic QEMU emulation error |

---

## INT 15h — Memory / Misc Services

| Code | Error |
|------|-------|
| `0x00` | Success |
| `0x01` | Keyboard buffer full |
| `0x03` | Interface error |
| `0x06` | A20 still in use / could not disable |
| `0x80` | Invalid command / not implemented |
| `0x82` | Function not supported by BIOS |
| `0x83` | Operation in progress |
| `0x86` | Function not supported (E820 not available) |
| `0x87` | A20 error — could not enable |

---

## INT 10h — Video Services

| Code | Error |
|------|-------|
| `0x00` | Success |
| `0x01` | Invalid mode |
| `0x03` | Function not supported in current mode |
| `0x05` | No such page |
| `0x06` | Scan line out of range |

---

## INT 16h — Keyboard Services

| Code | Error |
|------|-------|
| `0x00` | Success |
| `0x01` | No keystroke available (buffer empty) |

---

## General BIOS Convention

| Flag | Meaning |
|------|---------|
| `CF = 0` | Call succeeded |
| `CF = 1` | Call failed — error code in AH |
| `AH = 0x00` | No error |
| `AH != 0x00` | Error — see table above for interrupt |

---

*Reference for the CXK bootloader's early error reporting.*
*Error codes are captured from `AH` after the carry flag is set.*
*Once CXK enters protected mode, the kernel no longer uses BIOS interrupts;
this reference applies to the real-mode boot stage only.*