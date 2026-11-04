// GetPrivateProfileString, GetPrivateProfileInt and WritePrivateProfileString: the profile is
// read where the game sees it (its per-user copy, else the install's) and every change is written
// whole to the per-user copy with an atomic write.

#include "hle/kernel/Kernel.h"

#include <cstring>

namespace sfcop::hle::kernel {
namespace {

using platform::Root;

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool equalNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); i++) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

std::string_view trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

// The profile as lines without their line ends, and the line end it uses.
struct Profile {
    std::vector<std::string> lines;
    std::string lineEnd = "\r\n";

    explicit Profile(const std::string &content) {
        if (content.find('\n') != std::string::npos &&
            content.find("\r\n") == std::string::npos) {
            lineEnd = "\n";
        }
        std::size_t start = 0;
        while (start < content.size()) {
            std::size_t end = content.find('\n', start);
            std::string line = content.substr(start, end == std::string::npos ? std::string::npos
                                                                                : end - start);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            lines.push_back(line);
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
    }

    std::string Text() const {
        std::string out;
        for (const std::string &line : lines) {
            out += line + lineEnd;
        }
        return out;
    }

    // The section's name when the line is a section header.
    static std::optional<std::string_view> SectionOf(std::string_view line) {
        line = trimmed(line);
        if (line.empty() || line.front() != '[') {
            return std::nullopt;
        }
        std::size_t close = line.find(']');
        return trimmed(line.substr(1, close == std::string_view::npos ? line.size() - 1 : close - 1));
    }

    // The key and value of a `key=value` line.
    static bool KeyValueOf(std::string_view line, std::string_view &key, std::string_view &value) {
        std::string_view text = trimmed(line);
        std::size_t equals = text.find('=');
        if (text.empty() || text.front() == ';' || equals == std::string_view::npos) {
            return false;
        }
        key = trimmed(text.substr(0, equals));
        value = trimmed(text.substr(equals + 1));
        return true;
    }

    // [header, end) of the first section with that name; header is npos when there is none.
    void Find(std::string_view section, std::size_t &header, std::size_t &end) const {
        header = std::string::npos;
        end = lines.size();
        for (std::size_t i = 0; i < lines.size(); i++) {
            std::optional<std::string_view> name = SectionOf(lines[i]);
            if (!name) {
                continue;
            }
            if (header != std::string::npos) {
                end = i;
                return;
            }
            if (equalNoCase(*name, section)) {
                header = i;
            }
        }
    }

    std::optional<std::string> Value(std::string_view section, std::string_view key) const {
        std::size_t header = 0;
        std::size_t end = 0;
        Find(section, header, end);
        if (header == std::string::npos) {
            return std::nullopt;
        }
        for (std::size_t i = header + 1; i < end; i++) {
            std::string_view name;
            std::string_view value;
            if (KeyValueOf(lines[i], name, value) && equalNoCase(name, key)) {
                if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
                    value.back() == value.front()) {
                    value = value.substr(1, value.size() - 2);
                }
                return std::string(value);
            }
        }
        return std::nullopt;
    }
};

// A list of names, each NUL-terminated and the list ending in a second NUL, into the buffer: the
// characters copied without the last NUL, or size - 2 when it is cut short.
std::uint32_t answerList(const std::vector<std::string> &names, char *buffer, std::uint32_t size) {
    std::string list;
    for (const std::string &name : names) {
        list += name;
        list += '\0';
    }
    if (size < 2) {
        if (size == 1) {
            buffer[0] = '\0';
        }
        return 0;
    }
    if (list.size() + 1 <= size) {
        std::memcpy(buffer, list.data(), list.size());
        buffer[list.size()] = '\0';
        return static_cast<std::uint32_t>(list.size());
    }
    std::memcpy(buffer, list.data(), size - 2);
    buffer[size - 2] = '\0';
    buffer[size - 1] = '\0';
    return size - 2;
}

} // namespace

// A bare file name is in the Windows directory on Windows. The path model has no such folder, so
// it lives in `windows/` under the per-user `settings/` folder (made on the first write); a read
// that finds nothing there falls back to the install's game folder (GetPrivateProfileString).
GamePath Kernel::IniLocate(std::string_view file, bool *bare) const {
    *bare = !file.empty() && file.find_first_of("\\/:") == std::string_view::npos;
    if (!*bare) {
        return Locate(file);
    }
    GamePath where;
    if (!Absolute(file).empty() && file != "." && file != "..") {
        where.inGame = true;
        where.relative = "settings/windows/" + std::string(file);
    }
    return where;
}

