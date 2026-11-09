// OpenAhead: each key worked once, ahead on its thread or by the caller who got there first; a key
// in progress waited for; results never taken discarded; a throw on the thread rethrown to the
// caller. Work functions record which thread ran them (compared, never printed: some standard
// libraries cannot stream a thread id without <ostream>), and a gate holds one back so the order
// of events is fixed, not timed.
#include "app/scripts/loader/OpenAhead.h"

#include <doctest/doctest.h>

#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace sfcop::scripts;

namespace {

// What the work did: each key it worked and on which thread, in order.
struct Record {
    std::mutex mutex;
    std::vector<std::string> keys;
    std::vector<std::thread::id> threads;
    void add(const std::string &key) {
        std::lock_guard<std::mutex> guard(mutex);
        keys.push_back(key);
        threads.push_back(std::this_thread::get_id());
    }
    std::size_t count(const std::string &key) {
        std::lock_guard<std::mutex> guard(mutex);
        std::size_t n = 0;
        for (const std::string &k : keys) {
            n += k == key ? 1 : 0;
        }
        return n;
    }
};

// Holds the work on one key until opened; says when the work has reached it.
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool reached = false;
    bool open = false;
    void pass() {
        std::unique_lock<std::mutex> lock(mutex);
        reached = true;
        changed.notify_all();
        changed.wait(lock, [this] { return open; });
    }
    void waitReached() {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [this] { return reached; });
    }
    void release() {
        std::lock_guard<std::mutex> guard(mutex);
        open = true;
        changed.notify_all();
    }
};

} // namespace

TEST_SUITE("open ahead") {

TEST_CASE("every key is worked once on the thread, and taken in any order") {
    Record record;
    std::vector<std::string> discarded;
    {
        OpenAhead<std::string> ahead(
            {"a", "b", "c"},
            [&record](const std::string &key) {
                record.add(key);
                return key + "!";
            },
            [&discarded](std::string &result) { discarded.push_back(result); });
        ahead.WaitUntilDone();
        CHECK(ahead.WorkedAhead() == 3);
        CHECK(ahead.Take("c") == std::optional<std::string>("c!"));
        CHECK(ahead.Take("a") == std::optional<std::string>("a!"));
        CHECK(ahead.Take("b") == std::optional<std::string>("b!"));
        CHECK_FALSE(ahead.Take("a").has_value()); // taken already: the caller works it itself
        CHECK_FALSE(ahead.Take("z").has_value()); // never in the list
    }
    CHECK(record.keys == std::vector<std::string>{"a", "b", "c"});
    for (const std::thread::id &thread : record.threads) {
        const bool onTheThread = thread != std::this_thread::get_id();
        CHECK(onTheThread);
    }
    CHECK(discarded.empty()); // everything was taken
}

TEST_CASE("a key the thread has not reached is worked by the caller, and the thread passes it by") {
    Record record;
    Gate gate;
    OpenAhead<std::string> ahead(
        {"a", "b", "c"},
        [&record, &gate](const std::string &key) {
            if (key == "a") {
                gate.pass();
            }
            record.add(key);
            return key + "!";
        },
        [](std::string &) {});
    gate.waitReached(); // the thread is held on "a"; "c" is still waiting
    CHECK(ahead.Take("c") == std::optional<std::string>("c!"));
    gate.release();
    ahead.WaitUntilDone();
    CHECK(record.count("c") == 1);
    CHECK(ahead.WorkedAhead() == 2); // "a" and "b"
    REQUIRE(record.keys.size() == 3);
    CHECK(record.keys[0] == "c");
    const bool firstOnCaller = record.threads[0] == std::this_thread::get_id();
    CHECK(firstOnCaller);
    CHECK(record.keys[1] == "a");
    CHECK(record.keys[2] == "b");
    const bool secondOnTheThread = record.threads[1] != std::this_thread::get_id();
    CHECK(secondOnTheThread);
}

TEST_CASE("a key the thread is working on is waited for, never worked twice") {
    Record record;
    Gate gate;
    OpenAhead<std::string> ahead(
        {"a"},
        [&record, &gate](const std::string &key) {
            gate.pass();
            record.add(key);
            return key + "!";
        },
        [](std::string &) {});
    gate.waitReached();
    std::optional<std::string> taken;
    std::thread caller([&ahead, &taken] { taken = ahead.Take("a"); });
    gate.release();
    caller.join();
    CHECK(taken == std::optional<std::string>("a!"));
    CHECK(record.count("a") == 1);
}

TEST_CASE("results never taken are discarded when it is destroyed, after its thread stopped") {
    std::vector<std::string> discarded;
    {
        OpenAhead<std::string> ahead(
            {"a", "b", "c"}, [](const std::string &key) { return key + "!"; },
            [&discarded](std::string &result) { discarded.push_back(result); });
        ahead.WaitUntilDone();
        CHECK(ahead.Take("b") == std::optional<std::string>("b!"));
    }
    CHECK(discarded == std::vector<std::string>{"a!", "c!"});
}

TEST_CASE("work that throws on the thread throws from Take() of its key, and the others go on") {
    std::vector<std::string> discarded;
    {
        OpenAhead<std::string> ahead(
            {"a", "b", "c"},
            [](const std::string &key) -> std::string {
                if (key == "b") {
                    throw std::runtime_error("b cannot be worked");
                }
                return key + "!";
            },
            [&discarded](std::string &result) { discarded.push_back(result); });
        ahead.WaitUntilDone();
        CHECK(ahead.WorkedAhead() == 3);
        CHECK_THROWS_WITH_AS(ahead.Take("b"), "b cannot be worked", std::runtime_error);
        CHECK_FALSE(ahead.Take("b").has_value()); // taken already, as after a result
        CHECK(ahead.Take("c") == std::optional<std::string>("c!"));
    }
    CHECK(discarded == std::vector<std::string>{"a!"}); // nothing to discard for "b"
}

TEST_CASE("an empty list works nothing and finishes") {
    OpenAhead<int> ahead({}, [](const std::string &) { return 1; }, [](int &) {});
    ahead.WaitUntilDone();
    CHECK(ahead.WorkedAhead() == 0);
    CHECK_FALSE(ahead.Take("a").has_value());
}

} // TEST_SUITE
