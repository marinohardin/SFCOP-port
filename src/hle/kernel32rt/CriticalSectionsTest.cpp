#include "hle/kernel32rt/CriticalSections.h"

#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace sfcop::hle::kernel32rt {
namespace {

constexpr std::uint32_t kAddress = 0x00C81000u;
constexpr std::uint32_t kMain = 0x104u;  // thread ids as GetCurrentThreadId gives them
constexpr std::uint32_t kOther = 0x108u;
constexpr std::uint32_t kThird = 0x10Cu;

using Bytes = std::array<std::uint8_t, kSectionBytes>;

// A guest CRITICAL_SECTION in host memory, filled with a pattern no field ever holds.
struct Section {
    Bytes bytes;
    Section() { bytes.fill(0xCC); }
    GuestSection guest() { return GuestSection{kAddress, bytes.data()}; }
};

// The guest machine as a plain mutex: a test thread holds it while it "runs guest code".
class TestMachine final : public Machine {
public:
    void GiveUp() override {
        gaveUp++;
        lock.unlock();
    }
    void TakeBack() override { lock.lock(); }
    std::mutex lock;
    std::atomic<int> gaveUp{0};
};

// A machine for single-thread cases, where nothing may wait.
class NoWaitMachine final : public Machine {
public:
    void GiveUp() override { FAIL("an uncontended Enter gave the machine up"); }
    void TakeBack() override {}
};

// Takes `machine` once `ready` holds; the caller then holds it. A thread that sets the condition
// while it holds the machine and then waits in Enter is queued by the time this returns.
template <typename Ready> void takeMachineWhen(TestMachine &machine, Ready ready) {
    for (;;) {
        machine.lock.lock();
        if (ready()) {
            return;
        }
        machine.lock.unlock();
        std::this_thread::yield();
    }
}

// The 24 bytes, little-endian, as Windows XP has them: DebugInfo, LockCount, RecursionCount,
// OwningThread, LockSemaphore, SpinCount.
const Bytes kFreeBytes = {0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0,
                          0, 0, 0, 0, 0,    0,    0,    0,    0, 0, 0, 0};
const Bytes kHeldTwiceByMain = {0, 0, 0, 0, 0x01, 0, 0, 0, 0x02, 0, 0, 0,
                                0x04, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
const Bytes kHeldByMainOtherWaiting = {0, 0, 0, 0, 0x01, 0, 0, 0, 0x01, 0, 0, 0,
                                       0x04, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
const Bytes kHandedToOther = {0, 0, 0, 0, 0x00, 0, 0, 0, 0x01, 0, 0, 0,
                              0x08, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

TEST_CASE("InitializeCriticalSection writes a free section: LockCount -1, the rest zero") {
    CriticalSections sections;
    Section section;
    sections.Initialize(section.guest());
    CHECK(section.bytes == kFreeBytes);
}

TEST_CASE("a section is recursive: the owner, recursion and lock counts are in its bytes") {
    CriticalSections sections;
    NoWaitMachine machine;
    Section section;
    sections.Initialize(section.guest());
    sections.Enter(section.guest(), kMain, machine);
    sections.Enter(section.guest(), kMain, machine);
    CHECK(section.bytes == kHeldTwiceByMain);
    CHECK(sections.Leave(section.guest(), kMain));
    CHECK(sections.Leave(section.guest(), kMain));
    CHECK(section.bytes == kFreeBytes);
    CHECK_FALSE(sections.Leave(section.guest(), kMain)); // no longer held
    CHECK(section.bytes == kFreeBytes);
    CHECK(sections.WaitObjects() == 0u); // the uncontended path made no host object
}

TEST_CASE("TryEnter takes a free section, enters again for its owner, and refuses another thread") {
    CriticalSections sections;
    Section section;
    sections.Initialize(section.guest());
    CHECK(sections.TryEnter(section.guest(), kMain));
    CHECK(sections.TryEnter(section.guest(), kMain));
    CHECK(section.bytes == kHeldTwiceByMain);
    CHECK_FALSE(sections.TryEnter(section.guest(), kOther));
    CHECK(section.bytes == kHeldTwiceByMain); // a refused TryEnter changes nothing
    CHECK_FALSE(sections.Leave(section.guest(), kOther));
    CHECK(section.bytes == kHeldTwiceByMain);
    CHECK(sections.WaitObjects() == 0u);
}

TEST_CASE("two threads: one waits without the machine, then is handed the section") {
    CriticalSections sections;
    TestMachine machine;
    Section section;
    sections.Initialize(section.guest());

    machine.lock.lock();
    sections.Enter(section.guest(), kMain, machine);
    machine.lock.unlock();

    std::atomic<bool> entering{false};
    std::atomic<bool> got{false};
    std::thread other([&] {
        machine.lock.lock();
        CHECK_FALSE(sections.TryEnter(section.guest(), kOther));
        entering = true; // still holding the machine: it is queued before anyone else runs
        sections.Enter(section.guest(), kOther, machine);
        got = true;
        CHECK(section.bytes == kHandedToOther);
        CHECK(sections.Leave(section.guest(), kOther));
        machine.lock.unlock();
    });

    // Take the machine once the other thread has given it up inside Enter.
    for (;;) {
        machine.lock.lock();
        if (entering) {
            break;
        }
        machine.lock.unlock();
        std::this_thread::yield();
    }
    CHECK(machine.gaveUp.load() == 1);
    CHECK_FALSE(got.load());
    CHECK(section.bytes == kHeldByMainOtherWaiting);
    CHECK(sections.WaitObjects() == 1u);
    CHECK(sections.Leave(section.guest(), kMain)); // hands the section on
    CHECK(section.bytes == kHandedToOther);
    machine.lock.unlock();

    other.join();
    CHECK(got.load());
    CHECK(section.bytes == kFreeBytes);
}

TEST_CASE("a thread entering after a hand-off waits for the thread it was handed to") {
    CriticalSections sections;
    TestMachine machine;
    Section section;
    sections.Initialize(section.guest());
    machine.lock.lock();
    sections.Enter(section.guest(), kMain, machine);
    machine.lock.unlock();

    std::atomic<int> queued{0};
    std::atomic<int> order{0};
    std::atomic<int> otherAt{0}, thirdAt{0};
    auto waiter = [&](std::uint32_t id, std::atomic<int> &at) {
        machine.lock.lock();
        queued++;
        sections.Enter(section.guest(), id, machine);
        at = ++order;
        CHECK(sections.Leave(section.guest(), id));
        machine.lock.unlock();
    };
    std::thread other(waiter, kOther, std::ref(otherAt));
    for (;;) { // the first waiter is queued before the second starts
        machine.lock.lock();
        if (queued == 1) {
            break;
        }
        machine.lock.unlock();
        std::this_thread::yield();
    }
    machine.lock.unlock();
    std::thread third(waiter, kThird, std::ref(thirdAt));
    for (;;) {
        machine.lock.lock();
        if (queued == 2) {
            break;
        }
        machine.lock.unlock();
        std::this_thread::yield();
    }
    CHECK(sections.Leave(section.guest(), kMain));
    machine.lock.unlock();
    other.join();
    third.join();
    CHECK(otherAt.load() == 1); // in the order they queued
    CHECK(thirdAt.load() == 2);
    CHECK(section.bytes == kFreeBytes);
}

TEST_CASE("DeleteCriticalSection on an unowned section drops its host object and frees it") {
    CriticalSections sections;
    TestMachine machine;
    Section section;
    sections.Initialize(section.guest());
    // Contend once so that the section has a host object.
    machine.lock.lock();
    sections.Enter(section.guest(), kMain, machine);
    machine.lock.unlock();
    std::atomic<bool> entering{false};
    std::thread other([&] {
        machine.lock.lock();
        entering = true;
        sections.Enter(section.guest(), kOther, machine);
        CHECK(sections.Leave(section.guest(), kOther));
        machine.lock.unlock();
    });
    for (;;) {
        machine.lock.lock();
        if (entering) {
            break;
        }
        machine.lock.unlock();
        std::this_thread::yield();
    }
    CHECK(sections.Leave(section.guest(), kMain));
    machine.lock.unlock();
    other.join();
    REQUIRE(sections.WaitObjects() == 1u);

    sections.Delete(section.guest());
    CHECK(sections.WaitObjects() == 0u);
    CHECK(section.bytes == kFreeBytes);
    sections.Delete(section.guest()); // twice is harmless
    CHECK(sections.WaitObjects() == 0u);
    NoWaitMachine single;
    sections.Enter(section.guest(), kMain, single); // a later Enter uses it again
    CHECK(sections.Leave(section.guest(), kMain));
    CHECK(section.bytes == kFreeBytes);
}

TEST_CASE("DeleteCriticalSection while a thread waits hands it the section instead of stranding it") {
    CriticalSections sections;
    TestMachine machine;
    Section section;
    sections.Initialize(section.guest());
    machine.lock.lock();
    sections.Enter(section.guest(), kMain, machine);
    machine.lock.unlock();

    std::atomic<bool> entering{false};
    std::atomic<bool> got{false};
    std::thread other([&] {
        machine.lock.lock();
        entering = true;
        sections.Enter(section.guest(), kOther, machine);
        got = true;
        CHECK(sections.Leave(section.guest(), kOther));
        machine.lock.unlock();
    });
    for (;;) {
        machine.lock.lock();
        if (entering) {
            break;
        }
        machine.lock.unlock();
        std::this_thread::yield();
    }
    CHECK(section.bytes == kHeldByMainOtherWaiting);
    sections.Delete(section.guest()); // the owner deletes it with kOther queued
    CHECK(section.bytes == kHandedToOther);
    CHECK(sections.WaitObjects() == 0u);
    machine.lock.unlock();

    other.join(); // without the hand-over this join never returns
    CHECK(got.load());
    CHECK(section.bytes == kFreeBytes);
}

TEST_CASE("DeleteCriticalSection with two threads waiting hands it to each in turn") {
    CriticalSections sections;
    TestMachine machine;
    Section section;
    sections.Initialize(section.guest());
    machine.lock.lock();
    sections.Enter(section.guest(), kMain, machine);
    machine.lock.unlock();

    std::atomic<int> queued{0};
    std::atomic<int> order{0};
    std::atomic<int> otherAt{0}, thirdAt{0};
    auto waiter = [&](std::uint32_t id, std::atomic<int> &at) {
        machine.lock.lock();
        queued++;
        sections.Enter(section.guest(), id, machine);
        at = ++order;
        CHECK(sections.Leave(section.guest(), id));
        machine.lock.unlock();
    };
    std::thread other(waiter, kOther, std::ref(otherAt));
    takeMachineWhen(machine, [&] { return queued == 1; });
    machine.lock.unlock();
    std::thread third(waiter, kThird, std::ref(thirdAt));
    takeMachineWhen(machine, [&] { return queued == 2; });

    sections.Delete(section.guest()); // the owner deletes it with two threads queued
    const Bytes handedOtherThirdWaiting = {0, 0, 0, 0, 0x01, 0, 0, 0, 0x01, 0, 0, 0,
                                           0x08, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(section.bytes == handedOtherThirdWaiting);
    CHECK(sections.WaitObjects() == 1u); // kept until the last queued thread is handed it
    machine.lock.unlock();

    other.join(); // neither join returns if a waiter is left asleep
    third.join();
    CHECK(otherAt.load() == 1);
    CHECK(thirdAt.load() == 2);
    CHECK(section.bytes == kFreeBytes);
    CHECK(sections.WaitObjects() == 1u); // as after any contention: kept until the next Delete
}

TEST_CASE("a thread that ends inside a section, entered twice, releases it to its waiter") {
    CriticalSections sections;
    TestMachine machine;
    Section section;
    sections.Initialize(section.guest());
    machine.lock.lock();
    sections.Enter(section.guest(), kMain, machine);
    sections.Enter(section.guest(), kMain, machine);
    machine.lock.unlock();

    std::atomic<bool> entering{false};
    std::atomic<bool> got{false};
    std::thread other([&] {
        machine.lock.lock();
        entering = true;
        sections.Enter(section.guest(), kOther, machine);
        got = true;
        CHECK(section.bytes == kHandedToOther);
        CHECK(sections.Leave(section.guest(), kOther));
        machine.lock.unlock();
    });
    takeMachineWhen(machine, [&] { return entering.load(); });
    const Bytes heldTwiceOtherWaiting = {0, 0, 0, 0, 0x02, 0, 0, 0, 0x02, 0, 0, 0,
                                         0x04, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(section.bytes == heldTwiceOtherWaiting);
    const std::vector<std::uint32_t> released = sections.ThreadEnded(kMain);
    REQUIRE(released.size() == 1u);
    CHECK(released[0] == kAddress);
    CHECK(section.bytes == kHandedToOther);
    machine.lock.unlock();

    other.join(); // without the release this join never returns
    CHECK(got.load());
    CHECK(section.bytes == kFreeBytes);
    CHECK(sections.ThreadEnded(kOther).empty()); // it left before it ended
}

TEST_CASE("a thread that ends inside two free-standing sections frees both, last taken first") {
    CriticalSections sections;
    NoWaitMachine machine;
    Section first, second;
    sections.Initialize(first.guest());
    const GuestSection secondSection{kAddress + 0x40u, second.bytes.data()};
    sections.Initialize(secondSection);
    sections.Enter(first.guest(), kMain, machine);
    sections.Enter(secondSection, kMain, machine);
    const std::vector<std::uint32_t> released = sections.ThreadEnded(kMain);
    REQUIRE(released.size() == 2u);
    CHECK(released[0] == kAddress + 0x40u);
    CHECK(released[1] == kAddress);
    CHECK(first.bytes == kFreeBytes);
    CHECK(second.bytes == kFreeBytes);
    CHECK(sections.WaitObjects() == 0u);
}

TEST_CASE("a thread that left, or whose section was deleted, has nothing to release when it ends") {
    CriticalSections sections;
    NoWaitMachine machine;
    Section left, deleted;
    sections.Initialize(left.guest());
    const GuestSection deletedSection{kAddress + 0x40u, deleted.bytes.data()};
    sections.Initialize(deletedSection);
    sections.Enter(left.guest(), kMain, machine);
    CHECK(sections.Leave(left.guest(), kMain));
    sections.Enter(deletedSection, kMain, machine);
    sections.Delete(deletedSection);
    CHECK(sections.ThreadEnded(kMain).empty());
    CHECK(left.bytes == kFreeBytes);
    CHECK(deleted.bytes == kFreeBytes);
    CHECK(sections.ThreadEnded(kOther).empty()); // a thread that never entered one
}

TEST_CASE("a section entered without InitializeCriticalSection (all zero) is initialised on use") {
    CriticalSections sections;
    NoWaitMachine machine;
    Section section;
    section.bytes.fill(0);
    sections.Enter(section.guest(), kMain, machine);
    const Bytes heldOnce = {0, 0, 0, 0, 0x00, 0, 0, 0, 0x01, 0, 0, 0,
                            0x04, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(section.bytes == heldOnce);
    CHECK(sections.Leave(section.guest(), kMain));
    CHECK(section.bytes == kFreeBytes);
}

TEST_CASE("initialising a section again drops its host object") {
    CriticalSections sections;
    TestMachine machine;
    Section section;
    sections.Initialize(section.guest());
    machine.lock.lock();
    sections.Enter(section.guest(), kMain, machine);
    machine.lock.unlock();
    std::atomic<bool> entering{false};
    std::thread other([&] {
        machine.lock.lock();
        entering = true;
        sections.Enter(section.guest(), kOther, machine);
        CHECK(sections.Leave(section.guest(), kOther));
        machine.lock.unlock();
    });
    for (;;) {
        machine.lock.lock();
        if (entering) {
            break;
        }
        machine.lock.unlock();
        std::this_thread::yield();
    }
    CHECK(sections.Leave(section.guest(), kMain));
    machine.lock.unlock();
    other.join();
    REQUIRE(sections.WaitObjects() == 1u);
    sections.Initialize(section.guest());
    CHECK(sections.WaitObjects() == 0u);
    CHECK(section.bytes == kFreeBytes);
}

} // namespace
} // namespace sfcop::hle::kernel32rt