std::uint32_t Kernel::GetPrivateProfileString(const char *section, const char *key,
                                              const char *fallback, char *buffer,
                                              std::uint32_t size, std::string_view file) {
    if (buffer == nullptr || size == 0) {
        return 0;
    }
    std::string content;
    bool bare = false;
    GamePath where = IniLocate(file, &bare);
    std::optional<Entry> entry = where.inGame ? Lookup(where.relative) : std::nullopt;
    if (!entry && bare && where.inGame) {
        entry = Lookup(std::string(file)); // the install's game folder
    }
    if (entry && !entry->info.isDirectory) {
        content = ReadWhole(*entry).value_or(std::string());
    } else {
        SetError(win32::kErrorFileNotFound);
    }
    Profile profile(content);

    if (section == nullptr || key == nullptr) {
        std::vector<std::string> names;
        std::size_t header = 0;
        std::size_t end = profile.lines.size();
        if (key == nullptr && section != nullptr) {
            profile.Find(section, header, end);
        }
        for (std::size_t i = section == nullptr ? 0 : header + 1;
             header != std::string::npos && i < end; i++) {
            std::optional<std::string_view> name = Profile::SectionOf(profile.lines[i]);
            std::string_view keyName;
            std::string_view value;
            if (section == nullptr && name) {
                names.emplace_back(*name);
            } else if (section != nullptr && Profile::KeyValueOf(profile.lines[i], keyName, value)) {
                names.emplace_back(keyName);
            }
        }
        return answerList(names, buffer, size);
    }

    std::optional<std::string> value = profile.Value(section, key);
    std::string answer;
    if (value) {
        answer = *value;
    } else if (fallback != nullptr) {
        answer = fallback;
        while (!answer.empty() && answer.back() == ' ') {
            answer.pop_back();
        }
    }
    std::uint32_t count = static_cast<std::uint32_t>(answer.size()) < size
                              ? static_cast<std::uint32_t>(answer.size())
                              : size - 1;
    std::memcpy(buffer, answer.data(), count);
    buffer[count] = '\0';
    return count;
}

std::uint32_t Kernel::GetPrivateProfileInt(const char *section, const char *key,
                                           std::int32_t fallback, std::string_view file) {
    char text[64];
    const char kMissing[] = "\x01";
    GetPrivateProfileString(section, key, kMissing, text, sizeof text, file);
    if (std::strcmp(text, kMissing) == 0 || text[0] == '\0') {
        return static_cast<std::uint32_t>(fallback);
    }
    const char *at = text;
    bool negative = false;
    if (*at == '-' || *at == '+') {
        negative = *at == '-';
        at++;
    }
    // Decimal only, with an optional sign: "0x10" reads as 0, as on Windows.
    std::uint32_t number = 0;
    for (; *at >= '0' && *at <= '9'; at++) {
        number = number * 10 + static_cast<std::uint32_t>(*at - '0');
    }
    return negative ? 0u - number : number;
}

bool Kernel::WritePrivateProfileString(const char *section, const char *key, const char *value,
                                       std::string_view file) {
    if (section == nullptr) {
        if (key == nullptr && value == nullptr) {
            return true; // "flush the cache": there is none
        }
        SetError(win32::kErrorInvalidParameter);
        return false;
    }
    bool bare = false;
    GamePath where = IniLocate(file, &bare);
    if (!where.inGame || where.relative.empty()) {
        SetError(win32::kErrorPathNotFound);
        return false;
    }
    std::string content;
    std::optional<Entry> entry = Lookup(where.relative);
    if (!entry && bare) {
        entry = Lookup(std::string(file)); // start from the install's copy, if it has one
    }
    if (entry) {
        if (entry->info.isDirectory) {
            SetError(win32::kErrorAccessDenied);
            return false;
        }
        std::optional<std::string> read = ReadWhole(*entry);
        if (!read) {
            return false;
        }
        content = *read;
    } else if (!bare) {
        std::size_t slash = where.relative.rfind('/');
        if (!IsDirectory(slash == std::string::npos ? "" : where.relative.substr(0, slash))) {
            SetError(win32::kErrorPathNotFound);
            return false;
        }
    }

    Profile profile(content);
    std::size_t header = 0;
    std::size_t end = 0;
    profile.Find(section, header, end);
    auto &lines = profile.lines;
    if (key == nullptr) {
        if (header != std::string::npos) {
            lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(header),
                        lines.begin() + static_cast<std::ptrdiff_t>(end));
        }
    } else {
        std::size_t found = std::string::npos;
        for (std::size_t i = header + 1; header != std::string::npos && i < end; i++) {
            std::string_view name;
            std::string_view old;
            if (Profile::KeyValueOf(lines[i], name, old) && equalNoCase(name, key)) {
                found = i;
                break;
            }
        }
        std::string line = std::string(key) + "=" + (value != nullptr ? value : "");
        if (value == nullptr) {
            if (found != std::string::npos) {
                lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(found));
            }
        } else if (found != std::string::npos) {
            lines[found] = line;
        } else if (header != std::string::npos) {
            std::size_t at = end;
            while (at > header + 1 && trimmed(lines[at - 1]).empty()) {
                at--;
            }
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at), line);
        } else {
            lines.push_back("[" + std::string(section) + "]");
            lines.push_back(line);
        }
    }

    std::string user = UserPathOf(where.relative);
    std::string text = profile.Text();
    platform::Status written = MakeUserParent(user);
    if (written) {
        written = store_.WriteAtomic(Root::kUser, user, text.data(), text.size());
    }
    if (!written) {
        SetError(win32Error(written.Code(), true));
        return false;
    }
    return true;
}

} // namespace sfcop::hle::kernel
