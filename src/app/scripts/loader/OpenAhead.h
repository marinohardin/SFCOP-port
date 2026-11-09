// Work done ahead on a host thread, for keys the caller will ask for in about this order: the
// mission modules the game loads one after another at boot, each opened by the platform's loader
// while the game is still busy elsewhere (the loading screen, the module before it).
//
// One thread works through the keys in the order given, once each. Take(key) hands its result to
// the caller: at once when it is ready, after waiting when the thread is working on it, and by
// doing the work on the caller's thread when the thread has not reached it, so a caller never
// waits for keys in front of its own. A key is worked once: Take() of a key that is not in the
// list, or was taken already, answers nothing and the caller does the work itself. Results never
// taken are handed to `discard` when the object is destroyed, after the thread has stopped.
//
// Work that throws on the thread throws again from Take() of its key, on the caller's thread, as
// it would have had the caller done the work: an exception never ends the thread or the program.
//
// The work must be safe on another thread: it touches nothing the caller's thread is using.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace sfcop::scripts {

template <typename Result> class OpenAhead {
public:
    using Work = std::function<Result(const std::string &key)>;
    using Discard = std::function<void(Result &)>;

    OpenAhead(std::vector<std::string> keys, Work work, Discard discard)
        : order_(std::move(keys)), work_(std::move(work)), discard_(std::move(discard)) {
        for (const std::string &key : order_) {
            entries_.emplace(key, Entry{});
        }
        thread_ = std::thread([this] { Run(); });
    }

    ~OpenAhead() {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            stop_ = true;
        }
        thread_.join();
        for (auto &entry : entries_) {
            if (entry.second.state == State::kReady && entry.second.result) {
                discard_(*entry.second.result);
            }
        }
    }

    OpenAhead(const OpenAhead &) = delete;
    OpenAhead &operator=(const OpenAhead &) = delete;

    std::optional<Result> Take(const std::string &key) {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto found = entries_.find(key);
        if (found == entries_.end() || found->second.state == State::kTaken) {
            return std::nullopt;
        }
        Entry &entry = found->second;
        if (entry.state == State::kWaiting) {
            entry.state = State::kTaken; // the thread passes it by
            lock.unlock();
            return work_(key);
        }
        done_.wait(lock, [&entry] { return entry.state == State::kReady; });
        entry.state = State::kTaken;
        if (entry.failure) {
            std::exception_ptr failure = entry.failure;
            entry.failure = nullptr;
            std::rethrow_exception(failure);
        }
        std::optional<Result> result = std::move(entry.result);
        entry.result.reset();
        return result;
    }

    // Until the thread has been through every key (tests and measurements).
    void WaitUntilDone() {
        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [this] { return finished_; });
    }

    // How many keys the thread has worked itself (tests and measurements).
    std::size_t WorkedAhead() {
        std::lock_guard<std::mutex> guard(mutex_);
        return workedAhead_;
    }

private:
    enum class State { kWaiting, kWorking, kReady, kTaken };
    struct Entry {
        State state = State::kWaiting;
        std::optional<Result> result;
        std::exception_ptr failure; // what the work threw on the thread, for Take() to throw
    };

    void Run() {
        for (const std::string &key : order_) {
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (stop_) {
                    break;
                }
                Entry &entry = entries_.at(key);
                if (entry.state != State::kWaiting) {
                    continue; // a duplicate key, or one the caller took first
                }
                entry.state = State::kWorking;
            }
            std::optional<Result> result;
            std::exception_ptr failure;
            try {
                result.emplace(work_(key));
            } catch (...) {
                failure = std::current_exception();
            }
            {
                std::lock_guard<std::mutex> guard(mutex_);
                Entry &entry = entries_.at(key);
                entry.result = std::move(result);
                entry.failure = failure;
                entry.state = State::kReady;
                ++workedAhead_;
            }
            done_.notify_all();
        }
        {
            std::lock_guard<std::mutex> guard(mutex_);
            finished_ = true;
        }
        done_.notify_all();
    }

    const std::vector<std::string> order_;
    const Work work_;
    const Discard discard_;
    std::mutex mutex_;
    std::condition_variable done_;
    std::map<std::string, Entry> entries_;
    bool stop_ = false;
    bool finished_ = false;
    std::size_t workedAhead_ = 0;
    std::thread thread_; // last: started once everything above exists
};

} // namespace sfcop::scripts
