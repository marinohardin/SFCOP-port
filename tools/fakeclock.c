// fakeclock.c: shift a process's wall clock by $SFCOP_FAKE_CLOCK_OFFSET seconds (macOS, loaded
// with DYLD_INSERT_LIBRARIES). tools/boot_probe.sh -t builds and loads it so that a boot run sees
// a chosen time of day: the game seeds its random numbers from the clock. Monotonic clocks are
// left alone, so timers and the watchdog run at the real rate.
//
//   cc -dynamiclib -o fakeclock.dylib tools/fakeclock.c
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

static long offset(void) {
    const char *s = getenv("SFCOP_FAKE_CLOCK_OFFSET");
    return s ? atol(s) : 0;
}

static int fake_clock_gettime(clockid_t clock, struct timespec *ts) {
    int r = clock_gettime(clock, ts);
    if (r == 0 && clock == CLOCK_REALTIME) ts->tv_sec += offset();
    return r;
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
    {(const void *)fake_clock_gettime, (const void *)clock_gettime},
    {(const void *)fake_time, (const void *)time},
    {(const void *)fake_gettimeofday, (const void *)gettimeofday},
};
