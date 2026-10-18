// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/test/host/test_crashrec.c */
/* Aurora Tejeda / CATX Systems */
/* The XCRL crash record (diag/crashrec.h): what a panic leaves on disk must be
   read back exactly, and anything torn or foreign must be refused. */
#include "khost.h"
#include "diag/crashrec.h"
#include <string.h>

KH_TEST(xcrl_crc32_is_ieee) {
    KH_CHECK_EQ(xcrl_crc32(0, "123456789", 9), 0xCBF43926u);   /* the check value */
    KH_CHECK_EQ(xcrl_crc32(0, "", 0), 0u);
    /* chained in two pieces = in one */
    KH_CHECK_EQ(xcrl_crc32(xcrl_crc32(0, "1234", 4), "56789", 5), 0xCBF43926u);
}

KH_TEST(xcrl_round_trip) {
    static const char text[] = "*** KERNEL PANIC ***\nInvariant violated: test\n";
    uint8_t hdr[XCRL_SECTOR];
    uint16_t kind = 0; uint32_t len = 0;
    xcrl_seal(hdr, XCRL_KIND_CRASH, text, sizeof text - 1);
    KH_CHECK_EQ(xcrl_check_header(hdr, &kind, &len), XCRL_VALID);
    KH_CHECK_EQ(kind, XCRL_KIND_CRASH);
    KH_CHECK_EQ(len, (uint32_t)(sizeof text - 1));
    KH_CHECK_EQ(xcrl_check_text(hdr, text, len), XCRL_VALID);
    for (unsigned i = 16; i < XCRL_SECTOR; i++) KH_CHECK_EQ(hdr[i], 0);

    xcrl_seal(hdr, XCRL_KIND_REQUEST, 0, 0);
    KH_CHECK_EQ(xcrl_check_header(hdr, &kind, &len), XCRL_VALID);
    KH_CHECK_EQ(kind, XCRL_KIND_REQUEST);
    KH_CHECK_EQ(len, 0u);
    KH_CHECK_EQ(xcrl_check_text(hdr, 0, 0), XCRL_VALID);
}

KH_TEST(xcrl_refuses_torn_and_foreign) {
    char text[64]; memcpy(text, "Invariant violated: the record must be whole.", 46);
    uint8_t hdr[XCRL_SECTOR], bad[XCRL_SECTOR];
    xcrl_seal(hdr, XCRL_KIND_CRASH, text, 46);

    memset(bad, 0, sizeof bad);
    KH_CHECK_EQ(xcrl_check_header(bad, 0, 0), XCRL_EMPTY);       /* cleared area */

    text[10] ^= 1;                                               /* text torn */
    KH_CHECK_EQ(xcrl_check_text(hdr, text, 46), XCRL_BAD_CRC);
    text[10] ^= 1;
    KH_CHECK_EQ(xcrl_check_text(hdr, text, 45), XCRL_BAD_CRC);   /* short read */

    memcpy(bad, hdr, sizeof bad); bad[4] = 2;                    /* newer version */
    KH_CHECK_EQ(xcrl_check_header(bad, 0, 0), XCRL_BAD_VER);
    memcpy(bad, hdr, sizeof bad); bad[6] = 7;                    /* unknown kind */
    KH_CHECK_EQ(xcrl_check_header(bad, 0, 0), XCRL_BAD_KIND);
    memcpy(bad, hdr, sizeof bad); bad[10] = 0x01;                /* 64 KB + 46: too long */
    KH_CHECK_EQ(xcrl_check_header(bad, 0, 0), XCRL_BAD_LEN);
    memcpy(bad, hdr, sizeof bad); bad[8] = 1; bad[6] = XCRL_KIND_REQUEST;   /* a request with text */
    KH_CHECK_EQ(xcrl_check_header(bad, 0, 0), XCRL_BAD_LEN);
    memcpy(bad, hdr, sizeof bad); bad[2] ^= 0x20;  /* magic changed */
    KH_CHECK_EQ(xcrl_check_header(bad, 0, 0), XCRL_EMPTY);
    memcpy(bad, hdr, sizeof bad); bad[8] = 45;                   /* header length edited */
    KH_CHECK_EQ(xcrl_check_text(bad, text, 45), XCRL_BAD_CRC);
}

KH_TEST(xcrl_text_is_capped) {
    static char big[XCRL_TEXT_MAX + 100];
    memset(big, 'x', sizeof big);
    uint8_t hdr[XCRL_SECTOR]; uint32_t len = 0;
    xcrl_seal(hdr, XCRL_KIND_CRASH, big, sizeof big);
    KH_CHECK_EQ(xcrl_check_header(hdr, 0, &len), XCRL_VALID);
    KH_CHECK_EQ(len, XCRL_TEXT_MAX);
    KH_CHECK_EQ(xcrl_check_text(hdr, big, len), XCRL_VALID);
}
