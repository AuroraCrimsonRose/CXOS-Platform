/* /CXK/boot/uefi/cxboot.c */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/*
 * CXK UEFI boot stub - part 1 of 2: gather, fill CXBI, report, halt.
 *
 * This half collects everything the kernel cannot get for itself once firmware
 * is gone, fills a struct cxk_boot_info (abi/cxk_boot.h), prints it and stops.
 * It deliberately does NOT yet load the kernel, exit boot services, or make the
 * long-to-protected transition - that is part 2. Splitting there is on purpose:
 * everything here is observable under OVMF, whereas the transition either works
 * or triple-faults with nothing on screen, so it is worth having this half
 * proven before adding a step that cannot report its own failure.
 *
 * Build (either produces the same PE32+):
 *   MSVC   cl /c /GS- /Gs32768 /nologo /I..\..\abi cxboot.c
 *          link /SUBSYSTEM:EFI_APPLICATION /ENTRY:efi_main /NODEFAULTLIB
 *               /MACHINE:X64 /OUT:BOOTX64.EFI cxboot.obj
 *   clang  clang -target x86_64-unknown-windows -ffreestanding -fshort-wchar
 *                -mno-red-zone -I../../abi -c cxboot.c -o cxboot.o
 *          lld-link -subsystem:efi_application -entry:efi_main -nodefaultlib
 *                   -out:BOOTX64.EFI cxboot.o
 *
 * -mno-red-zone / /Gs matters: firmware interrupts run on this stack.
 * -fshort-wchar matters: UEFI strings are UTF-16, and without it L"..." is
 * 32-bit and every string prints as garbage.
 */

#include "efi.h"
#include "cxk_boot.h"

/* ---- globals set up by efi_main ----------------------------------------- */
static EFI_SYSTEM_TABLE                 *ST;
static EFI_BOOT_SERVICES                *BS;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *OUT;

/* ---- console ------------------------------------------------------------- */
static void print(CHAR16 *s) { OUT->OutputString(OUT, s); }

static void print_hex(UINT64 v) {
    CHAR16 buf[19];
    const CHAR16 *digits = L"0123456789ABCDEF";
    int i;
    buf[0] = L'0'; buf[1] = L'x';
    for (i = 0; i < 16; i++)
        buf[2 + i] = digits[(v >> ((15 - i) * 4)) & 0xF];
    buf[18] = 0;
    print(buf);
}

static void print_dec(UINT64 v) {
    CHAR16 buf[21];
    int i = 20;
    buf[20] = 0;
    if (v == 0) { print(L"0"); return; }
    while (v && i > 0) { buf[--i] = (CHAR16)(L'0' + (v % 10)); v /= 10; }
    print(&buf[i]);
}

/* ---- GUID compare -------------------------------------------------------- */
static int guid_eq(const EFI_GUID *a, const EFI_GUID *b) {
    const UINT8 *p = (const UINT8 *)a, *q = (const UINT8 *)b;
    for (int i = 0; i < 16; i++) if (p[i] != q[i]) return 0;
    return 1;
}

/* ---- CPUID --------------------------------------------------------------
 * The guard is on MSVC *and not clang*, because clang targeting Windows defines
 * _MSC_VER too while still supporting GCC-style inline asm. Testing _MSC_VER
 * alone sends clang down the intrinsic path and fails to build.
 *
 * The intrinsic is declared rather than pulled in via <intrin.h>, to keep this
 * translation unit free of any system header. */
#if defined(_MSC_VER) && !defined(__clang__)
void __cpuid(int CPUInfo[4], int InfoType);
#pragma intrinsic(__cpuid)
#endif

static void cpuid(UINT32 leaf, UINT32 *a, UINT32 *b, UINT32 *c, UINT32 *d) {
#if defined(_MSC_VER) && !defined(__clang__)
    int r[4];
    __cpuid(r, (int)leaf);
    *a = (UINT32)r[0]; *b = (UINT32)r[1]; *c = (UINT32)r[2]; *d = (UINT32)r[3];
#else
    __asm__ volatile ("cpuid"
                      : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                      : "a"(leaf), "c"(0));
#endif
}

