#include "app/input/ScriptPlayer.h"

#include "app/input/VirtualKeys.h"

#include <utility>

namespace sfcop::app::input {

namespace {

using platform::InputEventType;
using platform::WindowEvent;
using platform::WindowEventType;

std::string describeStep(const Step &step) {
    switch (step.kind) {
    case StepKind::kWindow:
        return "window " + std::to_string(step.milliseconds / 1000) + " " + step.text;
    case StepKind::kSleep:
        return "sleep " + std::to_string(step.milliseconds);
    case StepKind::kClick:
        return "click " + std::to_string(step.x) + " " + std::to_string(step.y);
    case StepKind::kKey:
        return "key " + std::to_string(step.virtualKey);
    case StepKind::kShot:
        return "shot " + step.text;
    case StepKind::kWaitFile:
        return "waitfile " + std::to_string(step.milliseconds / 1000) + " " + step.text;
    case StepKind::kList:
        return "list";
    case StepKind::kWaitMark:
        return "waitmark " + std::to_string(step.milliseconds / 1000) + " " + step.text;
    case StepKind::kFrames:
        return "frames " + std::to_string(step.frames);
    case StepKind::kKeyDown:
        return "keydown " + std::to_string(step.virtualKey);
    case StepKind::kKeyUp:
        return "keyup " + std::to_string(step.virtualKey);
    }
    return "?";
}

} // namespace

ScriptPlayer::ScriptPlayer(InputScript script, PlayerEnvironment environment)
    : script_(std::move(script)), environment_(std::move(environment)) {}

bool ScriptPlayer::Finished() const {
    return next_ >= script_.steps.size();
}

const std::optional<PlayerFailure> &ScriptPlayer::Failure() const {
    return failure_;
}

void ScriptPlayer::log(const std::string &line) const {
    if (environment_.log) {
        environment_.log(line);
    }
}

void ScriptPlayer::timedOut(const Step &step, std::string what) {
    failure_ = PlayerFailure{step.line, "waited " + std::to_string(step.milliseconds / 1000) +
                                            " s for " + std::move(what) + "; it did not come"};
}

std::vector<WindowEvent> ScriptPlayer::Advance(std::uint64_t frame, std::uint32_t nowMs) {
    std::vector<WindowEvent> out;
    while (!failure_ && next_ < script_.steps.size()) {
        const Step &step = script_.steps[next_];
        if (!started_) {
            started_ = true;
            startMs_ = nowMs;
            startFrame_ = frame;
            emitted_ = 0;
            log("line " + std::to_string(step.line) + " at " + std::to_string(nowMs) +
                " ms, frame " + std::to_string(frame) + ": " + describeStep(step));
        }
        if (!play(step, frame, nowMs, out)) {
            break;
        }
        started_ = false;
        ++next_;
    }
    return out;
}

WindowEvent ScriptPlayer::mouseEvent(WindowEventType type, InputEventType inputType,
                                     std::int32_t x, std::int32_t y, std::uint32_t nowMs) {
    WindowEvent event;
    event.type = type;
    event.timeMs = nowMs;
    event.input.type = inputType;
    event.input.timeMs = nowMs;
    event.input.button = platform::MouseButton::kLeft;
    event.input.x = x;
    event.input.y = y;
    if (inputType == InputEventType::kMouseMove) {
        event.input.dx = x - mouseX_;
        event.input.dy = y - mouseY_;
        mouseX_ = x;
        mouseY_ = y;
    }
    return event;
}

bool ScriptPlayer::play(const Step &step, std::uint64_t frame, std::uint32_t nowMs,
                        std::vector<WindowEvent> &out) {
    const std::uint32_t elapsed = nowMs - startMs_;
    switch (step.kind) {
    case StepKind::kWindow:
        // The player is advanced only from the game's own message pump: the game has its window.
        return true;
    case StepKind::kShot:
        log("line " + std::to_string(step.line) + ": shot is the Wine driver's; skipped (--shots "
                                                   "takes the native client's captures)");
        return true;
    case StepKind::kList:
        log("line " + std::to_string(step.line) + ": list is the Wine driver's; skipped");
        return true;
    case StepKind::kSleep:
        return elapsed >= step.milliseconds;
    case StepKind::kFrames:
        return frame - startFrame_ >= step.frames;
    case StepKind::kWaitFile:
        if (environment_.fileExists && environment_.fileExists(step.text)) {
            return true;
        }
        if (elapsed >= step.milliseconds) {
            timedOut(step, "the file " + step.text);
        }
        return false;
    case StepKind::kWaitMark:
        if (environment_.markReached && environment_.markReached(step.text)) {
            return true;
        }
        if (elapsed >= step.milliseconds) {
            timedOut(step, "the mark " + step.text);
        }
        return false;
    case StepKind::kClick:
    case StepKind::kKey:
    case StepKind::kKeyDown:
    case StepKind::kKeyUp: {
        // A click is three events and a key two; keydown and keyup are one each. Each event has
        // a frame of its own.
        const unsigned count = step.kind == StepKind::kClick ? 3
                               : step.kind == StepKind::kKey ? 2
                                                             : 1;
        if (emitted_ < count && lastInputFrame_ != frame) {
            if (step.kind == StepKind::kClick) {
                static constexpr struct {
                    WindowEventType window;
                    InputEventType input;
                } kClick[] = {
                    {WindowEventType::kMouseMove, InputEventType::kMouseMove},
                    {WindowEventType::kMouseButtonDown, InputEventType::kMouseButtonDown},
                    {WindowEventType::kMouseButtonUp, InputEventType::kMouseButtonUp},
                };
                out.push_back(mouseEvent(kClick[emitted_].window, kClick[emitted_].input, step.x,
                                         step.y, nowMs));
            } else {
                WindowEvent event;
                const bool down = step.kind == StepKind::kKeyDown ||
                                  (step.kind == StepKind::kKey && emitted_ == 0);
                event.type = down ? WindowEventType::kKeyDown : WindowEventType::kKeyUp;
                event.timeMs = nowMs;
                event.input.type = down ? InputEventType::kKeyDown : InputEventType::kKeyUp;
                event.input.timeMs = nowMs;
                // InputScript refuses a key the table lacks, so the lookup has a value.
                event.input.key = scancodeForVirtualKey(step.virtualKey).value_or(0);
                event.input.x = mouseX_;
                event.input.y = mouseY_;
                out.push_back(event);
            }
            lastInputFrame_ = frame;
            ++emitted_;
        }
        return emitted_ == count;
    }
    }
    return true;
}

} // namespace sfcop::app::input
