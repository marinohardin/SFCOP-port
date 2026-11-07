// Threads' host backend (makeThreads): the objects and waits on the C++ standard library, one
// lock and one condition variable for the whole object table, and the start of a thread with a
// chosen stack size in one file per platform (ThreadsHost_<platform>.cpp).
//
// One lock makes WaitForMultipleObjects' "all at once or none" simple and correct: a waiter tests
// and takes every handle under the same lock. Each object counts the threads blocked on it, and a
// change that can satisfy a wait (an event set, a mutex or semaphore released) wakes the waiters
// only when that object has one; a thread ended or resumed always wakes them. A take never
// satisfies another wait, so it wakes no one. The game enters and leaves its critical sections
// (mutexes here) tens of thousands of times a second: as on Windows, those that nobody else wants
// cost no wake-up of the threads blocked on something else.
//
// Thread ids: every thread that calls in gets a number of its own from one counter, starting at 1,
// the first time it asks; a thread this component starts gets its number before its body runs, so
// StartThread's caller and the body agree on it. Priorities are accepted and not applied: macOS
// and Linux let a user process lower a thread's priority only, and the game sets priorities as a
// hint.

#include "platform/Threads/Threads.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sfcop::platform {

namespace threadshost {
// The platform file's half: run `body` on a new detached thread whose stack is at least
// `stackBytes` (0: the platform default). False if the system refuses the thread.
bool startHostThread(std::function<void()> body, std::size_t stackBytes);

// How many times the backend has woken its waiters since the process began (ThreadsHostTest).
std::atomic<std::uint64_t> g_wakeUps{0};
std::uint64_t wakeUps() { return g_wakeUps.load(); }
} // namespace threadshost

namespace {

constexpr std::size_t kMaxWaitHandles = 64;
constexpr std::uint32_t kTlsSlots = 128;

std::atomic<std::uint32_t> g_nextThreadId{1};
thread_local std::uint32_t t_threadId = 0;

std::uint32_t currentId() {
    if (t_threadId == 0) {
        t_threadId = g_nextThreadId.fetch_add(1);
    }
    return t_threadId;
}

enum class Kind { kThread, kEvent, kMutex, kSemaphore };

struct Object {
    explicit Object(Kind k) : kind(k) {}
    Kind kind;
    std::string name;
    // Thread
    std::uint32_t threadId = 0;
    bool suspended = false;
    bool ended = false;
    std::uint32_t exitCode = 0;
    // Event
    bool manualReset = false;
    bool signaled = false;
    // Mutex: owned while owner != 0; `abandoned` until a waiter has been told.
    std::uint32_t owner = 0;
    std::uint32_t recursion = 0;
    bool abandoned = false;
    // Semaphore
    std::int32_t count = 0;
    std::int32_t maximum = 0;
    // Threads blocked in a wait that includes this object.
    std::uint32_t waiters = 0;
};

using ObjectPtr = std::shared_ptr<Object>;

// What the instance and the threads it started share, so that a thread may outlive the instance.
struct State {
    std::mutex lock;
    std::condition_variable changed;
    std::uint32_t nextHandle = 1;
    std::unordered_map<std::uint32_t, ObjectPtr> handles;
    std::map<std::string, std::weak_ptr<Object>, std::less<>> names;
    // Every live mutex, to abandon those a thread still owns when it ends.
    std::vector<std::weak_ptr<Object>> mutexes;
};

// Under the lock: wakes every waiter, to look again at what it waits for.
void wakeAll(State &state) {
    threadshost::g_wakeUps.fetch_add(1);
    state.changed.notify_all();
}

// Thread-local storage: a slot's generation changes when it is freed, which forgets every
// thread's value for it at once. Values are kept per instance, keyed by its serial number.
std::atomic<std::uint64_t> g_nextInstance{1};
struct TlsValue {
    std::uint64_t generation = 0;
    std::uint64_t value = 0;
};
thread_local std::unordered_map<std::uint64_t, std::vector<TlsValue>> t_tls;

class ThreadsHost final : public Threads {
public:
    ThreadsHost() : state_(std::make_shared<State>()), instance_(g_nextInstance.fetch_add(1)) {
        slotGeneration_.assign(kTlsSlots, 0);
        slotUsed_.assign(kTlsSlots, false);
    }