/* ---- map a UEFI memory type onto a CXBI one ------------------------------
 * Normalising here rather than passing EFI types through is the whole point:
 * the kernel gets one ingest path whichever loader it came from.
 *
 * BootServicesCode/Data become USABLE because they are free the moment
 * ExitBootServices returns, and they are usually the largest free regions on
 * the machine - treating them as reserved would throw away most of RAM. */
static UINT32 classify(UINT32 efi_type) {
    switch (efi_type) {
        case EfiConventionalMemory:
        case EfiBootServicesCode:
        case EfiBootServicesData:
            return CXBI_MEM_USABLE;
        case EfiLoaderCode:
        case EfiLoaderData:
            return CXBI_MEM_LOADER;      /* ours; reclaimable once copied out */
        case EfiACPIReclaimMemory:
            return CXBI_MEM_ACPI_RECLAIM;
        case EfiACPIMemoryNVS:
            return CXBI_MEM_ACPI_NVS;
        case EfiUnusableMemory:
            return CXBI_MEM_BAD;
        default:
            /* Reserved, RuntimeServices*, MemoryMappedIO*, PalCode, Persistent:
               all of it must be left alone by the PMM. */
            return CXBI_MEM_RESERVED;
    }
}

/* ---- allocate below 4 GB -------------------------------------------------
 * Every byte handed to a 32-bit kernel has to be addressable by it. UEFI will
 * happily satisfy AllocateAnyPages from above 4 GB on a machine with enough
 * RAM, and the resulting pointer truncates silently. AllocateMaxAddress is the
 * whole guard. */
static void *alloc_low(UINTN bytes) {
    EFI_PHYSICAL_ADDRESS addr = 0xFFFFFFFFULL;      /* ceiling, not a request */
    UINTN pages = (bytes + 0xFFF) / 0x1000;
    if (BS->AllocatePages(AllocateMaxAddress, EfiLoaderData, pages, &addr) != EFI_SUCCESS)
        return 0;
    return (void *)(UINTN)addr;
}

/* ---- firmware tables ----------------------------------------------------- */
static void find_tables(struct cxk_boot_info *bi) {
    EFI_GUID acpi20 = EFI_ACPI_20_TABLE_GUID;
    EFI_GUID acpi10 = EFI_ACPI_10_TABLE_GUID;
    EFI_GUID smbios3 = SMBIOS3_TABLE_GUID;
    EFI_GUID smbios  = SMBIOS_TABLE_GUID;

    for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *e = &ST->ConfigurationTable[i];
        /* ACPI 2.0 wins over 1.0 where both are present: the XSDT it points at
           carries 64-bit table addresses and is what any modern machine means. */
        if (guid_eq(&e->VendorGuid, &acpi20))
            bi->acpi_rsdp = (UINT64)(UINTN)e->VendorTable;
        else if (guid_eq(&e->VendorGuid, &acpi10) && bi->acpi_rsdp == 0)
            bi->acpi_rsdp = (UINT64)(UINTN)e->VendorTable;
        else if (guid_eq(&e->VendorGuid, &smbios3))
            bi->smbios = (UINT64)(UINTN)e->VendorTable;
        else if (guid_eq(&e->VendorGuid, &smbios) && bi->smbios == 0)
            bi->smbios = (UINT64)(UINTN)e->VendorTable;
    }
}

/* ---- graphics ------------------------------------------------------------
 * Picks the current mode rather than hunting for a preferred resolution: this
 * half only reports, and choosing a mode is a decision worth making once, in
 * part 2, alongside the kernel's preference list. What it does do is refuse a
 * framebuffer the kernel cannot reach. */
