# /CXK/tools/CXEX_Compiler/mkcxex.py
# Aurora Tejeda / CATX SYSTEMS LLC
#
# ELF -> CXEX converter. Reads a 32-bit ELF (as produced by i686-elf-ld) and
# emits a CXOS executable (.xkex / .xbex / .xcex) per CX_EXTENSION_SYSTEM.md
# section 9: a fixed CXEX header + a section table + the loadable section bytes.
#
# This is the build-time tool that makes .xkex "real" - the toolchain links as
# ELF (standard), then this repacks the loadable sections into CXOS's own format.
#
# Usage:
#   python CXEX_Compiler/mkcxex.py build/kernel.elf CXEX_Compiler/build/kernel.xkex [--type kernel|boot|os|user]
#
# It is intentionally simple and fixed-load: it does NOT emit relocations (the
# kernel/boot images are loaded at their link address). The relocatable path for
# userspace is reserved for later (the format carries the flag, this tool does
# not yet populate a reloc table).

import sys
import struct

# ---- CXEX constants (must match the kernel-side loader) ----
CXEX_MAGIC = b"CXEX"
FORMAT_VERSION = 1
ARCH_X86_32 = 1

# type_code values (mirror the extension family)
TYPE_KERNEL = 0x4B45   # 'KE' kernel executive
TYPE_BOOT   = 0x4245   # 'BE' boot executive
TYPE_USER   = 0x4345   # 'CE' compiled executive
TYPE_OS     = 0x4F45   # 'OE' OS executive (.xoex)

TYPE_MAP = {"kernel": TYPE_KERNEL, "boot": TYPE_BOOT, "os": TYPE_OS, "user": TYPE_USER}

# header flags (CX_EXTENSION_SYSTEM.md 9.6)
FLAG_EXECUTABLE        = 1 << 0
FLAG_RELOCATABLE       = 1 << 1
FLAG_SIGNED            = 1 << 2
FLAG_KERNEL_PRIV       = 1 << 3
FLAG_REQUIRE_ABI_MATCH = 1 << 4
FLAG_REQUIRE_ARCH_MATCH= 1 << 5

# section flags (9.4)
SEC_READ   = 1 << 0
SEC_WRITE  = 1 << 1
SEC_EXEC   = 1 << 2
SEC_NOBITS = 1 << 3

ABI_VERSION = 1

# ---- ELF parsing (32-bit little-endian, ET_EXEC) ----
# We read the PROGRAM headers (segments), which is what actually defines what
# gets loaded and where - cleaner than section headers for a loadable image.

ELF_MAGIC = b"\x7fELF"
PT_LOAD = 1
PF_X, PF_W, PF_R = 0x1, 0x2, 0x4

def parse_elf(data):
    if data[:4] != ELF_MAGIC:
        raise SystemExit("error: not an ELF file (bad magic)")
    ei_class = data[4]
    ei_data = data[5]
    if ei_class != 1:
        raise SystemExit("error: not a 32-bit ELF (need ELFCLASS32)")
    if ei_data != 1:
        raise SystemExit("error: not little-endian ELF")

    # ELF32 header
    (e_type, e_machine, e_version, e_entry, e_phoff, e_shoff,
     e_flags, e_ehsize, e_phentsize, e_phnum, e_shentsize,
     e_shnum, e_shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)

    if e_machine != 3:  # EM_386
        print("warning: ELF e_machine is not EM_386 (x86); continuing anyway")

    segments = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        (p_type, p_offset, p_vaddr, p_paddr, p_filesz,
         p_memsz, p_flags, p_align) = struct.unpack_from("<IIIIIIII", data, off)
        if p_type == PT_LOAD:
            segments.append({
                "offset": p_offset, "vaddr": p_vaddr, "paddr": p_paddr,
                "filesz": p_filesz, "memsz": p_memsz, "flags": p_flags,
            })
    if not segments:
        raise SystemExit("error: no PT_LOAD segments in ELF")
    return e_entry, segments

def seg_name(flags, idx):
    # derive a short readable name from the segment permissions
    if flags & PF_X:
        n = b".text"
    elif flags & PF_W:
        n = b".data"
    else:
        n = b".rodata"
    return n[:8].ljust(8, b"\x00")

# ---- CXEX header layout (CX_EXTENSION_SYSTEM.md 9.3) ----
# magic(4) type_code(2) format_version(2) arch_target(2) abi_version(2)
# flags(4) entry_point(4) load_base(4) image_min(4) image_max(4)
# section_count(2) section_offset(2) reloc_offset(4) signature_offset(4)
# dependency_offset(4) reserved(8)
HEADER_FMT = "<4sHHHHIIIIIHHIIII4s"   # ...phys_base(4) reserved(4)
HEADER_SIZE = struct.calcsize(HEADER_FMT)

