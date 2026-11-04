// GuestExceptions: the host's half of guest exceptions (the walk itself is hle/kernel32rt's
// RaiseException and RtlUnwind).
//
// Every lifted function that owns a catching frame has a landing pad (RECOMP_EH_ENTER, a setjmp).
// When a catch has run, the C runtime moves control into the frame further up the stack
// (_JumpToContinuation, or _except_handler3 entering an __except block); lifted, the frames in
// between are still host calls, so the host longjmps to the owner's pad instead. Each pad records
// the machine lock's depth when it was entered, and a jump to it puts that depth back: a jump over
// a native-to-guest call (a callback the game throws out of) would otherwise leave the lock held
// one level too deep, and no other guest thread would ever run again.
//
// A hardware fault on guest memory and a zero divisor are raised as the guest's own exceptions
// (EXCEPTION_ACCESS_VIOLATION, EXCEPTION_INT_DIVIDE_BY_ZERO), as Windows delivers them, so the
// game's catch(...) and __except blocks see them.
#pragma once

#include <cstdint>

namespace sfcop::guest {

// Installs the zero-divisor hook and the fault handlers. Once, before the game runs. Only a
// fault whose instruction is lifted code becomes a guest exception (FaultClass.h); a fault in
// host code ends the run.
void InstallExceptionHooks();

// Looks up the RaiseException row that RaiseFromHost() calls. Once, after the modules' tables are
// registered.
void BindRaiseException();

// Raises `code` with `count` parameters from host code running inside lifted code (a fault
// handler, the divide hook): RaiseException's dispatch over the guest's frames. Returns only if
// a handler continued execution.
void RaiseFromHost(std::uint32_t code, std::uint32_t count, const std::uint32_t *parameters);

} // namespace sfcop::guest
