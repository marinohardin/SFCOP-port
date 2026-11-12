// A survey of a script's code before it runs (DESIGN §5.9): the instructions control can reach
// from where the game and the script's own tables enter it, decoded as the interpreter would
// decode them, so a script holding an instruction the interpreter does not know is refused at load
// with its name and the instruction's address, not stopped in the middle of a mission.
//
// It is not a linear sweep: it decodes only what control flow reaches, so the data a code section
// holds (jump tables, byte tables, strings, padding) is not read as code. Control is followed from
// the DLL entry point and the two contract exports, from every relocated pointer into code that a
// data section holds (virtual tables, the C runtime's initializer lists, function pointers), from
// every relocated immediate into code that an instruction carries (a handler pushed, a callback
// stored), and through every jump table an indirect `jmp` indexes. A path ends at a `ret`, an
// unconditional jump, an indirect jump, a trap, the end of the code, a relocated word (data), or an
// instruction of an extension the processor does not report (MMX, 3DNow!, SSE: the script stops
// there if it runs one, so nothing after it runs).
//
// Where the survey cannot be sure it is still in code, it does not refuse. The one place a
// compiler puts data straight after an instruction is after a call that does not return, so the
// instruction after a call is certain only when the call returns: a call to a function of the
// script from which the survey reaches a `ret`, or a call out of the script, except to an import
// that never returns (ExitProcess, ExitThread, RaiseException, FatalExit, FatalAppExitA). What only
// such a call leads to is surveyed as uncertain: an instruction the interpreter does not know there
// is counted, not refused, and stops the script with its address if it ever runs.
//
// Code that only a computed, unrelocated address reaches is not found; running it is the same
// run-time stop.
#pragma once

#include "app/scripts/interp/Interpreter.h"
#include "app/scripts/interp/ScriptImage.h"

#include <cstdint>
#include <optional>

namespace sfcop::scripts::interp {

struct SurveyResult {
    std::uint64_t instructions = 0; // decoded, each once
    // The first instruction the interpreter does not know that certain control flow reaches,
    // where the survey stops: its guest address. The script is refused.
    std::optional<std::uint32_t> unknown;
    // Instructions it does not know where the survey was not sure it was in code: how many, and
    // the first. The script loads.
    std::uint32_t uncertain = 0;
    std::optional<std::uint32_t> firstUncertain;
    // Instructions of extensions the processor does not report, reached.
    std::uint32_t extensions = 0;
};

// Surveys `image` (laid out at its base), whose executable sections `interpreter` has as code.
SurveyResult surveyCode(const Interpreter &interpreter, const ScriptImage &image);

} // namespace sfcop::scripts::interp
