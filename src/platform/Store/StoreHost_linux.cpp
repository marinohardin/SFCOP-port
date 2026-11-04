// Store's platform primitives on Linux: POSIX descriptors; fsync so that a flushed file is on the
// disk before the atomic write renames it.

#include "platform/Store/Store.h"

#include <cstdint>
#include <string>

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace sfcop::platform::storehost {

// The primitives StoreHost.cpp declares, for this platform.
using OsFile = std::int64_t;

namespace {

Error fromErrno(int error) {
    switch (error) {
    case ENOENT:
    case ENOTDIR:
        return Error::kNotFound;
    case EEXIST:
        return Error::kAlreadyExists;
    case EACCES:
    case EPERM:
    case EROFS:
    case EBADF:
        return Error::kAccessDenied;
    case EISDIR:
        return Error::kWrongState;
    case EMFILE:
    case ENFILE:
    case ENOMEM:
        return Error::kOutOfResources;
    default:
        return Error::kIoError;
    }
}

FileInfo infoOf(const struct stat &st) {
    FileInfo info;
    info.isDirectory = S_ISDIR(st.st_mode);
    info.size = info.isDirectory ? 0 : static_cast<std::uint64_t>(st.st_size);
    info.readOnly = (st.st_mode & S_IWUSR) == 0;
    info.modifiedNs = static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1000000000 +
                      st.st_mtim.tv_nsec;
    return info;
}

int fd(OsFile file) { return static_cast<int>(file); }

} // namespace

Result<OsFile> osOpen(const std::string &path, OpenMode mode) {
    int flags = O_CLOEXEC;
    switch (mode) {
    case OpenMode::kRead:
        flags |= O_RDONLY;
        break;
    case OpenMode::kReadWrite:
        flags |= O_RDWR;
        break;
    case OpenMode::kCreate:
        flags |= O_RDWR | O_CREAT | O_TRUNC;
        break;
    case OpenMode::kCreateNew:
        flags |= O_RDWR | O_CREAT | O_EXCL;
        break;
    case OpenMode::kOpenOrCreate:
        flags |= O_RDWR | O_CREAT;
        break;
    }
    int file = ::open(path.c_str(), flags, 0644);
    if (file < 0) {
        return fromErrno(errno);
    }
    return static_cast<OsFile>(file);
}

Result<std::size_t> osRead(OsFile file, void *out, std::size_t bytes, std::uint64_t offset) {
    std::size_t done = 0;
    while (done < bytes) {
        ssize_t got = ::pread(fd(file), static_cast<char *>(out) + done, bytes - done,
                              static_cast<off_t>(offset + done));
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got < 0) {
            return Error::kIoError;
        }
        if (got == 0) {
            break;
        }
        done += static_cast<std::size_t>(got);
    }
    return done;
}

Result<std::size_t> osWrite(OsFile file, const void *data, std::size_t bytes,
                            std::uint64_t offset) {
    std::size_t done = 0;
    while (done < bytes) {
        ssize_t put = ::pwrite(fd(file), static_cast<const char *>(data) + done, bytes - done,
                               static_cast<off_t>(offset + done));
        if (put < 0 && errno == EINTR) {
            continue;
        }
        if (put <= 0) {
            return errno == EBADF ? Error::kAccessDenied : Error::kIoError;
        }
        done += static_cast<std::size_t>(put);
    }
    return done;
}

Result<FileInfo> osInfo(OsFile file) {
    struct stat st;
    if (::fstat(fd(file), &st) != 0) {
        return Error::kIoError;
    }
    return infoOf(st);
}

Status osSetSize(OsFile file, std::uint64_t bytes) {
    if (::ftruncate(fd(file), static_cast<off_t>(bytes)) != 0) {
        return errno == EBADF || errno == EINVAL ? Error::kAccessDenied : Error::kIoError;
    }
    return {};
}

Status osSetModifiedTime(OsFile file, std::int64_t modifiedNs) {
    struct timespec times[2];
    times[0].tv_sec = 0;
    times[0].tv_nsec = UTIME_OMIT;
    std::int64_t seconds = modifiedNs / 1000000000;
    std::int64_t rest = modifiedNs % 1000000000;
    if (rest < 0) {
        seconds -= 1;
        rest += 1000000000;
    }
    times[1].tv_sec = static_cast<time_t>(seconds);
    times[1].tv_nsec = static_cast<long>(rest);
    if (::futimens(fd(file), times) != 0) {
        return errno == EPERM || errno == EACCES ? Error::kAccessDenied : Error::kIoError;
    }
    return {};
}

Status osFlush(OsFile file) {
    if (::fsync(fd(file)) != 0) {
        return Error::kIoError;
    }
    return {};
}

Status osClose(OsFile file) {
    if (::close(fd(file)) != 0 && errno != EINTR) {
        return Error::kIoError;
    }
    return {};
}

Result<FileInfo> osStat(const std::string &path) {
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0) {
        return errno == ENOENT || errno == ENOTDIR ? Error::kNotFound : Error::kIoError;
    }
    return infoOf(st);
}

Status osReplace(const std::string &from, const std::string &to) {
    if (::rename(from.c_str(), to.c_str()) != 0) {
        return errno == EISDIR || errno == ENOTEMPTY ? Error::kAlreadyExists : fromErrno(errno);
    }
    return {};
}

} // namespace sfcop::platform::storehost
