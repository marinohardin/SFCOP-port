// AttachHost: how the guest runtime gives the module its components (User32.h). Lift-free, so
// the runtime and the tests link it without the game.
#include "hle/user32/User32.h"

#include <cstdio>
#include <deque>
#include <thread>

namespace sfcop::hle::user32 {

namespace {

constexpr std::size_t kMaxKeptEvents = 256; // events the host pumped before the User32 existed

struct Attached {
    Host host;
    std::thread::id mainThread;
    bool set = false;
    std::function<void(const platform::WindowEvent &)> sink;
    std::deque<platform::WindowEvent> kept;
};

Attached &attached() {
    static Attached state;
    return state;
}

} // namespace

bool AttachHost(Host host) {
    Attached &state = attached();
    if (state.set) {
        std::fprintf(stderr, "[user32] error: AttachHost called twice\n");
        return false;
    }
    if (host.window == nullptr || host.input == nullptr) {
        std::fprintf(stderr, "[user32] error: AttachHost given no window or input\n");
        return false;
    }
    state.host = std::move(host);
    state.mainThread = std::this_thread::get_id();
    state.set = true;
    return true;
}

const Host *AttachedHost() {
    const Attached &state = attached();
    return state.set ? &state.host : nullptr;
}

bool OnMainThread() {
    const Attached &state = attached();
    return state.set && std::this_thread::get_id() == state.mainThread;
}

void DeliverEvent(const platform::WindowEvent &event) {
    Attached &state = attached();
    if (state.sink) {
        state.sink(event);
        return;
    }
    if (state.kept.size() == kMaxKeptEvents) {
        state.kept.pop_front();
    }
    state.kept.push_back(event);
}

void SetEventSink(std::function<void(const platform::WindowEvent &)> sink) {
    Attached &state = attached();
    state.sink = std::move(sink);
    while (state.sink && !state.kept.empty()) {
        const platform::WindowEvent event = state.kept.front();
        state.kept.pop_front();
        state.sink(event);
    }
}

} // namespace sfcop::hle::user32
