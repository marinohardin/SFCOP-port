// The low-memory region (fix_hle_low_memory_reads): its text, its report line, and a block of the
// test's own standing in for guest 0x00010000, read with the region armed (on) and not (off).
#include "app/guest/LowMemory.h"

#include <doctest/doctest.h>

#include <cstring>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <csignal>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace sfcop::guest {

namespace {

std::uint32_t dwordAt(const std::vector<std::uint8_t> &bytes, std::size_t at) {
    return static_cast<std::uint32_t>(bytes[at]) | static_cast<std::uint32_t>(bytes[at + 1]) << 8 |
           static_cast<std::uint32_t>(bytes[at + 2]) << 16 |
           static_cast<std::uint32_t>(bytes[at + 3]) << 24;
}

std::vector<std::uint8_t> region() {
    std::vector<std::uint8_t> bytes(kLowMemoryEnd - kLowMemoryBegin);
    FillLowMemory(bytes.data(), kLowMemoryBegin, bytes.size());
    return bytes;
}

} // namespace

TEST_CASE("the low-memory region is guest 0x00010000 to 0x0001FFFF") {
    CHECK_FALSE(InLowMemory(0x0000FFFFu));
    CHECK(InLowMemory(0x00010000u));
    CHECK(InLowMemory(0x0001038Cu));
    CHECK(InLowMemory(0x0001FFFFu));
    CHECK_FALSE(InLowMemory(0x00020000u));
    CHECK_FALSE(InLowMemory(0u));
}

TEST_CASE("the region's text is printable ASCII with no NUL, about an environment's length") {
    const std::string text = LowMemoryText();
    CHECK(text.size() == LowMemoryTextLength());
    CHECK(text.size() >= 750);  // 1.5 KB of UTF-16
    CHECK(text.size() <= 1500); // 3 KB
    for (char c : text) {
        CHECK(c >= 0x20);
        CHECK(c <= 0x7E);
    }
    CHECK(text.rfind("ALLUSERSPROFILE=", 0) == 0);
}

TEST_CASE("the region reads as the text in UTF-16, from its first byte") {
    const std::vector<std::uint8_t> bytes = region();
    CHECK(dwordAt(bytes, 0) == 0x004C0041u);     // "AL"
    CHECK(dwordAt(bytes, 0x38C) == 0x0054005Cu); // "\T": guest 0x0001038C, the patrol missions' read
}

TEST_CASE("every 32-bit read in the region, aligned or not, is at least 0x00200020") {
    const std::vector<std::uint8_t> bytes = region();
    std::uint32_t lowest = 0xFFFFFFFFu;
    for (std::size_t at = 0; at + 4 <= bytes.size(); ++at) {
        const std::uint32_t value = dwordAt(bytes, at);
        lowest = value < lowest ? value : lowest;
    }
    CHECK(lowest >= 0x00200020u);
}

TEST_CASE("the text runs on across page boundaries whatever the page size") {
    const std::vector<std::uint8_t> whole = region();
    for (std::size_t page : {std::size_t{0x1000}, std::size_t{0x4000}}) {
        std::vector<std::uint8_t> pages(whole.size());
        for (std::size_t at = 0; at < pages.size(); at += page) {
            FillLowMemory(pages.data() + at, kLowMemoryBegin + static_cast<std::uint32_t>(at), page);
        }
        CHECK(pages == whole);
    }
}

TEST_CASE("the report line names the reader and the address") {
    char line[128];
    const std::size_t length =
        FormatLowMemoryRead(line, sizeof line, "Met_17Patrol+0xEBB0", 0x0001038Cu);
    CHECK(std::string(line) ==
          "low-memory read: Met_17Patrol+0xEBB0 read 0x0001038C (fix on, continuing)\n");
    CHECK(length == std::strlen(line));
    FormatLowMemoryRead(line, sizeof line, "STARFLEETOP 0x00412345", 0x00010004u);
    CHECK(std::string(line) ==
          "low-memory read: STARFLEETOP 0x00412345 read 0x00010004 (fix on, continuing)\n");
}