static void find_framebuffer(struct cxk_boot_info *bi) {
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;

    if (BS->LocateProtocol(&gop_guid, 0, (void **)&gop) != EFI_SUCCESS || !gop)
        return;                                  /* no GOP: fb_addr stays 0 */

    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi = gop->Mode->Info;

    UINT64 base = gop->Mode->FrameBufferBase;
    UINT64 size = (UINT64)gop->Mode->FrameBufferSize;

    /* A 32-bit kernel cannot map this at all. Flag it and leave fb_addr zero
       rather than hand over a truncated pointer it would fault on. Part 2 can
       try other modes; some firmware places only certain modes high. */
    if (base + size > 0x100000000ULL) {
        bi->flags |= CXBI_FLAG_FB_ABOVE_4G;
        return;
    }

    bi->fb_addr   = base;
    bi->fb_size   = size;
    bi->fb_width  = mi->HorizontalResolution;
    bi->fb_height = mi->VerticalResolution;
    /* GOP reports a stride in PIXELS; CXBI carries BYTES, as fb.c wants. */
    bi->fb_pitch  = mi->PixelsPerScanLine * 4;
    bi->fb_bpp    = 32;

    switch (mi->PixelFormat) {
        case PixelRedGreenBlueReserved8BitPerColor:
            bi->fb_red_bits = bi->fb_green_bits = bi->fb_blue_bits = 8;
            bi->fb_red_shift = 0; bi->fb_green_shift = 8; bi->fb_blue_shift = 16;
            break;
        case PixelBlueGreenRedReserved8BitPerColor:
            bi->fb_red_bits = bi->fb_green_bits = bi->fb_blue_bits = 8;
            bi->fb_red_shift = 16; bi->fb_green_shift = 8; bi->fb_blue_shift = 0;
            break;
        default:
            /* PixelBitMask needs the masks decoding; PixelBltOnly has no linear
               framebuffer at all and is unusable here. Neither is handled yet,
               so report no framebuffer rather than a wrong one. */
            bi->fb_addr = 0;
            bi->fb_size = 0;
            break;
    }
}

/* ---- memory map ----------------------------------------------------------
 * Two calls by design: the first fails with BUFFER_TOO_SMALL and tells us the
 * size. The buffer is then over-allocated, because allocating it can itself
 * fragment the map and make it larger than the size just reported. */
static int build_memory_map(struct cxk_boot_info *bi, UINTN *out_map_key) {
    UINTN size = 0, map_key = 0, desc_size = 0;
    UINT32 desc_ver = 0;
    EFI_MEMORY_DESCRIPTOR *map = 0;

    EFI_STATUS st = BS->GetMemoryMap(&size, 0, &map_key, &desc_size, &desc_ver);
    if (st != EFI_BUFFER_TOO_SMALL) return 0;

    size += 8 * desc_size;                        /* headroom for the churn */
    if (BS->AllocatePool(EfiLoaderData, size, (void **)&map) != EFI_SUCCESS) return 0;
    if (BS->GetMemoryMap(&size, map, &map_key, &desc_size, &desc_ver) != EFI_SUCCESS) {
        BS->FreePool(map);
        return 0;
    }

    UINTN count = size / desc_size;
    struct cxk_mmap_entry *dst = alloc_low(count * sizeof(struct cxk_mmap_entry));
    if (!dst) { BS->FreePool(map); return 0; }

    for (UINTN i = 0; i < count; i++) {
        /* Stride by desc_size, never sizeof: firmware may report a LARGER
           descriptor than this build knows about, and indexing by sizeof would
           walk the map crooked. The spec is explicit about this. */
        EFI_MEMORY_DESCRIPTOR *d =
            (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)map + i * desc_size);
        dst[i].base   = d->PhysicalStart;
        dst[i].length = d->NumberOfPages * 0x1000ULL;
        dst[i].type   = classify(d->Type);
        dst[i].attr   = (UINT32)d->Attribute;
    }

    bi->mmap_addr       = (UINT64)(UINTN)dst;
    bi->mmap_entries    = (UINT32)count;
    bi->mmap_entry_size = (UINT32)sizeof(struct cxk_mmap_entry);
    *out_map_key = map_key;
    BS->FreePool(map);
    return 1;
}

/* ---- Secure Boot --------------------------------------------------------- */
static void find_secure_boot(struct cxk_boot_info *bi) {
    EFI_GUID gv = EFI_GLOBAL_VARIABLE_GUID;
    UINT8 v = 0;
    UINTN sz = sizeof(v);
    if (ST->RuntimeServices->GetVariable(L"SecureBoot", &gv, 0, &sz, &v) == EFI_SUCCESS && v == 1)
        bi->flags |= CXBI_FLAG_SECURE_BOOT;
}

