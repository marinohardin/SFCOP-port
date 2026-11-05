#include "app/input/Marks.h"

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace sfcop::app::input {

namespace {

struct Reached {
    std::uint32_t address;
    std::string name;
};

std::mutex &marksLock() {
    static std::mutex lock;
    return lock;
}

std::vector<Reached> &reachedMarks() {
    static std::vector<Reached> marks;
    return marks;
}

bool sameIgnoringCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

std::string hexAddress(std::uint32_t address) {
    char text[16];
    std::snprintf(text, sizeof text, "0x%08X", address);
    return text;
}

} // namespace

void NoteMark(std::uint32_t address, std::string_view name) {
    std::lock_guard<std::mutex> hold(marksLock());
    reachedMarks().push_back(Reached{address, std::string(name)});
}

bool MarkReached(std::string_view mark) {
    std::lock_guard<std::mutex> hold(marksLock());
    for (const Reached &reached : reachedMarks()) {
        if ((!reached.name.empty() && reached.name == mark) ||
            sameIgnoringCase(hexAddress(reached.address), mark)) {
            return true;
        }
    }
    return false;
}

void ForgetMarks() {
    std::lock_guard<std::mutex> hold(marksLock());
    reachedMarks().clear();
}

} // namespace sfcop::app::input
