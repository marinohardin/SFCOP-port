#include "hle/kernel32rt/Dispatcher.h"

#include <doctest/doctest.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace sfcop::hle::kernel32rt {
namespace {

constexpr std::uint32_t kNested = 0x7F000010u; // the dispatcher's own frame handler
constexpr std::uint32_t kRecord = 0x0012F000u; // EXCEPTION_RECORD
constexpr std::uint32_t kContext = 0x0012E000u;
constexpr std::uint32_t kScratch = 0x0012D000u;

// Guest memory as a map of 32-bit words, and handlers as functions of the test's.
class FakeMachine final : public GuestMachine {
public:
    using Handler = std::function<std::uint32_t(const std::uint32_t *arguments)>;
    std::map<std::uint32_t, std::uint32_t> memory;
    std::uint32_t head = kChainEnd;
    std::map<std::uint32_t, Handler> handlers;
    std::vector<std::string> calls;

    std::uint32_t Read32(std::uint32_t address) override { return memory[address]; }
    void Write32(std::uint32_t address, std::uint32_t value) override { memory[address] = value; }
    std::uint32_t ChainHead() override { return head; }
    void SetChainHead(std::uint32_t record) override { head = record; }
    std::uint32_t Call(std::uint32_t address, const std::uint32_t *arguments,
                       std::uint32_t count) override {
        REQUIRE(count == 4);
        calls.push_back(std::to_string(address) + " frame " + std::to_string(arguments[1]) +
                        " flags " + std::to_string(memory[arguments[0] + 4]) + " head " +
                        std::to_string(head));
        if (address == kNested) {
            return Dispatcher::NestedFrameHandler(*this, arguments[0], arguments[1], arguments[3]);
        }
        return handlers.at(address)(arguments);
    }

