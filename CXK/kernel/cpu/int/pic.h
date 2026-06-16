/* /CXLite/kernel/cpu/pic.h */
/* Aurora Tejeda */

#ifndef PIC_H
#define PIC_H

#include <stdint.h>

/* remap the 8259 PIC so hardware IRQs land on vectors 32..47 */
void pic_remap(void);

/* send end-of-interrupt to the PIC for the given vector (32..47) */
void pic_send_eoi(uint32_t int_no);

#endif