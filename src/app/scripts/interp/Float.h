// The x87 instructions (DESIGN §12.7): the register stack is the lifted game's own, eight doubles
// with st(0) first, so values cross calls between script and game exactly as between lifted
// functions; arithmetic is done in double as the lifter does it, with its rules for what a double
// stack cannot hold (80-bit loads and stores, the exact shadow of a 64-bit integer load, rounding
// by the control word, the status word built from the last compare).
#pragma once

#include "app/scripts/interp/Insn.h"

#include <cstdint>

namespace sfcop::scripts::interp {

// Fills i.run for the escape opcode `op` (0xD8 to 0xDF), its ModRM already read into i.
// Returns false when the interpreter does not know the instruction.
bool decodeFloat(std::uint8_t op, Insn &i);

} // namespace sfcop::scripts::interp
