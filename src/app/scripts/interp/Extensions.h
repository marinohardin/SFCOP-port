// Instructions of the extensions a processor reports through cpuid: MMX, 3DNow!, and SSE with
// what came after it. The interpreter's cpuid reports none of them (Decode.cpp), so a script that
// checks the processor, as a C runtime does, never runs one; its code still carries them behind
// that check. They are decoded for their length only, so the load-time survey knows them for
// instructions and walks on (CodeSurvey.h), and running one stops the script with a line that
// names it, the address and the extension (Interpreter::stopScript).
#pragma once

#include <cstdint>

namespace sfcop::scripts::interp {

enum class Extension : std::uint8_t { kNone, kMmx, k3dNow, kSse };

// The extension of the two-byte opcode 0F op2, or kNone. `prefixed`: a 66, F2 or F3 prefix came
// before it, which makes an MMX opcode an SSE one.
Extension extensionOf(std::uint8_t op2, bool prefixed);
// Its operands: 0F 38 and 0F 3A carry a third opcode byte first; all but emms and femms a ModRM
// byte; the shifts by a count, the shuffles, the compares with a predicate, 0F 3A and 3DNow!'s
// operation byte one byte after it.
bool extensionHasThirdByte(std::uint8_t op2);
bool extensionHasModrm(std::uint8_t op2);
bool extensionHasImmediate(std::uint8_t op2);

// "an MMX", "a 3DNow!", "an SSE": for "... reached an MMX instruction".
const char *extensionArticleName(Extension extension);

} // namespace sfcop::scripts::interp
