// A Threads for tests: records each call and returns the values the test programs. It starts no
// thread and never blocks; StartThread() keeps the body in `lastBody` for the test to run.
#pragma once

#include "platform/Core/MockLog.h"
#include "platform/Threads/Threads.h"

#include <vector>

namespace sfcop::platform {

class ThreadsMock final : public Threads {
public:
    MockLog log;

    std::function<std::uint32_t()> lastBody;
    ThreadOptions lastThreadOptions;
    std::vector<Waitable> lastWaitHandles;

    Result<Waitable> startThreadResult = Waitable{1};
    Result<std::uint32_t> exitCodeResult = std::uint32_t{0};
    Status resumeThreadResult;
    Status setThreadPriorityResult;
    Result<std::uint32_t> currentThreadIdResult = std::uint32_t{1};
    Result<Created> makeEventResult = Created{Waitable{2}, false};
    Status setEventResult;
    Status resetEventResult;
    Result<Created> makeMutexResult = Created{Waitable{3}, false};
    Status releaseMutexResult;
    Result<Created> makeSemaphoreResult = Created{Waitable{4}, false};
    Result<std::int32_t> releaseSemaphoreResult = std::int32_t{0};
    Result<Waitable> openResult = Waitable{2};
    Result<WaitResult> waitResult = WaitResult{WaitOutcome::kSignaled, 0};
    Status closeResult;
    Result<std::uint32_t> allocateTlsSlotResult = std::uint32_t{0};
    Status freeTlsSlotResult;
    Status setTlsValueResult;
    Result<std::uint64_t> getTlsValueResult = std::uint64_t{0};

    Result<Waitable> StartThread(std::function<std::uint32_t()> body,
                                 const ThreadOptions &options) override {
        log.Record("StartThread", options.name, options.startSuspended);
        lastBody = std::move(body);
        lastThreadOptions = options;
        return startThreadResult;
    }
    Result<std::uint32_t> GetExitCode(Waitable thread) override {
        log.Record("GetExitCode", thread);
        return exitCodeResult;
    }
    Status ResumeThread(Waitable thread) override {
        log.Record("ResumeThread", thread);
        return resumeThreadResult;
    }
    Status SetThreadPriority(Waitable thread, ThreadPriority priority) override {
        log.Record("SetThreadPriority", thread, priority);
        return setThreadPriorityResult;
    }
    Result<std::uint32_t> CurrentThreadId() override {
        log.Record("CurrentThreadId");
        return currentThreadIdResult;
    }
    Result<Created> MakeEvent(bool manualReset, bool initiallySignaled,
                              std::string_view name) override {
        log.Record("MakeEvent", manualReset, initiallySignaled, name);
        return makeEventResult;
    }
    Status SetEvent(Waitable event) override {
        log.Record("SetEvent", event);
        return setEventResult;
    }
    Status ResetEvent(Waitable event) override {
        log.Record("ResetEvent", event);
        return resetEventResult;
    }
    Result<Created> MakeMutex(bool initiallyOwned, std::string_view name) override {
        log.Record("MakeMutex", initiallyOwned, name);
        return makeMutexResult;
    }
    Status ReleaseMutex(Waitable mutex) override {
        log.Record("ReleaseMutex", mutex);
        return releaseMutexResult;
    }
    Result<Created> MakeSemaphore(std::int32_t initialCount, std::int32_t maximumCount,
                                  std::string_view name) override {
        log.Record("MakeSemaphore", initialCount, maximumCount, name);
        return makeSemaphoreResult;
    }
    Result<std::int32_t> ReleaseSemaphore(Waitable semaphore, std::int32_t count) override {
        log.Record("ReleaseSemaphore", semaphore, count);
        return releaseSemaphoreResult;
    }
    Result<Waitable> Open(std::string_view name) override {
        log.Record("Open", name);
        return openResult;
    }
    Result<WaitResult> Wait(const Waitable *handles, std::size_t count, WaitMode mode,
                            std::optional<std::uint32_t> timeoutMs) override {
        lastWaitHandles.assign(handles, handles + count);
        if (timeoutMs) {
            log.Record("Wait", count, mode, *timeoutMs);
        } else {
            log.Record("Wait", count, mode, "forever");
        }
        return waitResult;
    }
    Status Close(Waitable handle) override {
        log.Record("Close", handle);
        return closeResult;
    }
    Result<std::uint32_t> AllocateTlsSlot() override {
        log.Record("AllocateTlsSlot");
        return allocateTlsSlotResult;
    }
    Status FreeTlsSlot(std::uint32_t slot) override {
        log.Record("FreeTlsSlot", slot);
        return freeTlsSlotResult;
    }
    Status SetTlsValue(std::uint32_t slot, std::uint64_t value) override {
        log.Record("SetTlsValue", slot, value);
        return setTlsValueResult;
    }
    Result<std::uint64_t> GetTlsValue(std::uint32_t slot) override {
        log.Record("GetTlsValue", slot);
        return getTlsValueResult;
    }
};

} // namespace sfcop::platform
