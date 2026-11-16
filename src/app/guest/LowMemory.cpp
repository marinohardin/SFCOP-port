#include "LowMemory.h"

#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace sfcop::guest {

namespace {

// An invented environment, its entries separated by spaces (LowMemory.h says why). No entry
// names a real machine, user or folder.
constexpr char kText[] =
    R"(ALLUSERSPROFILE=Q:\Profiles\Shared )"
    R"(APPDATA=Q:\Profiles\TestCaptain\Application Data )"
    R"(CLIENTNAME=Console )"
    R"(COMMONPROGRAMFILES=Q:\Programs\Shared Files )"
    R"(COMPUTERNAME=TESTHOST-01 )"
    R"(COMSPEC=Q:\System\Tools\Shell.exe )"
    R"(EXAMPLE_HOME=Q:\Programs\Example Tools )"
    R"(HOMEDRIVE=Q: )"
    R"(HOMEPATH=\Profiles\TestCaptain )"
    R"(INCLUDE=Q:\Programs\Example Tools\Include;Q:\Programs\Example Tools\Include\Extra )"
    R"(LIB=Q:\Programs\Example Tools\Lib;Q:\Programs\Example Tools\Lib\Extra )"
    R"(LOGONSERVER=\\TESTHOST-01 )"
    R"(NUMBER_OF_PROCESSORS=1 )"
    R"(OS=Example_Operating_System )"
    R"(PATH=Q:\System;Q:\System\Tools;Q:\System\Tools\Admin;Q:\Programs\Shared Files\Bin;)"
    R"(Q:\Programs\Example Tools\Bin;Q:\Programs\Example Tools\Utilities;Q:\Programs\Sample )"
    R"(Viewer;Q:\Programs\Sample Player\Codecs;Q:\Programs\Sample Archiver )"
    R"(PATHEXT=.COM;.EXE;.BAT;.CMD;.VBS;.JS )"
    R"(PROCESSOR_ARCHITECTURE=x86 )"
    R"(PROCESSOR_IDENTIFIER=Example Family 6 Model 8 Stepping 3, ExampleVendor )"
    R"(PROCESSOR_LEVEL=6 )"
    R"(PROCESSOR_REVISION=0803 )"
    R"(PROGRAMFILES=Q:\Programs )"
    R"(PROMPT=$P$G )"
    R"(SAMPLE_PLAYER_CONFIG=Q:\Programs\Sample Player\Settings\Default Profile.cfg )"
    R"(SESSIONNAME=Console )"
    R"(SYSTEMDRIVE=Q: )"
    R"(SYSTEMROOT=Q:\System )"
    R"(TEMP=Q:\Profiles\TestCaptain\Local Settings\Scratch )"
    R"(TMP=Q:\Profiles\TestCaptain\Local Settings\Scratch )"
    R"(USERDOMAIN=TESTHOST-01 )"
    R"(USERNAME=TestCaptain )"
    R"(USERPROFILE=Q:\Profiles\TestCaptain )"
    R"(WINDIR=Q:\System )";

constexpr std::size_t kTextLength = sizeof kText - 1;

enum class Protection { kNone, kReadOnly, kReadWrite };

bool protect(std::uint8_t *at, std::size_t bytes, Protection protection) {
#if defined(_WIN32)
    const DWORD flags = protection == Protection::kNone       ? PAGE_NOACCESS
                        : protection == Protection::kReadOnly ? PAGE_READONLY
                                                              : PAGE_READWRITE;
    DWORD old = 0;
    return VirtualProtect(at, bytes, flags, &old) != 0;
#else
    const int flags = protection == Protection::kNone       ? PROT_NONE
                      : protection == Protection::kReadOnly ? PROT_READ
                                                            : PROT_READ | PROT_WRITE;
    return mprotect(at, bytes, flags) == 0;
#endif
}

} // namespace

const char *LowMemoryText() {
    return kText;
}

std::size_t LowMemoryTextLength() {
    return kTextLength;
}

