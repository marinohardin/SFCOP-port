// The stub backend: compiles on every platform and supports nothing.
#include "platform/Clock/Clock.h"

namespace sfcop::platform {
namespace {

class ClockStub final : public Clock {
public:
    Result<std::uint32_t> Ticks() override { return Error::kUnsupported; }
    Result<std::uint64_t> Counter() override { return Error::kUnsupported; }
    Result<std::uint64_t> CounterFrequency() override { return Error::kUnsupported; }
    Result<CivilTime> Now(TimeZone) override { return Error::kUnsupported; }
    Result<std::int64_t> UnixTime() override { return Error::kUnsupported; }
    Status Sleep(std::uint32_t) override { return Error::kUnsupported; }
};

} // namespace

std::unique_ptr<Clock> makeClockStub() { return std::make_unique<ClockStub>(); }

} // namespace sfcop::platform
