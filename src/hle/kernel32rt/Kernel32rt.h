// The kernel32rt module: the KERNEL32 rows the game's runtime rests on (threads, synchronisation,
// thread-local storage, the heap and virtual memory, exceptions, time, the environment, module
// handles and the C runtime's start-up), with WINMM's timers, DBGHELP's stack walk and OLE32's
// CoInitialize and CoCreateInstance. This header holds what its parts share: the Win32 values the
// rows return and the kernel object behind a guest HANDLE.
//
// The parts take explicit arguments and work only through Threads and Clock (src/platform/), so
// each is tested without a guest. Kernel32rtGuest.cpp reads the guest's arguments, calls them and
// writes the answers back (src/hle/README.md, "Implementing a module").
#pragma once

#include "platform/Threads/Threads.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace sfcop::hle::kernel32rt {

// Win32 values the rows return, from the Platform SDK headers.
constexpr std::uint32_t kWaitObject0 = 0x00000000u;
constexpr std::uint32_t kWaitAbandoned0 = 0x00000080u;
constexpr std::uint32_t kWaitTimeout = 0x00000102u;
constexpr std::uint32_t kWaitFailed = 0xFFFFFFFFu;
constexpr std::uint32_t kInfinite = 0xFFFFFFFFu;
constexpr std::uint32_t kMaximumWaitObjects = 64;
constexpr std::uint32_t kStillActive = 259;
// The host stack of every thread that runs guest code (the game's threads, the timers' threads).
// Lifted code keeps its locals and each guest call on the host stack, much deeper than the
// platform's default for a secondary thread.
constexpr std::size_t kGuestThreadHostStackBytes = 16u << 20;
constexpr std::uint32_t kTlsOutOfIndexes = 0xFFFFFFFFu;

constexpr std::uint32_t kErrorSuccess = 0;
constexpr std::uint32_t kErrorInvalidFunction = 1;
constexpr std::uint32_t kErrorFileNotFound = 2;
constexpr std::uint32_t kErrorInvalidHandle = 6;
constexpr std::uint32_t kErrorNotEnoughMemory = 8;
constexpr std::uint32_t kErrorInvalidParameter = 87;
constexpr std::uint32_t kErrorAlreadyExists = 183;
constexpr std::uint32_t kErrorNotOwner = 288;
constexpr std::uint32_t kErrorTooManyPosts = 298;
constexpr std::uint32_t kErrorModNotFound = 126;
constexpr std::uint32_t kErrorProcNotFound = 127;

// What a row gives back: the value the guest finds in eax, and the thread's last error if the row
// sets it (std::nullopt leaves it as it was, as Windows does on most successful calls).
struct Answer {
    std::uint32_t value = 0;
    std::optional<std::uint32_t> lastError;
};

// The kernel object a guest HANDLE names. The shim layer keeps one per handle in win32hle's
// handle table and frees it when the guest closes the handle.
enum class Kind { kThread, kEvent, kMutex, kSemaphore };

struct Object {
    Kind kind = Kind::kEvent;
    platform::Waitable waitable;
    int suspendCount = 0; // a thread created suspended: how many ResumeThread calls it waits for
};

} // namespace sfcop::hle::kernel32rt
