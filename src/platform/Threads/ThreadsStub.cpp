// The stub backend: compiles on every platform and supports nothing.
#include "platform/Threads/Threads.h"

namespace sfcop::platform {
namespace {

class ThreadsStub final : public Threads {
public:
    Result<Waitable> StartThread(std::function<std::uint32_t()>, const ThreadOptions &) override {
        return Error::kUnsupported;
    }
    Result<std::uint32_t> GetExitCode(Waitable) override { return Error::kUnsupported; }
    Status ResumeThread(Waitable) override { return Error::kUnsupported; }
    Status SetThreadPriority(Waitable, ThreadPriority) override { return Error::kUnsupported; }
    Result<std::uint32_t> CurrentThreadId() override { return Error::kUnsupported; }
    Result<Created> MakeEvent(bool, bool, std::string_view) override {
        return Error::kUnsupported;
    }
    Status SetEvent(Waitable) override { return Error::kUnsupported; }
    Status ResetEvent(Waitable) override { return Error::kUnsupported; }
    Result<Created> MakeMutex(bool, std::string_view) override { return Error::kUnsupported; }
    Status ReleaseMutex(Waitable) override { return Error::kUnsupported; }
    Result<Created> MakeSemaphore(std::int32_t, std::int32_t, std::string_view) override {
        return Error::kUnsupported;
    }
    Result<std::int32_t> ReleaseSemaphore(Waitable, std::int32_t) override {
        return Error::kUnsupported;
    }
    Result<Waitable> Open(std::string_view) override { return Error::kUnsupported; }
    Result<WaitResult> Wait(const Waitable *, std::size_t, WaitMode,
                            std::optional<std::uint32_t>) override {
        return Error::kUnsupported;
    }
    Status Close(Waitable) override { return Error::kUnsupported; }
    Result<std::uint32_t> AllocateTlsSlot() override { return Error::kUnsupported; }
    Status FreeTlsSlot(std::uint32_t) override { return Error::kUnsupported; }
    Status SetTlsValue(std::uint32_t, std::uint64_t) override { return Error::kUnsupported; }
    Result<std::uint64_t> GetTlsValue(std::uint32_t) override { return Error::kUnsupported; }
};

} // namespace

std::unique_ptr<Threads> makeThreadsStub() { return std::make_unique<ThreadsStub>(); }

} // namespace sfcop::platform
