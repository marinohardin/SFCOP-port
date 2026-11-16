// The client's log, in two files per run. Everything the client prints goes to stderr, which the
// app bundle's launcher writes to the full log (logs/launch-<stamp>.log). A line that is
// IMPORTANT also goes to the important log (logs/important-<same stamp>.log), the few dozen lines a
// player or the owner needs after a crash: the start line, refused or failed script loads,
// low-memory reads, faults and crashes, and the exit with its code. A clean run gives a handful.
//
// The port's own code gives each line its severity here, at the source; a line printed with plain
// fprintf (the original's debug text, the override report, the per-function chatter) is full-log
// only, so nothing routes by what a line says. The important file's path comes from
// SFCOP_IMPORTANT_LOG, set by the launcher; with it unset (a terminal run) there is one stream.
#pragma once

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SFCOP_LOG_DETAIL = 0,   // the full log only (the same as a plain fprintf to stderr)
    SFCOP_LOG_IMPORTANT = 1 // the full log and the important log
};

// Opens the important log named by SFCOP_IMPORTANT_LOG, if set. Call once, early, before threads
// start; a line logged earlier or with no file open goes to the full log only.
void sfcop_log_init(void);

// printf to stderr; at IMPORTANT also appended to the important log, flushed per line.
void sfcop_logf(int severity, const char *format, ...) __attribute__((format(printf, 2, 3)));

#ifdef __cplusplus
}

namespace sfcop::platform {

// The routing itself, on explicit streams: `text` goes to `full`, and at IMPORTANT to `important`
// when it is not null.
void RouteLog(int severity, const char *text, FILE *full, FILE *important);

} // namespace sfcop::platform
#endif
