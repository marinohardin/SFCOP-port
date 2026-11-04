// Threads: the threads and synchronisation objects the kernel HLE module builds the game's
// CreateThread, events, mutexes, semaphores, waits and thread-local storage on (DESIGN §4.7,
// §6.2, §7.3). Threads, events, mutexes and semaphores share one handle space, Waitable, so that
// one Wait() serves WaitForSingleObject and WaitForMultipleObjects. Mutexes are recursive and
// owned by a thread, as the game's mutexes and critical sections are.
//
// Method names avoid the Windows SDK's A/W macro names (CreateEvent, CreateMutex, ...) so that
// the Windows backend can include <windows.h> and this header in one file.
//
// A periodic timer (the game's timeSetEvent) is not a method here; the winmm HLE module composes
// it from Threads and Clock: StartThread() a timer thread that loops on
// Wait({stop event}, period - elapsed) and, on each timeout, reads Clock::Ticks(), runs the
// callback and computes the next due time from the previous due time (not from "now"), so the
// period does not drift; timeKillEvent sets the stop event and waits on the thread's handle. A
// one-shot timer is the same loop run once.
//
// Threading: every method may be called from any thread at the same time, including from a
// thread this component created.
#pragma once

#include "platform/Core/Handle.h"
#include "platform/Core/Result.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace sfcop::platform {

struct WaitableTag {};
using Waitable = Handle<WaitableTag>; // a thread, event, mutex or semaphore

enum class ThreadPriority { kLowest, kBelowNormal, kNormal, kAboveNormal, kHighest, kTimeCritical };

struct ThreadOptions {
    std::string name;            // for debuggers and logs only; may be empty
    bool startSuspended = false; // the body does not run until ResumeThread()
    std::size_t stackBytes = 0;  // 0 = the platform default
};

// The result of creating a named object: the handle, and whether an object of that name existed
// already (in which case the handle refers to it and the creation arguments were ignored).
struct Created {
    Waitable handle;
    bool alreadyExisted = false;
};

enum class WaitMode { kAny, kAll };

enum class WaitOutcome {
    kSignaled,  // the object (index) was signalled, or all were for kAll
    kAbandoned, // the object (index) is a mutex whose owner thread ended without releasing it
    kTimedOut,  // the timeout passed first
};

struct WaitResult {
    WaitOutcome outcome = WaitOutcome::kTimedOut;
    std::size_t index = 0; // for kAny: which handle; for kAll: 0
};

class Threads {
public:
    virtual ~Threads() = default;

    // Starts a thread that runs `body` and then ends; the value `body` returns is the thread's
    // exit code (the kernel HLE implements the game's ExitThread by making the body return it).
    // The handle becomes signalled when the thread ends. With options.startSuspended the body
    // waits for ResumeThread(). Errors: kUnsupported; kInvalidArgument if `body` is empty;
    // kOutOfResources if the system refuses a thread.
    virtual Result<Waitable> StartThread(std::function<std::uint32_t()> body,
                                         const ThreadOptions &options) = 0;

    // The exit code of an ended thread. Joining is Wait() on the handle, then this. The code
    // stays readable until the handle is closed. Errors: kUnsupported; kInvalidHandle if
    // `thread` is not a thread; kWrongState while the thread has not ended.
    virtual Result<std::uint32_t> GetExitCode(Waitable thread) = 0;

    // Lets a thread created suspended start running. Resuming a running or ended thread does
    // nothing and succeeds. Errors: kUnsupported; kInvalidHandle if `thread` is not a thread.
    virtual Status ResumeThread(Waitable thread) = 0;

    // Asks the scheduler for a priority. The platform may ignore it (macOS and Linux limit what a
    // user process can raise); the call still succeeds. Errors: kUnsupported; kInvalidHandle.
    virtual Status SetThreadPriority(Waitable thread, ThreadPriority priority) = 0;

    // A number identifying the calling thread, unique among live threads and never 0. Threads
    // this component did not create (the main thread, SDL's, miniaudio's) have one too.
    // Errors: kUnsupported.
    virtual Result<std::uint32_t> CurrentThreadId() = 0;

