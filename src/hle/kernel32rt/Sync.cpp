#include "hle/kernel32rt/Sync.h"

#include "fixes/fixes.h"

#include <utility>

namespace sfcop::hle::kernel32rt {

namespace {

using platform::Error;
using platform::ThreadPriority;
using platform::WaitMode;
using platform::WaitOutcome;

// fix_hle_named_objects: an empty name is no name, as on Windows, so each such object is its own.
// With the fix off the empty name is a name like any other and every such object of a kind is one
// shared object (the diagnosis position). The shared object is kept under a reserved key, one per
// kind, whose first character is a backslash: Windows forbids it in an object name, so the game
// cannot pass a name that collides with it. `key` holds the string `name` is replaced by.
std::string_view effectiveName(std::string_view name, const char *reserved, std::string &key) {
    if (!name.empty() || fixes::isEnabled("fix_hle_named_objects")) {
        return name;
    }
    key = reserved;
    return key;
}

Answer failed(std::uint32_t error, std::uint32_t value = 0) {
    return Answer{value, error};
}

// The Win32 error a failed Threads call becomes.
std::uint32_t errorOf(Error error) {
    switch (error) {
    case Error::kInvalidHandle:
        return kErrorInvalidHandle;
    case Error::kInvalidArgument:
        return kErrorInvalidParameter;
    case Error::kNotOwner:
        return kErrorNotOwner;
    case Error::kUnsupported:
        return kErrorInvalidFunction;
    default:
        return kErrorNotEnoughMemory;
    }
}

std::optional<ThreadPriority> priorityOf(std::int32_t priority) {
    switch (priority) {
    case -15:
        return ThreadPriority::kLowest; // THREAD_PRIORITY_IDLE: the lowest the component has
    case -2:
        return ThreadPriority::kLowest;
    case -1:
        return ThreadPriority::kBelowNormal;
    case 0:
        return ThreadPriority::kNormal;
    case 1:
        return ThreadPriority::kAboveNormal;
    case 2:
        return ThreadPriority::kHighest;
    case 15:
        return ThreadPriority::kTimeCritical;
    default:
        return std::nullopt;
    }
}

} // namespace

Sync::Sync(platform::Threads &threads) : threads_(threads) {}

Answer Sync::StartThread(std::function<std::uint32_t()> body, bool suspended,
                         std::uint32_t stackBytes, Object &out) {
    platform::ThreadOptions options;
    options.startSuspended = suspended;
    // The guest's stack is its own (pcrecomp gives each thread one); the host stack runs the
    // lifted code and is never smaller than kGuestThreadHostStackBytes.
    options.stackBytes = stackBytes > kGuestThreadHostStackBytes ? stackBytes
                                                                  : kGuestThreadHostStackBytes;
    auto started = threads_.StartThread(std::move(body), options);
    if (!started.Ok()) {
        return failed(kErrorNotEnoughMemory);
    }
    out = Object{Kind::kThread, started.Value(), suspended ? 1 : 0};
    return Answer{1, std::nullopt};
}

Answer Sync::ResumeThread(Object &thread) {
    if (thread.kind != Kind::kThread) {
        return failed(kErrorInvalidHandle, 0xFFFFFFFFu);
    }
    const int previous = thread.suspendCount;
    if (previous > 0 && --thread.suspendCount == 0) {
        const auto resumed = threads_.ResumeThread(thread.waitable);
        if (!resumed.Ok()) {
            thread.suspendCount = previous;
            return failed(errorOf(resumed.Code()), 0xFFFFFFFFu);
        }
    }
    return Answer{static_cast<std::uint32_t>(previous), std::nullopt};
}

Answer Sync::SetThreadPriority(const Object &thread, std::int32_t priority) {
    if (thread.kind != Kind::kThread) {
        return failed(kErrorInvalidHandle);
    }
    const auto mapped = priorityOf(priority);
    if (!mapped) {
        return failed(kErrorInvalidParameter);
    }
    const auto set = threads_.SetThreadPriority(thread.waitable, *mapped);
    return set.Ok() ? Answer{1, std::nullopt} : failed(errorOf(set.Code()));
}

Answer Sync::GetExitCode(const Object &thread, std::uint32_t &code) {
    if (thread.kind != Kind::kThread) {
        return failed(kErrorInvalidHandle);
    }
    const auto exit = threads_.GetExitCode(thread.waitable);
    if (exit.Ok()) {
        code = exit.Value();
    } else if (exit.Code() == Error::kWrongState) {
        code = kStillActive;
    } else {
        return failed(errorOf(exit.Code()));
    }
    return Answer{1, std::nullopt};
}

Answer Sync::Make(Kind kind, std::string_view name, platform::Result<platform::Created> made,
                  Object &out) {
    if (!made.Ok()) {
        // A name taken by an object of another kind: Windows answers ERROR_INVALID_HANDLE.
        return failed(made.Code() == Error::kInvalidArgument && !name.empty()
                          ? kErrorInvalidHandle
                          : errorOf(made.Code()));
    }
    out = Object{kind, made.Value().handle, 0};
    if (!name.empty()) {
        std::lock_guard<std::mutex> hold(lock_);
        kinds_.emplace(std::string(name), kind);
        handleNames_[made.Value().handle.value] = std::string(name);
    }
    if (made.Value().alreadyExisted) {
        return Answer{1, kErrorAlreadyExists};
    }
    // Windows clears the last error when the object is new, so that a caller can test it for
    // ERROR_ALREADY_EXISTS.
    return Answer{1, kErrorSuccess};
}

Answer Sync::MakeEvent(bool manualReset, bool initiallySignaled, std::string_view name,
                       Object &out) {
    std::string key;
    name = effectiveName(name, "\\EmptyNameEvent", key);
    return Make(Kind::kEvent, name, threads_.MakeEvent(manualReset, initiallySignaled, name), out);
}

Answer Sync::MakeMutex(bool initiallyOwned, std::string_view name, Object &out) {
    std::string key;
    name = effectiveName(name, "\\EmptyNameMutex", key);
    return Make(Kind::kMutex, name, threads_.MakeMutex(initiallyOwned, name), out);
}

Answer Sync::MakeSemaphore(std::int32_t initialCount, std::int32_t maximumCount,
                           std::string_view name, Object &out) {
    if (maximumCount <= 0 || initialCount < 0 || initialCount > maximumCount) {
        return failed(kErrorInvalidParameter);
    }
    std::string key;
    name = effectiveName(name, "\\EmptyNameSemaphore", key);
    return Make(Kind::kSemaphore, name, threads_.MakeSemaphore(initialCount, maximumCount, name),
                out);
}

Answer Sync::OpenMutex(std::string_view name, Object &out) {
    std::string key;
    name = effectiveName(name, "\\EmptyNameMutex", key);
    {
        std::lock_guard<std::mutex> hold(lock_);
        const auto known = kinds_.find(name);
        if (known != kinds_.end() && known->second != Kind::kMutex) {
            return failed(kErrorInvalidHandle);
        }
    }
    const auto opened = threads_.Open(name);
    if (!opened.Ok()) {
        return failed(opened.Code() == Error::kNotFound ? kErrorFileNotFound
                                                        : errorOf(opened.Code()));
    }
    out = Object{Kind::kMutex, opened.Value(), 0};
    {
        std::lock_guard<std::mutex> hold(lock_);
        handleNames_[opened.Value().value] = std::string(name);
    }
    return Answer{1, std::nullopt};
}

Answer Sync::SetEvent(const Object &event) {
    if (event.kind != Kind::kEvent) {
        return failed(kErrorInvalidHandle);
    }
    const auto set = threads_.SetEvent(event.waitable);
    return set.Ok() ? Answer{1, std::nullopt} : failed(errorOf(set.Code()));
}

Answer Sync::ResetEvent(const Object &event) {
    if (event.kind != Kind::kEvent) {
        return failed(kErrorInvalidHandle);
    }
    const auto reset = threads_.ResetEvent(event.waitable);
    return reset.Ok() ? Answer{1, std::nullopt} : failed(errorOf(reset.Code()));
}

Answer Sync::ReleaseMutex(const Object &mutex) {
    if (mutex.kind != Kind::kMutex) {
        return failed(kErrorInvalidHandle);
    }
    const auto released = threads_.ReleaseMutex(mutex.waitable);
    return released.Ok() ? Answer{1, std::nullopt} : failed(errorOf(released.Code()));
}

Answer Sync::ReleaseSemaphore(const Object &semaphore, std::int32_t count,
                              std::int32_t &previous) {
    if (semaphore.kind != Kind::kSemaphore) {
        return failed(kErrorInvalidHandle);
    }
    if (count < 1) {
        return failed(kErrorInvalidParameter);
    }
    const auto released = threads_.ReleaseSemaphore(semaphore.waitable, count);
    if (!released.Ok()) {
        return failed(released.Code() == Error::kInvalidArgument ? kErrorTooManyPosts
                                                                 : errorOf(released.Code()));
    }
    previous = released.Value();
    return Answer{1, std::nullopt};
}

Answer Sync::Wait(const std::vector<const Object *> &objects, bool waitAll,
                  std::uint32_t timeoutMs) {
    if (objects.empty() || objects.size() > kMaximumWaitObjects) {
        return failed(kErrorInvalidParameter, kWaitFailed);
    }
    std::vector<platform::Waitable> handles;
    handles.reserve(objects.size());
    for (const Object *object : objects) {
        if (object == nullptr) {
            return failed(kErrorInvalidHandle, kWaitFailed);
        }
        handles.push_back(object->waitable);
    }
    const std::optional<std::uint32_t> timeout =
        timeoutMs == kInfinite ? std::nullopt : std::optional<std::uint32_t>(timeoutMs);
    const auto waited = threads_.Wait(handles.data(), handles.size(),
                                      waitAll ? WaitMode::kAll : WaitMode::kAny, timeout);
    if (!waited.Ok()) {
        return failed(errorOf(waited.Code()), kWaitFailed);
    }
    const auto index = static_cast<std::uint32_t>(waited.Value().index);
    switch (waited.Value().outcome) {
    case WaitOutcome::kSignaled:
        return Answer{kWaitObject0 + index, std::nullopt};
    case WaitOutcome::kAbandoned:
        return Answer{kWaitAbandoned0 + index, std::nullopt};
    case WaitOutcome::kTimedOut:
        break;
    }
    return Answer{kWaitTimeout, std::nullopt};
}

void Sync::Close(Object &object) {
    if (object.waitable.Valid()) {
        std::lock_guard<std::mutex> hold(lock_);
        const auto named = handleNames_.find(object.waitable.value);
        if (named != handleNames_.end()) {
            const std::string name = std::move(named->second);
            handleNames_.erase(named);
            bool stillHeld = false;
            for (const auto &entry : handleNames_) {
                stillHeld = stillHeld || entry.second == name;
            }
            if (!stillHeld) {
                kinds_.erase(name); // the last handle: the object is gone, the name is free
            }
        }
        static_cast<void>(threads_.Close(object.waitable)); // CloseHandle reports no error here
        object.waitable = platform::Waitable{};
    }
}

} // namespace sfcop::hle::kernel32rt
