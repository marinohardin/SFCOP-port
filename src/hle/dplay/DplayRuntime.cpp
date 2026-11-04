#include "hle/dplay/DplayRuntime.h"

#include <mutex>

namespace sfcop::hle::dplay {

namespace {

std::mutex g_platformLock;
Platform g_platform;

} // namespace

void setPlatform(Platform platform) {
    std::lock_guard<std::mutex> guard(g_platformLock);
    g_platform = std::move(platform);
}

Platform currentPlatform() {
    std::lock_guard<std::mutex> guard(g_platformLock);
    return g_platform;
}

Environment environmentFor(const Platform &platform, std::function<void(std::uint32_t)> wait,
                           std::function<void(std::uint32_t)> signalHandle) {
    Environment env;
    env.transport = platform.transport;
    env.clock = platform.clock;
    env.threads = platform.threads;
    env.wait = std::move(wait);
    platform::Threads *threads = platform.threads;
    auto resolve = platform.waitableOfHandle;
    env.signal = [threads, resolve, signalHandle](std::uint32_t handle) {
        if (threads != nullptr && resolve) {
            if (std::optional<platform::Waitable> event = resolve(handle)) {
                (void)threads->SetEvent(*event);
                return;
            }
        }
        if (signalHandle) {
            signalHandle(handle);
        }
    };
    return env;
}

} // namespace sfcop::hle::dplay
