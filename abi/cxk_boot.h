// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /abi/cxk_boot.h */
/* Aurora Tejeda / CATX Systems */
/*
 * CXBI - the boot-loader -> kernel handoff contract.
 *
 * This is to the boot chain what cxk_abi.h is to userspace: the one structure
 * both sides agree on. A loader fills it, puts its physical address in a known
 * register, and jumps; the kernel reads it and never talks to firmware again.
 *
 * ---- Why this exists now -------------------------------------------------
 *
 * The BIOS path gets away without it. stage2 sets a VBE mode and parks the
 * geometry at a fixed physical address, the kernel scans the BIOS areas for the
 * ACPI RSDP, and E820 describes memory. None of that works under UEFI:
 *
 *   - There is no E820. The memory map comes only from GetMemoryMap, and only
 *     before ExitBootServices.
 *   - There is no VBE and no int 10h. The framebuffer comes only from GOP.
 *   - The legacy BIOS areas are usually not populated, so acpi.c's scan of the
 *     EBDA pointer at 0x40E and the 0xE0000-0xFFFFF ROM window finds nothing.
 *     Without the RSDP passed down here, ACPI silently does not come up: no
 *     shutdown, no ACPI reboot, no reset register.
 *
 * So a UEFI stub has to hand these over, and the moment there are two loaders
 * the kernel wants one structure rather than two ingest paths. The BIOS chain
 * can be migrated onto this later and the kernel's side does not change.
 *
 * ---- Design rules, and why ------------------------------------------------
 *
 * 1. EVERY ADDRESS IS 64-BIT, even though a 32-bit kernel only reads the low
 *    half. A UEFI stub runs in long mode and can see memory a 32-bit kernel
 *    cannot address, so the fields have to be able to express what firmware
 *    reported - including values the kernel must then reject. It also means
 *    this structure does not change shape when CXK eventually goes 64-bit.
 *
 * 2. EVERY ADDRESS IS PHYSICAL. The kernel builds its own page tables and the
 *    loader's mappings are gone by the time they matter.
 *
 * 3. VERSIONED, with the size in the header. A kernel can refuse a loader it
 *    does not understand, and fields may only ever be APPENDED - never
 *    reordered, never resized, never repurposed. Same rule cxk_abi.h learned
 *    the hard way: a number is frozen once it ships.
 *
 * 4. THE LAYOUT IS IDENTICAL COMPILED 32-BIT OR 64-BIT. All 64-bit members sit
 *    at 8-aligned offsets so the two ABIs agree without packing (on 32-bit x86
 *    SysV a uint64_t needs only 4-byte alignment, so a careless order would
 *    silently produce two different structures). CXBI_STATIC_ASSERT below
 *    checks it at compile time on both.
 *
 * ---- What a UEFI stub must do before ExitBootServices ---------------------
 *
 *   - GetMemoryMap, normalised into cxk_mmap_entry[] (below).
 *   - Choose a GOP mode and record the framebuffer.
 *   - Find the ACPI RSDP and SMBIOS anchors in the configuration table.
 *   - Load whatever the kernel needs to run - including, usefully, the whole
 *     /System payload off the ESP as a ramdisk. Firmware can read FAT on any
 *     bootable medium, USB included, so doing it here means the kernel never
 *     has to reach the device it booted from. That sidesteps the missing USB
 *     storage driver for boot entirely.
 *   - Allocate ALL of the above below 4 GB (AllocatePages with
 *     AllocateMaxAddress), or a 32-bit kernel cannot address what it is given.
 *     This includes checking the GOP framebuffer: if firmware reports one above
 *     4 GB, choose another mode or fail loudly rather than hand over a pointer
 *     the kernel will fault on.
 */

#ifndef CXK_BOOT_H
#define CXK_BOOT_H

#include <stdint.h>

#define CXBI_MAGIC    0x49425843u   /* "CXBI" little-endian */
#define CXBI_VERSION  1u

/* ---- memory map ----------------------------------------------------------
 * Normalised deliberately: E820 and UEFI describe memory differently, and the
 * kernel should not care which loader it came from. A BIOS loader translates
 * E820 types into these; a UEFI stub translates EfiMemoryType into them. */
#define CXBI_MEM_USABLE        1u   /* free for the PMM */
#define CXBI_MEM_RESERVED      2u   /* firmware or hardware; never touch */
#define CXBI_MEM_ACPI_RECLAIM  3u   /* ACPI tables; usable once parsed */
#define CXBI_MEM_ACPI_NVS      4u   /* must be preserved across sleep */
#define CXBI_MEM_BAD           5u   /* firmware reported it faulty */
#define CXBI_MEM_LOADER        6u   /* the loader's own pages: usable once the
                                       kernel has copied what it wants out,
                                       which is how the ramdisk gets reclaimed */
#define CXBI_MEM_FRAMEBUFFER   7u   /* the LFB; reserved, mapped by the video driver */

struct cxk_mmap_entry {
    uint64_t base;      /* physical */
    uint64_t length;    /* bytes */
    uint32_t type;      /* CXBI_MEM_* */
    uint32_t attr;      /* loader-specific; 0 if unknown */
};

