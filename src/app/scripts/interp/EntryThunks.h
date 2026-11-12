// The host functions that lifted code calls to enter script code. The lifter's dispatch answers a
// guest address with a function that takes no arguments, and a caller may keep that function for
// the address (a callback, a vtable it resolved once), so each script address the game asks for
// gets a function of its own: one of a fixed pool of distinct functions, each tied to an address
// and the interpreter that runs it until it is given back. Nothing is generated at run time.
//
// Threading: used with the guest machine held.
#pragma once

#include "app/scripts/interp/Machine.h"

#include <cstddef>
#include <cstdint>

namespace sfcop::scripts::interp {

class Interpreter;

// The pool's size: the script functions the game can hold at once, across every loaded script.
constexpr std::size_t kEntryThunks = 4096;

// The function that enters `interpreter` at `address`: the one it already has, or a free one.
// Null when the pool is spent.
HostFunction entryThunk(Interpreter &interpreter, std::uint32_t address);

// Gives back every function tied to an address in [start, end) of `interpreter`. A function given
// back stays tied to its address until the pool hands it out again, the longest given back first:
// called in that time, it fails the call (Interpreter::call, with no script at the address).
void releaseEntryThunks(const Interpreter &interpreter, std::uint32_t start, std::uint32_t end);

// The functions in use, for tests and the log.
std::size_t entryThunksInUse();

} // namespace sfcop::scripts::interp