void FillLowMemory(std::uint8_t *out, std::uint32_t guest, std::size_t bytes) {
    std::size_t unit = (guest - kLowMemoryBegin) / 2 % kTextLength;
    for (std::size_t i = 0; i + 1 < bytes; i += 2) {
        out[i] = static_cast<std::uint8_t>(kText[unit]);
        out[i + 1] = 0;
        if (++unit == kTextLength) {
            unit = 0;
        }
    }
}

std::size_t FormatLowMemoryRead(char *out, std::size_t size, const char *reader, std::uint32_t address) {
    const int length = std::snprintf(out, size, "low-memory read: %s read 0x%08X (fix on, continuing)\n",
                                     reader, address);
    if (length < 0 || size == 0) {
        return 0;
    }
    return static_cast<std::size_t>(length) < size ? static_cast<std::size_t>(length) : size - 1;
}

const char *ModuleStem(const char *path, char *out, std::size_t size) {
    if (size == 0) {
        return out;
    }
    const char *name = path;
    for (const char *p = path; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') {
            name = p + 1;
        }
    }
    const char *dot = std::strrchr(name, '.');
    std::size_t length = dot != nullptr && dot != name ? static_cast<std::size_t>(dot - name)
                                                       : std::strlen(name);
    if (length >= size) {
        length = size - 1;
    }
    std::memcpy(out, name, length);
    out[length] = '\0';
    return out;
}

LowMemoryRegion::LowMemoryRegion(std::uint8_t *guestZero, std::size_t hostPage)
    : guestZero_(guestZero), hostPage_(hostPage) {
    const std::size_t region = kLowMemoryEnd - kLowMemoryBegin;
    if (hostPage != 0 && hostPage <= region && region % hostPage == 0 && region / hostPage <= kMaxPages) {
        pages_ = region / hostPage;
    }
}

bool LowMemoryRegion::Arm() {
    if (pages_ == 0 || armed_) {
        return armed_;
    }
    if (!protect(guestZero_ + kLowMemoryBegin, kLowMemoryEnd - kLowMemoryBegin, Protection::kNone)) {
        return false;
    }
    armed_ = true;
    return true;
}

// The guest runs one thread at a time (the machine lock), so a page is filled while no other
// guest thread can read it; the states only keep two faults from mapping one page twice.
LowMemoryRegion::Outcome LowMemoryRegion::OnFault(std::uint32_t guest, FaultAccess access) {
    if (!armed_ || !InLowMemory(guest) || access == FaultAccess::kWrite) {
        return Outcome::kNotHandled;
    }
    const std::size_t index = (guest - kLowMemoryBegin) / hostPage_;
    std::atomic<std::uint8_t> &state = state_[index];
    std::uint8_t seen = kNone;
    if (state.compare_exchange_strong(seen, kMapping, std::memory_order_acq_rel)) {
        const std::uint32_t first = kLowMemoryBegin + static_cast<std::uint32_t>(index * hostPage_);
        std::uint8_t *page = guestZero_ + first;
        if (!protect(page, hostPage_, Protection::kReadWrite)) {
            state.store(kNone, std::memory_order_release);
            return Outcome::kNotHandled;
        }
        FillLowMemory(page, first, hostPage_);
        if (!protect(page, hostPage_, Protection::kReadOnly)) {
            // Readable all the same; a write that should fault would not, so say it is not ours.
            protect(page, hostPage_, Protection::kNone);
            state.store(kNone, std::memory_order_release);
            return Outcome::kNotHandled;
        }
        state.store(kReadable, std::memory_order_release);
        return Outcome::kMapped;
    }
    while (seen == kMapping) {
        seen = state.load(std::memory_order_acquire);
    }
    // Readable already: a read that faulted while another fault mapped the page is retried; an
    // access of unknown kind there may be a write, which must not be retried for ever.
    return seen == kReadable && access == FaultAccess::kRead ? Outcome::kRetry : Outcome::kNotHandled;
}

} // namespace sfcop::guest
