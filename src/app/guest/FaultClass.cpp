#include "FaultClass.h"

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace sfcop::guest {

CodeRange SpanOfFunctions(const std::uintptr_t *addresses, std::size_t count,
                          std::uintptr_t margin) {
    if (count == 0) {
        return {};
    }
    const auto [low, high] = std::minmax_element(addresses, addresses + count);
    return CodeRange{*low, *high + margin};
}

bool IsLeafMemoryRoutine(const char *symbol) {
    if (symbol == nullptr) {
        return false;
    }
    for (const char *name : {"memmove", "memcpy", "memset", "memcmp", "bzero"}) {
        if (std::strstr(symbol, name) != nullptr) {
            return true;
        }
    }
    return false;
}

bool IsWindowsMemoryFault(std::uint32_t code) {
    return code == 0xC0000005u     // EXCEPTION_ACCESS_VIOLATION
           || code == 0xC0000006u; // EXCEPTION_IN_PAGE_ERROR
}

bool IsWindowsHardwareFault(std::uint32_t code) {
    if (IsWindowsMemoryFault(code)) {
        return true;
    }
    switch (code) {
    case 0x80000002u: // EXCEPTION_DATATYPE_MISALIGNMENT (SIGBUS)
    case 0xC000001Du: // EXCEPTION_ILLEGAL_INSTRUCTION (SIGILL)
    case 0xC0000096u: // EXCEPTION_PRIV_INSTRUCTION (SIGILL)
    case 0xC00000FDu: // EXCEPTION_STACK_OVERFLOW (SIGSEGV)
    case 0xC000008Cu: // EXCEPTION_ARRAY_BOUNDS_EXCEEDED (SIGSEGV)
        return true;
    default:
        // EXCEPTION_FLT_DENORMAL_OPERAND to EXCEPTION_INT_OVERFLOW, 0xC000008D to 0xC0000095 (SIGFPE)
        return code >= 0xC000008Du && code <= 0xC0000095u;
    }
}

#if defined(_WIN32)

CodeRange MemoryRoutineSpan(const void *routine) {
    const auto address = reinterpret_cast<std::uintptr_t>(routine);
    DWORD64 imageBase = 0;
    const RUNTIME_FUNCTION *entry =
        RtlLookupFunctionEntry(static_cast<DWORD64>(address), &imageBase, nullptr);
    if (entry != nullptr && imageBase + entry->BeginAddress == address) {
#if defined(_M_ARM64) || defined(__aarch64__)
        const DWORD unwind = entry->UnwindData;
        const std::uint32_t header =
            (unwind & 3u) != 0 ? 0u : *reinterpret_cast<const std::uint32_t *>(imageBase + unwind);
        return CodeRange{address, address + Arm64FunctionLength(unwind, header)};
#else
        return CodeRange{address, static_cast<std::uintptr_t>(imageBase + entry->EndAddress)};
#endif
    }
    // No entry of its own: up to the next function the image's table lists, within the cap.
    HMODULE image = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCSTR>(routine), &image)) {
        return RoutineSpan(address, 0, kNoEntryLimit);
    }
    const auto base = reinterpret_cast<std::uintptr_t>(image);
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY &directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    const auto *entries =
        reinterpret_cast<const RUNTIME_FUNCTION *>(base + directory.VirtualAddress);
    const std::size_t count = directory.Size / sizeof(RUNTIME_FUNCTION);
    const auto offset = static_cast<DWORD>(address - base);
    std::uintptr_t next = 0;
    for (std::size_t i = 0; i < count; ++i) { // sorted by start; the first past the routine
        if (entries[i].BeginAddress > offset) {
            next = base + entries[i].BeginAddress;
            break;
        }
    }
    return RoutineSpan(address, next, kNoEntryLimit);
}

#endif

} // namespace sfcop::guest
