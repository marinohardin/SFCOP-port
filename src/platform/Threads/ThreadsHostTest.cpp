// The host Threads backend through the interface: every method's success and the errors its
// contract lists. Cross-thread checks hand off through events and waits without a timeout, so
// they depend on no timing; the one test whose subject is a timeout runs a real 30 ms wait and is
// skipped under SFCOP_SKIP_TIMING_TESTS=1.

#include "platform/Threads/Threads.h"

#include <doctest/doctest.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <vector>

namespace sfcop::platform {

namespace threadshost {
// ThreadsHost.cpp: how many times the backend has woken its waiters since the process began.
std::uint64_t wakeUps();
} // namespace threadshost

namespace {

bool timingTestsSkipped() {
    const char *skip = std::getenv("SFCOP_SKIP_TIMING_TESTS");
    return skip != nullptr && std::strcmp(skip, "1") == 0;
}

const std::optional<std::uint32_t> kForever = std::nullopt;

Waitable event(Threads &threads, bool manualReset, bool signaled, std::string_view name = "") {
    return threads.MakeEvent(manualReset, signaled, name).Value().handle;
}

WaitOutcome waitOne(Threads &threads, Waitable handle, std::optional<std::uint32_t> timeout) {
    return threads.Wait(&handle, 1, WaitMode::kAny, timeout).Value().outcome;
}

} // namespace

TEST_CASE("a host thread's body runs, its handle signals, and its return value is its exit code") {
    auto threads = makeThreads();
    Result<Waitable> thread = threads->StartThread([] { return 42u; }, ThreadOptions{"t", false, 0});
    REQUIRE(thread.Ok());
    CHECK(waitOne(*threads, thread.Value(), kForever) == WaitOutcome::kSignaled);
    CHECK(threads->GetExitCode(thread.Value()).Value() == 42u);
    CHECK(threads->Close(thread.Value()).Ok());
    CHECK(threads->GetExitCode(thread.Value()).Code() == Error::kInvalidHandle);
}

namespace {
// Recurses to `bytes` of stack, one 4 KB frame at a time; returns what it touched.
std::uint32_t useStack(std::size_t bytes);
// Called through a volatile pointer so the compiler cannot turn the recursion into a loop.
std::uint32_t (*volatile useStackAgain)(std::size_t) = useStack;
std::uint32_t useStack(std::size_t bytes) {
    volatile char frame[4096];
    frame[0] = 1;
    frame[sizeof frame - 1] = 1;
    return bytes <= sizeof frame ? frame[0] : useStackAgain(bytes - sizeof frame) + frame[sizeof frame - 1];
}
} // namespace

TEST_CASE("a host thread started with an odd stack size gets a stack at least that large") {
    auto threads = makeThreads();
    // 3,000,001 bytes: not a page multiple. The default secondary stack is 512 KB, so a thread
    // left on it would fault at the 2 MB mark below.
    ThreadOptions options;
    options.stackBytes = 3000001;
    Result<Waitable> thread = threads->StartThread([] { return useStack(2u << 20); }, options);
    REQUIRE(thread.Ok());
    CHECK(waitOne(*threads, thread.Value(), kForever) == WaitOutcome::kSignaled);
    CHECK(threads->GetExitCode(thread.Value()).Value() == 2u << 20 >> 12); // one per frame
}

TEST_CASE("a stack size the system refuses fails the start") {
    auto threads = makeThreads();
    ThreadOptions options;
    options.stackBytes = static_cast<std::size_t>(-1) - 100; // rounds past the end of the range
    Result<Waitable> thread = threads->StartThread([] { return 0u; }, options);
    CHECK_FALSE(thread.Ok());
    CHECK(thread.Code() == Error::kOutOfResources);
}

TEST_CASE("a running host thread has no exit code yet, and its id is its own") {
    auto threads = makeThreads();
    Waitable go = event(*threads, true, false);
    std::atomic<std::uint32_t> seenId{0};
    Result<Waitable> thread = threads->StartThread(
        [&] {
            seenId = threads->CurrentThreadId().Value();
            return waitOne(*threads, go, kForever) == WaitOutcome::kSignaled ? 7u : 8u;
        },
        ThreadOptions{});
    REQUIRE(thread.Ok());
    CHECK(threads->GetExitCode(thread.Value()).Code() == Error::kWrongState);
    CHECK(threads->SetEvent(go).Ok());
    CHECK(waitOne(*threads, thread.Value(), kForever) == WaitOutcome::kSignaled);
    CHECK(threads->GetExitCode(thread.Value()).Value() == 7u);
    const std::uint32_t mine = threads->CurrentThreadId().Value();
    CHECK(mine != 0u);
    CHECK(seenId.load() != 0u);
    CHECK(seenId.load() != mine);
}

TEST_CASE("a host thread started suspended runs only once resumed") {
    auto threads = makeThreads();
    std::atomic<int> ran{0};
    Result<Waitable> thread = threads->StartThread(
        [&] {
            ran = 1;
            return 0u;
        },
        ThreadOptions{"suspended", true, 1u << 20});
    REQUIRE(thread.Ok());
    CHECK(waitOne(*threads, thread.Value(), 0) == WaitOutcome::kTimedOut);
    CHECK(ran.load() == 0);
    CHECK(threads->SetThreadPriority(thread.Value(), ThreadPriority::kHighest).Ok());
    CHECK(threads->ResumeThread(thread.Value()).Ok());
    CHECK(waitOne(*threads, thread.Value(), kForever) == WaitOutcome::kSignaled);
    CHECK(ran.load() == 1);
    CHECK(threads->ResumeThread(thread.Value()).Ok()); // an ended thread: nothing, success
}

TEST_CASE("host thread calls refuse an empty body and handles that are not threads") {
    auto threads = makeThreads();
    CHECK(threads->StartThread({}, ThreadOptions{}).Code() == Error::kInvalidArgument);
    Waitable e = event(*threads, true, false);
    CHECK(threads->GetExitCode(e).Code() == Error::kInvalidHandle);
    CHECK(threads->ResumeThread(e).Code() == Error::kInvalidHandle);
    CHECK(threads->SetThreadPriority(Waitable{999}, ThreadPriority::kNormal).Code() ==
          Error::kInvalidHandle);
}

TEST_CASE("a manual-reset host event stays signalled; an auto-reset one releases one wait") {
    auto threads = makeThreads();
    Waitable manual = event(*threads, true, true);
    CHECK(waitOne(*threads, manual, 0) == WaitOutcome::kSignaled);
    CHECK(waitOne(*threads, manual, 0) == WaitOutcome::kSignaled);
    CHECK(threads->ResetEvent(manual).Ok());
    CHECK(waitOne(*threads, manual, 0) == WaitOutcome::kTimedOut);

    Waitable automatic = event(*threads, false, false);
    CHECK(waitOne(*threads, automatic, 0) == WaitOutcome::kTimedOut);
    CHECK(threads->SetEvent(automatic).Ok());
    CHECK(waitOne(*threads, automatic, 0) == WaitOutcome::kSignaled);
    CHECK(waitOne(*threads, automatic, 0) == WaitOutcome::kTimedOut);

    Result<Created> mutex = threads->MakeMutex(false, "");
    CHECK(threads->SetEvent(mutex.Value().handle).Code() == Error::kInvalidHandle);
    CHECK(threads->ResetEvent(Waitable{0}).Code() == Error::kInvalidHandle);
}

TEST_CASE("an auto-reset host event set by another thread wakes the waiter") {
    auto threads = makeThreads();
    Waitable wake = event(*threads, false, false);
    Result<Waitable> setter = threads->StartThread(
        [&] { return threads->SetEvent(wake).Ok() ? 0u : 1u; }, ThreadOptions{});
    REQUIRE(setter.Ok());
    CHECK(waitOne(*threads, wake, kForever) == WaitOutcome::kSignaled);
    CHECK(waitOne(*threads, setter.Value(), kForever) == WaitOutcome::kSignaled);
    CHECK(threads->GetExitCode(setter.Value()).Value() == 0u);
}

TEST_CASE("a host mutex is recursive and owned by one thread") {
    auto threads = makeThreads();
    Result<Created> made = threads->MakeMutex(true, "");
    REQUIRE(made.Ok());
    Waitable mutex = made.Value().handle;
    CHECK(waitOne(*threads, mutex, 0) == WaitOutcome::kSignaled); // the owner again: depth 2
    Result<Waitable> other = threads->StartThread(
        [&] {
            const bool blocked = waitOne(*threads, mutex, 0) == WaitOutcome::kTimedOut;
            const bool refused = threads->ReleaseMutex(mutex).Code() == Error::kNotOwner;
            return blocked && refused ? 0u : 1u;
        },
        ThreadOptions{});
    CHECK(waitOne(*threads, other.Value(), kForever) == WaitOutcome::kSignaled);
    CHECK(threads->GetExitCode(other.Value()).Value() == 0u);
    CHECK(threads->ReleaseMutex(mutex).Ok());
    CHECK(threads->ReleaseMutex(mutex).Ok());
    CHECK(threads->ReleaseMutex(mutex).Code() == Error::kNotOwner);
    Result<Waitable> taker = threads->StartThread(
        [&] {
            const bool took = waitOne(*threads, mutex, 0) == WaitOutcome::kSignaled;
            const bool released = threads->ReleaseMutex(mutex).Ok();
            return took && released ? 0u : 1u;
        },
        ThreadOptions{});
    CHECK(waitOne(*threads, taker.Value(), kForever) == WaitOutcome::kSignaled);
    CHECK(threads->GetExitCode(taker.Value()).Value() == 0u);
}

// Windows enters and leaves a critical section nobody else wants without waking any thread, and
// the game does so tens of thousands of times a second. A take, a release, a set and a reset with
// no waiter on that object wake no waiter.
TEST_CASE("taking and releasing host objects nobody waits on wakes no waiter") {
    auto threads = makeThreads();
    Waitable mutex = threads->MakeMutex(false, "").Value().handle;
    Waitable flag = event(*threads, true, false);
    Waitable count = threads->MakeSemaphore(0, 10, "").Value().handle;
    const std::uint64_t before = threadshost::wakeUps();
    for (int i = 0; i < 100; i++) {
        REQUIRE(waitOne(*threads, mutex, 0) == WaitOutcome::kSignaled);
        REQUIRE(threads->ReleaseMutex(mutex).Ok());
    }
    CHECK(threads->SetEvent(flag).Ok());
    CHECK(threads->ResetEvent(flag).Ok());
    CHECK(threads->ReleaseSemaphore(count, 1).Ok());
    CHECK(waitOne(*threads, count, 0) == WaitOutcome::kSignaled);
    CHECK(threadshost::wakeUps() - before == 0u);
}

// A thread waiting on one object is woken by a change to that object, not by every change to
// another: here the waiter is blocked on its event while the main thread churns a mutex, and the
// event's set is what ends the wait.
TEST_CASE("a host waiter wakes when its own object changes while another object churns") {
    auto threads = makeThreads();
    Waitable mutex = threads->MakeMutex(false, "").Value().handle;
    Waitable go = event(*threads, false, false);
    Waitable ready = event(*threads, false, false);
    Result<Waitable> waiter = threads->StartThread(
        [&] {
            if (!threads->SetEvent(ready).Ok()) {
                return 1u;
            }
            return waitOne(*threads, go, kForever) == WaitOutcome::kSignaled ? 0u : 2u;
        },
        ThreadOptions{});
    REQUIRE(waiter.Ok());
    CHECK(waitOne(*threads, ready, kForever) == WaitOutcome::kSignaled);
    for (int i = 0; i < 1000; i++) {
        REQUIRE(waitOne(*threads, mutex, 0) == WaitOutcome::kSignaled);
        REQUIRE(threads->ReleaseMutex(mutex).Ok());
    }
    CHECK(threads->SetEvent(go).Ok());
    CHECK(waitOne(*threads, waiter.Value(), kForever) == WaitOutcome::kSignaled);
    CHECK(threads->GetExitCode(waiter.Value()).Value() == 0u);
}

namespace {
// Takes and releases `mutex` `times` times on this thread: changes to an object no waiter is on.
void churn(Threads &threads, Waitable mutex, int times) {
    for (int i = 0; i < times; i++) {
        REQUIRE(waitOne(threads, mutex, 0) == WaitOutcome::kSignaled);
        REQUIRE(threads.ReleaseMutex(mutex).Ok());
    }
}
} // namespace

// The waiter blocks on a mutex this thread owns; another mutex churns; the owner's release of the
// first is what ends the wait.
TEST_CASE("a host waiter on a mutex is woken by its release while another mutex churns") {
    auto threads = makeThreads();
    Waitable held = threads->MakeMutex(true, "").Value().handle;
    Waitable other = threads->MakeMutex(false, "").Value().handle;
    Waitable ready = event(*threads, false, false);
    Result<Waitable> waiter = threads->StartThread(
        [&] {
            if (!threads->SetEvent(ready).Ok()) {
                return 1u;
            }
            if (waitOne(*threads, held, kForever) != WaitOutcome::kSignaled) {
                return 2u;
            }
            return threads->ReleaseMutex(held).Ok() ? 0u : 3u;
        },
        ThreadOptions{});
    REQUIRE(waiter.Ok());
    CHECK(waitOne(*threads, ready, kForever) == WaitOutcome::kSignaled);
    churn(*threads, other, 1000);
    CHECK(waitOne(*threads, waiter.Value(), 0) == WaitOutcome::kTimedOut); // still blocked
    CHECK(threads->ReleaseMutex(held).Ok());
    CHECK(waitOne(*threads, waiter.Value(), kForever) == WaitOutcome::kSignaled);
    CHECK(threads->GetExitCode(waiter.Value()).Value() == 0u);
}

TEST_CASE("a host waiter on a semaphore is woken by its release while a mutex churns") {
    auto threads = makeThreads();
    Waitable count = threads->MakeSemaphore(0, 1, "").Value().handle;
    Waitable other = threads->MakeMutex(false, "").Value().handle;
    Waitable ready = event(*threads, false, false);
    Result<Waitable> waiter = threads->StartThread(
        [&] {
            if (!threads->SetEvent(ready).Ok()) {
                return 1u;
            }
            return waitOne(*threads, count, kForever) == WaitOutcome::kSignaled ? 0u : 2u;
        },
        ThreadOptions{});
    REQUIRE(waiter.Ok());
    CHECK(waitOne(*threads, ready, kForever) == WaitOutcome::kSignaled);
    churn(*threads, other, 1000);
    CHECK(waitOne(*threads, waiter.Value(), 0) == WaitOutcome::kTimedOut);
    CHECK(threads->ReleaseSemaphore(count, 1).Value() == 0); // the count before the release
    CHECK(waitOne(*threads, waiter.Value(), kForever) == WaitOutcome::kSignaled);
    CHECK(threads->GetExitCode(waiter.Value()).Value() == 0u);
    CHECK(waitOne(*threads, count, 0) == WaitOutcome::kTimedOut); // the waiter took the one
}

// A wait for all on {mutex, event} blocks on both objects. Setting the event alone does not end
// it (the mutex is still owned here); another mutex churns; the mutex's release ends it, and the
// waiter then owns the mutex and the event is still set (manual reset).
TEST_CASE("a host wait for all on a mutex and an event ends when the last of them is released") {
    auto threads = makeThreads();
    Waitable held = threads->MakeMutex(true, "").Value().handle;
    Waitable flag = event(*threads, true, false);
    Waitable other = threads->MakeMutex(false, "").Value().handle;
    Waitable ready = event(*threads, false, false);
    Result<Waitable> waiter = threads->StartThread(
        [&] {
            if (!threads->SetEvent(ready).Ok()) {
                return 1u;
            }
            const Waitable both[2] = {held, flag};
            Result<WaitResult> got = threads->Wait(both, 2, WaitMode::kAll, kForever);
            if (!got.Ok() || got.Value().outcome != WaitOutcome::kSignaled) {
                return 2u;
            }
            return threads->ReleaseMutex(held).Ok() ? 0u : 3u;
        },
        ThreadOptions{});
    REQUIRE(waiter.Ok());
    CHECK(waitOne(*threads, ready, kForever) == WaitOutcome::kSignaled);
    CHECK(threads->SetEvent(flag).Ok());
    churn(*threads, other, 1000);
    CHECK(waitOne(*threads, waiter.Value(), 0) == WaitOutcome::kTimedOut); // the mutex is still owned
    CHECK(threads->ReleaseMutex(held).Ok());
    CHECK(waitOne(*threads, waiter.Value(), kForever) == WaitOutcome::kSignaled);
    CHECK(threads->GetExitCode(waiter.Value()).Value() == 0u);
    CHECK(waitOne(*threads, flag, 0) == WaitOutcome::kSignaled);
}

TEST_CASE("a host mutex whose owner thread ends is abandoned to the next waiter") {
    auto threads = makeThreads();
    Waitable mutex = threads->MakeMutex(false, "").Value().handle;
    Result<Waitable> owner = threads->StartThread(
        [&] { return waitOne(*threads, mutex, 0) == WaitOutcome::kSignaled ? 0u : 1u; },
        ThreadOptions{});
    CHECK(waitOne(*threads, owner.Value(), kForever) == WaitOutcome::kSignaled);
    CHECK(threads->GetExitCode(owner.Value()).Value() == 0u);
    CHECK(waitOne(*threads, mutex, 0) == WaitOutcome::kAbandoned);
    CHECK(threads->ReleaseMutex(mutex).Ok());
    CHECK(waitOne(*threads, mutex, 0) == WaitOutcome::kSignaled);
}

TEST_CASE("a host semaphore counts, and refuses counts out of range") {
    auto threads = makeThreads();
    CHECK(threads->MakeSemaphore(0, 0, "").Code() == Error::kInvalidArgument);
    CHECK(threads->MakeSemaphore(3, 2, "").Code() == Error::kInvalidArgument);
    CHECK(threads->MakeSemaphore(-1, 2, "").Code() == Error::kInvalidArgument);
    Waitable sem = threads->MakeSemaphore(1, 3, "").Value().handle;
    CHECK(waitOne(*threads, sem, 0) == WaitOutcome::kSignaled);
    CHECK(waitOne(*threads, sem, 0) == WaitOutcome::kTimedOut);
    CHECK(threads->ReleaseSemaphore(sem, 2).Value() == 0);
    CHECK(threads->ReleaseSemaphore(sem, 2).Code() == Error::kInvalidArgument);
    CHECK(threads->ReleaseSemaphore(sem, 0).Code() == Error::kInvalidArgument);
    CHECK(threads->ReleaseSemaphore(sem, 1).Value() == 2);
    CHECK(waitOne(*threads, sem, 0) == WaitOutcome::kSignaled);
    CHECK(threads->ReleaseSemaphore(Waitable{999}, 1).Code() == Error::kInvalidHandle);
}

TEST_CASE("named host objects are found again, and a name keeps its kind") {
    auto threads = makeThreads();
    Result<Created> first = threads->MakeEvent(true, false, "TestCaptainReady");
    REQUIRE(first.Ok());
    CHECK_FALSE(first.Value().alreadyExisted);
    Result<Created> again = threads->MakeEvent(false, true, "TestCaptainReady");
    REQUIRE(again.Ok());
    CHECK(again.Value().alreadyExisted);
    CHECK(again.Value().handle != first.Value().handle);
    // One object: setting it through one handle signals the other.
    CHECK(threads->SetEvent(again.Value().handle).Ok());
    CHECK(waitOne(*threads, first.Value().handle, 0) == WaitOutcome::kSignaled);
    CHECK(threads->MakeMutex(false, "TestCaptainReady").Code() == Error::kInvalidArgument);
    Result<Waitable> opened = threads->Open("TestCaptainReady");
    REQUIRE(opened.Ok());
    CHECK(waitOne(*threads, opened.Value(), 0) == WaitOutcome::kSignaled); // manual reset
    CHECK(threads->Open("").Code() == Error::kInvalidArgument);
    CHECK(threads->Open("NoSuchObject").Code() == Error::kNotFound);
    // Unnamed objects are never shared.
    Result<Created> a = threads->MakeEvent(true, false, "");
    Result<Created> b = threads->MakeEvent(true, false, "");
    CHECK_FALSE(b.Value().alreadyExisted);
    CHECK(threads->SetEvent(a.Value().handle).Ok());
    CHECK(waitOne(*threads, b.Value().handle, 0) == WaitOutcome::kTimedOut);
}

TEST_CASE("a host wait for any takes the lowest signalled index; for all, all or none") {
    auto threads = makeThreads();
    Waitable e0 = event(*threads, false, false);
    Waitable e1 = event(*threads, false, true);
    Waitable e2 = event(*threads, false, true);
    Waitable any[] = {e0, e1, e2};
    WaitResult first = threads->Wait(any, 3, WaitMode::kAny, 0).Value();
    CHECK(first.outcome == WaitOutcome::kSignaled);
    CHECK(first.index == 1u);
    // e1 was auto-reset by that wait, e2 was not touched.
    WaitResult second = threads->Wait(any, 3, WaitMode::kAny, 0).Value();
    CHECK(second.index == 2u);

    Waitable sem = threads->MakeSemaphore(1, 5, "").Value().handle;
    Waitable unset = event(*threads, true, false);
    Waitable all[] = {sem, unset};
    CHECK(threads->Wait(all, 2, WaitMode::kAll, 0).Value().outcome == WaitOutcome::kTimedOut);
    // Nothing was consumed: the semaphore still has its count.
    CHECK(waitOne(*threads, sem, 0) == WaitOutcome::kSignaled);
    CHECK(threads->ReleaseSemaphore(sem, 1).Ok());
    CHECK(threads->SetEvent(unset).Ok());
    WaitResult both = threads->Wait(all, 2, WaitMode::kAll, 0).Value();
    CHECK(both.outcome == WaitOutcome::kSignaled);
    CHECK(both.index == 0u);
    CHECK(waitOne(*threads, sem, 0) == WaitOutcome::kTimedOut);
    // A semaphore listed twice needs two counts.
    CHECK(threads->ReleaseSemaphore(sem, 1).Ok());
    Waitable twice[] = {sem, sem};
    CHECK(threads->Wait(twice, 2, WaitMode::kAll, 0).Value().outcome == WaitOutcome::kTimedOut);
}

TEST_CASE("a host wait refuses no handles, too many, and a dead handle") {
    auto threads = makeThreads();
    std::vector<Waitable> many;
    for (int i = 0; i < 65; i++) {
        many.push_back(event(*threads, true, true));
    }
    CHECK(threads->Wait(many.data(), 0, WaitMode::kAny, 0).Code() == Error::kInvalidArgument);
    CHECK(threads->Wait(many.data(), 65, WaitMode::kAny, 0).Code() == Error::kInvalidArgument);
    CHECK(threads->Wait(many.data(), 64, WaitMode::kAll, 0).Value().outcome ==
          WaitOutcome::kSignaled);
    Waitable dead = many[0];
    CHECK(threads->Close(dead).Ok());
    CHECK(threads->Close(dead).Code() == Error::kInvalidHandle);
    CHECK(threads->Wait(&dead, 1, WaitMode::kAny, 0).Code() == Error::kInvalidHandle);
}

TEST_CASE("a host wait with a timeout returns when it passes" *
          doctest::skip(timingTestsSkipped())) {
    auto threads = makeThreads();
    Waitable never = event(*threads, true, false);
    CHECK(waitOne(*threads, never, 30) == WaitOutcome::kTimedOut);
}

TEST_CASE("host thread-local slots hold one value per thread and forget them when freed") {
    auto threads = makeThreads();
    const std::uint32_t slot = threads->AllocateTlsSlot().Value();
    CHECK(threads->GetTlsValue(slot).Value() == 0u);
    CHECK(threads->SetTlsValue(slot, 0x1234567890ull).Ok());
    CHECK(threads->GetTlsValue(slot).Value() == 0x1234567890ull);
    Result<Waitable> other = threads->StartThread(
        [&] {
            const bool fresh = threads->GetTlsValue(slot).Value() == 0u;
            const bool set = threads->SetTlsValue(slot, 5).Ok();
            const bool own = threads->GetTlsValue(slot).Value() == 5u;
            return fresh && set && own ? 0u : 1u;
        },
        ThreadOptions{});
    CHECK(waitOne(*threads, other.Value(), kForever) == WaitOutcome::kSignaled);
    CHECK(threads->GetExitCode(other.Value()).Value() == 0u);
    CHECK(threads->GetTlsValue(slot).Value() == 0x1234567890ull);
    CHECK(threads->FreeTlsSlot(slot).Ok());
    CHECK(threads->FreeTlsSlot(slot).Code() == Error::kInvalidArgument);
    CHECK(threads->GetTlsValue(slot).Code() == Error::kInvalidArgument);
    CHECK(threads->SetTlsValue(slot, 1).Code() == Error::kInvalidArgument);
    const std::uint32_t again = threads->AllocateTlsSlot().Value();
    CHECK(again == slot);
    CHECK(threads->GetTlsValue(again).Value() == 0u);
}

TEST_CASE("host thread-local slots run out after at least 64") {
    auto threads = makeThreads();
    std::uint32_t count = 0;
    while (threads->AllocateTlsSlot().Ok()) {
        count++;
        REQUIRE(count <= 4096u);
    }
    CHECK(count >= 64u);
    CHECK(threads->AllocateTlsSlot().Code() == Error::kOutOfResources);
}

TEST_CASE("host thread-local slots number exactly 128 before kOutOfResources") {
    auto threads = makeThreads();
    std::vector<std::uint32_t> slots;
    while (true) {
        const Result<std::uint32_t> slot = threads->AllocateTlsSlot();
        if (!slot.Ok()) {
            CHECK(slot.Code() == Error::kOutOfResources);
            break;
        }
        slots.push_back(slot.Value());
        REQUIRE(slots.size() <= 128u);
    }
    CHECK(slots.size() == 128u);
    CHECK(threads->FreeTlsSlot(slots[7]).Ok());
    CHECK(threads->AllocateTlsSlot().Value() == slots[7]); // a freed slot is the next one made
    CHECK(threads->AllocateTlsSlot().Code() == Error::kOutOfResources);
}

TEST_CASE("an auto-reset host event with two waiters releases exactly one") {
    auto threads = makeThreads();
    Waitable gate = event(*threads, false, false);    // auto-reset: the subject
    Waitable woke = event(*threads, false, false);    // a waiter reports in
    Waitable release = event(*threads, true, false);  // lets the waiters end
    std::atomic<int> through{0};
    auto waiter = [&] {
        Waitable handle = gate;
        const bool got = threads->Wait(&handle, 1, WaitMode::kAny, kForever).Ok();
        through++;
        static_cast<void>(threads->SetEvent(woke));
        Waitable end = release;
        static_cast<void>(threads->Wait(&end, 1, WaitMode::kAny, kForever));
        return got ? 0u : 1u;
    };
    Result<Waitable> first = threads->StartThread(waiter, ThreadOptions{});
    Result<Waitable> second = threads->StartThread(waiter, ThreadOptions{});
    REQUIRE(first.Ok());
    REQUIRE(second.Ok());

    CHECK(threads->SetEvent(gate).Ok());
    CHECK(waitOne(*threads, woke, kForever) == WaitOutcome::kSignaled);
    CHECK(through.load() == 1);
    if (!timingTestsSkipped()) {
        // Give a second release 30 ms to show itself: none does.
        CHECK(waitOne(*threads, woke, 30) == WaitOutcome::kTimedOut);
        CHECK(through.load() == 1);
    }
    CHECK(threads->SetEvent(gate).Ok());
    CHECK(waitOne(*threads, woke, kForever) == WaitOutcome::kSignaled);
    CHECK(through.load() == 2);

    CHECK(threads->SetEvent(release).Ok());
    CHECK(waitOne(*threads, first.Value(), kForever) == WaitOutcome::kSignaled);
    CHECK(waitOne(*threads, second.Value(), kForever) == WaitOutcome::kSignaled);
}

TEST_CASE("a host wait for all that includes an abandoned mutex reports it and owns the mutex") {
    auto threads = makeThreads();
    Waitable mutex = threads->MakeMutex(false, "").Value().handle;
    Waitable signalled = event(*threads, true, true);
    // A thread takes the mutex and ends without releasing it.
    Result<Waitable> owner = threads->StartThread(
        [&] {
            Waitable handle = mutex;
            return threads->Wait(&handle, 1, WaitMode::kAny, kForever).Ok() ? 0u : 1u;
        },
        ThreadOptions{});
    REQUIRE(owner.Ok());
    CHECK(waitOne(*threads, owner.Value(), kForever) == WaitOutcome::kSignaled);

    Waitable both[] = {signalled, mutex};
    const Result<WaitResult> all = threads->Wait(both, 2, WaitMode::kAll, 0);
    REQUIRE(all.Ok());
    CHECK(all.Value().outcome == WaitOutcome::kAbandoned);
    CHECK(threads->ReleaseMutex(mutex).Ok()); // the wait made this thread its owner
    CHECK(waitOne(*threads, signalled, 0) == WaitOutcome::kSignaled); // a manual event stays set
}

TEST_CASE("two host Threads keep their thread-local values apart") {
    auto one = makeThreads();
    auto two = makeThreads();
    const std::uint32_t a = one->AllocateTlsSlot().Value();
    const std::uint32_t b = two->AllocateTlsSlot().Value();
    CHECK(one->SetTlsValue(a, 11).Ok());
    CHECK(two->GetTlsValue(b).Value() == 0u);
}

} // namespace sfcop::platform
