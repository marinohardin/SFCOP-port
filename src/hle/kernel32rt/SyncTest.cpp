#include "hle/kernel32rt/Sync.h"

#include "fixes/fixes.h"
#include "platform/Threads/ThreadsMock.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace sfcop::hle::kernel32rt {
namespace {

using platform::Created;
using platform::Error;
using platform::ThreadsMock;
using platform::Waitable;
using platform::WaitOutcome;
using platform::WaitResult;

TEST_CASE("CreateEventA makes an event and clears the last error") {
    ThreadsMock threads;
    Sync sync(threads);
    Object event;
    const Answer answer = sync.MakeEvent(true, false, "", event);
    CHECK(answer.value == 1);
    CHECK(answer.lastError == kErrorSuccess);
    CHECK(event.kind == Kind::kEvent);
    CHECK(event.waitable.value == 2);
    CHECK(threads.log.Calls() == std::vector<std::string>{"MakeEvent(true, false, \"\")"});
}

TEST_CASE("CreateMutexA on a name that exists reports ERROR_ALREADY_EXISTS and opens it") {
    ThreadsMock threads;
    threads.makeMutexResult = Created{Waitable{9}, true};
    Sync sync(threads);
    Object mutex;
    const Answer answer = sync.MakeMutex(false, "TestCaptainMutex", mutex);
    CHECK(answer.value == 1);
    CHECK(answer.lastError == kErrorAlreadyExists);
    CHECK(mutex.waitable.value == 9);
}

TEST_CASE("a name held by another kind of object fails with ERROR_INVALID_HANDLE") {
    ThreadsMock threads;
    threads.makeEventResult = Error::kInvalidArgument;
    Sync sync(threads);
    Object event;
    const Answer answer = sync.MakeEvent(false, false, "Taken", event);
    CHECK(answer.value == 0);
    CHECK(answer.lastError == kErrorInvalidHandle);
}

TEST_CASE("OpenMutexA finds a mutex by name and refuses an event's name") {
    ThreadsMock threads;
    Sync sync(threads);
    Object event;
    sync.MakeEvent(false, false, "AnEvent", event);
    Object opened;
    const Answer refused = sync.OpenMutex("AnEvent", opened);
    CHECK(refused.value == 0);
    CHECK(refused.lastError == kErrorInvalidHandle);
    CHECK(threads.log.Count("Open") == 0);

    threads.openResult = Waitable{7};
    const Answer found = sync.OpenMutex("AMutex", opened);
    CHECK(found.value == 1);
    CHECK(opened.kind == Kind::kMutex);
    CHECK(opened.waitable.value == 7);

    threads.openResult = Error::kNotFound;
    const Answer missing = sync.OpenMutex("Nothing", opened);
    CHECK(missing.value == 0);
    CHECK(missing.lastError == kErrorFileNotFound);
}

TEST_CASE("CreateSemaphoreA refuses counts Windows refuses") {
    ThreadsMock threads;
    Sync sync(threads);
    Object semaphore;
    CHECK(sync.MakeSemaphore(2, 1, "", semaphore).lastError == kErrorInvalidParameter);
    CHECK(sync.MakeSemaphore(0, 0, "", semaphore).lastError == kErrorInvalidParameter);
    CHECK(sync.MakeSemaphore(-1, 3, "", semaphore).lastError == kErrorInvalidParameter);
    CHECK(threads.log.Count("MakeSemaphore") == 0);
    CHECK(sync.MakeSemaphore(1, 3, "", semaphore).value == 1);
    CHECK(semaphore.kind == Kind::kSemaphore);
}

TEST_CASE("SetEvent, ResetEvent and ReleaseMutex check the object's kind") {
    ThreadsMock threads;
    Sync sync(threads);
    const Object mutex{Kind::kMutex, Waitable{3}, 0};
    const Object event{Kind::kEvent, Waitable{2}, 0};
    CHECK(sync.SetEvent(mutex).lastError == kErrorInvalidHandle);
    CHECK(sync.ResetEvent(mutex).lastError == kErrorInvalidHandle);
    CHECK(sync.ReleaseMutex(event).lastError == kErrorInvalidHandle);
    CHECK(sync.SetEvent(event).value == 1);
    CHECK(sync.ResetEvent(event).value == 1);
    CHECK(threads.log.Calls() == std::vector<std::string>{"SetEvent(2)", "ResetEvent(2)"});
}

TEST_CASE("ReleaseMutex by a thread that does not own it fails with ERROR_NOT_OWNER") {
    ThreadsMock threads;
    threads.releaseMutexResult = Error::kNotOwner;
    Sync sync(threads);
    const Answer answer = sync.ReleaseMutex(Object{Kind::kMutex, Waitable{3}, 0});
    CHECK(answer.value == 0);
    CHECK(answer.lastError == kErrorNotOwner);
}

TEST_CASE("ReleaseSemaphore gives the previous count and refuses a post past the maximum") {
    ThreadsMock threads;
    threads.releaseSemaphoreResult = std::int32_t{2};
    Sync sync(threads);
    const Object semaphore{Kind::kSemaphore, Waitable{4}, 0};
    std::int32_t previous = -1;
    CHECK(sync.ReleaseSemaphore(semaphore, 1, previous).value == 1);
    CHECK(previous == 2);
    CHECK(sync.ReleaseSemaphore(semaphore, 0, previous).lastError == kErrorInvalidParameter);
    threads.releaseSemaphoreResult = Error::kInvalidArgument;
    CHECK(sync.ReleaseSemaphore(semaphore, 5, previous).lastError == kErrorTooManyPosts);
}

TEST_CASE("a wait answers WAIT_OBJECT_0 + i, WAIT_ABANDONED_0 + i or WAIT_TIMEOUT") {
    ThreadsMock threads;
    Sync sync(threads);
    const Object a{Kind::kEvent, Waitable{2}, 0};
    const Object b{Kind::kMutex, Waitable{3}, 0};

    threads.waitResult = WaitResult{WaitOutcome::kSignaled, 1};
    CHECK(sync.Wait({&a, &b}, false, 50).value == 1u);
    threads.waitResult = WaitResult{WaitOutcome::kAbandoned, 1};
    CHECK(sync.Wait({&a, &b}, false, 50).value == 0x81u);
    threads.waitResult = WaitResult{WaitOutcome::kTimedOut, 0};
    CHECK(sync.Wait({&a}, false, 0).value == 0x102u);
    threads.waitResult = WaitResult{WaitOutcome::kSignaled, 0};
    CHECK(sync.Wait({&a, &b}, true, kInfinite).value == 0u);
    CHECK(threads.log.Calls() == std::vector<std::string>{
                                     "Wait(2, 0, 50)", "Wait(2, 0, 50)", "Wait(1, 0, 0)",
                                     "Wait(2, 1, \"forever\")"});
    CHECK(threads.lastWaitHandles == std::vector<Waitable>{Waitable{2}, Waitable{3}});
}

TEST_CASE("a wait on no objects, too many, or a bad handle fails") {
    ThreadsMock threads;
    Sync sync(threads);
    const Object a{Kind::kEvent, Waitable{2}, 0};
    const Answer none = sync.Wait({}, false, 0);
    CHECK(none.value == kWaitFailed);
    CHECK(none.lastError == kErrorInvalidParameter);
    const std::vector<const Object *> tooMany(65, &a);
    CHECK(sync.Wait(tooMany, false, 0).lastError == kErrorInvalidParameter);
    CHECK(sync.Wait({nullptr}, false, 0).lastError == kErrorInvalidHandle);
    threads.waitResult = Error::kInvalidHandle;
    const Answer bad = sync.Wait({&a}, false, 0);
    CHECK(bad.value == kWaitFailed);
    CHECK(bad.lastError == kErrorInvalidHandle);
}

TEST_CASE("a thread created suspended runs after one ResumeThread") {
    ThreadsMock threads;
    Sync sync(threads);
    Object thread;
    CHECK(sync.StartThread([] { return 7u; }, true, 65536, thread).value == 1);
    CHECK(thread.kind == Kind::kThread);
    CHECK(thread.suspendCount == 1);
    CHECK(threads.lastThreadOptions.startSuspended);
    CHECK(threads.lastThreadOptions.stackBytes == 16u * 1024 * 1024); // never below 16 MB
    CHECK(threads.lastBody() == 7u);
    CHECK(sync.ResumeThread(thread).value == 1);
    CHECK(sync.ResumeThread(thread).value == 0);
    CHECK(threads.log.Count("ResumeThread") == 1);
    Object larger;
    CHECK(sync.StartThread([] { return 0u; }, false, 32u * 1024 * 1024, larger).value == 1);
    CHECK(threads.lastThreadOptions.stackBytes == 32u * 1024 * 1024);
    Object event{Kind::kEvent, Waitable{2}, 0};
    CHECK(sync.ResumeThread(event).value == 0xFFFFFFFFu);
}

TEST_CASE("GetExitCodeThread answers STILL_ACTIVE while the thread runs") {
    ThreadsMock threads;
    threads.exitCodeResult = Error::kWrongState;
    Sync sync(threads);
    const Object thread{Kind::kThread, Waitable{1}, 0};
    std::uint32_t code = 0;
    CHECK(sync.GetExitCode(thread, code).value == 1);
    CHECK(code == kStillActive);
    threads.exitCodeResult = std::uint32_t{3};
    CHECK(sync.GetExitCode(thread, code).value == 1);
    CHECK(code == 3);
}

TEST_CASE("SetThreadPriority takes Windows' priority numbers") {
    ThreadsMock threads;
    Sync sync(threads);
    const Object thread{Kind::kThread, Waitable{1}, 0};
    CHECK(sync.SetThreadPriority(thread, 2).value == 1);
    CHECK(sync.SetThreadPriority(thread, -15).value == 1);
    CHECK(sync.SetThreadPriority(thread, 7).lastError == kErrorInvalidParameter);
    CHECK(threads.log.Calls() ==
          std::vector<std::string>{"SetThreadPriority(1, 4)", "SetThreadPriority(1, 0)"});
}

TEST_CASE("a name is free for another kind once its last handle is closed") {
    ThreadsMock threads;
    threads.makeEventResult = Created{Waitable{4}, false};
    Sync sync(threads);
    Object first, second;
    sync.MakeEvent(false, false, "Shared", first);
    threads.makeEventResult = Created{Waitable{5}, true};
    sync.MakeEvent(false, false, "Shared", second);

    Object opened;
    threads.openResult = Waitable{7};
    sync.Close(first); // one handle left: the name still belongs to an event
    CHECK(sync.OpenMutex("Shared", opened).lastError == kErrorInvalidHandle);
    sync.Close(second); // the last: the event is gone
    CHECK(sync.OpenMutex("Shared", opened).value == 1);
    CHECK(opened.kind == Kind::kMutex);
}

TEST_CASE("CloseHandle gives the Threads handle back once") {
    ThreadsMock threads;
    Sync sync(threads);
    Object event{Kind::kEvent, Waitable{2}, 0};
    sync.Close(event);
    sync.Close(event);
    CHECK(threads.log.Calls() == std::vector<std::string>{"Close(2)"});
}

// fix_hle_named_objects, set by the test and put back as it was when the test ends.
struct NamedObjectsFix {
    explicit NamedObjectsFix(bool on) : before_(fixes::current()) {
        fixes::Settings settings;
        if (!on) {
            REQUIRE(settings.applyCommandLine({"fix_hle_named_objects=off"}).empty());
        }
        fixes::install(settings);
    }
    ~NamedObjectsFix() { fixes::install(before_); }

private:
    fixes::Settings before_; // what the process had installed, put back whatever it was
};

TEST_CASE("fix on: an empty name reaches Threads unchanged, so each object is its own") {
    NamedObjectsFix fix(true);
    ThreadsMock threads;
    Sync sync(threads);
    Object event, mutex, semaphore, opened;
    sync.MakeEvent(false, false, "", event);
    sync.MakeMutex(false, "", mutex);
    sync.MakeSemaphore(0, 1, "", semaphore);
    sync.OpenMutex("", opened);
    CHECK(threads.log.Calls() ==
          std::vector<std::string>{"MakeEvent(false, false, \"\")", "MakeMutex(false, \"\")",
                                   "MakeSemaphore(0, 1, \"\")", "Open(\"\")"});
}

TEST_CASE("fix off: an empty name is one reserved name per kind that no game name can equal") {
    NamedObjectsFix fix(false);
    ThreadsMock threads;
    threads.openResult = Waitable{7};
    Sync sync(threads);
    Object event, mutex, semaphore, opened;
    sync.MakeEvent(false, false, "", event);
    sync.MakeMutex(false, "", mutex);
    sync.MakeSemaphore(0, 1, "", semaphore);
    sync.OpenMutex("", opened);
    CHECK(threads.log.Calls() ==
          std::vector<std::string>{"MakeEvent(false, false, \"\\EmptyNameEvent\")",
                                   "MakeMutex(false, \"\\EmptyNameMutex\")",
                                   "MakeSemaphore(0, 1, \"\\EmptyNameSemaphore\")",
                                   "Open(\"\\EmptyNameMutex\")"});
    // A real name is never changed.
    threads.log.Clear();
    sync.MakeEvent(false, false, "Real", event);
    CHECK(threads.log.Calls() == std::vector<std::string>{"MakeEvent(false, false, \"Real\")"});
}

// Through the real Threads backend: whether signalling one empty-named object reaches another.
TEST_CASE("empty-named events: distinct with the fix on, one object with it off") {
    for (const bool on : {true, false}) {
        NamedObjectsFix fix(on);
        const auto threads = platform::makeThreads();
        Sync sync(*threads);
        Object first, second;
        REQUIRE(sync.MakeEvent(true, false, "", first).value == 1);
        const Answer again = sync.MakeEvent(true, false, "", second);
        REQUIRE(again.value == 1);
        CHECK(sync.SetEvent(first).value == 1);
        const Answer woke = sync.Wait({&second}, false, 0);
        if (on) {
            CHECK(again.lastError == kErrorSuccess);
            CHECK(woke.value == kWaitTimeout); // the second event was not signalled
        } else {
            CHECK(again.lastError == kErrorAlreadyExists);
            CHECK(woke.value == kWaitObject0); // the same object
        }
        sync.Close(first);
        sync.Close(second);
    }
}

TEST_CASE("fix off: OpenMutexA of an empty name does not open an empty-named event") {
    NamedObjectsFix fix(false);
    const auto threads = platform::makeThreads();
    Sync sync(*threads);
    Object event, opened;
    REQUIRE(sync.MakeEvent(false, false, "", event).value == 1);
    // The reserved key is one per kind, so no mutex of that name exists: not found, never the event.
    const Answer refused = sync.OpenMutex("", opened);
    CHECK(refused.value == 0);
    CHECK(refused.lastError == kErrorFileNotFound);
    sync.Close(event);
}

TEST_CASE("empty-named mutexes and semaphores: distinct with the fix on, one object with it off") {
    for (const bool on : {true, false}) {
        NamedObjectsFix fix(on);
        const auto threads = platform::makeThreads();
        Sync sync(*threads);
        Object m1, m2, s1, s2;
        sync.MakeMutex(false, "", m1);
        const Answer m = sync.MakeMutex(false, "", m2);
        sync.MakeSemaphore(0, 2, "", s1);
        const Answer s = sync.MakeSemaphore(0, 2, "", s2);
        CHECK(m.lastError == (on ? kErrorSuccess : kErrorAlreadyExists));
        CHECK(s.lastError == (on ? kErrorSuccess : kErrorAlreadyExists));
        Object opened;
        CHECK(sync.OpenMutex("", opened).value == (on ? 0u : 1u));
        if (!on) {
            sync.Close(opened);
        }
        sync.Close(m1);
        sync.Close(m2);
        sync.Close(s1);
        sync.Close(s2);
    }
}

} // namespace
} // namespace sfcop::hle::kernel32rt