    // A registration record {next, handler} at `frame`, pushed on the chain.
    void Push(std::uint32_t frame, std::uint32_t handler) {
        memory[frame] = head;
        memory[frame + 4] = handler;
        head = frame;
    }
};

std::uint32_t passOn(const std::uint32_t *) {
    return kDispositionContinueSearch;
}

TEST_CASE("an exception every frame passes on is unhandled, and each handler saw it once") {
    FakeMachine m;
    m.Push(0x0012FF00u, 100);
    m.Push(0x0012FE00u, 200);
    m.handlers[100] = passOn;
    m.handlers[200] = [&](const std::uint32_t *a) {
        CHECK(a[0] == kRecord);
        CHECK(a[1] == 0x0012FE00u);
        CHECK(a[2] == kContext);
        CHECK(a[3] == kScratch + 12);
        return kDispositionContinueSearch;
    };
    Dispatcher dispatcher(m, kNested);
    const DispatchOutcome outcome = dispatcher.Dispatch(kRecord, kContext, kScratch);
    CHECK(outcome.kind == DispatchOutcome::kUnhandled);
    // Around each call the dispatcher's own record is the chain head, and the head is back after.
    CHECK(m.calls == std::vector<std::string>{
                         "200 frame 1244672 flags 0 head " + std::to_string(kScratch),
                         "100 frame 1244928 flags 0 head " + std::to_string(kScratch)});
    CHECK(m.head == 0x0012FE00u);
    CHECK(m.memory[kScratch + 8] == 0x0012FF00u);
}

TEST_CASE("a handler that continues execution ends the dispatch") {
    FakeMachine m;
    m.Push(0x0012FF00u, 100);
    m.Push(0x0012FE00u, 200);
    m.handlers[200] = [](const std::uint32_t *) { return kDispositionContinueExecution; };
    Dispatcher dispatcher(m, kNested);
    CHECK(dispatcher.Dispatch(kRecord, kContext, kScratch).kind ==
          DispatchOutcome::kContinueExecution);
    CHECK(m.calls.size() == 1);
}

TEST_CASE("continuing a noncontinuable exception is reported, with the handler") {
    FakeMachine m;
    m.memory[kRecord + 4] = kExceptionNoncontinuable;
    m.Push(0x0012FE00u, 200);
    m.handlers[200] = [](const std::uint32_t *) { return kDispositionContinueExecution; };
    Dispatcher dispatcher(m, kNested);
    const DispatchOutcome outcome = dispatcher.Dispatch(kRecord, kContext, kScratch);
    CHECK(outcome.kind == DispatchOutcome::kContinuedNoncontinuable);
    CHECK(outcome.handler == 200);
}

TEST_CASE("a disposition no handler may return is reported") {
    FakeMachine m;
    m.Push(0x0012FE00u, 200);
    m.handlers[200] = [](const std::uint32_t *) { return 9u; };
    Dispatcher dispatcher(m, kNested);
    const DispatchOutcome outcome = dispatcher.Dispatch(kRecord, kContext, kScratch);
    CHECK(outcome.kind == DispatchOutcome::kBadDisposition);
    CHECK(outcome.disposition == 9);
}

TEST_CASE("an exception raised inside a handler is nested until the dispatch passes that frame") {
    // Frames, newest first: A (0x12FC00), the first dispatch's own record (served frame B at
    // 0x12FE00), B, C (0x12FF00). The nested dispatch must flag A and B as nested and clear the
    // flag before it reaches C.
    FakeMachine m;
    m.Push(0x0012FF00u, 300);
    m.Push(0x0012FE00u, 200);
    const std::uint32_t outer = 0x0012FD00u;
    m.memory[outer] = m.head;
    m.memory[outer + 4] = kNested;
    m.memory[outer + 8] = 0x0012FE00u;
    m.head = outer;
    m.Push(0x0012FC00u, 100);
    m.handlers[100] = passOn;
    m.handlers[200] = passOn;
    m.handlers[300] = passOn;
    Dispatcher dispatcher(m, kNested);
    CHECK(dispatcher.Dispatch(kRecord, kContext, kScratch).kind == DispatchOutcome::kUnhandled);
    const std::string own = " head " + std::to_string(kScratch);
    CHECK(m.calls == std::vector<std::string>{
                         "100 frame 1244160 flags 0" + own,
                         std::to_string(kNested) + " frame 1244416 flags 0" + own,
                         "200 frame 1244672 flags 16" + own,
                         "300 frame 1244928 flags 0" + own});
}

TEST_CASE("a chain that loops back on itself ends the dispatch as corrupt, each handler seen once") {
    FakeMachine m;
    m.Push(0x0012FF00u, 100);
    m.Push(0x0012FE00u, 200);
    m.memory[0x0012FF00u] = 0x0012FE00u; // the outer record points back at the inner one
    int seen = 0;
    m.handlers[100] = [&](const std::uint32_t *) { ++seen; return kDispositionContinueSearch; };
    m.handlers[200] = [&](const std::uint32_t *) { ++seen; return kDispositionContinueSearch; };
    Dispatcher dispatcher(m, kNested);
    const DispatchOutcome outcome = dispatcher.Dispatch(kRecord, kContext, kScratch);
    CHECK(outcome.kind == DispatchOutcome::kCorruptChain);
    CHECK(seen == 2);
}

TEST_CASE("a record pointing at itself ends the dispatch and an unwind as corrupt") {
    FakeMachine m;
    m.Push(0x0012FE00u, 200);
    m.memory[0x0012FE00u] = 0x0012FE00u;
    m.handlers[200] = passOn;
    Dispatcher dispatcher(m, kNested);
    CHECK(dispatcher.Dispatch(kRecord, kContext, kScratch).kind == DispatchOutcome::kCorruptChain);
    m.head = 0x0012FE00u;
    CHECK(dispatcher.Unwind(0, kRecord, kContext, kScratch).kind == UnwindOutcome::kCorruptChain);
}

TEST_CASE("an unwind calls every handler above the target with EXCEPTION_UNWINDING, and unlinks") {
    FakeMachine m;
    m.Push(0x0012FF00u, 300);
    m.Push(0x0012FE00u, 200);
    m.Push(0x0012FD00u, 100);
    m.handlers[100] = passOn;
    m.handlers[200] = passOn;
    Dispatcher dispatcher(m, kNested);
    const UnwindOutcome outcome = dispatcher.Unwind(0x0012FF00u, kRecord, kContext, kScratch);
    CHECK(outcome.kind == UnwindOutcome::kDone);
    CHECK(outcome.frames == 2);
    CHECK(m.head == 0x0012FF00u);
    CHECK(m.calls == std::vector<std::string>{"100 frame 1244416 flags 2 head 1244416",
                                              "200 frame 1244672 flags 2 head 1244672"});
}

TEST_CASE("an unwind with no target unwinds every frame as an exit unwind") {
    FakeMachine m;
    m.Push(0x0012FF00u, 300);
    m.handlers[300] = passOn;
    Dispatcher dispatcher(m, kNested);
    const UnwindOutcome outcome = dispatcher.Unwind(0, kRecord, kContext, kScratch);
    CHECK(outcome.kind == UnwindOutcome::kDone);
    CHECK(m.head == kChainEnd);
    CHECK(m.memory[kRecord + 4] == (kExceptionUnwinding | kExceptionExitUnwind));
}

TEST_CASE("an unwind meeting an unwind in progress carries on from where that one is") {
    FakeMachine m;
    m.Push(0x0012FF00u, 300);
    m.Push(0x0012FE00u, 200);
    m.Push(0x0012FD00u, 100);
    m.handlers[100] = [&](const std::uint32_t *a) {
        m.memory[a[3]] = 0x0012FE00u; // the other unwind has reached B
        return kDispositionCollided;
    };
    Dispatcher dispatcher(m, kNested);
    const UnwindOutcome outcome = dispatcher.Unwind(0x0012FF00u, kRecord, kContext, kScratch);
    CHECK(outcome.kind == UnwindOutcome::kDone);
    CHECK(m.calls.size() == 1); // B's handler is not called again
    CHECK(m.head == 0x0012FF00u);
}

TEST_CASE("an unwind to a frame that is not on the chain is reported") {
    FakeMachine m;
    m.Push(0x0012FE00u, 200);
    m.handlers[200] = passOn;
    Dispatcher dispatcher(m, kNested);
    CHECK(dispatcher.Unwind(0x0012FF80u, kRecord, kContext, kScratch).kind ==
          UnwindOutcome::kTargetMissing);
    FakeMachine below;
    below.Push(0x0012FE00u, 200);
    Dispatcher second(below, kNested);
    CHECK(second.Unwind(0x0012F000u, kRecord, kContext, kScratch).kind ==
          UnwindOutcome::kTargetBelow);
}

TEST_CASE("the dispatcher's own frame passes an unwind on") {
    FakeMachine m;
    m.memory[kRecord + 4] = kExceptionUnwinding;
    CHECK(Dispatcher::NestedFrameHandler(m, kRecord, 0x0012FD00u, kScratch) ==
          kDispositionContinueSearch);
}

} // namespace
} // namespace sfcop::hle::kernel32rt
