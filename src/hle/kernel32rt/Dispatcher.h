// Dispatcher: what RaiseException and RtlUnwind do with the guest's own exception frames, the
// x86 dispatch Windows performs for a structured or C++ exception.
//
// The guest's statically linked C runtime holds everything else a throw needs: _CxxThrowException,
// __CxxFrameHandler with its catch matching and unwind funclets, _except_handler3 for
// __try/__except. Windows supplies only the walk over the fs:[0] chain, and that walk is this:
//
//   Dispatch  calls each frame's handler, newest first, with the EXCEPTION_RECORD, the frame, the
//             CONTEXT and a dispatcher context, until one asks to continue execution. Around each
//             call it puts a record of its own on the chain, so that an exception raised inside a
//             handler (a throw in a catch block) finds it and is marked nested
//             (EXCEPTION_NESTED_CALL) until the dispatch passes the frame that handler served.
//   Unwind    calls the handler of every frame above the target with EXCEPTION_UNWINDING (and
//             EXCEPTION_EXIT_UNWIND when there is no target) and unlinks it.
//
// A handler that catches never returns to the dispatch: its continuation is entered by a
// transfer the guest host makes (src/app/guest/). The guest's memory, its frame chain and calls
// into guest code come through GuestMachine, so the walk is tested with a machine of the test's.
//
// Threading: one dispatch per guest thread at a time, on that thread.
#pragma once

#include <cstdint>

namespace sfcop::hle::kernel32rt {

// EXCEPTION_RECORD flags and handler dispositions.
constexpr std::uint32_t kExceptionNoncontinuable = 0x01u;
constexpr std::uint32_t kExceptionUnwinding = 0x02u;
constexpr std::uint32_t kExceptionExitUnwind = 0x04u;
constexpr std::uint32_t kExceptionNestedCall = 0x10u;
constexpr std::uint32_t kExceptionUnwindMask = 0x66u;
constexpr std::uint32_t kDispositionContinueExecution = 0;
constexpr std::uint32_t kDispositionContinueSearch = 1;
constexpr std::uint32_t kDispositionNested = 2;
constexpr std::uint32_t kDispositionCollided = 3;
constexpr std::uint32_t kChainEnd = 0xFFFFFFFFu;
constexpr std::uint32_t kStatusUnwind = 0xC0000027u;
constexpr std::uint32_t kMsvcCxxException = 0xE06D7363u;

// The guest as the dispatcher sees it.
class GuestMachine {
public:
    virtual ~GuestMachine() = default;
    virtual std::uint32_t Read32(std::uint32_t address) = 0;
    virtual void Write32(std::uint32_t address, std::uint32_t value) = 0;
    // fs:[0] of the current thread: the newest exception registration record.
    virtual std::uint32_t ChainHead() = 0;
    virtual void SetChainHead(std::uint32_t record) = 0;
    // Calls guest code at `address` with `count` arguments (cdecl: the caller's stack and
    // registers are as they were afterwards) and returns eax.
    virtual std::uint32_t Call(std::uint32_t address, const std::uint32_t *arguments,
                               std::uint32_t count) = 0;
};

struct DispatchOutcome {
    enum Kind {
        kContinueExecution, // a handler asked to resume where the exception was raised
        kUnhandled,         // every frame passed it on
        kContinuedNoncontinuable, // a handler asked to resume an EXCEPTION_NONCONTINUABLE one
        kBadDisposition,    // a handler returned something no handler may
        kCorruptChain,      // a record points at itself or back down the stack, or the chain is
                            // implausibly long: the walk stops, the exception is unhandled
    } kind = kUnhandled;
    std::uint32_t handler = 0;     // for the errors: the handler that did it
    std::uint32_t disposition = 0; // for kBadDisposition
};

struct UnwindOutcome {
    enum Kind {
        kDone,           // every frame above the target was unwound and unlinked
        kTargetBelow,    // the target lies below the newest frame (already gone)
        kTargetMissing,  // the chain ended before the target
        kBadDisposition, // a handler returned something an unwind does not allow
        kCorruptChain,   // as for a dispatch
    } kind = kDone;
    std::uint32_t frames = 0; // how many frames were unwound
    std::uint32_t handler = 0;
    std::uint32_t disposition = 0;
};

class Dispatcher {
public:
    // `nestedHandler` is the guest address that runs NestedFrameHandler() (a shim).
    Dispatcher(GuestMachine &machine, std::uint32_t nestedHandler);

    // RaiseException's walk for the EXCEPTION_RECORD at `record` and the CONTEXT at `context`;
    // `scratch` is 16 bytes of guest stack for the dispatcher's own record and context.
    DispatchOutcome Dispatch(std::uint32_t record, std::uint32_t context, std::uint32_t scratch);

    // RtlUnwind's walk to the frame `target` (0: every frame) for the EXCEPTION_RECORD at
    // `record`, whose flags it marks; `scratch` is 4 bytes of guest stack.
    UnwindOutcome Unwind(std::uint32_t target, std::uint32_t record, std::uint32_t context,
                         std::uint32_t scratch);

    // The handler of the dispatcher's own record (cdecl: record, frame, context, dispatcher
    // context): during a dispatch it names the frame being handled and answers
    // ExceptionNestedException; during an unwind it passes.
    static std::uint32_t NestedFrameHandler(GuestMachine &machine, std::uint32_t record,
                                            std::uint32_t frame, std::uint32_t dispatcherContext);

private:
    GuestMachine &machine_;
    std::uint32_t nestedHandler_;
};

} // namespace sfcop::hle::kernel32rt
