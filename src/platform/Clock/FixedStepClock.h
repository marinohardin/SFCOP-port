// FixedStepClock: a Clock whose ticks and counter advance by a fixed quantum per frame the game
// runs, never by the wall clock, so that two runs of the same input read the same times (the
// replay oracle, DESIGN §5.10). `sfcop --oracle`, `--record-trace` and `--fixed-step MS` install
// it in place of the host clock (src/app/oracle/README.md, "The fixed step").
//
// Ticks(), Counter() and CounterFrequency() answer from one count of 100 ns units (the counter's
// frequency is 10 MHz, as the host clock's), starting at 0. The count grows only:
// - by the quantum on AdvanceFrame(), which the guest host calls once per turn of its pump that
//   read no event from the Window: the turn after which PeekMessageA finds the queue empty and
//   the game runs a frame;
// - by the milliseconds asked on Sleep() from the frame thread (the thread that called
//   BindToCallingThread(), the game's main thread), so that the game's own waits end;
// - by one millisecond when the frame thread has read Ticks() or Counter() kSpinReads times
//   (10,000) since the count last grew: a loop that waits for time to pass without pumping or
//   sleeping would otherwise never end. The count of such steps is SpinSteps().
// Sleep() on any other thread leaves the count alone: those threads run as the scheduler lets
// them, and their waits would carry the wall clock into the count.
//
// The time of day is pinned too: Now() and UnixTime() answer kEpochUnixSeconds plus the count,
// the same in both zones (UTC, a zone offset of 0), so the C runtime's time() and every srand()
// seeded from it read the same values in every run, on every host and in every zone. Reading it
// is not a read of the count: it never adds to the spin guard's reads. Only the real part of
// Sleep() (the thread does sleep, for as long as asked) is the inner clock's.
//
// Threading: every method may be called from any thread at the same time.
#pragma once

#include "platform/Clock/Clock.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

namespace sfcop::platform {

class FixedStepClock final : public Clock {
public:
    // Counter() units per second.
    static constexpr std::uint64_t kFrequency = 10'000'000;
    // Units in one millisecond.
    static constexpr std::uint64_t kUnitsPerMs = kFrequency / 1000;
    // Reads of the frame thread without growth after which the count grows by a millisecond.
    static constexpr std::uint64_t kSpinReads = 10'000;
    // The time of day at a count of 0: 2026-01-01 00:00:00 UTC, a Thursday. Any fixed date the
    // game accepts would do; this one is after every date the game or its files carry.
    static constexpr std::int64_t kEpochUnixSeconds = 1'767'225'600;

    // `inner` sleeps for real; `quantumUnits` (at least 1) is the
    // growth per frame in 100 ns units.
    FixedStepClock(std::unique_ptr<Clock> inner, std::uint64_t quantumUnits);

    // Makes the calling thread the frame thread. Once, from the game's main thread.
    void BindToCallingThread();
    // One frame: the count grows by the quantum.
    void AdvanceFrame();

    std::uint64_t QuantumUnits() const { return quantum_; }
    std::uint64_t Frames() const { return frames_.load(); }
    std::uint64_t SpinSteps() const { return spinSteps_.load(); }
    // The count, as Counter() gives it, without counting as a read.
    std::uint64_t Units() const { return units_.load(); }

    Result<std::uint32_t> Ticks() override;
    Result<std::uint64_t> Counter() override;
    Result<std::uint64_t> CounterFrequency() override;
    Result<CivilTime> Now(TimeZone zone) override;
    Result<std::int64_t> UnixTime() override;
    Status Sleep(std::uint32_t milliseconds) override;

private:
    bool OnFrameThread() const;
    void Grow(std::uint64_t units);
    std::uint64_t Read();

    std::unique_ptr<Clock> inner_;
    const std::uint64_t quantum_;
    std::atomic<std::uint64_t> units_{0};
    std::atomic<std::uint64_t> frames_{0};
    std::atomic<std::uint64_t> spinSteps_{0};
    std::uint64_t readsSinceGrowth_ = 0; // the frame thread's only
    std::atomic<bool> bound_{false};
    std::thread::id frameThread_{};
};

// The UTC date and time `seconds` after 1970-01-01 00:00:00, with `millisecond` as given: the
// pinned time of day's calendar, independent of the host's zone and of its C library.
CivilTime civilTimeFromUnix(std::int64_t seconds, std::uint16_t millisecond);

// The quantum for a frame of `microseconds`, in 100 ns units.
constexpr std::uint64_t fixedStepQuantumFromMicroseconds(std::uint32_t microseconds) {
    return static_cast<std::uint64_t>(microseconds) * 10u;
}

// The default frame: 1000/60 ms, to the microsecond (src/app/oracle/README.md says why).
constexpr std::uint32_t kDefaultFixedStepMicroseconds = 16'667;

} // namespace sfcop::platform
