#include "platform/Clock/FixedStepClock.h"

#include <utility>

namespace sfcop::platform {

CivilTime civilTimeFromUnix(std::int64_t seconds, std::uint16_t millisecond) {
    // Days and seconds of the day, flooring towards the past for a time before 1970.
    std::int64_t days = seconds / 86'400;
    std::int64_t ofDay = seconds % 86'400;
    if (ofDay < 0) {
        ofDay += 86'400;
        --days;
    }
    CivilTime civil;
    // 1970-01-01 was a Thursday (4).
    civil.weekday = static_cast<std::uint8_t>(((days % 7) + 11) % 7);
    // The proleptic Gregorian calendar counted in 400-year eras from 0000-03-01, so that the leap
    // day is the last day of its year.
    const std::int64_t shifted = days + 719'468; // days from 0000-03-01 to 1970-01-01
    const std::int64_t era = (shifted >= 0 ? shifted : shifted - 146'096) / 146'097;
    const std::int64_t dayOfEra = shifted - era * 146'097;                           // 0 to 146,096
    const std::int64_t yearOfEra =
        (dayOfEra - dayOfEra / 1'460 + dayOfEra / 36'524 - dayOfEra / 146'096) / 365; // 0 to 399
    const std::int64_t dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const std::int64_t monthFromMarch = (5 * dayOfYear + 2) / 153; // 0 = March to 11 = February
    civil.day = static_cast<std::uint8_t>(dayOfYear - (153 * monthFromMarch + 2) / 5 + 1);
    civil.month = static_cast<std::uint8_t>(monthFromMarch < 10 ? monthFromMarch + 3 : monthFromMarch - 9);
    civil.year = static_cast<std::int32_t>(yearOfEra + era * 400 + (civil.month <= 2 ? 1 : 0));
    civil.hour = static_cast<std::uint8_t>(ofDay / 3'600);
    civil.minute = static_cast<std::uint8_t>(ofDay / 60 % 60);
    civil.second = static_cast<std::uint8_t>(ofDay % 60);
    civil.millisecond = millisecond;
    return civil;
}

FixedStepClock::FixedStepClock(std::unique_ptr<Clock> inner, std::uint64_t quantumUnits)
    : inner_(std::move(inner)), quantum_(quantumUnits == 0 ? 1 : quantumUnits) {}

void FixedStepClock::BindToCallingThread() {
    frameThread_ = std::this_thread::get_id();
    bound_.store(true);
}

bool FixedStepClock::OnFrameThread() const {
    return bound_.load() && std::this_thread::get_id() == frameThread_;
}

void FixedStepClock::Grow(std::uint64_t units) {
    units_.fetch_add(units);
    if (OnFrameThread()) {
        readsSinceGrowth_ = 0;
    }
}

void FixedStepClock::AdvanceFrame() {
    frames_.fetch_add(1);
    Grow(quantum_);
}

std::uint64_t FixedStepClock::Read() {
    if (OnFrameThread() && ++readsSinceGrowth_ >= kSpinReads) {
        spinSteps_.fetch_add(1);
        Grow(kUnitsPerMs);
    }
    return units_.load();
}

Result<std::uint32_t> FixedStepClock::Ticks() {
    // Truncated to 32 bits: the counter wraps as the game's tick counters do.
    return static_cast<std::uint32_t>(Read() / kUnitsPerMs);
}

Result<std::uint64_t> FixedStepClock::Counter() { return Read(); }

Result<std::uint64_t> FixedStepClock::CounterFrequency() { return kFrequency; }

Result<CivilTime> FixedStepClock::Now(TimeZone /*zone: both are UTC on the pinned clock*/) {
    const std::uint64_t units = units_.load(); // not a read: the spin guard counts Ticks/Counter
    return civilTimeFromUnix(kEpochUnixSeconds + static_cast<std::int64_t>(units / kFrequency),
                             static_cast<std::uint16_t>(units / kUnitsPerMs % 1000));
}

Result<std::int64_t> FixedStepClock::UnixTime() {
    return kEpochUnixSeconds + static_cast<std::int64_t>(units_.load() / kFrequency);
}

Status FixedStepClock::Sleep(std::uint32_t milliseconds) {
    if (milliseconds != 0 && OnFrameThread()) {
        Grow(static_cast<std::uint64_t>(milliseconds) * kUnitsPerMs);
    }
    if (!inner_) {
        return {};
    }
    return inner_->Sleep(milliseconds);
}

} // namespace sfcop::platform
