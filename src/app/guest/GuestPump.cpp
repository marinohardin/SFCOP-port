#include "GuestPump.h"

#include <chrono>
#include <utility>

namespace sfcop::guest {

EventPump::EventPump(platform::Window &window,
                     std::function<void(const platform::WindowEvent &)> onEvent, MonotonicMs now)
    : window_(window), onEvent_(std::move(onEvent)), now_(std::move(now)) {}

void EventPump::BindToCallingThread() {
    mainThread_ = std::this_thread::get_id();
    bound_ = true;
    boundAtMs_ = now();
    lastTurnMs_ = boundAtMs_;
}

bool EventPump::onBoundThread() const {
    return bound_ && std::this_thread::get_id() == mainThread_;
}

std::uint64_t EventPump::now() const {
    if (now_) {
        return now_();
    }
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

void EventPump::beginTurn(std::uint64_t nowMs, bool insideCall) {
    const std::uint64_t length = nowMs >= lastTurnMs_ ? nowMs - lastTurnMs_ : 0;
    lastTurnMs_ = nowMs;
    Gap gap{nowMs - boundAtMs_, length, insideCall};
    if (length > counts_.longest.lengthMs) {
        counts_.longest = gap;
    }
    if (gapSink_ && length >= gapThresholdMs_) {
        gapSink_(gap);
    }
}

std::uint64_t EventPump::drain() {
    static_cast<void>(window_.PumpEvents());
    std::uint64_t read = 0;
    for (;;) {
        auto event = window_.PollEvent();
        if (!event.Ok() || !event.Value()) {
            break;
        }
        ++read;
        if (onEvent_) {
            onEvent_(*event.Value());
        }
    }
    return read;
}

bool EventPump::Pump() {
    if (!onBoundThread()) {
        // Another guest thread asked: it may yield to the main thread, which pumps in its turn.
        std::this_thread::yield();
        return false;
    }
    beginTurn(now(), false);
    ++counts_.messageTurns;
    const std::uint64_t read = drain();
    frameEnded_ = read == 0 && !readInsideCall_;
    readInsideCall_ = false;
    return true;
}

void EventPump::EnableInsideCalls(std::uint32_t intervalMs) {
    insideCalls_ = true;
    intervalMs_ = intervalMs;
}

bool EventPump::PumpInsideCall() {
    if (!insideCalls_ || !onBoundThread()) {
        return false;
    }
    const std::uint64_t nowMs = now();
    if (nowMs < lastTurnMs_ + intervalMs_) {
        return false;
    }
    beginTurn(nowMs, true);
    ++counts_.insideCallTurns;
    const std::uint64_t read = drain();
    counts_.eventsInsideCalls += read;
    readInsideCall_ = readInsideCall_ || read != 0;
    return true;
}

void EventPump::SetGapTrace(std::uint32_t thresholdMs, std::function<void(const Gap &)> sink) {
    gapThresholdMs_ = thresholdMs;
    gapSink_ = std::move(sink);
}

} // namespace sfcop::guest