    Result<Waitable> StartThread(std::function<std::uint32_t()> body,
                                 const ThreadOptions &options) override {
        if (!body) {
            return Error::kInvalidArgument;
        }
        auto thread = std::make_shared<Object>(Kind::kThread);
        thread->name = options.name;
        thread->suspended = options.startSuspended;
        thread->threadId = g_nextThreadId.fetch_add(1);
        Waitable handle;
        {
            std::lock_guard<std::mutex> hold(state_->lock);
            handle = AddHandle(thread);
        }
        std::shared_ptr<State> state = state_;
        auto run = [state, thread, body = std::move(body)]() {
            t_threadId = thread->threadId;
            {
                std::unique_lock<std::mutex> hold(state->lock);
                state->changed.wait(hold, [&] { return !thread->suspended; });
            }
            const std::uint32_t code = body();
            std::lock_guard<std::mutex> hold(state->lock);
            thread->exitCode = code;
            thread->ended = true;
            AbandonMutexesOf(*state, thread->threadId);
            wakeAll(*state);
        };
        if (!threadshost::startHostThread(std::move(run), options.stackBytes)) {
            std::lock_guard<std::mutex> hold(state_->lock);
            state_->handles.erase(handle.value);
            return Error::kOutOfResources;
        }
        return handle;
    }

    Result<std::uint32_t> GetExitCode(Waitable thread) override {
        std::lock_guard<std::mutex> hold(state_->lock);
        ObjectPtr object = Find(thread, Kind::kThread);
        if (!object) {
            return Error::kInvalidHandle;
        }
        if (!object->ended) {
            return Error::kWrongState;
        }
        return object->exitCode;
    }

    Status ResumeThread(Waitable thread) override {
        std::lock_guard<std::mutex> hold(state_->lock);
        ObjectPtr object = Find(thread, Kind::kThread);
        if (!object) {
            return Error::kInvalidHandle;
        }
        object->suspended = false;
        wakeAll(*state_);
        return {};
    }

    Status SetThreadPriority(Waitable thread, ThreadPriority) override {
        std::lock_guard<std::mutex> hold(state_->lock);
        return Find(thread, Kind::kThread) ? Status{} : Status{Error::kInvalidHandle};
    }

    Result<std::uint32_t> CurrentThreadId() override { return currentId(); }

    Result<Created> MakeEvent(bool manualReset, bool initiallySignaled,
                              std::string_view name) override {
        return MakeNamed(Kind::kEvent, name, [&](Object &event) {
            event.manualReset = manualReset;
            event.signaled = initiallySignaled;
        });
    }

    Status SetEvent(Waitable event) override {
        return Change(event, Kind::kEvent, [](Object &object) {
            object.signaled = true;
            return Status{};
        });
    }

    Status ResetEvent(Waitable event) override {
        return Change(event, Kind::kEvent, [](Object &object) {
            object.signaled = false;
            return Status{};
        });
    }

    Result<Created> MakeMutex(bool initiallyOwned, std::string_view name) override {
        const std::uint32_t me = currentId();
        return MakeNamed(Kind::kMutex, name, [&](Object &mutex) {
            if (initiallyOwned) {
                mutex.owner = me;
                mutex.recursion = 1;
            }
        });
    }

    Status ReleaseMutex(Waitable mutex) override {
        const std::uint32_t me = currentId();
        return Change(mutex, Kind::kMutex, [me](Object &object) {
            if (object.owner != me) {
                return Status{Error::kNotOwner};
            }
            if (--object.recursion == 0) {
                object.owner = 0;
            }
            return Status{};
        });
    }

    Result<Created> MakeSemaphore(std::int32_t initialCount, std::int32_t maximumCount,
                                  std::string_view name) override {
        if (maximumCount <= 0 || initialCount < 0 || initialCount > maximumCount) {
            return Error::kInvalidArgument;
        }
        return MakeNamed(Kind::kSemaphore, name, [&](Object &semaphore) {
            semaphore.count = initialCount;
            semaphore.maximum = maximumCount;
        });
    }

    Result<std::int32_t> ReleaseSemaphore(Waitable semaphore, std::int32_t count) override {
        std::lock_guard<std::mutex> hold(state_->lock);
        ObjectPtr object = Find(semaphore, Kind::kSemaphore);
        if (!object) {
            return Error::kInvalidHandle;
        }
        if (count <= 0 || count > object->maximum - object->count) {
            return Error::kInvalidArgument;
        }
        const std::int32_t before = object->count;
        object->count += count;
        if (object->waiters != 0) {
            wakeAll(*state_);
        }
        return before;
    }

    Result<Waitable> Open(std::string_view name) override {
        if (name.empty()) {
            return Error::kInvalidArgument;
        }
        std::lock_guard<std::mutex> hold(state_->lock);
        auto found = state_->names.find(name);
        ObjectPtr object = found == state_->names.end() ? nullptr : found->second.lock();
        if (!object) {
            return Error::kNotFound;
        }
        return AddHandle(object);
    }

