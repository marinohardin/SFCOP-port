// The one error type every platform component returns (DESIGN §6.2). A method either succeeds
// with a value or fails with an Error; nothing in src/platform/ throws for an expected failure
// (CONVENTIONS §4.2). The HLE modules translate an Error into the Win32 or DirectX code the game
// expects; the components never see those codes.
#pragma once

#include <cassert>
#include <optional>
#include <utility>

namespace sfcop::platform {

// Why a call failed. The list is shared by all eight components so that the HLE layer needs one
// mapping per Win32 module, not one per component. A component documents which values each of
// its methods can return; a value it does not list is a backend defect.
enum class Error {
    kNone,              // no error; only Result::Code() of a successful result returns it
    kUnsupported,       // this backend does not implement the call (every stub method)
    kInvalidArgument,   // an argument is out of range, malformed or inconsistent
    kInvalidHandle,     // a handle is zero, already destroyed, or from another component
    kWrongState,        // the call is not allowed in the object's current state
    kNotFound,          // a file, key, value, name or host does not exist
    kAlreadyExists,     // an exclusive create met an existing object
    kAccessDenied,      // the target is read-only or the caller may not change it
    kNotOwner,          // a mutex was released by a thread that does not own it
    kOutOfResources,    // memory, handles, voices or device resources are exhausted
    kIoError,           // the operating system reported a read or write failure
    kWouldBlock,        // a non-blocking socket call cannot complete now
    kInProgress,        // a non-blocking connect has started and has not finished
    kTimedOut,          // a timed operation ran out of time
    kConnectionRefused, // the peer refused a connection
    kConnectionReset,   // the peer reset or aborted the connection
    kNotConnected,      // the socket has no peer
    kAddressInUse,      // the local address is taken
    kHostUnreachable,   // no route to the peer
};

// The enumerator's name without its `k`, for logs and test messages.
constexpr const char *errorName(Error error) {
    switch (error) {
    case Error::kNone:
        return "None";
    case Error::kUnsupported:
        return "Unsupported";
    case Error::kInvalidArgument:
        return "InvalidArgument";
    case Error::kInvalidHandle:
        return "InvalidHandle";
    case Error::kWrongState:
        return "WrongState";
    case Error::kNotFound:
        return "NotFound";
    case Error::kAlreadyExists:
        return "AlreadyExists";
    case Error::kAccessDenied:
        return "AccessDenied";
    case Error::kNotOwner:
        return "NotOwner";
    case Error::kOutOfResources:
        return "OutOfResources";
    case Error::kIoError:
        return "IoError";
    case Error::kWouldBlock:
        return "WouldBlock";
    case Error::kInProgress:
        return "InProgress";
    case Error::kTimedOut:
        return "TimedOut";
    case Error::kConnectionRefused:
        return "ConnectionRefused";
    case Error::kConnectionReset:
        return "ConnectionReset";
    case Error::kNotConnected:
        return "NotConnected";
    case Error::kAddressInUse:
        return "AddressInUse";
    case Error::kHostUnreachable:
        return "HostUnreachable";
    }
    return "Unknown";
}

// A value of type T, or the Error that prevented it. Converts implicitly from either, so a method
// writes `return value;` or `return Error::kNotFound;`. Reading Value() of a failed result is a
// programming error (asserted).
template <typename T> class [[nodiscard]] Result {
public:
    Result(T value) : value_(std::move(value)) {}
    Result(Error error) : error_(error) { assert(error != Error::kNone); }

    bool Ok() const { return value_.has_value(); }
    explicit operator bool() const { return Ok(); }
    Error Code() const { return error_; }

    T &Value() & {
        assert(Ok());
        return *value_;
    }
    const T &Value() const & {
        assert(Ok());
        return *value_;
    }
    T ValueOr(T fallback) const { return Ok() ? *value_ : std::move(fallback); }

private:
    std::optional<T> value_;
    Error error_ = Error::kNone;
};

// Success with no value, or an Error. Default-constructed is success: `return {};`.
template <> class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(error) { assert(error != Error::kNone); }

    bool Ok() const { return error_ == Error::kNone; }
    explicit operator bool() const { return Ok(); }
    Error Code() const { return error_; }

private:
    Error error_ = Error::kNone;
};

using Status = Result<void>;

} // namespace sfcop::platform