/* ---- firmware the kernel was launched by --------------------------------- */
#define CXBI_FW_BIOS  1u
#define CXBI_FW_UEFI  2u

/* ---- flags --------------------------------------------------------------- */
#define CXBI_FLAG_SECURE_BOOT   (1u << 0)  /* firmware reported Secure Boot on */
#define CXBI_FLAG_KERNEL_VERIFIED (1u << 1)/* the loader checked the kernel's
                                              signature before jumping. On BIOS
                                              nothing does; under Secure Boot a
                                              stub the firmware already verified
                                              can, which is the only way CXK gets
                                              a real root of trust. */
#define CXBI_FLAG_RAMDISK       (1u << 2)  /* ramdisk_addr/size are valid */
#define CXBI_FLAG_FB_ABOVE_4G   (1u << 3)  /* firmware's framebuffer was above
                                              4 GB and has NOT been made usable;
                                              a 32-bit kernel must ignore it
                                              rather than map a truncated address */

/* ---- the handoff structure ----------------------------------------------
 * Field order is load-bearing: the 8-byte members are grouped so every one
 * lands on an 8-aligned offset under both ABIs. Append only. */
struct cxk_boot_info {
    /* identity - first, and never moves */
    uint32_t magic;             /*  0 */
    uint16_t version;           /*  4 */
    uint16_t size;              /*  6  sizeof(struct cxk_boot_info) */

    /* 64-bit block: offsets 8..71, all 8-aligned */
    uint64_t mmap_addr;         /*  8  physical addr of cxk_mmap_entry[] */
    uint64_t fb_addr;           /* 16  physical LFB base; 0 = no framebuffer */
    uint64_t fb_size;           /* 24  bytes */
    uint64_t acpi_rsdp;         /* 32  physical; 0 = not found */
    uint64_t smbios;            /* 40  physical; 0 = not found */
    uint64_t ramdisk_addr;      /* 48  physical; 0 = none */
    uint64_t ramdisk_size;      /* 56  bytes */
    uint64_t rng_seed;          /* 64  firmware entropy; 0 = none */

    /* 32-bit block */
    uint32_t firmware;          /* 72  CXBI_FW_* */
    uint32_t flags;             /* 76  CXBI_FLAG_* */
    uint32_t mmap_entries;      /* 80  count */
    uint32_t mmap_entry_size;   /* 84  sizeof(struct cxk_mmap_entry), so the
                                       kernel can stride a newer, longer entry */
    uint32_t fb_width;          /* 88  pixels */
    uint32_t fb_height;         /* 92 */
    uint32_t fb_pitch;          /* 96  BYTES per scanline, not pixels */
    uint32_t cpuid_max_leaf;    /* 100 */
    uint32_t cpuid1_ecx;        /* 104 leaf 1 feature bits, read by the loader */
    uint32_t cpuid1_edx;        /* 108 */

    /* pixel format, as bit widths and shifts - covers 16bpp 5-6-5 and 32bpp
       8-8-8 without the kernel having to infer either from bpp alone */
    uint8_t  fb_bpp;            /* 112 */
    uint8_t  fb_red_bits;       /* 113 */
    uint8_t  fb_green_bits;     /* 114 */
    uint8_t  fb_blue_bits;      /* 115 */
    uint8_t  fb_red_shift;      /* 116 */
    uint8_t  fb_green_shift;    /* 117 */
    uint8_t  fb_blue_shift;     /* 118 */
    uint8_t  _reserved0;        /* 119 */
};

/* Layout must be identical whether this is compiled for the 32-bit kernel or a
   64-bit loader. If either fires, a field was added or reordered carelessly and
   the two sides no longer agree on where anything is. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
  #define CXBI_STATIC_ASSERT(c, m) _Static_assert(c, m)
#else
  #define CXBI_STATIC_ASSERT(c, m) typedef char cxbi_assert_##__LINE__[(c) ? 1 : -1]
#endif

CXBI_STATIC_ASSERT(sizeof(struct cxk_mmap_entry) == 24,
                   "cxk_mmap_entry must be 24 bytes on every ABI");
CXBI_STATIC_ASSERT(sizeof(struct cxk_boot_info) == 120,
                   "cxk_boot_info must be 120 bytes on every ABI");

/* Spot-check the members most likely to drift if the order is disturbed. */
#if defined(__GNUC__)
CXBI_STATIC_ASSERT(__builtin_offsetof(struct cxk_boot_info, mmap_addr) == 8,
                   "mmap_addr must stay at offset 8");
CXBI_STATIC_ASSERT(__builtin_offsetof(struct cxk_boot_info, acpi_rsdp) == 32,
                   "acpi_rsdp must stay at offset 32");
CXBI_STATIC_ASSERT(__builtin_offsetof(struct cxk_boot_info, firmware) == 72,
                   "the 32-bit block must start at offset 72");
CXBI_STATIC_ASSERT(__builtin_offsetof(struct cxk_boot_info, fb_bpp) == 112,
                   "the pixel-format block must start at offset 112");
#endif

#endif /* CXK_BOOT_H */