    Result<WaitResult> Wait(const Waitable *handles, std::size_t count, WaitMode mode,
                            std::optional<std::uint32_t> timeoutMs) override {
        if (count == 0 || count > kMaxWaitHandles || handles == nullptr) {
            return Error::kInvalidArgument;
        }
        const std::uint32_t me = currentId();
        // Read the clock only for a wait that can block for a time: a zero timeout (every
        // EnterCriticalSection's first try) never needs it.
        const bool timed = timeoutMs.has_value() && *timeoutMs != 0;
        const auto deadline = timed ? std::chrono::steady_clock::now() +
                                          std::chrono::milliseconds(*timeoutMs)
                                    : std::chrono::steady_clock::time_point{};
        std::unique_lock<std::mutex> hold(state_->lock);
        std::vector<ObjectPtr> objects(count);
        for (std::size_t i = 0; i < count; i++) {
            auto found = state_->handles.find(handles[i].value);
            if (found == state_->handles.end()) {
                return Error::kInvalidHandle;
            }
            objects[i] = found->second;
        }
        for (;;) {
            if (std::optional<WaitResult> done = TryTake(objects, mode, me)) {
                return *done;
            }
            if (timeoutMs.has_value() && *timeoutMs == 0) {
                return WaitResult{WaitOutcome::kTimedOut, 0};
            }
            for (const ObjectPtr &object : objects) {
                object->waiters++;
            }
            bool timedOut = false;
            if (timed) {
                timedOut = state_->changed.wait_until(hold, deadline) == std::cv_status::timeout;
            } else {
                state_->changed.wait(hold);
            }
            for (const ObjectPtr &object : objects) {
                object->waiters--;
            }
            if (timedOut) {
                if (std::optional<WaitResult> late = TryTake(objects, mode, me)) {
                    return *late;
                }
                return WaitResult{WaitOutcome::kTimedOut, 0};
            }
        }
    }

    Status Close(Waitable handle) override {
        std::lock_guard<std::mutex> hold(state_->lock);
        return state_->handles.erase(handle.value) == 1 ? Status{} : Status{Error::kInvalidHandle};
    }

    Result<std::uint32_t> AllocateTlsSlot() override {
        std::lock_guard<std::mutex> hold(tlsLock_);
        for (std::uint32_t slot = 0; slot < kTlsSlots; slot++) {
            if (!slotUsed_[slot]) {
                slotUsed_[slot] = true;
                return slot;
            }
        }
        return Error::kOutOfResources;
    }

    Status FreeTlsSlot(std::uint32_t slot) override {
        std::lock_guard<std::mutex> hold(tlsLock_);
        if (slot >= kTlsSlots || !slotUsed_[slot]) {
            return Error::kInvalidArgument;
        }
        slotUsed_[slot] = false;
        slotGeneration_[slot]++;
        return {};
    }

    Status SetTlsValue(std::uint32_t slot, std::uint64_t value) override {
        const std::optional<std::uint64_t> generation = SlotGeneration(slot);
        if (!generation) {
            return Error::kInvalidArgument;
        }
        TlsValue &cell = Cell(slot);
        cell.generation = *generation;
        cell.value = value;
        return {};
    }

    Result<std::uint64_t> GetTlsValue(std::uint32_t slot) override {
        const std::optional<std::uint64_t> generation = SlotGeneration(slot);
        if (!generation) {
            return Error::kInvalidArgument;
        }
        const TlsValue &cell = Cell(slot);
        return cell.generation == *generation ? cell.value : 0;
    }

private:
    // Under the lock.
    Waitable AddHandle(ObjectPtr object) {
        std::uint32_t value = state_->nextHandle++;
        if (state_->nextHandle == 0) {
            state_->nextHandle = 1;
        }
        state_->handles.emplace(value, std::move(object));
        return Waitable{value};
    }

    // Under the lock: the object behind `handle` if it is live and of `kind`.
    ObjectPtr Find(Waitable handle, Kind kind) const {
        auto found = state_->handles.find(handle.value);
        if (found == state_->handles.end() || found->second->kind != kind) {
            return nullptr;
        }
        return found->second;
    }

    template <typename Init>
    Result<Created> MakeNamed(Kind kind, std::string_view name, Init init) {
        std::lock_guard<std::mutex> hold(state_->lock);
        if (!name.empty()) {
            auto found = state_->names.find(name);
            if (found != state_->names.end()) {
                if (ObjectPtr existing = found->second.lock()) {
                    if (existing->kind != kind) {
                        return Error::kInvalidArgument;
                    }
                    return Created{AddHandle(existing), true};
                }
                state_->names.erase(found);
            }
        }
        auto object = std::make_shared<Object>(kind);
        object->name = std::string(name);
        init(*object);
        if (!name.empty()) {
            state_->names.emplace(std::string(name), object);
        }
        if (kind == Kind::kMutex) {
            state_->mutexes.push_back(object);
        }
        return Created{AddHandle(object), false};
    }

