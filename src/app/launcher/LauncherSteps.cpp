#include "app/launcher/LauncherSteps.h"

#include <utility>

namespace sfcop::app::launcher {

namespace {

constexpr unsigned kMaxFrames = 100000;

struct Named {
    const char *name;
    Action action;
};

const Named kButtons[] = {
    {"use-path", Action::kUseTypedPath}, {"cancel", Action::kCancel},
    {"play", Action::kPlay},             {"check", Action::kCheck},
    {"change", Action::kChangeGame},     {"back", Action::kBackToGame},
    {"another", Action::kChooseAnother}, {"retry", Action::kTryAgain},
    {"quit", Action::kQuit},
};

std::optional<Screen> screenNamed(const std::string &name) {
    if (name == "welcome") {
        return Screen::kWelcome;
    }
    if (name == "working") {
        return Screen::kWorking;
    }
    if (name == "result") {
        return Screen::kResult;
    }
    if (name == "ready") {
        return Screen::kReady;
    }
    return std::nullopt;
}

std::optional<unsigned> count(const std::string &digits) {
    if (digits.empty() || digits.size() > 6 ||
        digits.find_first_not_of("0123456789") != std::string::npos) {
        return std::nullopt;
    }
    const unsigned value = static_cast<unsigned>(std::stoul(digits));
    if (value == 0 || value > kMaxFrames) {
        return std::nullopt;
    }
    return value;
}

} // namespace

ParsedSteps parseLauncherSteps(const std::string &text) {
    ParsedSteps parsed;
    std::size_t start = 0;
    int line = 0;
    const auto fail = [&parsed, &line](const std::string &what) {
        parsed.steps.clear();
        parsed.error = "line " + std::to_string(line) + ": " + what;
        return parsed;
    };
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string row = text.substr(start, end - start);
        start = end + 1;
        ++line;
        if (!row.empty() && row.back() == '\r') {
            row.pop_back();
        }
        const std::size_t first = row.find_first_not_of(" \t");
        if (first == std::string::npos || row[first] == '#') {
            continue;
        }
        row = row.substr(first);
        const std::size_t space = row.find(' ');
        const std::string word = row.substr(0, space);
        std::string rest = space == std::string::npos ? std::string() : row.substr(space + 1);
        while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\t')) {
            rest.pop_back();
        }
        Step step;
        step.line = line;
        if (word == "wait") {
            const std::optional<Screen> screen = screenNamed(rest);
            if (!screen) {
                return fail("wait takes welcome, working, result or ready, not '" + rest + "'");
            }
            step.kind = StepKind::kWait;
            step.screen = *screen;
        } else if (word == "frames") {
            const std::optional<unsigned> frames = count(rest);
            if (!frames) {
                return fail("frames takes a count from 1 to 100000, not '" + rest + "'");
            }
            step.kind = StepKind::kFrames;
            step.frames = *frames;
        } else if (word == "choose" || word == "type" || word == "shot") {
            if (rest.empty() && word != "type") {
                return fail(word + " needs a path");
            }
            step.kind = word == "choose" ? StepKind::kChoose
                        : word == "type" ? StepKind::kType
                                         : StepKind::kShot;
            step.text = rest;
        } else if (word == "press") {
            bool known = false;
            for (const Named &button : kButtons) {
                if (rest == button.name) {
                    step.action = button.action;
                    known = true;
                }
            }
            if (!known) {
                return fail("press takes use-path, cancel, play, check, change, back, another, "
                            "retry or quit, not '" + rest + "'");
            }
            step.kind = StepKind::kPress;
        } else if (word == "close" && rest.empty()) {
            step.kind = StepKind::kClose;
        } else {
            return fail("unknown step '" + row + "'");
        }
        parsed.steps.push_back(std::move(step));
    }
    return parsed;
}

} // namespace sfcop::app::launcher