/* ---- report -------------------------------------------------------------- */
static void report(struct cxk_boot_info *bi) {
    print(L"\r\n=== CXK UEFI stub: CXBI gathered ===\r\n");

    print(L"  struct      : "); print_dec(bi->size);
    print(L" bytes, version "); print_dec(bi->version); print(L"\r\n");

    print(L"  memory map  : "); print_dec(bi->mmap_entries);
    print(L" entries at "); print_hex(bi->mmap_addr); print(L"\r\n");

    UINT64 usable = 0;
    struct cxk_mmap_entry *m = (struct cxk_mmap_entry *)(UINTN)bi->mmap_addr;
    for (UINT32 i = 0; i < bi->mmap_entries; i++)
        if (m[i].type == CXBI_MEM_USABLE) usable += m[i].length;
    print(L"  usable RAM  : "); print_dec(usable / (1024 * 1024)); print(L" MiB\r\n");

    print(L"  framebuffer : ");
    if (bi->fb_addr) {
        print_dec(bi->fb_width); print(L"x"); print_dec(bi->fb_height);
        print(L"x"); print_dec(bi->fb_bpp);
        print(L" pitch "); print_dec(bi->fb_pitch);
        print(L" @ "); print_hex(bi->fb_addr); print(L"\r\n");
    } else if (bi->flags & CXBI_FLAG_FB_ABOVE_4G) {
        print(L"REJECTED - above 4 GB, unreachable from a 32-bit kernel\r\n");
    } else {
        print(L"none\r\n");
    }

    print(L"  ACPI RSDP   : "); print_hex(bi->acpi_rsdp);
    if (!bi->acpi_rsdp) print(L"  (none - ACPI would be dead on this machine)");
    print(L"\r\n");
    print(L"  SMBIOS      : "); print_hex(bi->smbios); print(L"\r\n");
    print(L"  CPUID max   : "); print_dec(bi->cpuid_max_leaf);
    print(L"  edx "); print_hex(bi->cpuid1_edx); print(L"\r\n");
    print(L"  Secure Boot : ");
    print((bi->flags & CXBI_FLAG_SECURE_BOOT) ? L"ON\r\n" : L"off\r\n");
    print(L"  flags       : "); print_hex(bi->flags); print(L"\r\n");

    print(L"\r\n  part 1 complete - not exiting boot services.\r\n");
}

/* ---- entry --------------------------------------------------------------- */
EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    (void)ImageHandle;
    ST  = SystemTable;
    BS  = ST->BootServices;
    OUT = ST->ConOut;

    print(L"CXK UEFI boot stub\r\n");

    struct cxk_boot_info bi;
    for (UINTN i = 0; i < sizeof bi; i++) ((UINT8 *)&bi)[i] = 0;

    bi.magic    = CXBI_MAGIC;
    bi.version  = CXBI_VERSION;
    bi.size     = (UINT16)sizeof(struct cxk_boot_info);
    bi.firmware = CXBI_FW_UEFI;

    UINT32 a, b, c, d;
    cpuid(0, &a, &b, &c, &d);
    bi.cpuid_max_leaf = a;
    cpuid(1, &a, &b, &c, &d);
    bi.cpuid1_ecx = c;
    bi.cpuid1_edx = d;

    find_tables(&bi);
    find_secure_boot(&bi);
    find_framebuffer(&bi);

    UINTN map_key = 0;
    if (!build_memory_map(&bi, &map_key))
        print(L"  WARNING: memory map unavailable\r\n");

    report(&bi);

    /* Part 2 continues from here: load kernel.xkex and the /System payload off
       the ESP (below 4 GB), verify the kernel, ExitBootServices(map_key), build
       identity paging, leave long mode and jump with &bi in a known register.
       Note map_key is only valid until the next allocation - ExitBootServices
       must follow the final GetMemoryMap immediately. */
    (void)map_key;

    for (;;) __asm__ volatile ("hlt");
}