    template <typename Op> Status Change(Waitable handle, Kind kind, Op op) {
        std::lock_guard<std::mutex> hold(state_->lock);
        ObjectPtr object = Find(handle, kind);
        if (!object) {
            return Error::kInvalidHandle;
        }
        Status status = op(*object);
        if (status.Ok() && object->waiters != 0) {
            wakeAll(*state_);
        }
        return status;
    }

    static bool Ready(const Object &object, std::uint32_t me) {
        switch (object.kind) {
        case Kind::kThread:
            return object.ended;
        case Kind::kEvent:
            return object.signaled;
        case Kind::kMutex:
            return object.owner == 0 || object.owner == me;
        case Kind::kSemaphore:
            return object.count > 0;
        }
        return false;
    }

    // Takes what a satisfied wait consumes; true if it was an abandoned mutex.
    static bool Take(Object &object, std::uint32_t me) {
        switch (object.kind) {
        case Kind::kEvent:
            if (!object.manualReset) {
                object.signaled = false;
            }
            return false;
        case Kind::kMutex: {
            const bool abandoned = object.abandoned;
            object.abandoned = false;
            object.owner = me;
            object.recursion++;
            return abandoned;
        }
        case Kind::kSemaphore:
            object.count--;
            return false;
        case Kind::kThread:
            return false;
        }
        return false;
    }

    // Under the lock: the wait's result if it can be satisfied now.
    static std::optional<WaitResult> TryTake(const std::vector<ObjectPtr> &objects, WaitMode mode,
                                             std::uint32_t me) {
        if (mode == WaitMode::kAny) {
            for (std::size_t i = 0; i < objects.size(); i++) {
                if (Ready(*objects[i], me)) {
                    const bool abandoned = Take(*objects[i], me);
                    return WaitResult{abandoned ? WaitOutcome::kAbandoned : WaitOutcome::kSignaled,
                                      i};
                }
            }
            return std::nullopt;
        }
        for (const ObjectPtr &object : objects) {
            if (!Ready(*object, me)) {
                return std::nullopt;
            }
        }
        // A semaphore listed twice needs two counts.
        for (std::size_t i = 0; i < objects.size(); i++) {
            if (objects[i]->kind == Kind::kSemaphore) {
                std::int32_t uses = 0;
                for (const ObjectPtr &other : objects) {
                    uses += other == objects[i] ? 1 : 0;
                }
                if (objects[i]->count < uses) {
                    return std::nullopt;
                }
            }
        }
        bool abandoned = false;
        for (const ObjectPtr &object : objects) {
            abandoned = Take(*object, me) || abandoned;
        }
        return WaitResult{abandoned ? WaitOutcome::kAbandoned : WaitOutcome::kSignaled, 0};
    }

    // Under the lock: a thread has ended; the mutexes it owns are abandoned.
    static void AbandonMutexesOf(State &state, std::uint32_t threadId) {
        auto &list = state.mutexes;
        for (std::size_t i = 0; i < list.size();) {
            ObjectPtr mutex = list[i].lock();
            if (!mutex) {
                list[i] = list.back();
                list.pop_back();
                continue;
            }
            if (mutex->owner == threadId) {
                mutex->owner = 0;
                mutex->recursion = 0;
                mutex->abandoned = true;
            }
            i++;
        }
    }

    std::optional<std::uint64_t> SlotGeneration(std::uint32_t slot) {
        std::lock_guard<std::mutex> hold(tlsLock_);
        if (slot >= kTlsSlots || !slotUsed_[slot]) {
            return std::nullopt;
        }
        return slotGeneration_[slot];
    }

    TlsValue &Cell(std::uint32_t slot) {
        std::vector<TlsValue> &values = t_tls[instance_];
        if (values.size() < kTlsSlots) {
            values.resize(kTlsSlots);
        }
        return values[slot];
    }

    std::shared_ptr<State> state_;
    const std::uint64_t instance_;
    std::mutex tlsLock_;
    std::vector<std::uint64_t> slotGeneration_;
    std::vector<bool> slotUsed_;
};

} // namespace

std::unique_ptr<Threads> makeThreads() { return std::make_unique<ThreadsHost>(); }

} // namespace sfcop::platform
