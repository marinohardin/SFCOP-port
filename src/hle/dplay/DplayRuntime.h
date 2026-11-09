// The components the guest shims build each DirectPlay object on (DplayGuest.cpp), given by the
// runtime before the game runs, as the other modules' setPlatform() are.
#pragma once

#include "hle/dplay/DirectPlay.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace sfcop::hle::dplay {

struct Platform {
    platform::Transport *transport = nullptr; // required for a DirectPlay object
    platform::Clock *clock = nullptr;         // required for a DirectPlay object
    platform::Threads *threads = nullptr;     // the service thread, and the players' events
    // The Threads event behind a game's event HANDLE (the kernel module's handle table), or
    // std::nullopt if it names none.
    std::function<std::optional<platform::Waitable>(std::uint32_t)> waitableOfHandle;
};

// Replaces the components for objects created from now on. Until it is called there is no
// Transport, and CoCreateInstance(CLSID_DirectPlay) answers as if DirectPlay were not installed.
void setPlatform(Platform platform);
Platform currentPlatform();

// Whether CoCreateInstance(CLSID_DirectPlay) finds the class: only once the runtime has given a
// Transport and a Clock. Otherwise it answers REGDB_E_CLASSNOTREG, as on a machine without
// DirectPlay.
bool directPlayInstalled(const Platform &platform);

// The object CoCreateInstance(CLSID_DirectPlay) makes, on `platform` through `environmentOf`
// (the guest's waits and SetEvent), or nullptr when the class is not installed.
std::unique_ptr<DirectPlay>
createDirectPlay(const Platform &platform,
                 const std::function<Environment(const Platform &)> &environmentOf, Config config);

// The Environment an object works on. A player's event is signalled through Threads when
// `waitableOfHandle` resolves it, and otherwise through `signalHandle` (the guest's SetEvent);
// `wait` is how a blocking method waits on the game's thread.
Environment environmentFor(const Platform &platform, std::function<void(std::uint32_t)> wait,
                           std::function<void(std::uint32_t)> signalHandle);

} // namespace sfcop::hle::dplay
