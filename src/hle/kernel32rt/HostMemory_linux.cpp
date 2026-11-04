#include "hle/kernel32rt/HostMemory.h"

#include <cerrno>
#include <cstdint>
#include <mutex>
#include <unistd.h>

namespace sfcop::hle::kernel32rt {

bool hostRangeAccessible(const void *address, std::size_t size, bool forWrite) {
    (void)forWrite; // a write probe would need a write; a readable range counts
    static std::mutex lock;
    static int pipeEnds[2] = {-1, -1};
    std::lock_guard<std::mutex> hold(lock);
    if (pipeEnds[0] < 0 && pipe(pipeEnds) != 0) {
        return true;
    }
    char sink[4096];
    const auto *bytes = static_cast<const char *>(address);
    for (std::size_t offset = 0; offset < size; offset += sizeof sink) {
        const std::size_t chunk = size - offset < sizeof sink ? size - offset : sizeof sink;
        const ssize_t put = write(pipeEnds[1], bytes + offset, chunk);
        if (put < 0) {
            return errno != EFAULT;
        }
        // A write that faults part-way returns the bytes before the fault, not -1: a short write
        // means the range runs into an unmapped page.
        if (put > 0 && read(pipeEnds[0], sink, static_cast<std::size_t>(put)) < 0) {
            return true;
        }
        if (static_cast<std::size_t>(put) != chunk) {
            return false;
        }
    }
    return true;
}

} // namespace sfcop::hle::kernel32rt
