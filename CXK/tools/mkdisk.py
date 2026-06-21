#!/usr/bin/env python3
# /CXK/tools/mkdisk.py
# Aurora Tejeda / CATX SYSTEMS LLC
#
# Build a CXK disk image with an XBPT partition table (see partition.h). Lays
# down the canonical on-disk layout that the bootloader and kernel expect:
#
#   LBA 0          stage1 boot sector (512B, ends 0xAA55)
#   LBA 1          XBPT partition table (this tool writes it)
#   LBA 2 ..       stage2 (raw, optional)
#   <align 2048>
#   BOOT  part     (type 0xCB) kernel.xkex, raw at the partition start
#   SYSTEM part    (type 0xC5) CXFS volume - left ZEROED here; the kernel formats
#                              it and populates /System on first boot (model 3).
#
# The bootloader stays dumb: it loads stage2 from a fixed early LBA and reads
# this table; it never parses a filesystem.
#
# Usage:
#   python mkdisk.py --out disk.img --size-mb 64 \
#        [--stage1 boot.bin] [--stage2 stage2.bin] [--kernel kernel.xkex] \
#        [--boot-mb 8]
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

def main():
    ap = argparse.ArgumentParser(description="Build a CXK XBPT disk image")
    ap.add_argument("--out", required=True)
    ap.add_argument("--size-mb", type=int, default=64)
    ap.add_argument("--stage1")
    ap.add_argument("--stage2")
    ap.add_argument("--kernel")
    ap.add_argument("--boot-mb", type=int, default=8)
    a = ap.parse_args()

    disk_sectors = a.size_mb * 1024 * 1024 // SECTOR
    img = bytearray(disk_sectors * SECTOR)

    def put(lba, data):
        img[lba*SECTOR : lba*SECTOR + len(data)] = data

    # LBA 0: stage1 (or a placeholder boot sector with just the signature)
    if a.stage1:
        s1 = open(a.stage1, "rb").read()
        if len(s1) > SECTOR: raise SystemExit("stage1 > 512 bytes")
        s1 = s1.ljust(SECTOR, b"\x00")
        s1 = s1[:510] + b"\x55\xaa"
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

    # partition layout
    boot_start = align_up(STAGE2_LBA + s2_sectors, ALIGN_SECTORS)
    boot_len   = a.boot_mb * 1024 * 1024 // SECTOR
    sys_start  = boot_start + boot_len
    sys_len    = disk_sectors - sys_start
    if sys_len <= 0: raise SystemExit("disk too small for the chosen boot size")

    # kernel.xkex raw at the boot partition start
    if a.kernel:
        k = open(a.kernel, "rb").read()
        if len(k) > boot_len * SECTOR: raise SystemExit("kernel larger than boot partition")
        put(boot_start, k)

    # SYSTEM partition stays zeroed -> kernel formats CXFS + populates /System
    entries = [
        (boot_start, boot_len, PART_CXBOOT, FLAG_BOOTABLE, "BOOT"),
        (sys_start,  sys_len,  PART_CXFS,   0,             "SYSTEM"),
    ]
    put(XBPT_LBA, make_xbpt(entries, disk_sectors))

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    open(a.out, "wb").write(img)

    print(f"wrote {a.out}: {a.size_mb} MB ({disk_sectors} sectors)")
    print(f"  LBA 0      stage1 boot sector")
    print(f"  LBA {XBPT_LBA}      XBPT table (2 partitions)")
    print(f"  LBA {STAGE2_LBA}..{STAGE2_LBA+s2_sectors-1 if s2_sectors else STAGE2_LBA}  stage2 ({s2_sectors} sectors)")
    print(f"  BOOT       LBA {boot_start} .. {boot_start+boot_len-1}  (type 0x{PART_CXBOOT:02X}, {a.boot_mb} MB, kernel raw)")
    print(f"  SYSTEM     LBA {sys_start} .. {sys_start+sys_len-1}  (type 0x{PART_CXFS:02X}, {sys_len*SECTOR//1024//1024} MB, CXFS - zeroed)")

if __name__ == "__main__":
    main()