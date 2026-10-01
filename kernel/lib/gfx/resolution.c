/* /kernel/lib/gfx/resolution.c */
/* Aurora Tejeda / CATX Systems LLC */
/* Resolution catalog + classification. See resolution.h. Pure data + lookups. */

#include "resolution.h"

/* The catalog. Mirrors the preference table in boot/vbe.asm (kept in sync by
   hand). Order here is not significant - lookups are by value, not position. */
static const struct res_info catalog[] = {
    {  320,  200, "CGA",    RES_TIER_SUB_VGA  },
    {  640,  350, "EGA",    RES_TIER_SUB_VGA  },
    {  640,  480, "VGA",    RES_TIER_VGA      },
    {  800,  600, "SVGA",   RES_TIER_SVGA     },
    { 1024,  768, "XGA",    RES_TIER_XGA_PLUS },
    { 1152,  864, "XGA+",   RES_TIER_XGA_PLUS },
    { 1280,  720, "WXGA",   RES_TIER_XGA_PLUS },
    { 1280,  768, "WXGA",   RES_TIER_XGA_PLUS },
    { 1280,  800, "WXGA",   RES_TIER_XGA_PLUS },
    { 1280, 1024, "SXGA",   RES_TIER_XGA_PLUS },
    { 1400, 1050, "SXGA+",  RES_TIER_XGA_PLUS },
    { 1440,  900, "WXGA+",  RES_TIER_XGA_PLUS },
    { 1600,  900, "HD+",    RES_TIER_XGA_PLUS },
    { 1600, 1200, "UXGA",   RES_TIER_XGA_PLUS },
    { 1680, 1050, "WSXGA+", RES_TIER_XGA_PLUS },
    { 1920, 1080, "FHD",    RES_TIER_XGA_PLUS },
};

#define CATALOG_COUNT (sizeof(catalog) / sizeof(catalog[0]))

const struct res_info *res_lookup(uint32_t w, uint32_t h) {
    for (uint32_t i = 0; i < CATALOG_COUNT; i++)
        if (catalog[i].w == w && catalog[i].h == h)
            return &catalog[i];
    return 0;
}

const char *res_name(uint32_t w, uint32_t h) {
    const struct res_info *r = res_lookup(w, h);
    return r ? r->name : "custom";
}

enum res_tier res_classify(uint32_t w, uint32_t h) {
    /* Classify by dimensions (not the catalog) so an off-catalog mode still
       lands in the right tier. Thresholds are the lower bound of each tier. */
    if (w >= 1024 && h >= 768) return RES_TIER_XGA_PLUS;
    if (w >=  800 && h >= 600) return RES_TIER_SVGA;
    if (w >=  640 && h >= 480) return RES_TIER_VGA;
    return RES_TIER_SUB_VGA;
}

int res_is_gui_capable(uint32_t w, uint32_t h) {
    return res_classify(w, h) >= RES_TIER_SVGA;
}