// macOS: clonefile(2), a copy-on-write clone on APFS when both paths are on one volume.
#include "app/ImportFiles.h"

#include <sys/clonefile.h>

namespace sfcop::app {

bool cloneHostFile(const std::string &from, const std::string &to) {
    return ::clonefile(from.c_str(), to.c_str(), 0) == 0;
}

} // namespace sfcop::app
