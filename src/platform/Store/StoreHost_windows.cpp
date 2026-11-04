// Store's platform primitives on Windows: file HANDLEs opened by UTF-16 path, not inherited by
// child processes; FILETIME for modification times; MoveFileExW to rename over an existing file.

#include "platform/Store/Store.h"

#include <algorithm>
#include <cstdint>
#include <string>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace sfcop::platform::storehost {

// The primitives StoreHost.cpp declares, for this platform.
using OsFile = std::int64_t;

namespace {

// 100 ns intervals between 1601-01-01 and 1970-01-01.
constexpr std::int64_t kEpochDifference = 116444736000000000LL;

std::wstring widePath(const std::string &path) {
    int count = MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()),
                                    nullptr, 0);
    std::wstring out(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), out.data(),
                        count);
    for (wchar_t &c : out) {
        if (c == L'/') {
            c = L'\\';
        }
    }
    return out;
}

Error fromLastError(DWORD error) {
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
        return Error::kNotFound;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return Error::kAlreadyExists;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_WRITE_PROTECT:
        return Error::kAccessDenied;
    case ERROR_TOO_MANY_OPEN_FILES:
    case ERROR_NOT_ENOUGH_MEMORY:
        return Error::kOutOfResources;
    default:
        return Error::kIoError;
    }
}

HANDLE handleOf(OsFile file) { return reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(file)); }

std::int64_t nsFromFileTime(const FILETIME &time) {
    std::int64_t ticks = static_cast<std::int64_t>(
        static_cast<std::uint64_t>(time.dwHighDateTime) << 32 | time.dwLowDateTime);
    return (ticks - kEpochDifference) * 100;
}

FileInfo infoOf(DWORD attributes, std::uint64_t size, const FILETIME &written) {
    FileInfo info;
    info.isDirectory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    info.size = info.isDirectory ? 0 : size;
    info.readOnly = (attributes & FILE_ATTRIBUTE_READONLY) != 0;
    info.modifiedNs = nsFromFileTime(written);
    return info;
}

} // namespace

Result<OsFile> osOpen(const std::string &path, OpenMode mode) {
    DWORD access = GENERIC_READ | (mode == OpenMode::kRead ? 0 : GENERIC_WRITE);
    DWORD disposition = OPEN_EXISTING;
    if (mode == OpenMode::kCreate) {
        disposition = CREATE_ALWAYS;
    } else if (mode == OpenMode::kCreateNew) {
        disposition = CREATE_NEW;
    } else if (mode == OpenMode::kOpenOrCreate) {
        disposition = OPEN_ALWAYS;
    }
    // A null SECURITY_ATTRIBUTES makes the handle not inheritable.
    HANDLE file = CreateFileW(widePath(path).c_str(), access,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return fromLastError(GetLastError());
    }
    return static_cast<OsFile>(reinterpret_cast<std::intptr_t>(file));
}

Result<std::size_t> osRead(OsFile file, void *out, std::size_t bytes, std::uint64_t offset) {
    std::size_t done = 0;
    while (done < bytes) {
        OVERLAPPED at = {};
        std::uint64_t position = offset + done;
        at.Offset = static_cast<DWORD>(position);
        at.OffsetHigh = static_cast<DWORD>(position >> 32);
        DWORD chunk = static_cast<DWORD>((std::min)(bytes - done, std::size_t{1} << 30));
        DWORD got = 0;
        if (!ReadFile(handleOf(file), static_cast<char *>(out) + done, chunk, &got, &at)) {
            if (GetLastError() == ERROR_HANDLE_EOF) {
                break;
            }
            return Error::kIoError;
        }
        if (got == 0) {
            break;
        }
        done += got;
    }
    return done;
}

Result<std::size_t> osWrite(OsFile file, const void *data, std::size_t bytes,
                            std::uint64_t offset) {
    std::size_t done = 0;
    while (done < bytes) {
        OVERLAPPED at = {};
        std::uint64_t position = offset + done;
        at.Offset = static_cast<DWORD>(position);
        at.OffsetHigh = static_cast<DWORD>(position >> 32);
        DWORD chunk = static_cast<DWORD>((std::min)(bytes - done, std::size_t{1} << 30));
        DWORD put = 0;
        if (!WriteFile(handleOf(file), static_cast<const char *>(data) + done, chunk, &put,
                       &at) ||
            put == 0) {
            return GetLastError() == ERROR_ACCESS_DENIED ? Error::kAccessDenied
                                                         : Error::kIoError;
        }
        done += put;
    }
    return done;
}

Result<FileInfo> osInfo(OsFile file) {
    BY_HANDLE_FILE_INFORMATION data;
    if (!GetFileInformationByHandle(handleOf(file), &data)) {
        return Error::kIoError;
    }
    std::uint64_t size = static_cast<std::uint64_t>(data.nFileSizeHigh) << 32 | data.nFileSizeLow;
    return infoOf(data.dwFileAttributes, size, data.ftLastWriteTime);
}

Status osSetSize(OsFile file, std::uint64_t bytes) {
    FILE_END_OF_FILE_INFO end = {};
    end.EndOfFile.QuadPart = static_cast<LONGLONG>(bytes);
    if (!SetFileInformationByHandle(handleOf(file), FileEndOfFileInfo, &end, sizeof end)) {
        return GetLastError() == ERROR_ACCESS_DENIED ? Error::kAccessDenied : Error::kIoError;
    }
    return {};
}

Status osSetModifiedTime(OsFile file, std::int64_t modifiedNs) {
    std::uint64_t ticks = static_cast<std::uint64_t>(modifiedNs / 100 + kEpochDifference);
    FILETIME written;
    written.dwLowDateTime = static_cast<DWORD>(ticks);
    written.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
    if (!SetFileTime(handleOf(file), nullptr, nullptr, &written)) {
        return GetLastError() == ERROR_ACCESS_DENIED ? Error::kAccessDenied : Error::kIoError;
    }
    return {};
}

Status osFlush(OsFile file) {
    if (!FlushFileBuffers(handleOf(file))) {
        return Error::kIoError;
    }
    return {};
}

Status osClose(OsFile file) {
    if (!CloseHandle(handleOf(file))) {
        return Error::kIoError;
    }
    return {};
}

Result<FileInfo> osStat(const std::string &path) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(widePath(path).c_str(), GetFileExInfoStandard, &data)) {
        DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
                       error == ERROR_INVALID_NAME
                   ? Error::kNotFound
                   : Error::kIoError;
    }
    std::uint64_t size = static_cast<std::uint64_t>(data.nFileSizeHigh) << 32 | data.nFileSizeLow;
    return infoOf(data.dwFileAttributes, size, data.ftLastWriteTime);
}

Status osReplace(const std::string &from, const std::string &to) {
    if (!MoveFileExW(widePath(from).c_str(), widePath(to).c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD error = GetLastError();
        return error == ERROR_ACCESS_DENIED && GetFileAttributesW(widePath(to).c_str()) !=
                                                   INVALID_FILE_ATTRIBUTES &&
                       (GetFileAttributesW(widePath(to).c_str()) & FILE_ATTRIBUTE_DIRECTORY)
                   ? Error::kAlreadyExists
                   : fromLastError(error);
    }
    return {};
}

} // namespace sfcop::platform::storehost
