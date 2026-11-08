#include "app/input/InputScript.h"

#include <cstdint>

#include "app/input/VirtualKeys.h"

namespace sfcop::app::input {

namespace {

constexpr std::uint64_t kMaxSeconds = 86400;
constexpr std::uint64_t kMaxMilliseconds = kMaxSeconds * 1000;
constexpr std::uint64_t kMaxCoordinate = 16383;
constexpr std::uint64_t kMaxVirtualKey = 254;
constexpr std::uint64_t kMaxFrames = 1000000;

bool isBlank(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

// Splits off the next word of `rest`, skipping blanks first; empty when none is left.
std::string_view nextWord(std::string_view &rest) {
    std::size_t start = 0;
    while (start < rest.size() && isBlank(rest[start])) {
        ++start;
    }
    std::size_t end = start;
    while (end < rest.size() && !isBlank(rest[end])) {
        ++end;
    }
    const std::string_view word = rest.substr(start, end - start);
    rest.remove_prefix(end);
    return word;
}

std::string_view trimmed(std::string_view text) {
    while (!text.empty() && isBlank(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isBlank(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

// A whole number, decimal or (when `hex` is allowed) 0x hexadecimal, up to `max`.
std::optional<std::uint64_t> number(std::string_view word, std::uint64_t max, bool hex = false) {
    unsigned base = 10;
    if (hex && word.size() > 2 && word[0] == '0' && (word[1] == 'x' || word[1] == 'X')) {
        base = 16;
        word.remove_prefix(2);
    }
    if (word.empty() || word.size() > 12) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (char c : word) {
        unsigned digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<unsigned>(c - '0');
        } else if (base == 16 && c >= 'a' && c <= 'f') {
            digit = static_cast<unsigned>(c - 'a' + 10);
        } else if (base == 16 && c >= 'A' && c <= 'F') {
            digit = static_cast<unsigned>(c - 'A' + 10);
        } else {
            return std::nullopt;
        }
        value = value * base + digit;
    }
    if (value > max) {
        return std::nullopt;
    }
    return value;
}

struct LineResult {
    std::optional<Step> step;
    std::string error;
};

LineResult fail(std::string message) {
    return LineResult{std::nullopt, std::move(message)};
}

// `timeout_s` then the rest of the line, which must not be empty.
LineResult timeoutAndText(Step step, std::string_view verb, std::string_view rest,
                          std::string_view what) {
    const auto seconds = number(nextWord(rest), kMaxSeconds);
    const std::string_view text = trimmed(rest);
    if (!seconds || text.empty()) {
        return fail(std::string(verb) + " takes a timeout of 0 to 86400 seconds and " +
                    std::string(what));
    }
    step.milliseconds = static_cast<std::uint32_t>(*seconds * 1000);
    step.text = std::string(text);
    return LineResult{step, {}};
}

bool nothingLeft(std::string_view rest) {
    return trimmed(rest).empty();
}

LineResult parseLine(std::string_view line, unsigned number_) {
    std::string_view rest = line;
    const std::string_view verb = nextWord(rest);
    Step step;
    step.line = number_;
    if (verb == "window") {
        step.kind = StepKind::kWindow;
        return timeoutAndText(step, verb, rest, "a window title");
    }
    if (verb == "waitfile") {
        step.kind = StepKind::kWaitFile;
        return timeoutAndText(step, verb, rest, "a path");
    }
    if (verb == "waitmark") {
        step.kind = StepKind::kWaitMark;
        return timeoutAndText(step, verb, rest, "a mark's name or address");
    }
    if (verb == "sleep") {
        const auto ms = number(nextWord(rest), kMaxMilliseconds);
        if (!ms || !nothingLeft(rest)) {
            return fail("sleep takes one whole number of milliseconds, 0 to 86400000");
        }
        step.kind = StepKind::kSleep;
        step.milliseconds = static_cast<std::uint32_t>(*ms);
        return LineResult{step, {}};
    }
    if (verb == "click") {
        const auto x = number(nextWord(rest), kMaxCoordinate);
        const auto y = number(nextWord(rest), kMaxCoordinate);
        if (!x || !y || !nothingLeft(rest)) {
            return fail("click takes two coordinates, x and y, each 0 to 16383");
        }
        step.kind = StepKind::kClick;
        step.x = static_cast<std::int32_t>(*x);
        step.y = static_cast<std::int32_t>(*y);
        return LineResult{step, {}};
    }
    if (verb == "key" || verb == "keydown" || verb == "keyup") {
        const auto vk = number(nextWord(rest), kMaxVirtualKey, true);
        if (!vk || *vk == 0 || !nothingLeft(rest)) {
            return fail(std::string(verb) + " takes one virtual key, 1 to 254, decimal or 0x hex");
        }
        if (!scancodeForVirtualKey(static_cast<std::uint16_t>(*vk))) {
            return fail(std::string(verb) + " " + std::string(trimmed(line.substr(verb.size()))) +
                        " is a virtual key the player has no keyboard key for");
        }
        step.kind = verb == "key"       ? StepKind::kKey
                    : verb == "keydown" ? StepKind::kKeyDown
                                        : StepKind::kKeyUp;
        step.virtualKey = static_cast<std::uint16_t>(*vk);
        return LineResult{step, {}};
    }
    if (verb == "frames") {
        const auto frames = number(nextWord(rest), kMaxFrames);
        if (!frames || *frames == 0 || !nothingLeft(rest)) {
            return fail("frames takes one whole number of frames, 1 to 1000000");
        }
        step.kind = StepKind::kFrames;
        step.frames = static_cast<std::uint32_t>(*frames);
        return LineResult{step, {}};
    }
    if (verb == "shot") {
        const std::string_view path = trimmed(rest);
        if (path.empty()) {
            return fail("shot takes a file name");
        }
        step.kind = StepKind::kShot;
        step.text = std::string(path);
        return LineResult{step, {}};
    }
    if (verb == "list") {
        if (!nothingLeft(rest)) {
            return fail("list takes nothing after it");
        }
        step.kind = StepKind::kList;
        return LineResult{step, {}};
    }
    return fail("unknown step '" + std::string(verb) + "'");
}

} // namespace

ParsedScript parseInputScript(std::string_view text) {
    ParsedScript parsed;
    unsigned lineNumber = 0;
    while (!text.empty()) {
        const std::size_t newline = text.find('\n');
        const std::string_view line = text.substr(0, newline);
        text.remove_prefix(newline == std::string_view::npos ? text.size() : newline + 1);
        ++lineNumber;
        const std::string_view content = trimmed(line);
        if (content.empty() || content.front() == '#') {
            continue;
        }
        LineResult result = parseLine(content, lineNumber);
        if (!result.step) {
            parsed.script.steps.clear();
            parsed.error = ScriptError{lineNumber, std::move(result.error)};
            return parsed;
        }
        parsed.script.steps.push_back(std::move(*result.step));
    }
    return parsed;
}

std::string describe(const ScriptError &error) {
    return "line " + std::to_string(error.line) + ": " + error.message;
}

} // namespace sfcop::app::input
