// Linux: the FICLONE ioctl, a reflink on Btrfs, XFS and others that offer one, when both paths
// are on one file system. The new file is removed again when the clone fails.
#include "app/ImportFiles.h"

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace sfcop::app {

bool cloneHostFile(const std::string &from, const std::string &to) {
    const int in = ::open(from.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        return false;
    }
    const int out = ::open(to.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (out < 0) {
        ::close(in);
        return false;
    }
    const bool cloned = ::ioctl(out, FICLONE, in) == 0;
    ::close(in);
    const bool closed = ::close(out) == 0;
    if (!cloned || !closed) {
        ::unlink(to.c_str());
        return false;
    }
    return true;
}

} // namespace sfcop::app
