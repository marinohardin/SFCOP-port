// Test helper for the sound modules' shim tests: reads a *Guest.cpp file and src/hle/surface.tsv
// as text, so a doctest can check that each surface row has a shim and that the shim purges the
// stack the row's stack_bytes give. The shims themselves are built only with the lift.
#pragma once

#include <cctype>
#include <cstddef>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace sfcop::hle::shimrows {

inline std::string readFile(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// The text between the parenthesis at `open` and its match.
inline std::string inside(const std::string &text, std::size_t open) {
    int depth = 0;
    for (std::size_t i = open; i < text.size(); ++i) {
        if (text[i] == '(') {
            ++depth;
        } else if (text[i] == ')' && --depth == 0) {
            return text.substr(open + 1, i - open - 1);
        }
    }
    return {};
}

// The text after the last comma outside parentheses.
inline std::string lastArgument(const std::string &args) {
    int depth = 0;
    std::size_t last = std::string::npos;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == '(') {
            ++depth;
        } else if (args[i] == ')') {
            --depth;
        } else if (args[i] == ',' && depth == 0) {
            last = i;
        }
    }
    return last == std::string::npos ? args : args.substr(last + 1);
}

inline int number(const std::string &s) {
    std::string digits;
    for (char c : s) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            digits += c;
        } else if (!std::isspace(static_cast<unsigned char>(c))) {
            return -1; // not a literal
        }
    }
    return digits.empty() ? -1 : std::stoi(digits);
}

// Every argument count a body purges with: RET(value, n), RETV(n), a `_OR_FAIL(n)` guard (which
// purges n on its error path), and answerInterface(), which purges 3.
inline std::set<int> purges(const std::string &body) {
    std::set<int> counts;
    const std::regex call(R"((\bRET|\bRETV|_OR_FAIL|\banswerInterface)\()");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), call);
         it != std::sregex_iterator(); ++it) {
        std::string what = (*it)[1];
        std::size_t open = static_cast<std::size_t>(it->position(0) + it->length(0) - 1);
        std::string args = inside(body, open);
        if (what == "answerInterface") {
            counts.insert(3);
        } else if (what == "RET") {
            counts.insert(number(lastArgument(args)));
        } else if (args != "argc") {
            counts.insert(number(args));
        }
    }
    return counts;
}

struct Shims {
    std::map<std::string, std::string> functionOf;  // shim name -> function
    std::map<std::string, std::set<int>> purgesOf;   // function -> argument counts it purges
};

inline Shims parseGuest(const std::string &text) {
    Shims shims;
    // Functions written out: `void name() {` ... `}` at the start of a line.
    const std::regex function(R"(\nvoid (\w+)\(\) \{)");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), function);
         it != std::sregex_iterator(); ++it) {
        std::size_t start = static_cast<std::size_t>(it->position(0));
        std::size_t end = text.find("\n}", start + 1);
        shims.purgesOf[(*it)[1]] = purges(text.substr(start, end - start));
    }
    // Functions a macro writes: `#define MACRO(name, ...)` with `void name()` in its body, and
    // each `MACRO(fn, ...)` at the start of a line.
    const std::regex macro(R"(\n#define (SFCOP_\w+)\(name, )");
    std::map<std::string, std::set<int>> macroPurges;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), macro);
         it != std::sregex_iterator(); ++it) {
        std::size_t start = static_cast<std::size_t>(it->position(0) + 1);
        std::size_t end = start;
        while ((end = text.find('\n', end + 1)) != std::string::npos && text[end - 1] == '\\') {
        }
        macroPurges[(*it)[1]] = purges(text.substr(start, end - start));
    }
    for (const auto &[name, counts] : macroPurges) {
        const std::regex use("\n" + name + R"(\((\w+),)");
        for (auto it = std::sregex_iterator(text.begin(), text.end(), use);
             it != std::sregex_iterator(); ++it) {
            shims.purgesOf[(*it)[1]] = counts;
        }
    }
    // Table entries: {"name", fn} and X("name", fn).
    const std::regex entry(R"re((?:\{|X\()"([^"]+)", (\w+)(?:\}|\)))re");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), entry);
         it != std::sregex_iterator(); ++it) {
        shims.functionOf[(*it)[1]] = (*it)[2];
    }
    return shims;
}

struct Row {
    std::string module;
    std::string name;
    int stackBytes = 0;
    std::string status;
};

// The surface rows whose module is one of `modules`.
inline std::vector<Row> surfaceRows(const std::string &path, const std::set<std::string> &modules) {
    std::vector<Row> rows;
    std::istringstream text(readFile(path));
    std::string line;
    std::getline(text, line); // the header
    while (std::getline(text, line)) {
        std::vector<std::string> cells;
        std::istringstream fields(line);
        std::string cell;
        while (std::getline(fields, cell, '\t')) {
            cells.push_back(cell);
        }
        if (cells.size() >= 6 && modules.count(cells[0]) != 0) {
            rows.push_back(Row{cells[0], cells[1], std::stoi(cells[4]), cells[5]});
        }
    }
    return rows;
}

// The directory of the file a test was compiled from (CMake passes absolute paths).
inline std::string directoryOf(const std::string &file) {
    return file.substr(0, file.find_last_of('/'));
}

} // namespace sfcop::hle::shimrows