# section table entry (9.4): name(8) file_offset(4) virt_addr(4)
# file_size(4) mem_size(4) flags(4)
SECENT_FMT = "<8sIIIII"
SECENT_SIZE = struct.calcsize(SECENT_FMT)

def build_cxex(elf_data, type_code):
    entry, segments = parse_elf(elf_data)

    image_min = min(s["vaddr"] for s in segments)
    image_max = max(s["vaddr"] + s["memsz"] for s in segments)
    load_base = image_min
    phys_base = min(s["paddr"] for s in segments)   # physical load base (LMA)

    section_count = len(segments)
    section_offset = HEADER_SIZE
    data_start = section_offset + section_count * SECENT_SIZE

    # lay out section data (only the file-backed bytes; NOBITS store nothing)
    sec_entries = []
    blob = b""
    cursor = data_start
    for i, s in enumerate(segments):
        filesz = s["filesz"]
        memsz = s["memsz"]
        flags = 0
        if s["flags"] & PF_R: flags |= SEC_READ
        if s["flags"] & PF_W: flags |= SEC_WRITE
        if s["flags"] & PF_X: flags |= SEC_EXEC

        if filesz == 0:
            # pure NOBITS segment
            file_off = 0
            flags |= SEC_NOBITS
        else:
            file_off = cursor
            seg_bytes = elf_data[s["offset"]: s["offset"] + filesz]
            blob += seg_bytes
            cursor += filesz
            # a segment with memsz > filesz has trailing bss; the loader zero-
            # fills (memsz - filesz). We keep it as one section (not NOBITS),
            # carrying both sizes - the loader handles the zero-fill tail.

        sec_entries.append((seg_name(s["flags"], i), file_off,
                            s["vaddr"], filesz, memsz, flags))

    flags = (FLAG_EXECUTABLE | FLAG_REQUIRE_ABI_MATCH | FLAG_REQUIRE_ARCH_MATCH)
    if type_code == TYPE_KERNEL:
        flags |= FLAG_KERNEL_PRIV

    header = struct.pack(
        HEADER_FMT,
        CXEX_MAGIC, type_code, FORMAT_VERSION, ARCH_X86_32, ABI_VERSION,
        flags, entry, load_base, image_min, image_max,
        section_count, section_offset, 0, 0, 0, phys_base, b"\x00" * 4,
    )

    table = b""
    for (name, foff, vaddr, fsz, msz, fl) in sec_entries:
        table += struct.pack(SECENT_FMT, name, foff, vaddr, fsz, msz, fl)

    return header + table + blob, entry, image_min, image_max, sec_entries, phys_base

def main():
    args = [a for a in sys.argv[1:]]
    type_code = TYPE_KERNEL
    out_args = []
    i = 0
    while i < len(args):
        if args[i] == "--type":
            t = args[i + 1]
            if t not in TYPE_MAP:
                raise SystemExit("error: --type must be kernel|boot|os|user")
            type_code = TYPE_MAP[t]
            i += 2
        else:
            out_args.append(args[i])
            i += 1

    if len(out_args) != 2:
        print("usage: python mkcxex.py <input.elf> <output.xkex> "
              "[--type kernel|boot|os|user]")
        raise SystemExit(2)

    inp, outp = out_args
    with open(inp, "rb") as f:
        elf_data = f.read()

    cxex, entry, imin, imax, secs, pbase = build_cxex(elf_data, type_code)

    with open(outp, "wb") as f:
        f.write(cxex)

    print(f"wrote {outp}: {len(cxex)} bytes")
    print(f"  entry_point = 0x{entry:08x}")
    print(f"  phys_base   = 0x{pbase:08x}")
    print(f"  image span  = 0x{imin:08x} .. 0x{imax:08x} "
          f"({imax - imin} bytes)")
    print(f"  sections    = {len(secs)}")
    for (name, foff, vaddr, fsz, msz, fl) in secs:
        nm = name.rstrip(b"\x00").decode("ascii", "replace")
        bss = f" (+{msz - fsz} bss)" if msz > fsz else ""
        nob = " NOBITS" if (fl & SEC_NOBITS) else ""
        print(f"    {nm:<8} vaddr=0x{vaddr:08x} file={fsz} mem={msz}{bss}{nob}")

if __name__ == "__main__":
    main()