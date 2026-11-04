#include "hle/kernel32rt/HostMemory.h"

#include <cstdint>
#include <mach/mach.h>
#include <mach/mach_vm.h>

namespace sfcop::hle::kernel32rt {

bool hostRangeAccessible(const void *address, std::size_t size, bool forWrite) {
    if (size == 0) {
        return true;
    }
    mach_vm_address_t at = reinterpret_cast<std::uintptr_t>(address);
    const mach_vm_address_t end = at + size;
    if (end < at) {
        return false;
    }
    while (at < end) {
        mach_vm_address_t region = at;
        mach_vm_size_t regionSize = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        if (mach_vm_region(mach_task_self(), &region, &regionSize, VM_REGION_BASIC_INFO_64,
                           reinterpret_cast<vm_region_info_t>(&info), &count,
                           &object) != KERN_SUCCESS ||
            region > at) {
            return false; // unmapped: the next region starts above `at`
        }
        if (!(info.protection & VM_PROT_READ) || (forWrite && !(info.protection & VM_PROT_WRITE))) {
            return false;
        }
        at = region + regionSize;
    }
    return true;
}

} // namespace sfcop::hle::kernel32rt
