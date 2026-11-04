// HostMemory: whether a range of the host's address space can be read (or written) without a
// fault, for IsBadReadPtr, IsBadWritePtr and IsBadCodePtr. The game's C runtime checks a thrown
// object's type information and copy constructor with them before it uses them.
//
// One definition per platform: HostMemory_macos.cpp (the kernel's region table),
// HostMemory_linux.cpp (a probe through a pipe: the kernel reads the range or reports EFAULT, or writes part of it;
// writability is not probed, a readable range counts), HostMemory_windows.cpp (VirtualQuery).
#pragma once

#include <cstddef>

namespace sfcop::hle::kernel32rt {

// True when every byte of [address, address + size) is mapped and readable, and writable too if
// `forWrite`. An empty range is accessible.
bool hostRangeAccessible(const void *address, std::size_t size, bool forWrite);

} // namespace sfcop::hle::kernel32rt
