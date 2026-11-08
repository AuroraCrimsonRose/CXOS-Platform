// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/test/host/panic_qr_dump.c */
/* Aurora Tejeda / CATX Systems */
/* Prints the fixed record's QR code for a real reader to decode
   (PanicScreenTests): the module count, one row of 0s and 1s per line, a line
   "text" and then the text the code should hold. */
#include "diag/panicrep.h"
#include <stdio.h>

int main(void) {
    static struct panic_report r;
    static char text[PANIC_QR_BYTES_MAX];
    static uint8_t qr[PANIC_QR_BUF_LEN], tmp[PANIC_QR_BUF_LEN];

    panic_test_report(&r);
    size_t len = panic_qr_text(&r, text, sizeof text);
    unsigned modules = panic_qr_encode(text, len, qr, tmp);
    if (modules == 0) return 1;

    printf("%u\n", modules);
    for (unsigned y = 0; y < modules; y++) {
        for (unsigned x = 0; x < modules; x++) putchar(panic_qr_module(qr, x, y) ? '1' : '0');
        putchar('\n');
    }
    printf("text\n");
    fwrite(text, 1, len, stdout);
    return 0;
}
