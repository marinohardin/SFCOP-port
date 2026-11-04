// TEMPORARY: replaced by Wave 1 B6 (guest runtime)
/*
 * BootHost.h - the temporary boot platform's entry, called by src/app/main.cpp.
 *
 * SFCOP_MARK_WINMAIN and SFCOP_MARK_CDKEY_PROMPT are the guest addresses the
 * lift marks; the build defines both (cmake/SfcopLifter.cmake).
 */
#ifndef SFCOP_BOOT_HOST_H
#define SFCOP_BOOT_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

/* The spike host's command line: --game DIR [--run] [--headless] [--trace]
 * [--watchdog S] [--silent] [--shots DIR] [--shot-every S]. Returns, or exits
 * the process with, 0 on success (a watchdog stop at the CD-key prompt is a
 * success), 1 on a failure, 2 on bad arguments. */
int sfcop_boot_main(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif
