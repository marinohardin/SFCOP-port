// The fix switch registry and the effective set (fixes.h; DESIGN §5.14, §6.9).
#include "fixes/fixes.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>
#include <utility>

namespace sfcop::fixes {

namespace {

constexpr std::string_view kCommandLine = "--fix";

std::string_view trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = s.find_last_not_of(" \t\r");
    return s.substr(first, last - first + 1);
}

// "on", "off", "1" or "0", optionally in double quotes; -1 for anything else.
int parsePosition(std::string_view value) {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    if (value == "on" || value == "1") {
        return 1;
    }
    if (value == "off" || value == "0") {
        return 0;
    }
    return -1;
}

// The text before a `//` that is not inside double quotes.
std::string_view stripComment(std::string_view line) {
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"') {
            quoted = !quoted;
        } else if (!quoted && line[i] == '/' && i + 1 < line.size() && line[i + 1] == '/') {
            return line.substr(0, i);
        }
    }
    return line;
}

std::string_view sourceLabel(Source source) {
    switch (source) {
    case Source::File:
        return "settings";
    case Source::CommandLine:
        return kCommandLine;
    case Source::Default:
        break;
    }
    return "default";
}

}  // namespace

// A switch's starting position: a runtime switch's default, a build switch's compiled position.
// A Build line without its block does not compile here.
#define SFCOP_FIX_START_Runtime(name, on) ((on) != 0)
#define SFCOP_FIX_START_Build(name, on) (SFCOP_FIX(name) != 0)

const std::vector<Switch> &registry() {
    static const std::vector<Switch> all = [] {
        std::vector<Switch> v;
#define SFCOP_FIX_SWITCH(name, kind, on, origin, description)                                    \
    v.push_back({#name, SFCOP_FIX_START_##kind(name, on), Origin::origin, Kind::kind,              \
                 description});
#include "fixes/switches.def"
#undef SFCOP_FIX_SWITCH
        return v;
    }();
    return all;
}

Settings::Settings() : Settings(registry()) {}

Settings::Settings(std::vector<Switch> switches) {
    entries_.reserve(switches.size());
    for (const Switch &s : switches) {
        entries_.push_back(Entry{s});
    }
}

Settings::Entry *Settings::find(std::string_view name) {
    for (Entry &e : entries_) {
        if (e.decl.name == name) {
            return &e;
        }
    }
    return nullptr;
}

const Settings::Entry *Settings::find(std::string_view name) const {
    return const_cast<Settings*>(this)->find(name);
}

std::string Settings::set(std::string_view name, bool on, Source layer) {
    Entry *e = find(name);
    if (e == nullptr) {
        return "unknown fix switch '" + std::string(name) + "'";
    }
    if (e->decl.kind == Kind::Build && on != e->decl.defaultOn) {
        return std::string(name) + " is a build switch compiled " +
               (e->decl.defaultOn ? "on" : "off") + "; it changes only by rebuilding";
    }
    (layer == Source::File ? e->file : e->command) = on ? 1 : 0;
    return {};
}

std::vector<Problem> Settings::applyFile(std::string_view text, std::string_view source) {
    std::vector<Problem> problems;
    int number = 0;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        const std::string_view raw = text.substr(start, end - start);
        start = end + 1;
        ++number;
        const std::string_view line = trim(stripComment(raw));
        if (line.empty()) {
            continue;
        }
        auto problem = [&](std::string message) {
            problems.push_back(Problem{std::string(source), number, std::move(message)});
        };
        if (line.front() == '[') {
            problem("a section line '" + std::string(line) + "'; fixes.gf has no sections");
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) {
            problem("no '=' in '" + std::string(line) + "'");
            continue;
        }
        const std::string_view name = trim(line.substr(0, eq));
        const std::string_view value = trim(line.substr(eq + 1));
        const int position = parsePosition(value);
        if (position < 0) {
            problem("'" + std::string(value) + "' for " + std::string(name) +
                    " is not on, off, 1 or 0");
            continue;
        }
        std::string message = set(name, position == 1, Source::File);
        if (!message.empty()) {
            problem(std::move(message));
        }
    }
    return problems;
}

std::vector<Problem> Settings::loadFile(const std::filesystem::path &path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) && !ec) {
        return {};
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {Problem{path.string(), 0, "the file cannot be read"}};
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    return applyFile(text, path.string());
}

std::vector<Problem> Settings::applyCommandLine(const std::vector<std::string> &values) {
    std::vector<Problem> problems;
    int number = 0;
    for (const std::string &value : values) {
        ++number;
        auto problem = [&](std::string message) {
            problems.push_back(Problem{std::string(kCommandLine), number, std::move(message)});
        };
        const auto eq = value.find('=');
        if (eq == std::string::npos) {
            problem("'" + value + "' is not NAME=on|off");
            continue;
        }
        const std::string_view text = value;
        const std::string_view name = text.substr(0, eq);
        const int position = parsePosition(text.substr(eq + 1));
        if (position < 0) {
            problem("'" + value + "' is not NAME=on|off");
            continue;
        }
        std::string message = set(name, position == 1, Source::CommandLine);
        if (!message.empty()) {
            problem(std::move(message));
        }
    }
    return problems;
}

bool Settings::isEnabled(std::string_view name) const {
    const Entry *e = find(name);
    if (e == nullptr) {
        return false;
    }
    if (e->command >= 0) {
        return e->command == 1;
    }
    if (e->file >= 0) {
        return e->file == 1;
    }
    return e->decl.defaultOn;
}

Source Settings::sourceOf(std::string_view name) const {
    const Entry *e = find(name);
    if (e == nullptr || (e->command < 0 && e->file < 0)) {
        return Source::Default;
    }
    return e->command >= 0 ? Source::CommandLine : Source::File;
}

bool Settings::isDeclared(std::string_view name) const {
    return find(name) != nullptr;
}

std::string Settings::report() const {
    std::ostringstream out;
    for (const Entry &e : entries_) {
        out << e.decl.name << (isEnabled(e.decl.name) ? " on" : " off") << " ("
            << sourceLabel(sourceOf(e.decl.name)) << ")"
            << (e.decl.kind == Kind::Build ? " [build]" : "") << "\n";
    }
    return out.str();
}

namespace {

Settings &storage() {
    static Settings settings;
    return settings;
}

}  // namespace

void install(Settings settings) {
    storage() = std::move(settings);
}

const Settings &current() {
    return storage();
}

bool isEnabled(std::string_view name) {
    return storage().isEnabled(name);
}

}  // namespace sfcop::fixes
