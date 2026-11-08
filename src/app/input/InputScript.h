// InputScript: a steps file, as the oracle driver plays it (DESIGN §5.10).
//
// One step per line; a line whose first non-blank character is '#' is a comment, and blank lines
// are skipped. The vocabulary is the Wine driver's (oracle/driver/oracle_input.c), so one file
// drives the original under Wine and the native client:
//
//   window <timeout_s> <title>     wait until the game reads its window's messages
//   sleep <ms>                     wait that long
//   click <x> <y>                  left click at (x, y), in the game's display pixels
//   key <vk>                       press and release a Windows virtual key (decimal or 0x hex)
//   shot <file.bmp>                the Wine driver's capture; the native player logs and skips it
//   waitfile <timeout_s> <path>    wait until the file exists
//   list                           the Wine driver's window list; the native player skips it
//
// Four steps are the native player's own; the Wine driver refuses them, failing with their line:
//
//   waitmark <timeout_s> <mark>    wait until the game reaches a mark (its name or 0x address)
//   frames <n>                     wait n frames
//   keydown <vk>                   press a virtual key and hold it
//   keyup <vk>                     release a held virtual key
//
// A key held by keydown stays down for the steps after it, so `keydown 0x10`, `key 0x32`,
// `keyup 0x10` types '@' (the guest's TranslateMessage reads Shift from the held keys when the
// game takes the message; README.md, "Steps", says why a pause around each helps).
//
// Every wait with a timeout fails the run when it runs out (ScriptPlayer). Parsing checks form
// and ranges only.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sfcop::app::input {

enum class StepKind : std::uint8_t {
    kWindow,
    kSleep,
    kClick,
    kKey,
    kShot,
    kWaitFile,
    kList,
    kWaitMark,
    kFrames,
    kKeyDown,
    kKeyUp,
};

struct Step {
    StepKind kind = StepKind::kSleep;
    unsigned line = 0;            // 1-based line in the file
    std::uint32_t milliseconds = 0; // kSleep; the timeout of kWindow, kWaitFile and kWaitMark
    std::int32_t x = 0;           // kClick
    std::int32_t y = 0;
    std::uint16_t virtualKey = 0; // kKey, kKeyDown, kKeyUp
    std::uint32_t frames = 0;     // kFrames
    std::string text;             // kWindow's title, kShot's and kWaitFile's path, kWaitMark's mark
};

struct ScriptError {
    unsigned line = 0;
    std::string message; // one sentence naming what is wrong
};

struct InputScript {
    std::vector<Step> steps;
};

struct ParsedScript {
    InputScript script;
    std::optional<ScriptError> error; // set: the file is refused, and `script` is empty
};

// Reads a steps file's text. The first bad line refuses the whole file.
ParsedScript parseInputScript(std::string_view text);

// "line 4: ..." for an error, as the client prints it.
std::string describe(const ScriptError &error);

} // namespace sfcop::app::input
