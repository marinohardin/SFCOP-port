#include "hle/kernel32rt/Time.h"

namespace sfcop::hle::kernel32rt {

namespace {

constexpr std::int64_t kSecondsPerDay = 86'400;
constexpr std::int64_t kFileTimePerMinute = 60 * static_cast<std::int64_t>(kFileTimePerSecond);

// Days since 1970-01-01 of a proleptic Gregorian date (month 1 to 12).
std::int64_t daysFromCivil(std::int64_t year, unsigned month, unsigned day) {
    year -= month <= 2 ? 1 : 0;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const auto yearOfEra = static_cast<unsigned>(year - era * 400);
    const unsigned dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097 + static_cast<std::int64_t>(dayOfEra) - 719468;
}

// The date of a day counted from 1970-01-01.
void civilFromDays(std::int64_t days, std::int64_t &year, unsigned &month, unsigned &day) {
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const auto dayOfEra = static_cast<unsigned>(days - era * 146097);
    const unsigned yearOfEra =
        (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    const unsigned dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const unsigned monthIndex = (5 * dayOfYear + 2) / 153;
    day = dayOfYear - (153 * monthIndex + 2) / 5 + 1;
    month = monthIndex < 10 ? monthIndex + 3 : monthIndex - 9;
    year = static_cast<std::int64_t>(yearOfEra) + era * 400 + (month <= 2 ? 1 : 0);
}

const std::int64_t kDays1601 = daysFromCivil(1601, 1, 1); // -134774

bool leapYear(unsigned year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

unsigned daysInMonth(unsigned year, unsigned month) {
    static const unsigned kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && leapYear(year) ? 29 : kDays[month - 1];
}

SystemTime fromCivil(const platform::CivilTime &civil) {
    SystemTime time;
    time.year = static_cast<std::uint16_t>(civil.year);
    time.month = civil.month;
    time.dayOfWeek = civil.weekday;
    time.day = civil.day;
    time.hour = civil.hour;
    time.minute = civil.minute;
    time.second = civil.second > 59 ? 59 : civil.second; // a leap second has no SYSTEMTIME
    time.milliseconds = civil.millisecond;
    return time;
}

} // namespace

std::optional<std::uint64_t> fileTimeFromSystemTime(const SystemTime &time) {
    if (time.year < 1601 || time.year > 30827 || time.month < 1 || time.month > 12 ||
        time.day < 1 || time.day > daysInMonth(time.year, time.month) || time.hour > 23 ||
        time.minute > 59 || time.second > 59 || time.milliseconds > 999) {
        return std::nullopt;
    }
    const std::int64_t days = daysFromCivil(time.year, time.month, time.day) - kDays1601;
    const std::int64_t seconds =
        days * kSecondsPerDay + time.hour * 3600 + time.minute * 60 + time.second;
    return static_cast<std::uint64_t>(seconds) * kFileTimePerSecond +
           static_cast<std::uint64_t>(time.milliseconds) * 10'000u;
}

std::optional<SystemTime> systemTimeFromFileTime(std::uint64_t fileTime) {
    if (fileTime >> 63) {
        return std::nullopt;
    }
    const auto totalMs = static_cast<std::int64_t>(fileTime / 10'000u);
    const std::int64_t days = totalMs / (kSecondsPerDay * 1000);
    const std::int64_t msOfDay = totalMs % (kSecondsPerDay * 1000);
    std::int64_t year = 0;
    unsigned month = 0;
    unsigned day = 0;
    civilFromDays(days + kDays1601, year, month, day);
    SystemTime time;
    time.year = static_cast<std::uint16_t>(year);
    time.month = static_cast<std::uint16_t>(month);
    time.day = static_cast<std::uint16_t>(day);
    time.dayOfWeek = static_cast<std::uint16_t>((days + 1) % 7); // 1601-01-01 was a Monday
    time.hour = static_cast<std::uint16_t>(msOfDay / 3'600'000);
    time.minute = static_cast<std::uint16_t>(msOfDay / 60'000 % 60);
    time.second = static_cast<std::uint16_t>(msOfDay / 1000 % 60);
    time.milliseconds = static_cast<std::uint16_t>(msOfDay % 1000);
    return time;
}

Time::Time(platform::Clock &clock) : clock_(clock) {}

std::uint32_t Time::TickCount() {
    const auto ticks = clock_.Ticks();
    return ticks.Ok() ? ticks.Value() : 0u;
}

std::uint64_t Time::Counter() {
    const auto counter = clock_.Counter();
    return counter.Ok() ? counter.Value() : 0u;
}

std::uint64_t Time::CounterFrequency() {
    const auto frequency = clock_.CounterFrequency();
    return frequency.Ok() ? frequency.Value() : 0u;
}

SystemTime Time::Now(platform::TimeZone zone) {
    const auto now = clock_.Now(zone);
    if (!now.Ok()) {
        return *systemTimeFromFileTime(0); // 1601-01-01, as a clock that cannot be read
    }
    return fromCivil(now.Value());
}

std::uint64_t Time::FileTimeNow() {
    return fileTimeFromSystemTime(Now(platform::TimeZone::kUtc)).value_or(0);
}

std::int32_t Time::BiasMinutes() {
    const auto utc = fileTimeFromSystemTime(Now(platform::TimeZone::kUtc));
    const auto local = fileTimeFromSystemTime(Now(platform::TimeZone::kLocal));
    if (!utc || !local) {
        return 0;
    }
    const std::int64_t difference =
        static_cast<std::int64_t>(*utc) - static_cast<std::int64_t>(*local);
    // The two readings are a moment apart: round to the nearest minute.
    const std::int64_t half = difference >= 0 ? kFileTimePerMinute / 2 : -kFileTimePerMinute / 2;
    return static_cast<std::int32_t>((difference + half) / kFileTimePerMinute);
}

std::uint64_t Time::LocalFromUtc(std::uint64_t fileTime) {
    return fileTime - static_cast<std::uint64_t>(static_cast<std::int64_t>(BiasMinutes()) *
                                                 kFileTimePerMinute);
}

std::uint64_t Time::UtcFromLocal(std::uint64_t fileTime) {
    return fileTime + static_cast<std::uint64_t>(static_cast<std::int64_t>(BiasMinutes()) *
                                                 kFileTimePerMinute);
}

} // namespace sfcop::hle::kernel32rt