TEST_CASE("a module's stem drops its folders and extension") {
    char stem[32];
    CHECK(std::string(ModuleStem("/mods/missions/Met_17Patrol/Met_17Patrol.dylib", stem, sizeof stem)) ==
          "Met_17Patrol");
    CHECK(std::string(ModuleStem("C:\\mods\\Met_6Patrol.dll", stem, sizeof stem)) == "Met_6Patrol");
    CHECK(std::string(ModuleStem("libscript.so", stem, sizeof stem)) == "libscript");
    CHECK(std::string(ModuleStem("noext", stem, sizeof stem)) == "noext");
    CHECK(std::string(ModuleStem("a/very_long_module_name.dylib", stem, 5)) == "very");
}

TEST_CASE("a page size that does not divide the region leaves it disabled") {
    std::uint8_t unused = 0;
    CHECK(LowMemoryRegion(&unused, 0x4000).PageCount() == 4);
    CHECK(LowMemoryRegion(&unused, 0x1000).PageCount() == 16);
    CHECK(LowMemoryRegion(&unused, 0x10000).PageCount() == 1);
    LowMemoryRegion odd(&unused, 3000);
    CHECK(odd.PageCount() == 0);
    CHECK_FALSE(odd.Arm());
    CHECK(LowMemoryRegion(&unused, 0x20000).PageCount() == 0);
    // Not armed: nothing is handled.
    CHECK(odd.OnFault(0x0001038Cu, FaultAccess::kRead) == LowMemoryRegion::Outcome::kNotHandled);
}

#if !defined(_WIN32)

namespace {

// A host block standing in for guest 0x00010000-0x0001FFFF, and the region over it.
struct Stand {
    std::uint8_t *block = nullptr;
    std::size_t page = static_cast<std::size_t>(getpagesize());
    Stand() {
        void *p = mmap(nullptr, kLowMemoryEnd - kLowMemoryBegin, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        REQUIRE(p != MAP_FAILED);
        block = static_cast<std::uint8_t *>(p);
    }
    ~Stand() { munmap(block, kLowMemoryEnd - kLowMemoryBegin); }
    std::uint8_t *guestZero() const {
        return reinterpret_cast<std::uint8_t *>(reinterpret_cast<std::uintptr_t>(block) - kLowMemoryBegin);
    }
    std::uint32_t read(std::uint32_t guest) const {
        std::uint32_t value = 0;
        const volatile std::uint8_t *at = guestZero() + guest;
        for (int i = 3; i >= 0; --i) {
            value = value << 8 | at[i];
        }
        return value;
    }
};

LowMemoryRegion *g_region = nullptr;
std::uint8_t *g_guestZero = nullptr;
volatile int g_mapped = 0;
constexpr std::uint32_t kReader = 0x6000EBB0u; // the guest function the test's reads stand for

// The fault handler's part that the guest host's own plays: a handled fault resumes the read.
void onTestFault(int, siginfo_t *info, void *) {
    const auto guest = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(info->si_addr) -
                                                  reinterpret_cast<std::uintptr_t>(g_guestZero));
    const LowMemoryRegion::Outcome outcome = g_region->OnFault(
        guest, FaultAccess::kRead, kReader, reinterpret_cast<std::uintptr_t>(info->si_addr));
    if (outcome == LowMemoryRegion::Outcome::kMapped) {
        g_mapped = g_mapped + 1;
        return;
    }
    if (outcome == LowMemoryRegion::Outcome::kRetry) {
        return;
    }
    _exit(7); // not handled: the read ends the run, as without the fix
}

struct Handler {
    struct sigaction oldSegv {}, oldBus {};
    Handler(LowMemoryRegion &region, std::uint8_t *guestZero) {
        g_region = &region;
        g_guestZero = guestZero;
        g_mapped = 0;
        struct sigaction action {};
        action.sa_sigaction = onTestFault;
        action.sa_flags = SA_SIGINFO | SA_NODEFER;
        sigaction(SIGSEGV, &action, &oldSegv);
        sigaction(SIGBUS, &action, &oldBus);
    }
    ~Handler() {
        sigaction(SIGSEGV, &oldSegv, nullptr);
        sigaction(SIGBUS, &oldBus, nullptr);
        g_region = nullptr;
    }
};

} // namespace

