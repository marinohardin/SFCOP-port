// Sync: CreateThread's thread objects, events, mutexes and semaphores, and the waits on them, as
// KERNEL32 answers them, on Threads.
//
// Each Make* fills an Object the shim layer then puts behind a guest handle; Close() gives its
// Threads handle back. A wait never takes the guest machine into account: the shim gives the
// machine up around a wait that can block (hle_block_begin/end) and calls Wait() in between.
//
// Threading: every method may be called from any thread; the names table is behind a lock.
#pragma once

#include "hle/kernel32rt/Kernel32rt.h"
#include "platform/Threads/Threads.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace sfcop::hle::kernel32rt {

class Sync {
public:
    explicit Sync(platform::Threads &threads);

    // CreateThread: starts `body` (which runs the guest's start routine and returns its exit
    // code) with `stackBytes` (0: the platform's default), suspended if asked. Value 1 and `out`
    // filled, or 0 with ERROR_NOT_ENOUGH_MEMORY.
    Answer StartThread(std::function<std::uint32_t()> body, bool suspended,
                       std::uint32_t stackBytes, Object &out);
    // ResumeThread: the previous suspend count; the thread runs once it reaches 0.
    // 0xFFFFFFFF with ERROR_INVALID_HANDLE for an object that is not a thread.
    Answer ResumeThread(Object &thread);
    // SetThreadPriority: `priority` as Windows numbers it (-15, -2 to 2, 15). 1, or 0 with
    // ERROR_INVALID_HANDLE / ERROR_INVALID_PARAMETER.
    Answer SetThreadPriority(const Object &thread, std::int32_t priority);
    // GetExitCodeThread: 1 and `code` (STILL_ACTIVE while it runs), or 0 with
    // ERROR_INVALID_HANDLE.
    Answer GetExitCode(const Object &thread, std::uint32_t &code);

    // CreateEventA, CreateMutexA, CreateSemaphoreA: 1 and `out`, with ERROR_ALREADY_EXISTS when
    // the name was taken by an object of the same kind (`out` then refers to it) and last error 0
    // otherwise; 0 with
    // ERROR_INVALID_HANDLE when the name belongs to another kind, ERROR_INVALID_PARAMETER for a
    // semaphore's counts, ERROR_NOT_ENOUGH_MEMORY otherwise. An empty name makes an unnamed
    // object while fix_hle_named_objects is on; off, it is one shared object per kind.
    Answer MakeEvent(bool manualReset, bool initiallySignaled, std::string_view name, Object &out);
    Answer MakeMutex(bool initiallyOwned, std::string_view name, Object &out);
    Answer MakeSemaphore(std::int32_t initialCount, std::int32_t maximumCount,
                         std::string_view name, Object &out);
    // OpenMutexA: 1 and `out`, or 0 with ERROR_FILE_NOT_FOUND (no such name) or
    // ERROR_INVALID_HANDLE (the name is not a mutex's).
    Answer OpenMutex(std::string_view name, Object &out);

    // SetEvent, ResetEvent, ReleaseMutex: 1, or 0 with ERROR_INVALID_HANDLE for the wrong kind,
    // ERROR_NOT_OWNER for a mutex the caller does not own.
    Answer SetEvent(const Object &event);
    Answer ResetEvent(const Object &event);
    Answer ReleaseMutex(const Object &mutex);
    // ReleaseSemaphore: 1 and the previous count in `previous`, or 0 with ERROR_INVALID_HANDLE,
    // ERROR_INVALID_PARAMETER (count < 1) or ERROR_TOO_MANY_POSTS (past the maximum).
    Answer ReleaseSemaphore(const Object &semaphore, std::int32_t count, std::int32_t &previous);

    // WaitForSingleObject and WaitForMultipleObjects: WAIT_OBJECT_0 + i, WAIT_ABANDONED_0 + i,
    // WAIT_TIMEOUT, or WAIT_FAILED with ERROR_INVALID_PARAMETER (no objects, more than 64) or
    // ERROR_INVALID_HANDLE. `timeoutMs` INFINITE waits forever. Blocks the calling thread.
    Answer Wait(const std::vector<const Object *> &objects, bool waitAll, std::uint32_t timeoutMs);

    // CloseHandle's part: gives the Threads handle back. A running thread goes on running. When
    // the last handle to a name made here is closed the name is forgotten, so another kind of
    // object may take it.
    void Close(Object &object);

private:
    Answer Make(Kind kind, std::string_view name, platform::Result<platform::Created> made,
                Object &out);

    platform::Threads &threads_;
    std::mutex lock_;
    std::map<std::string, Kind, std::less<>> kinds_; // the kind behind each name made here
    std::map<std::uint32_t, std::string> handleNames_; // the name behind each named handle
};

} // namespace sfcop::hle::kernel32rt
