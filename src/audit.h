#ifndef AUDIT_H
#define AUDIT_H

#include <stdint.h>
#include "tlsutil.h"

/*
 * Append-only audit log.
 *
 * All writes are serialised by an internal mutex; entries never interleave.
 * A monotonically-increasing sequence number orders every entry absolutely.
 *
 * Format (pipe-delimited for easy grep/awk):
 *   seq|ISO-8601-ts|EVENT|username|detail|amount|RESULT
 *
 * `amount` is written as "rupees.paise" (e.g. "100.50"); use 0 for
 * non-monetary events.
 */

/* Initialise: open (or create) the log file.  Returns 0/-1. */
int  audit_init(const char *log_path);

/* Flush and close the log file. */
void audit_close(void);

/*
 * Append one entry.  amount_paise may be 0 for non-monetary events.
 * result must be "OK" or "ERR".
 */
void audit_log(const char *event, const char *username,
               const char *detail, int64_t amount_paise,
               const char *result);

/*
 * Stream the last `n` entries for `username` over `conn`.
 * Returns the number of lines written.
 */
int audit_history(const char *username, int n, LCConn *conn);

#endif /* AUDIT_H */