TEST_CASE("on: the first read of a page maps it read-only with the text, and the read resumes") {
    Stand stand;
    LowMemoryRegion region(stand.guestZero(), stand.page);
    REQUIRE(region.Arm());
    Handler handler(region, stand.guestZero());

    CHECK(stand.read(0x0001038Cu) == 0x0054005Cu);
    CHECK(g_mapped == 1);
    CHECK(stand.read(0x00010000u) == 0x004C0041u); // the same page: no second fault
    CHECK(g_mapped == 1);
    CHECK(stand.read(0x0001FFFCu) >= 0x00200020u); // the last page, another one below 64 KB pages
    CHECK(g_mapped == (stand.page == 0x10000 ? 1 : 2));

    // Mapped pages are read-only: a write is not handled, and neither is an access of unknown
    // kind there; a read is retried.
    CHECK(region.OnFault(0x0001038Cu, FaultAccess::kWrite) == LowMemoryRegion::Outcome::kNotHandled);
    CHECK(region.OnFault(0x0001038Cu, FaultAccess::kUnknown) == LowMemoryRegion::Outcome::kNotHandled);
    CHECK(region.OnFault(0x0001038Cu, FaultAccess::kRead) == LowMemoryRegion::Outcome::kRetry);
    // Outside the region: not handled.
    CHECK(region.OnFault(0x00020000u, FaultAccess::kRead) == LowMemoryRegion::Outcome::kNotHandled);
    CHECK(region.OnFault(0x0000FFFCu, FaultAccess::kRead) == LowMemoryRegion::Outcome::kNotHandled);
}

TEST_CASE("on: the fault path only records the read; each record is taken once, outside it") {
    Stand stand;
    LowMemoryRegion region(stand.guestZero(), stand.page);
    REQUIRE(region.Arm());
    LowMemoryRegion::Read reads[16];
    CHECK(region.TakeReads(reads, 16) == 0);
    {
        Handler handler(region, stand.guestZero());
        CHECK(stand.read(0x0001038Cu) == 0x0054005Cu);
        CHECK(stand.read(0x00010390u) != 0u); // the same page: nothing more recorded
    }
    // Stand::read takes its bytes from the highest, so the faulting byte is the dword's last; the
    // test's handler passes the faulting address as the pc.
    REQUIRE(region.TakeReads(reads, 16) == 1);
    CHECK(reads[0].address == 0x0001038Fu);
    CHECK(reads[0].function == kReader);
    CHECK(reads[0].pc == reinterpret_cast<std::uintptr_t>(stand.guestZero() + 0x0001038Fu));
    CHECK(region.TakeReads(reads, 16) == 0); // taken once

    {
        Handler handler(region, stand.guestZero());
        static_cast<void>(stand.read(0x0001FFF0u));
    }
    if (stand.page < 0x10000) {
        REQUIRE(region.TakeReads(reads, 16) == 1);
        CHECK(reads[0].address == 0x0001FFF3u);
    }
    // A record taken with no room is left for the next call.
    CHECK(region.TakeReads(reads, 0) == 0);
}

TEST_CASE("on: a write to the region still faults") {
    Stand stand;
    const pid_t child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        LowMemoryRegion region(stand.guestZero(), stand.page);
        if (!region.Arm()) {
            _exit(2);
        }
        struct sigaction action {};
        action.sa_handler = [](int) { _exit(7); };
        sigaction(SIGSEGV, &action, nullptr);
        sigaction(SIGBUS, &action, nullptr);
        *reinterpret_cast<volatile std::uint32_t *>(stand.guestZero() + 0x0001038Cu) = 1;
        _exit(0);
    }
    int status = 0;
    REQUIRE(waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 7);
}

TEST_CASE("off: the region is not armed and a read of it ends the run") {
    Stand stand;
    const pid_t child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        // The region without the switch: inaccessible as the guest's unreserved memory is, and
        // never armed.
        mprotect(stand.block, kLowMemoryEnd - kLowMemoryBegin, PROT_NONE);
        LowMemoryRegion region(stand.guestZero(), stand.page);
        Handler handler(region, stand.guestZero());
        const std::uint32_t value = stand.read(0x0001038Cu);
        _exit(value == 0 ? 3 : 0);
    }
    int status = 0;
    REQUIRE(waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 7);
}

#endif

} // namespace sfcop::guest
