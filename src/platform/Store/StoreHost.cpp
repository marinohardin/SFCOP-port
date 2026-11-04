// The host Store: path checks, the lookup that ignores ASCII case, open files, directories, the
// atomic write and the registry, on the platform primitives of StoreHost_<platform>.cpp.

#include "platform/Store/Store.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <system_error>
#include <utility>

namespace sfcop::platform {

// The platform primitives this file is built on, defined once per platform in
// StoreHost_<platform>.cpp (which repeats only the OsFile alias; a definition that does not
// match a declaration here fails to link). Paths are host
// paths, UTF-8. A file is an integer the platform chooses (a descriptor on POSIX); positions are
// passed in, so a primitive keeps no position of its own. Nothing outside the backend calls these.
namespace storehost {

using OsFile = std::int64_t;

// Opens or creates per `mode` (OpenMode's meanings), closed on exec. Errors: kNotFound,
// kAlreadyExists, kAccessDenied, kWrongState (a directory), kOutOfResources, kIoError.
Result<OsFile> osOpen(const std::string &path, OpenMode mode);
// Up to `bytes` at `offset`; fewer only at the end of the file. Errors: kIoError.
Result<std::size_t> osRead(OsFile file, void *out, std::size_t bytes, std::uint64_t offset);
// All `bytes` at `offset`, zero-filling any gap after the end. Errors: kAccessDenied, kIoError.
Result<std::size_t> osWrite(OsFile file, const void *data, std::size_t bytes,
                            std::uint64_t offset);
Result<FileInfo> osInfo(OsFile file);
Status osSetSize(OsFile file, std::uint64_t bytes);
Status osSetModifiedTime(OsFile file, std::int64_t modifiedNs);
// Through to the disk, so that a rename after it publishes complete content.
Status osFlush(OsFile file);
Status osClose(OsFile file);
// Size, type, time and whether the user may write it, by path, following no link. Errors:
// kNotFound, kIoError.
Result<FileInfo> osStat(const std::string &path);
// Renames `from` over `to`, replacing a file there in one step. Errors: kNotFound,
// kAccessDenied, kAlreadyExists (`to` is a directory), kIoError.
Status osReplace(const std::string &from, const std::string &to);

} // namespace storehost

namespace {

namespace fs = std::filesystem;
using storehost::OsFile;

constexpr const char kRegistryFile[] = "registry.tsv";
constexpr const char kRegistryHeader[] =
    "# registry.tsv: key<TAB>name<TAB>type<TAB>value, one line per key (type key, name and value "
    "empty) and one per value (type dword: 8 hex digits; string: the bytes as stored; binary: two "
    "hex digits a byte); keys and names compare without ASCII case";

char asciiLower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

std::string lowered(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), asciiLower);
    return out;
}

bool equalNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); i++) {
        if (asciiLower(a[i]) != asciiLower(b[i])) {
            return false;
        }
    }
    return true;
}

// Splits a Store path into its segments; false when it breaks the rules of Store.h. An empty path
// is the root, allowed only where `allowRoot`.
bool splitPath(std::string_view path, bool allowRoot, std::vector<std::string> &segments) {
    segments.clear();
    if (path.empty()) {
        return allowRoot;
    }
    std::size_t start = 0;
    for (;;) {
        std::size_t end = path.find('/', start);
        std::string_view segment =
            path.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (segment.empty() || segment == "." || segment == ".." ||
            segment.find_first_of(std::string_view("\\:\0", 3)) != std::string_view::npos) {
            return false;
        }
        segments.emplace_back(segment);
        if (end == std::string_view::npos) {
            return true;
        }
        start = end + 1;
    }
}

Error fromErrorCode(const std::error_code &error) {
    if (error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory) {
        return Error::kNotFound;
    }
    if (error == std::errc::permission_denied || error == std::errc::operation_not_permitted ||
        error == std::errc::read_only_file_system) {
        return Error::kAccessDenied;
    }
    if (error == std::errc::file_exists) {
        return Error::kAlreadyExists;
    }
    if (error == std::errc::directory_not_empty) {
        return Error::kWrongState;
    }
    return Error::kIoError;
}

