#include "hle/registry/RegistryBridge.h"
#include "hle/registry/RegistryBridgeTesting.h"

#include "hle/registry/Registry.h"
#include "platform/Store/Store.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>

namespace {

using sfcop::hle::registry::Registry;
namespace win32 = sfcop::hle::registry::win32;

struct Attached {
    std::unique_ptr<sfcop::platform::Store> store;
    std::unique_ptr<Registry> registry;
};

std::mutex &lock() {
    static std::mutex mutex;
    return mutex;
}

Attached &attached() {
    static Attached state;
    return state;
}

// The Win32 last error of the calling host thread (the lifted game runs its own threads as host
// threads): the event-log calls leave theirs here before they release the lock, and the shim
// reads it back on the same thread (sfcop_registry_take_error).
std::optional<std::uint32_t> &threadError() {
    thread_local std::optional<std::uint32_t> error;
    return error;
}

// Moves the registry's last error to the calling thread's when the call that set it ends, still
// under the lock.
struct HandOver {
    Registry *registry;
    ~HandOver() {
        std::optional<std::uint32_t> error = registry->TakeLastError();
        if (error) {
            threadError() = error;
        }
    }
};

// An exception must not unwind into the shim. Each entry logs its first failure by name (never
// the exception's text, which can carry a path) and answers a Win32 failure:
// ERROR_NOT_ENOUGH_MEMORY for an allocation, ERROR_GEN_FAILURE otherwise.
std::uint32_t noteException(const char *entry, std::uint32_t code) {
    static std::set<std::string> logged;
    threadError() = code;
    if (logged.insert(entry).second) {
        std::fprintf(stderr, "[hle-registry] %s: an exception was caught; answering Win32 error %u\n",
                     entry, static_cast<unsigned>(code));
    }
    return code;
}

// Runs `body` on the attached registry, under the lock. `failure` is the answer when nothing is
// attached; when the body throws, it is `failure` too, or the Win32 error itself where the call
// answers an error code (`codeIsResult`).
template <typename R, typename Body>
R guardedImpl(const char *entry, R failure, bool codeIsResult, Body body) {
    std::lock_guard<std::mutex> guard(lock());
    Registry *r = attached().registry.get();
    if (r == nullptr) {
        return failure;
    }
    std::uint32_t code = win32::kErrorGenFailure;
    try {
        HandOver handOver{r};
        return body(r);
    } catch (const std::bad_alloc &) {
        code = win32::kErrorNotEnoughMemory;
    } catch (...) {
        code = win32::kErrorGenFailure;
    }
    noteException(entry, code);
    return codeIsResult ? static_cast<R>(code) : failure;
}
template <typename R, typename Body> R guarded(const char *entry, R failure, Body body) {
    return guardedImpl(entry, failure, false, body);
}
template <typename R, typename Body> R guardedCode(const char *entry, R failure, Body body) {
    return guardedImpl(entry, failure, true, body);
}

} // namespace

extern "C" {

void sfcop_hle_registry_attach(const char *userDir) {
    std::lock_guard<std::mutex> guard(lock());
    Attached &state = attached();
    state.registry.reset();
    state.store.reset();
    try {
        std::string user = userDir != nullptr ? userDir : "";
        if (!user.empty()) {
            std::error_code ignored;
            std::filesystem::create_directories(user, ignored);
        }
        // The registry needs no install: its Store has the per-user root only.
        state.store = sfcop::platform::makeStore(std::string(), user);
        state.registry = std::make_unique<Registry>(*state.store);
    } catch (...) {
        state.registry.reset();
        state.store.reset();
        noteException("sfcop_hle_registry_attach", win32::kErrorGenFailure);
    }
}

void sfcop_hle_registry_detach(void) {
    std::lock_guard<std::mutex> guard(lock());
    attached().registry.reset();
    attached().store.reset();
}

int sfcop_hle_registry_attached(void) {
    std::lock_guard<std::mutex> guard(lock());
    return attached().registry != nullptr ? 1 : 0;
}

int sfcop_registry_take_error(uint32_t *code) {
    // The calling thread's own, set by its last call; no lock, no other thread's error.
    std::optional<std::uint32_t> error = threadError();
    threadError().reset();
    if (!error) {
        return 0;
    }
    *code = *error;
    return 1;
}

uint32_t sfcop_registry_open_key(uint32_t parent, const char *subKey, uint32_t *result) {
    return guardedCode("sfcop_registry_open_key", uint32_t(win32::kErrorRegistryIoFailed), [&](Registry *r) -> uint32_t {
        return r->OpenKey(parent, subKey, result);
    });
}

uint32_t sfcop_registry_create_key(uint32_t parent, const char *subKey, uint32_t *result,
                                   uint32_t *disposition) {
    return guardedCode("sfcop_registry_create_key", uint32_t(win32::kErrorRegistryIoFailed), [&](Registry *r) -> uint32_t {
        return r->CreateKey(parent, subKey, result, disposition);
    });
}

uint32_t sfcop_registry_close_key(uint32_t key) {
    return guardedCode("sfcop_registry_close_key", uint32_t(win32::kErrorInvalidHandle), [&](Registry *r) -> uint32_t {
        return r->CloseKey(key);
    });
}

uint32_t sfcop_registry_delete_key(uint32_t parent, const char *subKey) {
    return guardedCode("sfcop_registry_delete_key", uint32_t(win32::kErrorRegistryIoFailed), [&](Registry *r) -> uint32_t {
        return r->DeleteKey(parent, subKey);
    });
}

uint32_t sfcop_registry_query_value(uint32_t key, const char *name, uint32_t *type, uint8_t *data,
                                    uint32_t *size) {
    return guardedCode("sfcop_registry_query_value", uint32_t(win32::kErrorRegistryIoFailed), [&](Registry *r) -> uint32_t {
        return r->QueryValue(key, name, type, data, size);
    });
}

uint32_t sfcop_registry_set_value(uint32_t key, const char *name, uint32_t type,
                                  const uint8_t *data, uint32_t size) {
    return guardedCode("sfcop_registry_set_value", uint32_t(win32::kErrorRegistryIoFailed), [&](Registry *r) -> uint32_t {
        return r->SetValue(key, name, type, data, size);
    });
}

uint32_t sfcop_registry_register_event_source(const char *server, const char *source) {
    return guarded("sfcop_registry_register_event_source", uint32_t(0), [&](Registry *r) -> uint32_t {
        return r->RegisterEventSource(server, source);
    });
}

int sfcop_registry_deregister_event_source(uint32_t handle) {
    return guarded("sfcop_registry_deregister_event_source", int(0), [&](Registry *r) -> int {
        return r->DeregisterEventSource(handle) ? 1 : 0;
    });
}

int sfcop_registry_report_event(uint32_t handle) {
    return guarded("sfcop_registry_report_event", int(0), [&](Registry *r) -> int {
        return r->ReportEvent(handle) ? 1 : 0;
    });
}

} // extern "C"

namespace sfcop::hle::registry::test {

void attachRegistryStore(std::unique_ptr<platform::Store> store) {
    std::lock_guard<std::mutex> guard(lock());
    Attached &state = attached();
    state.registry.reset();
    state.store = std::move(store);
    state.registry = std::make_unique<Registry>(*state.store);
}

} // namespace sfcop::hle::registry::test
