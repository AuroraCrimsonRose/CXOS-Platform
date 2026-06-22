#!/usr/bin/env python3
# /CXK/tools/mkdisk.py
# Aurora Tejeda / CATX SYSTEMS LLC
#
# Build a CXK disk image with an XBPT partition table (see partition.h):
#
#   LBA 0          stage1 boot sector (512B, ends 0xAA55)
#   LBA 1          XBPT partition table
#   LBA 2 ..       stage2 (raw, optional)
#   <align 2048>
#   BOOT   part    (0xCB) kernel.xkex, raw at the partition start
#   STAGE  part    (0xCA) first-boot /System payload (XSTG manifest) - optional
#   SYSTEM part    (0xC5) CXFS volume - zeroed; the kernel formats it and copies
#                         the staged files into /System on first boot (model 3).
#
# Usage:
#   python mkdisk.py --out disk.img --size-mb 64 \
#        [--stage1 boot.bin] [--stage2 stage2.bin] [--kernel kernel.xkex] \
#        [--boot-mb 8] [--stage Boot.xoex=executive.xoex ...]
import argparse, struct, os

SECTOR        = 512
XBPT_LBA      = 1
STAGE2_LBA    = 2
XBPT_MAGIC    = b"XBPT"
XBPT_VERSION  = 1
ENTRY_SIZE    = 32
ALIGN_SECTORS = 2048            # 1 MiB partition alignment
PART_CXBOOT   = 0xCB
PART_CXFS     = 0xC5
PART_CXSTAGE  = 0xCA
FLAG_BOOTABLE = 0x01

def align_up(s, a):  return (s + a - 1) // a * a

def make_xbpt(entries, disk_sectors):
    """entries: list of (start_lba, sectors, type, flags, name)"""
    hdr = struct.pack("<4sHHHHQ12x", XBPT_MAGIC, XBPT_VERSION,
                      len(entries), ENTRY_SIZE, 0, disk_sectors)
    body = b""
    for (start, count, ptype, flags, name) in entries:
        nm = name.encode()[:12].ljust(12, b"\x00")
        body += struct.pack("<QQBBH12s", start, count, ptype, flags, 0, nm)
    blob = hdr + body
    assert len(blob) <= SECTOR, "XBPT exceeds one sector"
    return blob.ljust(SECTOR, b"\x00")

def build_stage(files):
    """files: list of (name, data) -> (blob bytes, total_sectors). Header sector
       holds the XSTG manifest; blobs follow, sector-aligned."""
    entries, blobs, cur = [], b"", 1            # sector 0 = header
    for name, data in files:
        entries.append((name, cur, len(data)))
        secs = (len(data) + SECTOR - 1) // SECTOR
        blobs += data.ljust(secs * SECTOR, b"\x00")
        cur += secs
    hdr = struct.pack("<4sHH8x", b"XSTG", 1, len(files))
    for (name, start, size) in entries:
        nm = name.encode()[:32].ljust(32, b"\x00")
        hdr += struct.pack("<32sI8xI", nm, start, size)   # name, start_sector, [pad], size
    if len(hdr) > SECTOR: raise SystemExit("too many staged files for one header sector")
    hdr = hdr.ljust(SECTOR, b"\x00")
    return hdr + blobs, cur

def main():
    ap = argparse.ArgumentParser(description="Build a CXK XBPT disk image")
    ap.add_argument("--out", required=True)
    ap.add_argument("--size-mb", type=int, default=64)
    ap.add_argument("--stage1")
    ap.add_argument("--stage2")
    ap.add_argument("--kernel")
    ap.add_argument("--boot-mb", type=int, default=8)
    ap.add_argument("--stage", nargs="*", default=[], metavar="NAME=PATH",
                    help="files to stage into /System on first boot")
    a = ap.parse_args()

    disk_sectors = a.size_mb * 1024 * 1024 // SECTOR
    img = bytearray(disk_sectors * SECTOR)

    def put(lba, data):
        img[lba*SECTOR : lba*SECTOR + len(data)] = data

    # LBA 0: stage1 (or a placeholder boot sector with just the signature)
    if a.stage1:
        s1 = open(a.stage1, "rb").read()
        if len(s1) > SECTOR: raise SystemExit("stage1 > 512 bytes")
        s1 = s1.ljust(SECTOR, b"\x00")[:510] + b"\x55\xaa"
        put(0, s1)
    else:
        put(510, b"\x55\xaa")
        print("  note: no --stage1; wrote placeholder boot signature only")

    # LBA 2..: stage2 (raw)
    s2_sectors = 0
    if a.stage2:
        s2 = open(a.stage2, "rb").read()
        put(STAGE2_LBA, s2)
        s2_sectors = (len(s2) + SECTOR - 1) // SECTOR

    # parse staged files
    stage_files = []
    for spec in a.stage:
        name, sep, path = spec.partition("=")
        if not sep: raise SystemExit(f"--stage expects NAME=PATH, got '{spec}'")
        stage_files.append((name, open(path, "rb").read()))

    # partition layout: BOOT, [STAGE], SYSTEM
    boot_start = align_up(STAGE2_LBA + s2_sectors, ALIGN_SECTORS)
    boot_len   = a.boot_mb * 1024 * 1024 // SECTOR
    nxt = boot_start + boot_len

    stage_start = stage_len = 0
    if stage_files:
        stage_blob, stage_secs = build_stage(stage_files)
        stage_start = nxt
        stage_len   = align_up(stage_secs, ALIGN_SECTORS)
        put(stage_start, stage_blob)
        nxt = stage_start + stage_len

    sys_start = nxt
    sys_len   = disk_sectors - sys_start
    if sys_len <= 0: raise SystemExit("disk too small for the chosen layout")

    if a.kernel:
        k = open(a.kernel, "rb").read()
        if len(k) > boot_len * SECTOR: raise SystemExit("kernel larger than boot partition")
        put(boot_start, k)

    entries = [(boot_start, boot_len, PART_CXBOOT, FLAG_BOOTABLE, "BOOT")]
    if stage_files:
        entries.append((stage_start, stage_len, PART_CXSTAGE, 0, "STAGE"))
    entries.append((sys_start, sys_len, PART_CXFS, 0, "SYSTEM"))
    put(XBPT_LBA, make_xbpt(entries, disk_sectors))

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    open(a.out, "wb").write(img)

    print(f"wrote {a.out}: {a.size_mb} MB ({disk_sectors} sectors)")
    print(f"  LBA 0      stage1 boot sector")
    print(f"  LBA {XBPT_LBA}      XBPT table ({len(entries)} partitions)")
    print(f"  LBA {STAGE2_LBA}..  stage2 ({s2_sectors} sectors)")
    print(f"  BOOT       LBA {boot_start} .. {boot_start+boot_len-1}  (0x{PART_CXBOOT:02X}, {a.boot_mb} MB, kernel raw)")
    if stage_files:
        names = ", ".join(n for n, _ in stage_files)
        print(f"  STAGE      LBA {stage_start} .. {stage_start+stage_len-1}  (0x{PART_CXSTAGE:02X}, staged: {names})")
    print(f"  SYSTEM     LBA {sys_start} .. {sys_start+sys_len-1}  (0x{PART_CXFS:02X}, {sys_len*SECTOR//1024//1024} MB, CXFS - zeroed)")

if __name__ == "__main__":
    main()