#include "hle/kernel32rt/Dispatcher.h"

namespace sfcop::hle::kernel32rt {

namespace {

// EXCEPTION_RECORD's flags field.
constexpr std::uint32_t kRecordFlags = 4;

// A chain a handler has damaged (a record pointing at itself or back down the stack) must end the
// walk, not loop: records rise up the stack, and no thread has this many.
constexpr std::uint32_t kMaxChainRecords = 65536;

} // namespace

Dispatcher::Dispatcher(GuestMachine &machine, std::uint32_t nestedHandler)
    : machine_(machine), nestedHandler_(nestedHandler) {}

DispatchOutcome Dispatcher::Dispatch(std::uint32_t record, std::uint32_t context,
                                     std::uint32_t scratch) {
    GuestMachine &m = machine_;
    // The dispatcher's own registration record {next, handler, frame being handled} and, after
    // it, the dispatcher context a nested handler writes the frame it served into.
    const std::uint32_t own = scratch;
    const std::uint32_t dispatcherContext = scratch + 12;
    std::uint32_t nestedUntil = 0;
    std::uint32_t previous = 0, visited = 0;
    for (std::uint32_t frame = m.ChainHead(); frame != 0 && frame != kChainEnd;
         frame = m.Read32(frame)) {
        if (frame <= previous || ++visited > kMaxChainRecords) {
            return DispatchOutcome{DispatchOutcome::kCorruptChain, 0, 0};
        }
        previous = frame;
        const std::uint32_t handler = m.Read32(frame + 4);
        m.Write32(own, m.ChainHead());
        m.Write32(own + 4, nestedHandler_);
        m.Write32(own + 8, frame);
        m.SetChainHead(own);
        m.Write32(dispatcherContext, 0);
        const std::uint32_t arguments[4] = {record, frame, context, dispatcherContext};
        const std::uint32_t disposition = m.Call(handler, arguments, 4);
        m.SetChainHead(m.Read32(own));
        if (nestedUntil == frame) {
            m.Write32(record + kRecordFlags, m.Read32(record + kRecordFlags) & ~kExceptionNestedCall);
            nestedUntil = 0;
        }
        switch (disposition) {
        case kDispositionContinueExecution:
            if (m.Read32(record + kRecordFlags) & kExceptionNoncontinuable) {
                return DispatchOutcome{DispatchOutcome::kContinuedNoncontinuable, handler, 0};
            }
            return DispatchOutcome{DispatchOutcome::kContinueExecution, 0, 0};
        case kDispositionContinueSearch:
            break;
        case kDispositionNested: {
            m.Write32(record + kRecordFlags, m.Read32(record + kRecordFlags) | kExceptionNestedCall);
            const std::uint32_t served = m.Read32(dispatcherContext);
            if (served > nestedUntil) {
                nestedUntil = served;
            }
            break;
        }
        default:
            return DispatchOutcome{DispatchOutcome::kBadDisposition, handler, disposition};
        }
    }
    return DispatchOutcome{DispatchOutcome::kUnhandled, 0, 0};
}

UnwindOutcome Dispatcher::Unwind(std::uint32_t target, std::uint32_t record, std::uint32_t context,
                                 std::uint32_t scratch) {
    GuestMachine &m = machine_;
    m.Write32(record + kRecordFlags, m.Read32(record + kRecordFlags) | kExceptionUnwinding |
                                         (target == 0 ? kExceptionExitUnwind : 0u));
    UnwindOutcome outcome;
    std::uint32_t frame = m.ChainHead();
    std::uint32_t previous = 0, visited = 0;
    while (frame != 0 && frame != kChainEnd && frame != target) {
        if (target != 0 && target < frame) {
            outcome.kind = UnwindOutcome::kTargetBelow;
            return outcome;
        }
        if (frame <= previous || ++visited > kMaxChainRecords) {
            outcome.kind = UnwindOutcome::kCorruptChain;
            return outcome;
        }
        previous = frame;
        const std::uint32_t handler = m.Read32(frame + 4);
        m.Write32(scratch, 0);
        const std::uint32_t arguments[4] = {record, frame, context, scratch};
        const std::uint32_t disposition = m.Call(handler, arguments, 4);
        if (disposition == kDispositionCollided) {
            // An unwind already in progress has passed this point: carry on from where it is.
            frame = m.Read32(scratch);
        } else if (disposition != kDispositionContinueSearch) {
            outcome.kind = UnwindOutcome::kBadDisposition;
            outcome.handler = handler;
            outcome.disposition = disposition;
            return outcome;
        }
        frame = m.Read32(frame);
        m.SetChainHead(frame);
        outcome.frames++;
    }
    if (target != 0 && frame != target) {
        outcome.kind = UnwindOutcome::kTargetMissing;
    }
    return outcome;
}

std::uint32_t Dispatcher::NestedFrameHandler(GuestMachine &machine, std::uint32_t record,
                                             std::uint32_t frame,
                                             std::uint32_t dispatcherContext) {
    if (machine.Read32(record + kRecordFlags) & kExceptionUnwindMask) {
        return kDispositionContinueSearch;
    }
    machine.Write32(dispatcherContext, machine.Read32(frame + 8));
    return kDispositionNested;
}

} // namespace sfcop::hle::kernel32rt