    // Creates an event. A manual-reset event stays signalled until ResetEvent(); an auto-reset
    // event releases one waiter and resets itself. A non-empty `name` makes it findable by
    // Open(); if the name exists already, the result says so and refers to the existing object.
    // Errors: kUnsupported; kInvalidArgument if the name belongs to an object of another kind;
    // kOutOfResources.
    virtual Result<Created> MakeEvent(bool manualReset, bool initiallySignaled,
                                        std::string_view name) = 0;

    // Signals an event. Errors: kUnsupported; kInvalidHandle if `event` is not an event.
    virtual Status SetEvent(Waitable event) = 0;

    // Puts an event in the unsignalled state. Errors: kUnsupported; kInvalidHandle.
    virtual Status ResetEvent(Waitable event) = 0;

    // Creates a recursive mutex, owned by the calling thread if `initiallyOwned`. Names as
    // MakeEvent(). A mutex is signalled when no thread owns it; a successful wait on it takes
    // ownership (once more, if the waiter already owns it). Errors: as MakeEvent().
    virtual Result<Created> MakeMutex(bool initiallyOwned, std::string_view name) = 0;

    // Gives up one level of the calling thread's ownership. Errors: kUnsupported; kInvalidHandle;
    // kNotOwner if the calling thread does not own `mutex`.
    virtual Status ReleaseMutex(Waitable mutex) = 0;

    // Creates a counting semaphore, signalled while its count is above 0; a successful wait takes
    // one from the count. Names as MakeEvent(). Errors: as MakeEvent(), and kInvalidArgument
    // unless 0 <= initialCount <= maximumCount and maximumCount > 0.
    virtual Result<Created> MakeSemaphore(std::int32_t initialCount, std::int32_t maximumCount,
                                            std::string_view name) = 0;

    // Adds `count` to a semaphore and returns the count before. Errors: kUnsupported;
    // kInvalidHandle; kInvalidArgument if `count` <= 0 or the sum would exceed the maximum (the
    // count is then unchanged).
    virtual Result<std::int32_t> ReleaseSemaphore(Waitable semaphore, std::int32_t count) = 0;

    // Finds a named event, mutex or semaphore created earlier in this process. Errors:
    // kUnsupported; kInvalidArgument if `name` is empty; kNotFound.
    virtual Result<Waitable> Open(std::string_view name) = 0;

    // Waits until one (kAny) or all (kAll) of `handles` are signalled, or `timeoutMs` passes
    // (std::nullopt = forever; 0 = test and return). For kAny, the lowest signalled index wins.
    // A satisfied wait consumes what it waited for: an auto-reset event resets, a semaphore's
    // count drops, a mutex becomes owned; for kAll, all at once or none. Joining a thread is a
    // wait on its handle. Errors: kUnsupported; kInvalidArgument if `count` is 0 or over 64;
    // kInvalidHandle if any handle is not live.
    virtual Result<WaitResult> Wait(const Waitable *handles, std::size_t count, WaitMode mode,
                                    std::optional<std::uint32_t> timeoutMs) = 0;

    // Drops this handle. The object is destroyed when its last handle is closed and, for a
    // thread, when it has also ended; closing a running thread's handle does not stop it.
    // Errors: kUnsupported; kInvalidHandle.
    virtual Status Close(Waitable handle) = 0;

    // Reserves a thread-local storage slot whose value is 0 in every thread until set. Errors:
    // kUnsupported; kOutOfResources when no slot is free (at least 64 exist).
    virtual Result<std::uint32_t> AllocateTlsSlot() = 0;

    // Releases a slot; its values in every thread are forgotten. Errors: kUnsupported;
    // kInvalidArgument if `slot` is not allocated.
    virtual Status FreeTlsSlot(std::uint32_t slot) = 0;

    // Sets the calling thread's value in `slot`. Errors: kUnsupported; kInvalidArgument if
    // `slot` is not allocated.
    virtual Status SetTlsValue(std::uint32_t slot, std::uint64_t value) = 0;

    // The calling thread's value in `slot` (0 if never set). Errors: kUnsupported;
    // kInvalidArgument if `slot` is not allocated.
    virtual Result<std::uint64_t> GetTlsValue(std::uint32_t slot) = 0;
};

// A Threads whose every method fails with kUnsupported.
std::unique_ptr<Threads> makeThreadsStub();

// The build's real backend, on the host threads, one file per platform (B6); declared here so that the client needs no
// backend header, defined by the backend's own source file once it lands.
std::unique_ptr<Threads> makeThreads();

} // namespace sfcop::platform
