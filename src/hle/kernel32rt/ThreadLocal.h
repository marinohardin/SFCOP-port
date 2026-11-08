// ThreadLocal: TlsAlloc, TlsFree, TlsGetValue and TlsSetValue on Threads' thread-local slots. The
// C runtime keeps its per-thread data in one of these (its first TlsAlloc, at start-up); a mission
// module's own C runtime takes one when it attaches and gives it back with TlsFree when it detaches.
//
// Threading: any thread; a value belongs to the thread that set it.
#pragma once

#include "hle/kernel32rt/Kernel32rt.h"
#include "platform/Threads/Threads.h"

#include <cstdint>

namespace sfcop::hle::kernel32rt {

class ThreadLocal {
public:
    explicit ThreadLocal(platform::Threads &threads);

    // TlsAlloc: a free index, its value 0 on every thread; TLS_OUT_OF_INDEXES with
    // ERROR_NOT_ENOUGH_MEMORY when none is left.
    Answer Allocate();
    // TlsFree: 1, and the index may be given out again; 0 with ERROR_INVALID_PARAMETER for an
    // index that is not allocated.
    Answer Free(std::uint32_t index);
    // TlsGetValue: the calling thread's value, and last error 0 (Windows clears it on success, so
    // that a stored 0 can be told from a failure); 0 with ERROR_INVALID_PARAMETER for an index
    // that is not allocated.
    Answer Get(std::uint32_t index);
    // TlsSetValue: 1, or 0 with ERROR_INVALID_PARAMETER.
    Answer Set(std::uint32_t index, std::uint32_t value);

private:
    platform::Threads &threads_;
};

} // namespace sfcop::hle::kernel32rt
