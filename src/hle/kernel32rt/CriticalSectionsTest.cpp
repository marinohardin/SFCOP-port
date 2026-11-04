#include "hle/kernel32rt/CriticalSections.h"

#include "platform/Threads/ThreadsMock.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace sfcop::hle::kernel32rt {
namespace {

using platform::Error;
using platform::ThreadsMock;
using platform::WaitOutcome;
using platform::WaitResult;

TEST_CASE("a critical section is a mutex found by its guest address") {
    ThreadsMock threads;
    CriticalSections sections(threads);
    sections.Initialize(0x00C81000u);
    CHECK(sections.TryEnter(0x00C81000u));
    sections.Enter(0x00C81000u);
    CHECK(sections.Leave(0x00C81000u));
    CHECK(threads.log.Calls() == std::vector<std::string>{"MakeMutex(false, \"\")", "Wait(1, 0, 0)",
                                                          "Wait(1, 0, \"forever\")",
                                                          "ReleaseMutex(3)"});
}

TEST_CASE("TryEnter is false while another thread holds the section") {
    ThreadsMock threads;
    threads.waitResult = WaitResult{WaitOutcome::kTimedOut, 0};
    CriticalSections sections(threads);
    sections.Initialize(0x00C81000u);
    CHECK_FALSE(sections.TryEnter(0x00C81000u));
}

TEST_CASE("a section entered without InitializeCriticalSection gets its mutex once") {
    ThreadsMock threads;
    CriticalSections sections(threads);
    CHECK(sections.TryEnter(0x00C82000u));
    CHECK(sections.TryEnter(0x00C82000u));
    CHECK(threads.log.Count("MakeMutex") == 1);
}

TEST_CASE("LeaveCriticalSection by a thread that does not hold it reports false") {
    ThreadsMock threads;
    threads.releaseMutexResult = Error::kNotOwner;
    CriticalSections sections(threads);
    sections.Initialize(0x00C81000u);
    CHECK_FALSE(sections.Leave(0x00C81000u));
}

TEST_CASE("DeleteCriticalSection closes the mutex and a later use makes a new one") {
    ThreadsMock threads;
    CriticalSections sections(threads);
    sections.Initialize(0x00C81000u);
    sections.Delete(0x00C81000u);
    sections.Delete(0x00C81000u);
    CHECK(threads.log.Count("Close") == 1);
    sections.Enter(0x00C81000u);
    CHECK(threads.log.Count("MakeMutex") == 2);
}

TEST_CASE("initialising a section again closes its earlier mutex") {
    ThreadsMock threads;
    CriticalSections sections(threads);
    sections.Initialize(0x00C81000u);
    sections.Initialize(0x00C81000u);
    CHECK(threads.log.Count("Close") == 1);
}

} // namespace
} // namespace sfcop::hle::kernel32rt
