/* /CXK/kernel/lib/cxex.c */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/* CXEX format parsing - explicit-offset, padding-immune. Owns the byte layout. */

#include "cxex.h"

/* little-endian readers (CXEX is little-endian; x86 is too, but we read byte
   by byte so this is correct regardless of host alignment/endianness). */
static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* HEADER field offsets (section 9.3), from mkcxes.py "<4sHHHHIIIIIHHIII8s":
   0 magic | 4 type | 6 fmt_ver | 8 arch | 10 abi | 12 flags | 16 entry |
   20 load_base | 24 image_min | 28 image_max | 32 sec_count | 34 sec_off |
   36 reloc_off | 40 sig_off | 44 dep_off | 48 phys_base | 52 reserved(4) = 56

   NOTE: offset 48 is phys_base, NOT reserved. boot/cxexload.asm depends on it
   (CXH_PHYS_BASE equ 48) to place a higher-half kernel while paging is off.
   Both mkcxes.py and CXEXWriter emit it. Zeroing it breaks the boot chain. */
int cxex_parse_header(const uint8_t *file, size_t len, struct cxex_header *out) {
    if (len < CXEX_HEADER_SIZE) return -1;
    if (!(file[0]==CXEX_MAGIC0 && file[1]==CXEX_MAGIC1 &&
          file[2]==CXEX_MAGIC2 && file[3]==CXEX_MAGIC3)) return -2;

    out->type_code        = rd16(file + 4);
    out->format_version   = rd16(file + 6);
    out->arch_target      = rd16(file + 8);
    out->abi_version      = rd16(file + 10);
    out->flags            = rd32(file + 12);
    out->entry_point      = rd32(file + 16);
    out->load_base        = rd32(file + 20);
    out->image_min        = rd32(file + 24);
    out->image_max        = rd32(file + 28);
    out->section_count    = rd16(file + 32);
    out->section_offset   = rd16(file + 34);
    out->reloc_offset     = rd32(file + 36);
    out->signature_offset = rd32(file + 40);
    out->dependency_offset= rd32(file + 44);
    out->phys_base        = rd32(file + 48);
    return 0;
}

int cxex_check_compat(const struct cxex_header *h,
                      uint16_t my_arch, uint16_t my_abi) {
    if ((h->flags & CXEX_FLAG_REQUIRE_ARCH_MATCH) && h->arch_target != my_arch)
        return -1;
    if ((h->flags & CXEX_FLAG_REQUIRE_ABI_MATCH) && h->abi_version != my_abi)
        return -2;
    return 0;
}

/* SECTION entry offsets (9.4) "<8sIIIII":
   0 name(8) | 8 file_off | 12 virt | 16 file_size | 20 mem_size | 24 flags = 28 */
int cxex_get_section(const uint8_t *file, size_t len,
                     const struct cxex_header *h, uint16_t i,
                     struct cxex_section *out) {
    if (i >= h->section_count) return -1;
    size_t base = (size_t)h->section_offset + (size_t)i * CXEX_SECTION_SIZE;
    if (base + CXEX_SECTION_SIZE > len) return -2;

    const uint8_t *p = file + base;
    for (int k = 0; k < 8; k++) out->name[k] = (char)p[k];
    out->name[8]     = '\0';
    out->file_offset = rd32(p + 8);
    out->virt_addr   = rd32(p + 12);
    out->file_size   = rd32(p + 16);
    out->mem_size    = rd32(p + 20);
    out->flags       = rd32(p + 24);
    return 0;
}

int cxex_is_signed(const struct cxex_header *h) {
    return (h->flags & CXEX_FLAG_SIGNED) && h->signature_offset != 0;
}

/* CXSG block (10.3) "<4sHH32sH" + signature:
   0 magic(4) | 4 sig_algo | 6 hash_algo | 8 fingerprint(32) | 40 sig_len | 42 sig */
int cxex_get_sig(const uint8_t *file, size_t len,
                 const struct cxex_header *h, struct cxex_sig *out) {
    if (!cxex_is_signed(h)) return -1;
    size_t so = h->signature_offset;
    if (so + CXEX_SIG_HDR_SIZE > len) return -2;

    const uint8_t *p = file + so;
    if (!(p[0]==CXSG_MAGIC0 && p[1]==CXSG_MAGIC1 &&
          p[2]==CXSG_MAGIC2 && p[3]==CXSG_MAGIC3)) return -3;

    out->sig_algo  = rd16(p + 4);
    out->hash_algo = rd16(p + 6);
    for (int k = 0; k < 32; k++) out->fingerprint[k] = p[8 + k];
    out->sig_len   = rd16(p + 40);
    out->sig_file_offset = (uint32_t)(so + CXEX_SIG_HDR_SIZE);

    if ((size_t)out->sig_file_offset + out->sig_len > len) return -2;
    return 0;
}