bool writesFile(OpenMode mode) { return mode != OpenMode::kRead; }
bool createsFile(OpenMode mode) {
    return mode == OpenMode::kCreate || mode == OpenMode::kCreateNew ||
           mode == OpenMode::kOpenOrCreate;
}

bool holdsLineBreakOrTab(std::string_view text) {
    return text.find_first_of("\t\r\n") != std::string_view::npos;
}

// A registry key as `\`-separated segments, none empty; false otherwise.
bool validKey(std::string_view key) {
    if (key.empty() || key.front() == '\\' || key.back() == '\\' ||
        key.find("\\\\") != std::string_view::npos || holdsLineBreakOrTab(key)) {
        return false;
    }
    return true;
}

std::string hexOf(const std::uint8_t *bytes, std::size_t count) {
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(count * 2);
    for (std::size_t i = 0; i < count; i++) {
        out.push_back(kDigits[bytes[i] >> 4]);
        out.push_back(kDigits[bytes[i] & 15]);
    }
    return out;
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = asciiLower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

bool parseHex(std::string_view text, std::vector<std::uint8_t> &out) {
    if (text.size() % 2 != 0) {
        return false;
    }
    out.clear();
    for (std::size_t i = 0; i < text.size(); i += 2) {
        int high = hexDigit(text[i]);
        int low = hexDigit(text[i + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        out.push_back(static_cast<std::uint8_t>(high << 4 | low));
    }
    return true;
}

class StoreHost final : public Store {
public:
    StoreHost(std::string installDir, std::string userDir)
        : roots_{std::move(installDir), std::move(userDir)} {
        loadRegistry();
    }

    ~StoreHost() override {
        for (auto &entry : files_) {
            (void)storehost::osClose(entry.second.os);
        }
    }

    Result<std::string> RootPath(Root root) override {
        const std::string &dir = rootDir(root);
        if (dir.empty()) {
            return Error::kNotFound;
        }
        return dir;
    }

    Result<FileHandle> Open(Root root, std::string_view path, OpenMode mode) override {
        std::vector<std::string> segments;
        if (!splitPath(path, false, segments)) {
            return Error::kInvalidArgument;
        }
        if (root == Root::kInstall && writesFile(mode)) {
            return Error::kAccessDenied;
        }
        std::lock_guard<std::mutex> lock(filesMutex_);
        Result<std::string> host = resolve(root, segments, createsFile(mode));
        if (!host) {
            return host.Code();
        }
        Result<FileInfo> existing = storehost::osStat(host.Value());
        if (existing && existing.Value().isDirectory) {
            return Error::kWrongState;
        }
        Result<OsFile> os = storehost::osOpen(host.Value(), mode);
        if (!os) {
            return os.Code();
        }
        if (!existing) {
            forgetLookups(root);
        }
        std::uint32_t id = nextFile_++;
        if (nextFile_ == 0) {
            nextFile_ = 1;
        }
        files_[id] = OpenFile{os.Value(), 0, writesFile(mode), root, host.Value()};
        return FileHandle{id};
    }

    Result<std::size_t> Read(FileHandle file, void *out, std::size_t bytes) override {
        OpenFile *open = find(file);
        if (open == nullptr) {
            return Error::kInvalidHandle;
        }
        Result<std::size_t> got = storehost::osRead(open->os, out, bytes, open->position);
        if (got) {
            open->position += got.Value();
        }
        return got;
    }

    Result<std::size_t> Write(FileHandle file, const void *data, std::size_t bytes) override {
        OpenFile *open = find(file);
        if (open == nullptr) {
            return Error::kInvalidHandle;
        }
        if (!open->writable) {
            return Error::kAccessDenied;
        }
        Result<std::size_t> put = storehost::osWrite(open->os, data, bytes, open->position);
        if (put) {
            open->position += put.Value();
        }
        return put;
    }

    Result<std::uint64_t> Seek(FileHandle file, std::int64_t offset, SeekOrigin origin) override {
        OpenFile *open = find(file);
        if (open == nullptr) {
            return Error::kInvalidHandle;
        }
        std::int64_t base = 0;
        if (origin == SeekOrigin::kCurrent) {
            base = static_cast<std::int64_t>(open->position);
        } else if (origin == SeekOrigin::kEnd) {
            Result<FileInfo> info = storehost::osInfo(open->os);
            if (!info) {
                return info.Code();
            }
            base = static_cast<std::int64_t>(info.Value().size);
        }
        if ((offset < 0 && base + offset < 0) ||
            (offset > 0 && base > INT64_MAX - offset)) {
            return Error::kInvalidArgument;
        }
        open->position = static_cast<std::uint64_t>(base + offset);
        return open->position;
    }

    Result<FileInfo> GetInfo(FileHandle file) override {
        OpenFile *open = find(file);
        if (open == nullptr) {
            return Error::kInvalidHandle;
        }
        Result<FileInfo> info = storehost::osInfo(open->os);
        if (info && open->root == Root::kInstall) {
            info.Value().readOnly = true;
        }
        return info;
    }

    Status SetSize(FileHandle file, std::uint64_t bytes) override {
        OpenFile *open = find(file);
        if (open == nullptr) {
            return Error::kInvalidHandle;
        }
        if (!open->writable) {
            return Error::kAccessDenied;
        }
        return storehost::osSetSize(open->os, bytes);
    }

    Status SetModifiedTime(FileHandle file, std::int64_t modifiedNs) override {
        OpenFile *open = find(file);
        if (open == nullptr) {
            return Error::kInvalidHandle;
        }
        if (!open->writable) {
            return Error::kAccessDenied;
        }
        return storehost::osSetModifiedTime(open->os, modifiedNs);
    }

    Status Flush(FileHandle file) override {
        OpenFile *open = find(file);
        if (open == nullptr) {
            return Error::kInvalidHandle;
        }
        return open->writable ? storehost::osFlush(open->os) : Status{};
    }

    Status Close(FileHandle file) override {
        OsFile os = 0;
        {
            std::lock_guard<std::mutex> lock(filesMutex_);
            auto found = files_.find(file.value);
            if (found == files_.end()) {
                return Error::kInvalidHandle;
            }
            os = found->second.os;
            files_.erase(found);
        }
        return storehost::osClose(os);
    }

    Result<FileInfo> Stat(Root root, std::string_view path) override {
        std::vector<std::string> segments;
        if (!splitPath(path, false, segments)) {
            return Error::kInvalidArgument;
        }
        std::lock_guard<std::mutex> lock(filesMutex_);
        Result<std::string> host = resolve(root, segments, false);
        if (!host) {
            return host.Code();
        }
        return statUnder(root, host.Value());
    }

    Result<std::vector<DirectoryEntry>> List(Root root, std::string_view path) override {
        std::vector<std::string> segments;
        if (!splitPath(path, true, segments)) {
            return Error::kInvalidArgument;
        }
        std::lock_guard<std::mutex> lock(filesMutex_);
        Result<std::string> host = resolve(root, segments, false);
        if (!host) {
            return host.Code();
        }
        Result<FileInfo> self = storehost::osStat(host.Value());
        if (!self) {
            return self.Code();
        }
        if (!self.Value().isDirectory) {
            return Error::kWrongState;
        }
        std::error_code error;
        std::vector<DirectoryEntry> entries;
        for (fs::directory_iterator it(fs::u8path(host.Value()), error), end; !error && it != end;
             it.increment(error)) {
            std::string name = it->path().filename().u8string();
            Result<FileInfo> info = statUnder(root, host.Value() + "/" + name);
            if (info) {
                entries.push_back(DirectoryEntry{name, info.Value()});
            }
        }
        if (error) {
            return fromErrorCode(error);
        }
        std::sort(entries.begin(), entries.end(),
                  [](const DirectoryEntry &a, const DirectoryEntry &b) { return a.name < b.name; });
        return entries;
    }

    Status MakeDirectory(Root root, std::string_view path) override {
        std::vector<std::string> segments;
        if (!splitPath(path, false, segments)) {
            return Error::kInvalidArgument;
        }
        if (root == Root::kInstall) {
            return Error::kAccessDenied;
        }
        std::lock_guard<std::mutex> lock(filesMutex_);
        std::vector<std::string> prefix;
        for (const std::string &segment : segments) {
            prefix.push_back(segment);
            Result<std::string> host = resolve(root, prefix, true);
            if (!host) {
                return host.Code();
            }
            Result<FileInfo> info = storehost::osStat(host.Value());
            if (info) {
                if (!info.Value().isDirectory) {
                    return Error::kAlreadyExists;
                }
                continue;
            }
            std::error_code error;
            fs::create_directory(fs::u8path(host.Value()), error);
            forgetLookups(root);
            if (error) {
                return fromErrorCode(error);
            }
        }
        return {};
    }

    Status Remove(Root root, std::string_view path) override {
        std::vector<std::string> segments;
        if (!splitPath(path, false, segments)) {
            return Error::kInvalidArgument;
        }
        if (root == Root::kInstall) {
            return Error::kAccessDenied;
        }
        std::lock_guard<std::mutex> lock(filesMutex_);
        Result<std::string> host = resolve(root, segments, false);
        if (!host) {
            return host.Code();
        }
        for (const auto &entry : files_) {
            if (entry.second.hostPath == host.Value()) {
                return Error::kAccessDenied;
            }
        }
        Result<FileInfo> info = storehost::osStat(host.Value());
        if (!info) {
            return info.Code();
        }
        std::error_code error;
        if (info.Value().isDirectory && !fs::is_empty(fs::u8path(host.Value()), error)) {
            return error ? fromErrorCode(error) : Error::kWrongState;
        }
        fs::remove(fs::u8path(host.Value()), error);
        forgetLookups(root);
        return error ? Status{fromErrorCode(error)} : Status{};
    }

    Status Rename(Root root, std::string_view from, std::string_view to) override {
        std::vector<std::string> fromSegments;
        std::vector<std::string> toSegments;
        if (!splitPath(from, false, fromSegments) || !splitPath(to, false, toSegments)) {
            return Error::kInvalidArgument;
        }
        if (root == Root::kInstall) {
            return Error::kAccessDenied;
        }
        std::lock_guard<std::mutex> lock(filesMutex_);
        Result<std::string> source = resolve(root, fromSegments, false);
        if (!source) {
            return source.Code();
        }
        Result<std::string> target = resolve(root, toSegments, true);
        if (!target) {
            return target.Code();
        }
        std::string targetPath = target.Value();
        if (targetPath == source.Value()) {
            // The same entry under another case: rename to the case given.
            targetPath = targetPath.substr(0, targetPath.rfind('/') + 1) + toSegments.back();
        } else {
            Result<FileInfo> existing = storehost::osStat(targetPath);
            if (existing && existing.Value().isDirectory) {
                return Error::kAlreadyExists;
            }
        }
        Status renamed = storehost::osReplace(source.Value(), targetPath);
        forgetLookups(root);
        return renamed;
    }

    Status WriteAtomic(Root root, std::string_view path, const void *data,
                       std::size_t bytes) override {
        std::vector<std::string> segments;
        if (!splitPath(path, false, segments)) {
            return Error::kInvalidArgument;
        }
        if (root == Root::kInstall) {
            return Error::kAccessDenied;
        }
        std::string target;
        std::string temporary;
        {
            std::lock_guard<std::mutex> lock(filesMutex_);
            Result<std::string> host = resolve(root, segments, true);
            if (!host) {
                return host.Code();
            }
            target = host.Value();
            Result<FileInfo> existing = storehost::osStat(target);
            if (existing && existing.Value().isDirectory) {
                return Error::kAccessDenied;
            }
            std::size_t slash = target.rfind('/');
            temporary = target.substr(0, slash + 1) + "." + target.substr(slash + 1) +
                        ".tmp" + std::to_string(nextTemporary_++);
        }
        Result<OsFile> os = storehost::osOpen(temporary, OpenMode::kCreate);
        if (!os) {
            return os.Code() == Error::kNotFound ? Error::kNotFound : Error::kIoError;
        }
        Result<std::size_t> put =
            bytes > 0 ? storehost::osWrite(os.Value(), data, bytes, 0) : Result<std::size_t>(0);
        Status flushed = put ? storehost::osFlush(os.Value()) : Status{put.Code()};
        Status closed = storehost::osClose(os.Value());
        Status replaced = flushed && closed ? storehost::osReplace(temporary, target)
                                            : Status{Error::kIoError};
        if (!replaced) {
            std::error_code ignored;
            fs::remove(fs::u8path(temporary), ignored);
            return Error::kIoError;
        }
        std::lock_guard<std::mutex> lock(filesMutex_);
        forgetLookups(root);
        return {};
    }

    Result<bool> KeyExists(std::string_view key) override {
        if (!validKey(key)) {
            return Error::kInvalidArgument;
        }
        std::lock_guard<std::mutex> lock(registryMutex_);
        return isRootKey(key) || keys_.count(lowered(key)) > 0;
    }

    Status CreateKey(std::string_view key) override {
        if (!validKey(key)) {
            return Error::kInvalidArgument;
        }
        std::lock_guard<std::mutex> lock(registryMutex_);
        Keys changed = keys_;
        addKey(changed, key);
        return commit(std::move(changed));
    }

    Status DeleteKey(std::string_view key) override {
        if (!validKey(key) || isRootKey(key)) {
            return Error::kInvalidArgument;
        }
        std::lock_guard<std::mutex> lock(registryMutex_);
        std::string lower = lowered(key);
        if (keys_.count(lower) == 0) {
            return Error::kNotFound;
        }
        Keys changed = keys_;
        std::string below = lower + "\\";
        for (auto it = changed.begin(); it != changed.end();) {
            if (it->first == lower || it->first.compare(0, below.size(), below) == 0) {
                it = changed.erase(it);
            } else {
                ++it;
            }
        }
        return commit(std::move(changed));
    }

    Result<RegistryValue> GetValue(std::string_view key, std::string_view name) override {
        if (!validKey(key)) {
            return Error::kInvalidArgument;
        }
        std::lock_guard<std::mutex> lock(registryMutex_);
        auto found = keys_.find(lowered(key));
        if (found == keys_.end()) {
            return Error::kNotFound;
        }
        auto value = found->second.values.find(lowered(name));
        if (value == found->second.values.end()) {
            return Error::kNotFound;
        }
        return value->second.value;
    }

    Status SetValue(std::string_view key, std::string_view name,
                    const RegistryValue &value) override {
        if (!validKey(key) || holdsLineBreakOrTab(name) ||
            (value.type == RegistryType::kString && holdsLineBreakOrTab(value.text))) {
            return Error::kInvalidArgument;
        }
        std::lock_guard<std::mutex> lock(registryMutex_);
        auto found = keys_.find(lowered(key));
        if (found == keys_.end()) {
            return Error::kNotFound;
        }
        Keys changed = keys_;
        RegistryValue stored;
        stored.type = value.type;
        if (value.type == RegistryType::kDword) {
            stored.dword = value.dword;
        } else if (value.type == RegistryType::kString) {
            stored.text = value.text;
        } else {
            stored.bytes = value.bytes;
        }
        changed[found->first].values[lowered(name)] = NamedValue{std::string(name), stored};
        return commit(std::move(changed));
    }

private:
    struct OpenFile {
        OsFile os = 0;
        std::uint64_t position = 0;
        bool writable = false;
        Root root = Root::kInstall;
        std::string hostPath;
    };

    struct NamedValue {
        std::string name; // as first set
        RegistryValue value;
    };

    struct Key {
        std::string name; // as first created
        std::map<std::string, NamedValue> values; // by lowered name
    };

    using Keys = std::map<std::string, Key>; // by lowered key

    const std::string &rootDir(Root root) const {
        return roots_[root == Root::kInstall ? 0 : 1];
    }

    OpenFile *find(FileHandle file) {
        std::lock_guard<std::mutex> lock(filesMutex_);
        auto found = files_.find(file.value);
        return found == files_.end() ? nullptr : &found->second;
    }

    Result<FileInfo> statUnder(Root root, const std::string &host) {
        Result<FileInfo> info = storehost::osStat(host);
        if (info && root == Root::kInstall) {
            info.Value().readOnly = true;
        }
        return info;
    }

    // The host path of `segments` under `root`. Each segment is taken as given when it exists,
    // else as the entry of its directory that equals it without ASCII case (the first in byte
    // order if several do). With `lastMayBeMissing` a missing last segment is kept as given, for
    // a create. Callers hold filesMutex_.
    Result<std::string> resolve(Root root, const std::vector<std::string> &segments,
                                bool lastMayBeMissing) {
        const std::string &base = rootDir(root);
        if (base.empty()) {
            return Error::kNotFound;
        }
        std::map<std::string, std::string> &cache = lookups_[root == Root::kInstall ? 0 : 1];
        std::string host = base;
        std::string key;
        for (std::size_t i = 0; i < segments.size(); i++) {
            const std::string &segment = segments[i];
            key += '/';
            key += lowered(segment);
            auto cached = cache.find(key);
            if (cached != cache.end()) {
                host = base + cached->second;
                continue;
            }
            std::string exact = host + "/" + segment;
            if (storehost::osStat(exact)) {
                host = exact;
            } else {
                std::string match = matchNoCase(host, segment);
                if (match.empty()) {
                    if (i + 1 == segments.size() && lastMayBeMissing) {
                        return exact;
                    }
                    return Error::kNotFound;
                }
                host += "/" + match;
            }
            cache[key] = host.substr(base.size());
        }
        return host;
    }

    static std::string matchNoCase(const std::string &dir, const std::string &segment) {
        std::error_code error;
        std::string best;
        for (fs::directory_iterator it(fs::u8path(dir), error), end; !error && it != end;
             it.increment(error)) {
            std::string name = it->path().filename().u8string();
            if (equalNoCase(name, segment) && (best.empty() || name < best)) {
                best = name;
            }
        }
        return best;
    }

    void forgetLookups(Root root) { lookups_[root == Root::kInstall ? 0 : 1].clear(); }

    static bool isRootKey(std::string_view key) { return key.find('\\') == std::string_view::npos; }

    static void addKey(Keys &keys, std::string_view key) {
        for (std::size_t end = key.find('\\'); ; end = key.find('\\', end + 1)) {
            std::string_view prefix = key.substr(0, end);
            if (!isRootKey(prefix)) {
                std::string lower = lowered(prefix);
                if (keys.count(lower) == 0) {
                    keys[lower].name = std::string(prefix);
                }
            }
            if (end == std::string_view::npos) {
                return;
            }
        }
    }

    static std::string serialize(const Keys &keys) {
        std::string out = std::string(kRegistryHeader) + "\n";
        for (const auto &entry : keys) {
            const Key &key = entry.second;
            out += key.name + "\t\tkey\t\n";
            for (const auto &named : key.values) {
                const RegistryValue &value = named.second.value;
                out += key.name + "\t" + named.second.name + "\t";
                if (value.type == RegistryType::kDword) {
                    char digits[9];
                    std::snprintf(digits, sizeof digits, "%08x", value.dword);
                    out += std::string("dword\t") + digits;
                } else if (value.type == RegistryType::kString) {
                    out += "string\t" + value.text;
                } else {
                    out += "binary\t" + hexOf(value.bytes.data(), value.bytes.size());
                }
                out += "\n";
            }
        }
        return out;
    }

    // Writes `changed` to registry.tsv and, only if that worked, makes it the registry. Callers
    // hold registryMutex_.
    Status commit(Keys changed) {
        if (rootDir(Root::kUser).empty()) {
            return Error::kIoError;
        }
        std::string text = serialize(changed);
        Status written = WriteAtomic(Root::kUser, kRegistryFile, text.data(), text.size());
        if (!written) {
            return Error::kIoError;
        }
        keys_ = std::move(changed);
        return {};
    }

    void loadRegistry() {
        if (rootDir(Root::kUser).empty()) {
            return;
        }
        std::vector<std::string> segments{kRegistryFile};
        Result<std::string> host = resolve(Root::kUser, segments, false);
        if (!host) {
            return;
        }
        Result<OsFile> os = storehost::osOpen(host.Value(), OpenMode::kRead);
        if (!os) {
            return;
        }
        std::string text;
        char chunk[4096];
        for (std::uint64_t offset = 0;;) {
            Result<std::size_t> got = storehost::osRead(os.Value(), chunk, sizeof chunk, offset);
            if (!got || got.Value() == 0) {
                break;
            }
            text.append(chunk, got.Value());
            offset += got.Value();
        }
        (void)storehost::osClose(os.Value());
        std::size_t start = 0;
        while (start < text.size()) {
            std::size_t end = text.find('\n', start);
            if (end == std::string::npos) {
                end = text.size();
            }
            loadLine(std::string_view(text).substr(start, end - start));
            start = end + 1;
        }
    }

    void loadLine(std::string_view line) {
        if (line.empty() || line.front() == '#') {
            return;
        }
        std::string_view fields[4];
        std::size_t start = 0;
        for (int i = 0; i < 3; i++) {
            std::size_t tab = line.find('\t', start);
            if (tab == std::string_view::npos) {
                return;
            }
            fields[i] = line.substr(start, tab - start);
            start = tab + 1;
        }
        fields[3] = line.substr(start);
        if (!validKey(fields[0]) || isRootKey(fields[0]) ||
            fields[3].find('\t') != std::string_view::npos) {
            return;
        }
        RegistryValue value;
        if (fields[2] == "key") {
            addKey(keys_, fields[0]);
            return;
        }
        if (fields[2] == "dword") {
            std::vector<std::uint8_t> bytes;
            if (fields[3].size() != 8 || !parseHex(fields[3], bytes)) {
                return;
            }
            value.dword = static_cast<std::uint32_t>(bytes[0]) << 24 |
                          static_cast<std::uint32_t>(bytes[1]) << 16 |
                          static_cast<std::uint32_t>(bytes[2]) << 8 | bytes[3];
        } else if (fields[2] == "string") {
            value.type = RegistryType::kString;
            value.text = std::string(fields[3]);
        } else if (fields[2] == "binary") {
            value.type = RegistryType::kBinary;
            if (!parseHex(fields[3], value.bytes)) {
                return;
            }
        } else {
            return;
        }
        addKey(keys_, fields[0]);
        keys_[lowered(fields[0])].values[lowered(fields[1])] =
            NamedValue{std::string(fields[1]), value};
    }

    const std::string roots_[2];
    std::mutex filesMutex_;
    std::map<std::uint32_t, OpenFile> files_;
    std::uint32_t nextFile_ = 1;
    std::uint64_t nextTemporary_ = 1;
    std::map<std::string, std::string> lookups_[2]; // lowered "/a/b" -> host "/A/b", per root
    std::mutex registryMutex_;
    Keys keys_;
};

} // namespace

// An empty path leaves that root unset: RootPath() answers kNotFound and every call under it fails
// with kNotFound. The per-user directory must exist; registry.tsv is read from it here, once, and
// a missing file is an empty registry. A registry.tsv line that cannot be read is skipped.
std::unique_ptr<Store> makeStore(std::string installDir, std::string userDir) {
    return std::make_unique<StoreHost>(std::move(installDir), std::move(userDir));
}

} // namespace sfcop::platform
