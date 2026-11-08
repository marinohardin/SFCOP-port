// ScriptPlayer: plays an InputScript against the game's frames and clock.
//
// The caller advances it once per frame with the frame number (increasing) and Clock::Ticks();
// it returns the window events due in that frame, which the caller delivers as a real window's
// would (ScriptedWindow). A step starts in the frame the previous one ended, and at most one
// input event is due per frame, so
//
//   click x y   is a mouse move to (x, y), a left button down and a left button up, each in a
//               frame of its own;
//   key vk      is the key down and the key up, each in a frame of its own;
//   keydown vk, keyup vk   are the key down alone and the key up alone, one frame each; the
//               key stays down for the steps between them;
//   sleep, frames, window, waitfile, waitmark   wait (by the clock, by frames, or for a condition);
//   shot, list  are the Wine driver's: skipped, with a log line.
//
// A wait that runs out fails the script: Failure() then names the step's line and what it waited
// for, and Advance() returns nothing from then on. Not thread-safe; the caller's one thread.
#pragma once

#include "app/input/InputScript.h"
#include "platform/Window/Window.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace sfcop::app::input {

// What the waits ask about, and where the step log goes. Each may be empty: an empty
// fileExists or markReached never answers yes, an empty log drops the lines.
struct PlayerEnvironment {
    std::function<bool(const std::string &path)> fileExists;
    std::function<bool(const std::string &mark)> markReached;
    std::function<void(const std::string &line)> log;
};

struct PlayerFailure {
    unsigned line = 0;
    std::string message; // one sentence, naming what the step waited for
};

class ScriptPlayer {
public:
    ScriptPlayer(InputScript script, PlayerEnvironment environment);

    // The events due in `frame` at `nowMs`, in order, each stamped with `nowMs`.
    std::vector<platform::WindowEvent> Advance(std::uint64_t frame, std::uint32_t nowMs);

    // Every step has been played.
    bool Finished() const;

    // Set once a wait has run out.
    const std::optional<PlayerFailure> &Failure() const;

private:
    // Plays the current step; true when it has ended, so the next may start.
    bool play(const Step &step, std::uint64_t frame, std::uint32_t nowMs,
              std::vector<platform::WindowEvent> &out);
    platform::WindowEvent mouseEvent(platform::WindowEventType type,
                                     platform::InputEventType inputType, std::int32_t x,
                                     std::int32_t y, std::uint32_t nowMs);
    void log(const std::string &line) const;
    void timedOut(const Step &step, std::string what);

    InputScript script_;
    PlayerEnvironment environment_;
    std::size_t next_ = 0;
    bool started_ = false;
    std::uint32_t startMs_ = 0;
    std::uint64_t startFrame_ = 0;
    unsigned emitted_ = 0;                 // input events of the current click or key so far
    std::optional<std::uint64_t> lastInputFrame_;
    std::int32_t mouseX_ = 0;
    std::int32_t mouseY_ = 0;
    std::optional<PlayerFailure> failure_;
};

} // namespace sfcop::app::input
