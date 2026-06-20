/* /CXK/kernel/lib/string/logging.h */
/* Aurora Tejeda / CATX Systems LLC */
/*
 * Severity-tagged kernel logging, built on the console primitives. Lines have a
 * fixed, column-aligned shape:
 *
 *     [tag     ] - [SEV ] - message
 *
 * The tag (a subsystem name) is left-aligned in an 8-col field; the severity is
 * one of a fixed set of presets, left-aligned in its own 4-col field. Because both fields are
 * fixed width, the message column lines up across every log line, and child
 * detail lines indent to sit under that same column.
 *
 * Severity presets (name / color):
 *     SEV_INFO  INFO  light blue     SEV_OK    OK    green
 *     SEV_WARN  WARN  yellow         SEV_ERR   ERR   red
 *     SEV_FAIL  FAIL  red
 *
 * NOTE: not pure - drives the console. Lives in lib/string as the text/log
 * layer; relocate beside console.c if that coupling ever matters.
 */

#ifndef LOGGING_H
#define LOGGING_H

#include <stdint.h>
#include "color.h"

/* field widths (tag is capped/left-aligned at LOG_TAG_W; longer tags truncate) */
#define LOG_TAG_W   8
#define LOG_SEV_W   4

/* default color for numeric values printed inside a log line */
#define LOG_COLOR_VALUE  VGA_ATTR(VGA_WHITE, VGA_BLACK)

/* severity presets - keep in sync with sev_tbl[] in logging.c */
typedef enum {
    SEV_INFO = 0,
    SEV_OK,
    SEV_WARN,
    SEV_ERR,
    SEV_FAIL
} log_sev_t;

/* "[ tag ] - [ SEV ] - msg" */
void klog(const char *tag, log_sev_t sev, const char *msg);

/* same, with a value: "...- label<value><suffix>", value in value_attr.
   label/suffix may be NULL. */
void klog_u32(const char *tag, log_sev_t sev,
              const char *label, uint32_t value, uint8_t value_attr,
              const char *suffix);

/* child / detail line, indented to align under the message column. */
void klog_child(const char *msg);
void klog_child_u32(const char *label, uint32_t value, uint8_t value_attr,
                    const char *suffix);

#endif