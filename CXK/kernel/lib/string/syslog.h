/* /CXK/kernel/lib/string/syslog.h */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/*
 * Structured system logger - kernel-side recorder. Every klog() and slog() call
 * lands in an in-memory ring buffer of fixed, pointer-free entries
 * {seq, sev, system, subsystem, event}, so the log survives a non-fatal fault
 * and can be dumped to screen (RSOD) or flushed to CXFS for post-mortem review.
 * The executive owns persistence policy + querying on top of this (model B);
 * the kernel just records, from the first instruction onward.
 */
#ifndef SYSLOG_H
#define SYSLOG_H
#include <stdint.h>
#include "logging.h"   /* log_sev_t */

#define SLOG_SYS_W   12
#define SLOG_SUB_W   12
#define SLOG_EVT_W   48
#define SLOG_RING    256   /* entries retained (ring wraps) */

struct slog_entry {
    uint32_t seq;                 /* monotonic order */
    uint8_t  sev;                 /* log_sev_t */
    uint8_t  _pad[3];
    char     system[SLOG_SYS_W];
    char     subsystem[SLOG_SUB_W];
    char     event[SLOG_EVT_W];
};

/* record a structured event (system - subsystem - event). subsystem may be "". */
void slog(const char *system, const char *subsystem, log_sev_t sev, const char *event);

/* low-level record (no console output); used by klog to mirror into the ring. */
extern int slog_in_progress;   /* set while slog() drives klog, to avoid double-record */
void slog_record(const char *system, const char *subsystem, log_sev_t sev, const char *event);

/* dump the ring (oldest -> newest) to the console, e.g. on an RSOD. */
void slog_dump_console(void);

/* number of entries currently retained + total ever recorded. */
uint32_t slog_count(void);
uint32_t slog_total(void);

/* read entry i (0 = oldest retained). Returns 0 if out of range. */
const struct slog_entry *slog_get(uint32_t i);

/* best-effort: write the ring (as text) to /System/crash.log; survives reboot.
   Guarded against re-entry (a secondary fault during flush). Returns 0 on success. */
int slog_flush_crash(void);

#endif