// fakeclock.c: shift the time of day a process reads by $SFCOP_FAKE_CLOCK_OFFSET seconds (macOS,
// loaded with DYLD_INSERT_LIBRARIES). tools/boot_probe.sh -t builds and loads it so that a boot
// run sees a chosen time of day: the game seeds its random numbers from the clock. Only time()
// and gettimeofday() are shifted; the host Clock reads its wall time through gettimeofday. The
// clocks that waits use (clock_gettime, which libc++ turns into the absolute deadlines of timed
// condition-variable waits) and the monotonic clocks are left alone, so waits, timers and the
// watchdog run at the real rate.
//
//   cc -dynamiclib -o fakeclock.dylib tools/fakeclock.c
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

static long offset(void) {
    const char *s = getenv("SFCOP_FAKE_CLOCK_OFFSET");
    return s ? atol(s) : 0;
}

static time_t fake_time(time_t *out) {
    time_t now = time(NULL) + offset();
    if (out) *out = now;
    return now;
}

static int fake_gettimeofday(struct timeval *tv, void *tz) {
    int r = gettimeofday(tv, tz);
    if (r == 0 && tv) tv->tv_sec += offset();
    return r;
}

// dyld's interposing table: each call to the second function from any image goes to the first.
__attribute__((used, section("__DATA,__interpose"))) static const struct {
    const void *replacement;
    const void *original;
} interposers[] = {
    {(const void *)fake_time, (const void *)time},
    {(const void *)fake_gettimeofday, (const void *)gettimeofday},
};
