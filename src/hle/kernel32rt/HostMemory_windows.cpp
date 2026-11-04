#include "hle/kernel32rt/HostMemory.h"

#include <cstdint>
#include <windows.h>

namespace sfcop::hle::kernel32rt {

bool hostRangeAccessible(const void *address, std::size_t size, bool forWrite) {
    if (size == 0) {
        return true;
    }
    auto at = reinterpret_cast<std::uintptr_t>(address);
    const std::uintptr_t end = at + size;
    if (end < at) {
        return false;
    }
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                           PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    const DWORD writable =
        PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    while (at < end) {
        MEMORY_BASIC_INFORMATION info;
        if (VirtualQuery(reinterpret_cast<const void *>(at), &info, sizeof info) == 0 ||
            info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) != 0 ||
            (info.Protect & (forWrite ? writable : readable)) == 0) {
            return false;
        }
        at = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    }
    return true;
}

} // namespace sfcop::hle::kernel32rt
