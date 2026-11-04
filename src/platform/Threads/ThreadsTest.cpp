#include "platform/Threads/Threads.h"
#include "platform/Threads/ThreadsMock.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace sfcop::platform {
namespace {

// A caller written against the interface only: starts a worker that sets an event, then waits up
// to `timeoutMs` for either the event or the worker to finish. Returns which one, or -1.
int startAndWait(Threads &threads, std::uint32_t timeoutMs) {
    Result<Created> done = threads.MakeEvent(true, false, "");
    if (!done.Ok()) {
        return -1;
    }
    Waitable event = done.Value().handle;
    Result<Waitable> worker =
        threads.StartThread([&threads, event] { return threads.SetEvent(event).Ok() ? 0u : 1u; },
                            ThreadOptions{"worker", false, 0});
    if (!worker.Ok()) {
        return -1;
    }
    Waitable handles[] = {event, worker.Value()};
    Result<WaitResult> waited = threads.Wait(handles, 2, WaitMode::kAny, timeoutMs);
    if (!waited.Ok() || waited.Value().outcome != WaitOutcome::kSignaled) {
        return -1;
    }
    return static_cast<int>(waited.Value().index);
}

} // namespace

TEST_CASE("a caller starts a worker and waits on its event through the threads interface") {
    ThreadsMock mock;
    mock.makeEventResult = Created{Waitable{20}, false};
    mock.startThreadResult = Waitable{21};
    mock.waitResult = WaitResult{WaitOutcome::kSignaled, 0};
    CHECK(startAndWait(mock, 250) == 0);
    CHECK(mock.lastWaitHandles == std::vector<Waitable>{Waitable{20}, Waitable{21}});
    CHECK(mock.lastThreadOptions.name == "worker");
    REQUIRE(mock.lastBody);
    CHECK(mock.lastBody() == 0);
    CHECK(mock.log.Calls() == std::vector<std::string>{
                                  "MakeEvent(true, false, \"\")",
                                  "StartThread(\"worker\", false)",
                                  "Wait(2, 0, 250)",
                                  "SetEvent(20)",
                              });
}

TEST_CASE("a timed-out wait and a failed create reach the caller as failures") {
    ThreadsMock mock;
    mock.waitResult = WaitResult{WaitOutcome::kTimedOut, 0};
    CHECK(startAndWait(mock, 0) == -1);
    mock.log.Clear();
    mock.makeEventResult = Error::kOutOfResources;
    CHECK(startAndWait(mock, 0) == -1);
    CHECK(mock.log.Calls() == std::vector<std::string>{"MakeEvent(true, false, \"\")"});
}

TEST_CASE("a caller joins a thread and reads its exit code, which is unreadable while running") {
    ThreadsMock mock;
    Threads &threads = mock;
    Waitable worker = threads.StartThread([] { return 3u; }, ThreadOptions{}).Value();
    mock.exitCodeResult = Error::kWrongState;
    CHECK(threads.GetExitCode(worker).Code() == Error::kWrongState);
    REQUIRE(threads.Wait(&worker, 1, WaitMode::kAny, std::nullopt).Ok());
    mock.exitCodeResult = mock.lastBody();
    CHECK(threads.GetExitCode(worker).Value() == 3);
    CHECK(mock.log.Calls() == std::vector<std::string>{
                                  "StartThread(\"\", false)",
                                  "GetExitCode(1)",
                                  "Wait(1, 0, \"forever\")",
                                  "GetExitCode(1)",
                              });
}

TEST_CASE("the threads mock records an infinite wait") {
    ThreadsMock mock;
    Waitable mutex{3};
    CHECK(mock.Wait(&mutex, 1, WaitMode::kAll, std::nullopt).Ok());
    CHECK(mock.log.Calls() == std::vector<std::string>{"Wait(1, 1, \"forever\")"});
}

TEST_CASE("every method of the threads stub reports unsupported") {
    std::unique_ptr<Threads> threads = makeThreadsStub();
    Waitable handle{1};
    CHECK(threads->StartThread([] { return 0u; }, ThreadOptions{}).Code() ==
          Error::kUnsupported);
    CHECK(threads->GetExitCode(handle).Code() == Error::kUnsupported);
    CHECK(threads->ResumeThread(handle).Code() == Error::kUnsupported);
    CHECK(threads->SetThreadPriority(handle, ThreadPriority::kNormal).Code() ==
          Error::kUnsupported);
    CHECK(threads->CurrentThreadId().Code() == Error::kUnsupported);
    CHECK(threads->MakeEvent(false, false, "").Code() == Error::kUnsupported);
    CHECK(threads->SetEvent(handle).Code() == Error::kUnsupported);
    CHECK(threads->ResetEvent(handle).Code() == Error::kUnsupported);
    CHECK(threads->MakeMutex(false, "TestMutex").Code() == Error::kUnsupported);
    CHECK(threads->ReleaseMutex(handle).Code() == Error::kUnsupported);
    CHECK(threads->MakeSemaphore(0, 1, "").Code() == Error::kUnsupported);
    CHECK(threads->ReleaseSemaphore(handle, 1).Code() == Error::kUnsupported);
    CHECK(threads->Open("TestMutex").Code() == Error::kUnsupported);
    CHECK(threads->Wait(&handle, 1, WaitMode::kAny, 0u).Code() == Error::kUnsupported);
    CHECK(threads->Close(handle).Code() == Error::kUnsupported);
    CHECK(threads->AllocateTlsSlot().Code() == Error::kUnsupported);
    CHECK(threads->FreeTlsSlot(0).Code() == Error::kUnsupported);
    CHECK(threads->SetTlsValue(0, 7).Code() == Error::kUnsupported);
    CHECK(threads->GetTlsValue(0).Code() == Error::kUnsupported);
}

} // namespace sfcop::platform
