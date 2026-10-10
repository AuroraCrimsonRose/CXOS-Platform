// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /CXLite/kernel/cpu/pic.h */
/* Aurora Tejeda */

#ifndef PIC_H
#define PIC_H

#include <stdint.h>

/* remap the 8259 PIC so hardware IRQs land on vectors 32..47 */
void pic_remap(void);

/* send end-of-interrupt to the PIC for the given vector (32..47) */
void pic_send_eoi(uint32_t int_no);

/* Allow IRQ `irq` (0..15) to be delivered.
   pic_remap preserves whatever masks the BIOS left, and irq_install_handler only
   records a C function pointer - neither touches the mask - so a line the
   firmware left masked stays silent no matter what handler is installed. Any
   driver claiming an IRQ the BIOS had no reason to enable must call this. */
void pic_unmask(int irq);
void pic_mask(int irq);

/* Mask both chips entirely - see the note in pic.c. */
void pic_mask_all(void);

/* Both chips' masks as one word, bit n set = IRQ n masked. */
uint16_t pic_get_masks(void);

#endif