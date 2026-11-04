// The guest shims of the kernel32rt module (src/hle/README.md, "Implementing a module"): the
// KERNEL32 rows for threads, synchronisation, critical sections, thread-local storage, the heap
// and virtual memory, exceptions, time, the environment, module handles and the C runtime's
// start-up queries, WINMM's timers, and DBGHELP's stack walk. One shim per row of
// src/hle/surface.tsv; each reads its arguments from the guest stack, calls the module and purges
// the row's stack bytes.
//
// The game's kernel objects (threads, events, mutexes, semaphores) are entries of win32hle's
// handle table of this module's type (Runtime.h, kHandleType), each holding an Object;
// CloseHandle, which the kernel module answers and hands on, releases them through the closer
// registered here. A row given a handle of another type, and a module-handle row given a module
// that is not built in, runs win32hle's own row of that name.
//
// A row that blocks (a wait, Sleep, entering an owned critical section, stopping a timer) gives
// the machine up for the length of the wait, so the thread it waits for can run guest code.
#include "hle/kernel32rt/Dispatcher.h"
#include "hle/kernel32rt/HostMemory.h"
#include "hle/kernel32rt/Kernel32rt.h"
#include "hle/kernel32rt/Runtime.h"
#include "hle/kernel32rt/VirtualMemory.h"

extern "C" {
#include "win32hle.h"
}

#include <algorithm>
#include <atomic>
#include <cctype>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace sfcop::hle::kernel32rt {

namespace {

// ---- guest memory and the rows this file hands on ----

void put32(std::uint32_t address, std::uint32_t value) {
    if (address != 0) {
        MEM32(address) = value;
    }
}

void put64(std::uint32_t address, std::uint64_t value) {
    if (address != 0) {
        MEM32(address) = static_cast<std::uint32_t>(value);
        MEM32(address + 4) = static_cast<std::uint32_t>(value >> 32);
    }
}

std::uint64_t get64(std::uint32_t address) {
    return static_cast<std::uint64_t>(MEM32(address)) |
           static_cast<std::uint64_t>(MEM32(address + 4)) << 32;
}

std::string guestString(std::uint32_t address) {
    return address != 0 ? std::string(static_cast<const char *>(hle_ptr(address))) : std::string();
}

// Hands an Answer to the guest: its last error, if it sets one, and its value.
std::uint32_t answer(const Answer &result) {
    if (result.lastError) {
        hle_set_last_error(*result.lastError);
    }
    return result.value;
}

// win32hle's own row of this name, which this table's registration hides.
recomp_func_t win32hleRow(const char *name) {
    for (const win32hle_shim *table : {win32hle_kernel32, win32hle_kernel32_ext,
                                       win32hle_kernel32_crt, win32hle_module, win32hle_winmm}) {
        for (const win32hle_shim *entry = table; entry->name != nullptr; ++entry) {
            if (std::strcmp(entry->name, name) == 0) {
                return entry->fn;
            }
        }
    }
    return nullptr;
}

// Runs win32hle's row, which reads its arguments and purges the stack; without one, fails as
// Windows does for a handle nobody made.
void handOn(const char *name, std::uint32_t argc, std::uint32_t failure) {
    if (recomp_func_t row = win32hleRow(name)) {
        row();
        return;
    }
    hle_set_last_error(kErrorInvalidHandle);
    RET(failure, argc);
}

Runtime &runtime() {
    return Runtime::Instance();
}

// ---- kernel objects ----

constexpr std::uint32_t kCurrentProcess = 0xFFFFFFFFu; // GetCurrentProcess's pseudo-handle
constexpr std::uint32_t kCurrentThread = 0xFFFFFFFEu;  // GetCurrentThread's

Object *objectOf(std::uint32_t handle) {
    return static_cast<Object *>(hle_handle_obj(handle, kHandleType));
}

void closeObject(void *object) {
    auto *owned = static_cast<Object *>(object);
    if (runtime().Attached()) {
        runtime().Objects().Close(*owned);
    }
    delete owned;
}

// A guest handle for a new Object, or 0 with ERROR_NOT_ENOUGH_MEMORY when the table is full.
std::uint32_t handleFor(const Object &made) {
    auto *object = new Object(made);
    const std::uint32_t handle = hle_handle_alloc(kHandleType, object);
    if (handle == 0) {
        closeObject(object);
        hle_set_last_error(kErrorNotEnoughMemory);
    }
    return handle;
}

[[maybe_unused]] const bool kHandlesReady = [] {
    hle_handle_set_closer(kHandleType, closeObject);
    Runtime::Instance().SetHandleLookup([](std::uint32_t handle) -> std::optional<platform::Waitable> {
        const Object *object = objectOf(handle);
        return object != nullptr ? std::optional<platform::Waitable>(object->waitable) : std::nullopt;
    });
    return true;
}();

// Runs `wait` with the machine given up, so that other guest threads run meanwhile.
template <typename Wait> auto withoutMachine(Wait wait) {
    hle_block_begin();
    auto result = wait();
    hle_block_end();
    return result;
}

// ---- threads ----

// Where ExitThread leaves a thread's body for: the jump back to it and the exit code.
struct ThreadExit {
    std::jmp_buf to;
    std::uint32_t code = 0;
};
thread_local ThreadExit *t_exit = nullptr;

void k_CreateThread() { // (attributes, stack, start, parameter, flags, &id)
    const std::uint32_t stack = A32(1), start = A32(2), parameter = A32(3), flags = A32(4);
    const std::uint32_t idOut = A32(5);
    const std::uint32_t id = hle_thread_id_new();
    auto body = [start, parameter, id]() -> std::uint32_t {
        hle_thread_id_adopt(id);
        ThreadExit ending;
        t_exit = &ending;
        if (setjmp(ending.to) == 0) {
            ending.code = hle_call_guest(start, 1, &parameter);
        } else {
            hle_mach_set_depth(0); // ExitThread gave the machine up from inside the guest call
        }
        t_exit = nullptr;
        return ending.code;
    };
    Object thread;
    const Answer started = runtime().Objects().StartThread(std::move(body), (flags & 4u) != 0,
                                                           stack, thread); // CREATE_SUSPENDED
    if (started.value == 0) {
        RET(answer(started), 6);
    }
    const std::uint32_t handle = handleFor(thread);
    put32(idOut, id);
    RET(handle, 6);
}

void k_ExitThread() { // (code)
    if (t_exit == nullptr) {
        // The thread the host started the game on: ending it ends the game.
        hle_exit(static_cast<int>(A32(0)));
    }
    t_exit->code = A32(0);
    hle_block_begin(); // the thread never hands the machine back
    std::longjmp(t_exit->to, 1);
}

void k_ResumeThread() { // (thread)
    Object *thread = objectOf(A32(0));
    if (thread == nullptr) {
        hle_set_last_error(kErrorInvalidHandle);
        RET(0xFFFFFFFFu, 1);
    }
    RET(answer(runtime().Objects().ResumeThread(*thread)), 1);
}

void k_SetThreadPriority() { // (thread, priority)
    if (A32(0) == kCurrentThread) {
        RET(1, 2); // accepted; Threads sets a priority only through a thread's handle
    }
    const Object *thread = objectOf(A32(0));
    if (thread == nullptr) {
        hle_set_last_error(kErrorInvalidHandle);
        RET(0, 2);
    }
    RET(answer(runtime().Objects().SetThreadPriority(*thread, static_cast<std::int32_t>(A32(1)))), 2);
}

void k_GetCurrentThread() {
    RET(kCurrentThread, 0);
}

void k_GetCurrentThreadId() {
    RET(hle_thread_id(), 0);
}

// The game reads another thread's registers only for its crash report; there are none to give.
void k_GetThreadContext() { // (thread, &context)
    hle_set_last_error(kErrorInvalidFunction);
    RET(0, 2);
}

// ---- events, mutexes, semaphores and waits ----

void k_CreateEventA() { // (attributes, manualReset, initialState, name)
    Object event;
    const Answer made = runtime().Objects().MakeEvent(A32(1) != 0, A32(2) != 0,
                                                      guestString(A32(3)), event);
    RET(made.value != 0 ? (answer(made), handleFor(event)) : answer(made), 4);
}

void k_CreateMutexA() { // (attributes, initialOwner, name)
    Object mutex;
    const Answer made = runtime().Objects().MakeMutex(A32(1) != 0, guestString(A32(2)), mutex);
    RET(made.value != 0 ? (answer(made), handleFor(mutex)) : answer(made), 3);
}

void k_CreateSemaphoreA() { // (attributes, initialCount, maximumCount, name)
    Object semaphore;
    const Answer made = runtime().Objects().MakeSemaphore(static_cast<std::int32_t>(A32(1)),
                                                          static_cast<std::int32_t>(A32(2)),
                                                          guestString(A32(3)), semaphore);
    RET(made.value != 0 ? (answer(made), handleFor(semaphore)) : answer(made), 4);
}

void k_OpenMutexA() { // (access, inherit, name)
    Object mutex;
    const Answer opened = runtime().Objects().OpenMutex(guestString(A32(2)), mutex);
    RET(opened.value != 0 ? (answer(opened), handleFor(mutex)) : answer(opened), 3);
}

template <Answer (Sync::*Signal)(const Object &)> void signalRow(const char *name) {
    const Object *object = objectOf(A32(0));
    if (object == nullptr) {
        handOn(name, 1, 0);
        return;
    }
    RET(answer((runtime().Objects().*Signal)(*object)), 1);
}

void k_SetEvent() {
    signalRow<&Sync::SetEvent>("SetEvent");
}
void k_ResetEvent() {
    signalRow<&Sync::ResetEvent>("ResetEvent");
}
void k_ReleaseMutex() {
    signalRow<&Sync::ReleaseMutex>("ReleaseMutex");
}

void k_ReleaseSemaphore() { // (semaphore, count, &previous)
    const Object *semaphore = objectOf(A32(0));
    if (semaphore == nullptr) {
        handOn("ReleaseSemaphore", 3, 0);
        return;
    }
    std::int32_t previous = 0;
    const Answer released =
        runtime().Objects().ReleaseSemaphore(*semaphore, static_cast<std::int32_t>(A32(1)), previous);
    if (released.value != 0) {
        put32(A32(2), static_cast<std::uint32_t>(previous));
    }
    RET(answer(released), 3);
}

// WaitForMultipleObjects over guest handles. Returns false when none is this module's (the
// caller hands the row on).
bool waitOn(const std::uint32_t *handles, std::uint32_t count, bool all, std::uint32_t timeoutMs,
            std::uint32_t &result) {
    std::vector<Object> objects;
    std::uint32_t others = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (const Object *object = objectOf(handles[i])) {
            objects.push_back(*object); // a copy: CloseHandle may run on another thread meanwhile
        } else {
            ++others;
        }
    }
    if (objects.empty() && count != 0) {
        return false;
    }
    if (others != 0) {
        hle_set_last_error(kErrorInvalidHandle);
        result = kWaitFailed;
        return true;
    }
    std::vector<const Object *> waited;
    for (const Object &object : objects) {
        waited.push_back(&object);
    }
    const auto wait = [&] { return runtime().Objects().Wait(waited, all, timeoutMs); };
    result = answer(timeoutMs == 0 ? wait() : withoutMachine(wait));
    return true;
}

void k_WaitForSingleObject() { // (handle, milliseconds)
    const std::uint32_t handle = A32(0);
    std::uint32_t result = 0;
    if (!waitOn(&handle, 1, false, A32(1), result)) {
        handOn("WaitForSingleObject", 2, kWaitFailed);
        return;
    }
    RET(result, 2);
}

void k_WaitForMultipleObjects() { // (count, handles, waitAll, milliseconds)
    const std::uint32_t count = A32(0), array = A32(1);
    if (count == 0 || count > kMaximumWaitObjects || array == 0) {
        hle_set_last_error(kErrorInvalidParameter);
        RET(kWaitFailed, 4);
    }
    std::vector<std::uint32_t> handles(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        handles[i] = MEM32(array + 4 * i);
    }
    std::uint32_t result = 0;
    if (!waitOn(handles.data(), count, A32(2) != 0, A32(3), result)) {
        handOn("WaitForMultipleObjects", 4, kWaitFailed);
        return;
    }
    RET(result, 4);
}

void k_Sleep() { // (milliseconds)
    const std::uint32_t milliseconds = A32(0);
    if (milliseconds == 0) {
        hle_yield();
    } else {
        withoutMachine([&] { return runtime().HostClock().Sleep(milliseconds).Ok(); });
    }
    RET(0, 1);
}

// ---- critical sections and interlocked operations ----

// The guest's CRITICAL_SECTION (24 bytes) only names the section; its lock is the module's.
void k_InitializeCriticalSection() { // (section)
    const std::uint32_t section = A32(0);
    if (section != 0) {
        std::memset(hle_ptr(section), 0, 24);
        MEM32(section + 4) = 0xFFFFFFFFu; // LockCount, as Windows leaves it
        runtime().Sections().Initialize(section);
    }
    RET(0, 1);
}

void k_DeleteCriticalSection() {
    runtime().Sections().Delete(A32(0));
    RET(0, 1);
}

void k_EnterCriticalSection() {
    const std::uint32_t section = A32(0);
    if (!runtime().Sections().TryEnter(section)) {
        withoutMachine([&] {
            runtime().Sections().Enter(section);
            return true;
        });
    }
    RET(0, 1);
}

void k_LeaveCriticalSection() {
    static_cast<void>(runtime().Sections().Leave(A32(0))); // Windows reports nothing either
    RET(0, 1);
}

// One thread runs guest code at a time, so a read and a write under the machine are atomic.
void k_InterlockedIncrement() {
    const std::uint32_t value = MEM32(A32(0)) + 1;
    MEM32(A32(0)) = value;
    RET(value, 1);
}

void k_InterlockedDecrement() {
    const std::uint32_t value = MEM32(A32(0)) - 1;
    MEM32(A32(0)) = value;
    RET(value, 1);
}

void k_InterlockedExchange() { // (target, value)
    const std::uint32_t previous = MEM32(A32(0));
    MEM32(A32(0)) = A32(1);
    RET(previous, 2);
}

// ---- thread-local storage ----

void k_TlsAlloc() {
    RET(answer(runtime().Slots().Allocate()), 0);
}
void k_TlsGetValue() {
    RET(answer(runtime().Slots().Get(A32(0))), 1);
}
void k_TlsSetValue() {
    RET(answer(runtime().Slots().Set(A32(0), A32(1))), 2);
}

// ---- the heap ----
//
// The system allocator in guest memory (pcrecomp's hle_heap_*) answers every heap: the process
// heap, each HeapCreate's and the Global/Local blocks. HeapCreate gives a distinct handle so that
// a destroyed heap is refused; the blocks of a destroyed heap stay allocated.

constexpr std::uint32_t kProcessHeap = 0x00150000u;
constexpr std::uint32_t kHeapZeroMemory = 0x08u;
constexpr std::uint32_t kGlobalZeroInit = 0x40u;
constexpr std::uint32_t kGlobalModify = 0x80u;

std::atomic<std::uint32_t> g_nextHeap{0x00160000u};

void k_GetProcessHeap() {
    RET(kProcessHeap, 0);
}

void k_HeapCreate() { // (options, initialSize, maximumSize)
    RET(g_nextHeap.fetch_add(0x10000u), 3);
}

void k_HeapDestroy() { // (heap)
    const std::uint32_t heap = A32(0);
    if (heap == kProcessHeap || heap < 0x00160000u) {
        hle_set_last_error(kErrorInvalidHandle);
        RET(0, 1);
    }
    RET(1, 1);
}

std::uint32_t allocate(std::uint32_t bytes, bool zero) {
    void *block = hle_heap_alloc(bytes != 0 ? bytes : 1u, zero ? 1 : 0);
    if (block == nullptr) {
        hle_set_last_error(kErrorNotEnoughMemory);
        return 0;
    }
    return hle_va(block);
}

bool isBlock(std::uint32_t address) {
    return address != 0 && hle_heap_ok(hle_ptr(address)) != 0;
}

void k_HeapAlloc() { // (heap, flags, bytes)
    RET(allocate(A32(2), (A32(1) & kHeapZeroMemory) != 0), 3);
}

void k_HeapFree() { // (heap, flags, block)
    const std::uint32_t block = A32(2);
    if (block == 0) {
        RET(1, 3);
    }
    if (!isBlock(block)) {
        hle_set_last_error(kErrorInvalidParameter);
        RET(0, 3);
    }
    hle_heap_free(hle_ptr(block));
    RET(1, 3);
}

// A Global or Local handle is the block's address, moveable or not.
void k_GlobalAlloc() { // (flags, bytes)
    RET(allocate(A32(1), (A32(0) & kGlobalZeroInit) != 0), 2);
}

void k_GlobalLock() { // (memory)
    const std::uint32_t memory = A32(0);
    if (!isBlock(memory)) {
        hle_set_last_error(kErrorInvalidHandle);
        RET(0, 1);
    }
    RET(memory, 1);
}

// Blocks are never moved, so no lock count is kept.
void k_GlobalUnlock() {
    RET(1, 1);
}

void k_GlobalHandle() { // (address)
    const std::uint32_t address = A32(0);
    if (!isBlock(address)) {
        hle_set_last_error(kErrorInvalidHandle);
        RET(0, 1);
    }
    RET(address, 1);
}

// GlobalFree and LocalFree: 0 when freed, else the handle.
void freeRow(std::uint32_t argc) {
    const std::uint32_t memory = A32(0);
    if (memory == 0) {
        RET(0, argc);
    }
    if (!isBlock(memory)) {
        hle_set_last_error(kErrorInvalidHandle);
        RET(memory, argc);
    }
    hle_heap_free(hle_ptr(memory));
    RET(0, argc);
}

void k_GlobalFree() {
    freeRow(1);
}
void k_LocalFree() {
    freeRow(1);
}

void k_GlobalReAlloc() { // (memory, bytes, flags)
    const std::uint32_t memory = A32(0), bytes = A32(1), flags = A32(2);
    if (!isBlock(memory)) {
        hle_set_last_error(kErrorInvalidHandle);
        RET(0, 3);
    }
    if ((flags & kGlobalModify) != 0) {
        RET(memory, 3);
    }
    void *moved = hle_heap_realloc(hle_ptr(memory), bytes != 0 ? bytes : 1u,
                                   (flags & kGlobalZeroInit) != 0 ? 1 : 0);
    if (moved == nullptr) {
        hle_set_last_error(kErrorNotEnoughMemory);
        RET(0, 3);
    }
    RET(hle_va(moved), 3);
}

// MEMORYSTATUS for a machine of the game's day with room to spare: 256 MB of memory, half free.
void k_GlobalMemoryStatus() { // (&status)
    const std::uint32_t status = A32(0);
    if (status != 0) {
        std::memset(hle_ptr(status), 0, 32);
        MEM32(status) = 32;                   // dwLength
        MEM32(status + 4) = 50;               // dwMemoryLoad, percent
        MEM32(status + 8) = 0x10000000u;      // dwTotalPhys
        MEM32(status + 12) = 0x08000000u;     // dwAvailPhys
        MEM32(status + 16) = 0x20000000u;     // dwTotalPageFile
        MEM32(status + 20) = 0x18000000u;     // dwAvailPageFile
        MEM32(status + 24) = 0x7FFE0000u;     // dwTotalVirtual
        MEM32(status + 28) = 0x60000000u;     // dwAvailVirtual
    }
    RET(0, 1);
}

// ---- virtual memory ----

VirtualMemory &virtualMemory() {
    static VirtualMemory memory([] {
        VirtualMemory::Backend backend;
        backend.reserve = [](std::uint32_t address, std::uint32_t size) {
            return hle_vm_reserve(address, size);
        };
        backend.release = [](std::uint32_t address, std::uint32_t size) {
            hle_vm_release(address, size);
        };
        backend.zero = [](std::uint32_t address, std::uint32_t size) { hle_vm_zero(address, size); };
        backend.mapped = [](std::uint32_t page) {
            return page != 0 && hostRangeAccessible(hle_ptr(page), 0x1000u, false);
        };
        return backend;
    }());
    return memory;
}

void k_VirtualAlloc() { // (address, size, type, protect)
    RET(answer(virtualMemory().Allocate(A32(0), A32(1), A32(2), A32(3))), 4);
}

void k_VirtualFree() { // (address, size, type)
    RET(answer(virtualMemory().Free(A32(0), A32(1), A32(2))), 3);
}

std::uint32_t query(std::uint32_t address, std::uint32_t out, std::uint32_t length) {
    if (out == 0 || length < 28) {
        hle_set_last_error(kErrorInvalidParameter);
        return 0;
    }
    const MemoryInfo info = virtualMemory().Query(address);
    const std::uint32_t fields[] = {info.baseAddress, info.allocationBase, info.allocationProtect,
                                    info.regionSize,  info.state,          info.protect,
                                    info.type};
    for (std::uint32_t i = 0; i < 7; ++i) {
        MEM32(out + 4 * i) = fields[i];
    }
    return 28;
}

void k_VirtualQuery() { // (address, &info, length)
    RET(query(A32(0), A32(1), A32(2)), 3);
}

void k_VirtualQueryEx() { // (process, address, &info, length)
    if (A32(0) != kCurrentProcess) {
        hle_set_last_error(kErrorInvalidHandle);
        RET(0, 4);
    }
    RET(query(A32(1), A32(2), A32(3)), 4);
}

// Nothing is paged out, so every page is as good as locked.
void k_VirtualLock() {
    RET(1, 2);
}

// ---- exceptions ----

constexpr std::uint32_t kContextSize = 0x2D0u; // CONTEXT is 0x2CC bytes; kept 16-byte aligned
constexpr std::uint32_t kRecordSize = 0x50u;   // EXCEPTION_RECORD
constexpr std::uint32_t kContextEdi = 0x9Cu, kContextEsi = 0xA0u, kContextEbx = 0xA4u;
constexpr std::uint32_t kContextEdx = 0xA8u, kContextEcx = 0xACu, kContextEax = 0xB0u;
constexpr std::uint32_t kContextEbp = 0xB4u, kContextEip = 0xB8u, kContextEflags = 0xC0u;
constexpr std::uint32_t kContextEsp = 0xC4u;
constexpr std::uint32_t kContextFull = 0x10007u;
constexpr std::uint32_t kExecuteHandler = 1;
constexpr const char *kNestedFrameRow = "kernel32rt.NestedFrameHandler";

// The guest as the dispatcher sees it, on the thread that holds the machine.
class LiveMachine final : public GuestMachine {
public:
    std::uint32_t Read32(std::uint32_t address) override { return MEM32(address); }
    void Write32(std::uint32_t address, std::uint32_t value) override { MEM32(address) = value; }
    std::uint32_t ChainHead() override { return MEM32(g_fs_base); }
    void SetChainHead(std::uint32_t record) override { MEM32(g_fs_base) = record; }

    // As a call from guest code: the callee-saved registers and esp are the caller's again
    // afterwards. The machine is already held, so its depth does not change.
    std::uint32_t Call(std::uint32_t address, const std::uint32_t *arguments,
                       std::uint32_t count) override {
        recomp_func_t function = recomp_lookup_manual(address);
        if (function == nullptr) {
            function = recomp_lookup(address);
        }
        if (function == nullptr) {
            function = recomp_lookup_import(address);
        }
        if (function == nullptr) {
            hle_fatal("[eh] exception handler 0x%08X is not lifted", address);
        }
        const std::uint32_t esp = g_esp, ebp = g_ebp, ebx = g_ebx, esi = g_esi, edi = g_edi;
        const std::uint32_t current = g_cur_func;
        for (std::uint32_t i = count; i > 0; --i) {
            g_esp -= 4;
            MEM32(g_esp) = arguments[i - 1];
        }
        g_esp -= 4;
        MEM32(g_esp) = RECOMP_RETADDR;
        function();
        const std::uint32_t result = g_eax;
        g_esp = esp;
        g_ebp = ebp;
        g_ebx = ebx;
        g_esi = esi;
        g_edi = edi;
        g_cur_func = current;
        return result;
    }
};

std::uint32_t nestedFrameHandler() {
    static const std::uint32_t address = hle_resolve(kNestedFrameRow);
    return address;
}

bool guestRangeReadable(std::uint32_t address, std::uint32_t bytes) {
    return address != 0 && static_cast<std::uint64_t>(address) + bytes <= 0x100000000ull &&
           hostRangeAccessible(hle_ptr(address), bytes, false);
}

// The decorated name of the type a C++ exception record carries, or nullptr.
const char *cxxType(std::uint32_t record) {
    if (MEM32(record) != kMsvcCxxException || MEM32(record + 16) < 3) {
        return nullptr;
    }
    const std::uint32_t throwInfo = MEM32(record + 28);
    if (throwInfo == 0) {
        return "(rethrow)";
    }
    const std::uint32_t types = guestRangeReadable(throwInfo, 16) ? MEM32(throwInfo + 12) : 0;
    if (types == 0 || !guestRangeReadable(types, 8) || MEM32(types) == 0) {
        return "?";
    }
    const std::uint32_t descriptor = MEM32(MEM32(types + 4) + 4);
    return guestRangeReadable(descriptor, 12) ? static_cast<const char *>(hle_ptr(descriptor + 8))
                                              : "?";
}

// For an unhandled exception: every frame on the chain and, for a C++ frame (its handler a
// `mov eax, FuncInfo; jmp __CxxFrameHandler` thunk), whether a try block covers its state.
void reportChain() {
    int frames = 0;
    for (std::uint32_t frame = MEM32(g_fs_base); frame != 0 && frame != kChainEnd && frames < 64;
         frame = MEM32(frame), ++frames) {
        const std::uint32_t handler = MEM32(frame + 4);
        std::uint32_t info = 0;
        if (guestRangeReadable(handler, 10) && MEM8(handler) == 0xB8 && MEM8(handler + 5) == 0xE9) {
            info = MEM32(handler + 1);
        }
        if (info != 0 && guestRangeReadable(info, 20) &&
            (MEM32(info) & 0xFFFFFFF0u) == 0x19930520u) {
            const auto state = static_cast<std::int32_t>(MEM32(frame + 8));
            int covering = 0;
            for (std::uint32_t t = 0; t < MEM32(info + 12); ++t) {
                const std::uint32_t block = MEM32(info + 16) + 20 * t;
                if (static_cast<std::int32_t>(MEM32(block)) <= state &&
                    state <= static_cast<std::int32_t>(MEM32(block + 4))) {
                    ++covering;
                }
            }
            std::fprintf(stderr, "[eh]   frame 0x%08X: C++, FuncInfo 0x%08X, state %d, %u try block(s), "
                                 "%d cover it\n",
                         frame, info, state, MEM32(info + 12), covering);
        } else {
            std::fprintf(stderr, "[eh]   frame 0x%08X: handler 0x%08X\n", frame, handler);
        }
    }
    if (frames == 0) {
        std::fprintf(stderr, "[eh]   (the guest frame chain is empty)\n");
    }
}

void report(const char *what, std::uint32_t record) {
    const char *type = cxxType(record);
    std::fprintf(stderr, "[eh] %s guest exception 0x%08X%s%s raised at 0x%08X\n", what,
                 MEM32(record), type != nullptr ? ", C++ type " : "", type != nullptr ? type : "",
                 MEM32(record + 12));
    reportChain();
    std::fflush(stderr);
}

void captureContext(std::uint32_t context, std::uint32_t eip, std::uint32_t esp) {
    std::memset(hle_ptr(context), 0, kContextSize);
    MEM32(context) = kContextFull;
    MEM32(context + kContextEdi) = g_edi;
    MEM32(context + kContextEsi) = g_esi;
    MEM32(context + kContextEbx) = g_ebx;
    MEM32(context + kContextEdx) = g_edx;
    MEM32(context + kContextEcx) = g_ecx;
    MEM32(context + kContextEax) = g_eax;
    MEM32(context + kContextEbp) = g_ebp;
    MEM32(context + kContextEip) = eip;
    MEM32(context + kContextEsp) = esp;
    MEM32(context + kContextEflags) = 0x202u;
}

// UnhandledExceptionFilter's work: the game's top-level filter, if it set one, decides;
// otherwise the exception is reported and its handler runs (the process ends).
// The report comes first: the program's filter may end the process (the C runtime's calls
// terminate()).
std::uint32_t unhandledFilter(std::uint32_t pointers) {
    report("unhandled", MEM32(pointers));
    if (const std::uint32_t filter = runtime().UnhandledFilter()) {
        std::fprintf(stderr, "[eh] the program's top-level filter 0x%08X runs\n", filter);
        std::fflush(stderr);
        LiveMachine machine;
        const std::uint32_t verdict = machine.Call(filter, &pointers, 1);
        if (verdict != 0) { // EXCEPTION_CONTINUE_SEARCH falls through to the default
            return verdict;
        }
    }
    return kExecuteHandler;
}

// The end of a dispatch no frame took: the top-level filter, then the process ends, as Windows
// ends a thread whose exception reaches its outermost frame.
[[noreturn]] void endUnhandled(std::uint32_t record, std::uint32_t context, std::uint32_t pointers) {
    MEM32(pointers) = record;
    MEM32(pointers + 4) = context;
    static_cast<void>(unhandledFilter(pointers));
    hle_exit(1);
    std::abort(); // hle_exit does not return
}

void k_RaiseException() { // (code, flags, count, arguments)
    const std::uint32_t code = A32(0), flags = A32(1), arguments = A32(3);
    const std::uint32_t count = std::min<std::uint32_t>(A32(2), 15);
    const std::uint32_t returnAddress = MEM32(g_esp), entryEsp = g_esp;
    // The records go below the caller's stack, clear of anything it holds there; every call the
    // dispatch makes pushes below them. A hardware fault comes here from the guest host with
    // esp already moved 64 KB down.
    g_esp = (g_esp - kContextSize - kRecordSize - 32) & ~15u;
    const std::uint32_t context = g_esp, record = context + kContextSize, scratch = record + kRecordSize;
    std::memset(hle_ptr(record), 0, kRecordSize);
    MEM32(record) = code;
    MEM32(record + 4) = flags & kExceptionNoncontinuable;
    MEM32(record + 12) = returnAddress != RECOMP_RETADDR ? returnAddress : g_cur_func;
    MEM32(record + 16) = arguments != 0 ? count : 0;
    for (std::uint32_t i = 0; arguments != 0 && i < count; ++i) {
        MEM32(record + 20 + 4 * i) = MEM32(arguments + 4 * i);
    }
    captureContext(context, MEM32(record + 12), entryEsp + 4 + 16);
    if (win32hle_trace) {
        const char *type = cxxType(record);
        std::fprintf(stderr, "[eh] raise 0x%08X%s%s in sub_%08X\n", code, type != nullptr ? " " : "",
                     type != nullptr ? type : "", g_cur_func);
    }
    LiveMachine machine;
    Dispatcher dispatcher(machine, nestedFrameHandler());
    const DispatchOutcome outcome = dispatcher.Dispatch(record, context, scratch);
    switch (outcome.kind) {
    case DispatchOutcome::kContinueExecution:
        g_edi = MEM32(context + kContextEdi);
        g_esi = MEM32(context + kContextEsi);
        g_ebx = MEM32(context + kContextEbx);
        g_edx = MEM32(context + kContextEdx);
        g_ecx = MEM32(context + kContextEcx);
        g_ebp = MEM32(context + kContextEbp);
        g_esp = entryEsp;
        RET(MEM32(context + kContextEax), 4);
    case DispatchOutcome::kContinuedNoncontinuable:
        std::fprintf(stderr, "[eh] handler 0x%08X continued noncontinuable exception 0x%08X\n",
                     outcome.handler, code);
        break;
    case DispatchOutcome::kBadDisposition:
        std::fprintf(stderr, "[eh] handler 0x%08X returned disposition %u\n", outcome.handler,
                     outcome.disposition);
        break;
    case DispatchOutcome::kCorruptChain:
        std::fprintf(stderr, "[eh] the exception chain is damaged (0x%08X raised); ending the "
                             "thread as an unhandled exception\n", code);
        break;
    case DispatchOutcome::kUnhandled:
        break;
    }
    endUnhandled(record, context, scratch);
}

void k_RtlUnwind() { // (targetFrame, targetIp, record, returnValue)
    const std::uint32_t target = A32(0), targetIp = A32(1), returnValue = A32(3);
    std::uint32_t record = A32(2);
    const std::uint32_t returnAddress = MEM32(g_esp), entryEsp = g_esp;
    g_esp = (g_esp - kContextSize - kRecordSize - 32) & ~15u;
    const std::uint32_t context = g_esp, local = context + kContextSize, scratch = local + kRecordSize;
    if (record == 0) {
        record = local;
        std::memset(hle_ptr(record), 0, kRecordSize);
        MEM32(record) = kStatusUnwind;
        MEM32(record + 12) = returnAddress;
    }
    captureContext(context, targetIp, entryEsp + 4 + 16);
    MEM32(context + kContextEax) = returnValue;
    LiveMachine machine;
    Dispatcher dispatcher(machine, nestedFrameHandler());
    const UnwindOutcome outcome = dispatcher.Unwind(target, record, context, scratch);
    if (outcome.kind != UnwindOutcome::kDone) {
        hle_fatal("[eh] RtlUnwind to frame 0x%08X failed (%d, handler 0x%08X, disposition %u)",
                  target, static_cast<int>(outcome.kind), outcome.handler, outcome.disposition);
    }
    if (win32hle_trace) {
        std::fprintf(stderr, "[eh] unwind to 0x%08X: %u frame(s)\n", target, outcome.frames);
    }
    // The C runtime's TargetIp is always RtlUnwind's own return address: returning is the jump.
    g_esp = entryEsp;
    RET(returnValue, 4);
}

void k_NestedFrameHandler() { // cdecl (record, frame, context, dispatcherContext)
    LiveMachine machine;
    const std::uint32_t disposition =
        Dispatcher::NestedFrameHandler(machine, A32(0), A32(1), A32(3));
    RET(disposition, 0);
}

void k_SetUnhandledExceptionFilter() { // (filter)
    RET(runtime().SetUnhandledFilter(A32(0)), 1);
}

// EXCEPTION_EXECUTE_HANDLER, or what the game's filter answered (EXCEPTION_CONTINUE_EXECUTION).
void k_UnhandledExceptionFilter() { // (pointers)
    RET(unhandledFilter(A32(0)), 1);
}

std::atomic<std::uint32_t> g_errorMode{0};

void k_SetErrorMode() { // (mode): the previous mode; there are no system dialogs to suppress
    RET(g_errorMode.exchange(A32(0)), 1);
}

bool badRange(std::uint32_t address, std::uint32_t bytes, bool forWrite) {
    if (bytes == 0) {
        return false;
    }
    if (address == 0 || static_cast<std::uint64_t>(address) + bytes > 0x100000000ull) {
        return true;
    }
    return !hostRangeAccessible(hle_ptr(address), bytes, forWrite);
}

void k_IsBadReadPtr() {
    RET(badRange(A32(0), A32(1), false) ? 1u : 0u, 2);
}
void k_IsBadWritePtr() {
    RET(badRange(A32(0), A32(1), true) ? 1u : 0u, 2);
}
void k_IsBadCodePtr() {
    RET(badRange(A32(0), 1, false) ? 1u : 0u, 1);
}

// ---- time ----

void putSystemTime(std::uint32_t address, const SystemTime &time) {
    if (address == 0) {
        return;
    }
    const std::uint16_t fields[] = {time.year,   time.month,  time.dayOfWeek, time.day,
                                    time.hour,   time.minute, time.second,    time.milliseconds};
    for (std::uint32_t i = 0; i < 8; ++i) {
        MEM16(address + 2 * i) = fields[i];
    }
}

SystemTime getSystemTime(std::uint32_t address) {
    SystemTime time;
    std::uint16_t *fields[] = {&time.year,  &time.month,  &time.dayOfWeek, &time.day,
                               &time.hour,  &time.minute, &time.second,    &time.milliseconds};
    for (std::uint32_t i = 0; i < 8; ++i) {
        *fields[i] = MEM16(address + 2 * i);
    }
    return time;
}

void k_GetTickCount() {
    RET(runtime().Timing().TickCount(), 0);
}

void k_QueryPerformanceCounter() { // (&count)
    const std::uint64_t count = runtime().Timing().Counter();
    put64(A32(0), count);
    RET(1u, 1); // always TRUE: a counter that reads zero (the first instant) is still a reading
}

void k_QueryPerformanceFrequency() { // (&frequency)
    const std::uint64_t frequency = runtime().Timing().CounterFrequency();
    put64(A32(0), frequency);
    RET(frequency != 0 ? 1u : 0u, 1);
}

void k_GetSystemTime() {
    putSystemTime(A32(0), runtime().Timing().Now(platform::TimeZone::kUtc));
    RET(0, 1);
}

void k_GetLocalTime() {
    putSystemTime(A32(0), runtime().Timing().Now(platform::TimeZone::kLocal));
    RET(0, 1);
}

void k_GetSystemTimeAsFileTime() {
    put64(A32(0), runtime().Timing().FileTimeNow());
    RET(0, 1);
}

void k_FileTimeToLocalFileTime() { // (&utc, &local)
    put64(A32(1), runtime().Timing().LocalFromUtc(get64(A32(0))));
    RET(1, 2);
}

void k_LocalFileTimeToFileTime() { // (&local, &utc)
    put64(A32(1), runtime().Timing().UtcFromLocal(get64(A32(0))));
    RET(1, 2);
}

void k_FileTimeToSystemTime() { // (&fileTime, &systemTime)
    const auto time = systemTimeFromFileTime(get64(A32(0)));
    if (!time) {
        hle_set_last_error(kErrorInvalidParameter);
        RET(0, 2);
    }
    putSystemTime(A32(1), *time);
    RET(1, 2);
}

void k_SystemTimeToFileTime() { // (&systemTime, &fileTime)
    const auto time = fileTimeFromSystemTime(getSystemTime(A32(0)));
    if (!time) {
        hle_set_last_error(kErrorInvalidParameter);
        RET(0, 2);
    }
    put64(A32(1), *time);
    RET(1, 2);
}

// TIME_ZONE_INFORMATION (172 bytes) with today's bias and no daylight rule: the Clock gives the
// offset in force, not the zone's rules. TIME_ZONE_ID_UNKNOWN says so.
void k_GetTimeZoneInformation() { // (&information)
    const std::uint32_t information = A32(0);
    if (information != 0) {
        std::memset(hle_ptr(information), 0, 172);
        MEM32(information) = static_cast<std::uint32_t>(runtime().Timing().BiasMinutes());
    }
    RET(0, 1);
}

// WINMM: timeGetTime and the multimedia timers. A timer's callback runs on the timer's own
// thread, as on Windows: TIME_CALLBACK_FUNCTION calls the guest's procedure (id, 0, user, 0, 0);
// TIME_CALLBACK_EVENT_SET and _PULSE signal the event its procedure argument names.
constexpr std::uint32_t kTimePeriodic = 0x01u;
constexpr std::uint32_t kTimeCallbackEventSet = 0x10u;
constexpr std::uint32_t kTimeCallbackEventPulse = 0x20u;
constexpr std::uint32_t kMmsysErrInvalidParameter = 11;

void k_timeGetTime() {
    RET(runtime().Timing().TickCount(), 0);
}

void k_timeSetEvent() { // (delay, resolution, procedure, user, flags)
    const std::uint32_t delay = A32(0), procedure = A32(2), user = A32(3), flags = A32(4);
    const bool signals = (flags & (kTimeCallbackEventSet | kTimeCallbackEventPulse)) != 0;
    const bool pulses = (flags & kTimeCallbackEventPulse) != 0;
    std::optional<platform::Waitable> event;
    if (signals) {
        event = runtime().WaitableOf(procedure);
        if (!event) {
            RET(0, 5);
        }
    } else if (procedure == 0) {
        RET(0, 5); // a function callback with no function
    }
    const std::uint32_t id = runtime().TimerQueue().Start(
        delay, (flags & kTimePeriodic) != 0, [procedure, user, event, pulses](std::uint32_t timer) {
            if (event) {
                // TIME_CALLBACK_EVENT_PULSE: set, let the waiting threads go, reset.
                static_cast<void>(runtime().HostThreads().SetEvent(*event));
                if (pulses) {
                    static_cast<void>(runtime().HostThreads().ResetEvent(*event));
                }
                return;
            }
            const std::uint32_t arguments[5] = {timer, 0, user, 0, 0};
            hle_call_guest(procedure, 5, arguments);
        });
    RET(id, 5);
}

void k_timeKillEvent() { // (id)
    const std::uint32_t id = A32(0);
    const bool killed = withoutMachine([&] { return runtime().TimerQueue().Kill(id); });
    RET(killed ? 0u : kMmsysErrInvalidParameter, 1);
}

// ---- the environment and the C runtime's start-up queries ----
//
// A private environment of the two variables any Windows has; nothing of the host's is shown.

struct NoCase {
    bool operator()(const std::string &a, const std::string &b) const {
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
            return std::toupper(static_cast<unsigned char>(x)) < std::toupper(static_cast<unsigned char>(y));
        });
    }
};

std::map<std::string, std::string, NoCase> &environment() {
    static std::map<std::string, std::string, NoCase> variables{
        {"PATH", "C:\\WINDOWS\\SYSTEM32;C:\\WINDOWS"},
        {"WINDIR", "C:\\WINDOWS"},
    };
    return variables;
}

void k_GetEnvironmentStrings() {
    std::string block;
    for (const auto &[name, value] : environment()) {
        block += name + "=" + value;
        block.push_back('\0');
    }
    block.push_back('\0');
    const std::uint32_t copy = allocate(static_cast<std::uint32_t>(block.size()), false);
    if (copy != 0) {
        std::memcpy(hle_ptr(copy), block.data(), block.size());
    }
    RET(copy, 0);
}

// The wide block is refused as on Windows 95 and 98, which the game supports; its C runtime
// then reads the narrow one.
void k_GetEnvironmentStringsW() {
    hle_set_last_error(120); // ERROR_CALL_NOT_IMPLEMENTED
    RET(0, 0);
}

void k_FreeEnvironmentStringsA() { // (block)
    const std::uint32_t block = A32(0);
    if (isBlock(block)) {
        hle_heap_free(hle_ptr(block));
    }
    RET(1, 1);
}

void k_FreeEnvironmentStringsW() {
    RET(1, 1);
}

void k_GetEnvironmentVariableA() { // (name, buffer, size)
    const auto found = environment().find(guestString(A32(0)));
    if (found == environment().end()) {
        hle_set_last_error(203); // ERROR_ENVVAR_NOT_FOUND
        RET(0, 3);
    }
    const std::string &value = found->second;
    const std::uint32_t buffer = A32(1), size = A32(2);
    if (buffer == 0 || size <= value.size()) {
        RET(static_cast<std::uint32_t>(value.size() + 1), 3);
    }
    std::memcpy(hle_ptr(buffer), value.c_str(), value.size() + 1);
    RET(static_cast<std::uint32_t>(value.size()), 3);
}

void k_SetEnvironmentVariableA() { // (name, value)
    const std::string name = guestString(A32(0));
    if (name.empty() || name.find('=') != std::string::npos) {
        hle_set_last_error(kErrorInvalidParameter);
        RET(0, 2);
    }
    if (A32(1) == 0) {
        environment().erase(name);
    } else {
        environment()[name] = guestString(A32(1));
    }
    RET(1, 2);
}

void k_GetStartupInfoA() { // (&info): STARTUPINFOA, 68 bytes, nothing asked of the window
    const std::uint32_t info = A32(0);
    if (info != 0) {
        std::memset(hle_ptr(info), 0, 68);
        MEM32(info) = 68;
    }
    RET(0, 1);
}

void k_SetHandleCount() { // (count): Windows NT has no limit to raise
    RET(A32(0), 1);
}

// Windows XP Service Pack 3, the newest Windows the original supports.
constexpr std::uint32_t kMajor = 5, kMinor = 1, kBuild = 2600, kPlatformNt = 2;

void k_GetVersion() {
    RET(kMajor | kMinor << 8 | kBuild << 16, 0);
}

void k_GetVersionExA() { // (&info): OSVERSIONINFOA, 148 bytes
    const std::uint32_t info = A32(0);
    if (info == 0 || MEM32(info) < 148) {
        hle_set_last_error(kErrorInvalidParameter);
        RET(0, 1);
    }
    std::memset(hle_ptr(info + 4), 0, 144);
    MEM32(info + 4) = kMajor;
    MEM32(info + 8) = kMinor;
    MEM32(info + 12) = kBuild;
    MEM32(info + 16) = kPlatformNt;
    std::memcpy(hle_ptr(info + 20), "Service Pack 3", 15);
    RET(1, 1);
}

void k_GetSystemInfo() { // (&info): SYSTEM_INFO, 36 bytes; one Pentium-class processor
    const std::uint32_t info = A32(0);
    if (info != 0) {
        std::memset(hle_ptr(info), 0, 36);
        MEM32(info + 4) = 0x1000u;       // dwPageSize
        MEM32(info + 8) = 0x10000u;      // lpMinimumApplicationAddress
        MEM32(info + 12) = 0x7FFEFFFFu;  // lpMaximumApplicationAddress
        MEM32(info + 16) = 1;            // dwActiveProcessorMask
        MEM32(info + 20) = 1;            // dwNumberOfProcessors
        MEM32(info + 24) = 586;          // dwProcessorType: PROCESSOR_INTEL_PENTIUM
        MEM32(info + 28) = 0x10000u;     // dwAllocationGranularity
        MEM16(info + 32) = 6;            // wProcessorLevel
    }
    RET(0, 1);
}

// The ANSI code page is Windows-1252 and the OEM one 437; both are single-byte.
constexpr std::uint32_t kAnsiCodePage = 1252, kOemCodePage = 437;

void k_GetACP() {
    RET(kAnsiCodePage, 0);
}
void k_GetOEMCP() {
    RET(kOemCodePage, 0);
}

bool knownCodePage(std::uint32_t page) {
    return page == kAnsiCodePage || page == kOemCodePage || page == 0 /* CP_ACP */ ||
           page == 1 /* CP_OEMCP */;
}

void k_IsValidCodePage() {
    RET(A32(0) == kAnsiCodePage || A32(0) == kOemCodePage ? 1u : 0u, 1);
}

void k_GetCPInfo() { // (page, &info): CPINFO, 20 bytes
    const std::uint32_t info = A32(1);
    if (!knownCodePage(A32(0)) || info == 0) {
        hle_set_last_error(kErrorInvalidParameter);
        RET(0, 2);
    }
    std::memset(hle_ptr(info), 0, 20);
    MEM32(info) = 1;       // MaxCharSize
    MEM8(info + 4) = '?';  // DefaultChar
    RET(1, 2);
}

void k_GetCurrentProcess() {
    RET(kCurrentProcess, 0);
}

void k_ExitProcess() { // (code)
    hle_exit(static_cast<int>(A32(0)));
}

void k_TerminateProcess() { // (process, code)
    if (A32(0) != kCurrentProcess) {
        hle_set_last_error(kErrorInvalidHandle);
        RET(0, 2);
    }
    hle_exit(static_cast<int>(A32(1)));
}

// ---- module handles ----

void k_LoadLibraryA() { // (name)
    if (const auto handle = runtime().BuiltIns().Find(guestString(A32(0)))) {
        RET(*handle, 1);
    }
    handOn("LoadLibraryA", 1, 0);
}

void k_GetModuleHandleA() { // (name): NULL names the exe, which win32hle's row answers
    if (A32(0) != 0) {
        if (const auto handle = runtime().BuiltIns().Find(guestString(A32(0)))) {
            RET(*handle, 1);
        }
    }
    handOn("GetModuleHandleA", 1, 0);
}

void k_GetProcAddress() { // (module, name)
    const std::uint32_t module = A32(0), name = A32(1);
    if (!runtime().BuiltIns().IsBuiltIn(module)) {
        handOn("GetProcAddress", 2, 0);
        return;
    }
    // A name below 64 KB is an ordinal; the built-in modules export by name only.
    const auto address = name >= 0x10000u ? runtime().BuiltIns().Export(module, guestString(name))
                                          : std::nullopt;
    if (!address) {
        hle_set_last_error(kErrorProcNotFound);
        RET(0, 2);
    }
    RET(*address, 2);
}

void k_FreeLibrary() { // (module): a built-in module stays
    if (runtime().BuiltIns().IsBuiltIn(A32(0))) {
        RET(1, 1);
    }
    handOn("FreeLibrary", 1, 0);
}

// ---- DBGHELP ----
//
// The game walks its own stack for a crash report. Lifted code has no x86 frames to walk, so
// StackWalk finds none and the symbol rows find nothing; SymInitialize succeeds so that the report
// is still written, without a call stack.

void k_SymInitialize() {
    RET(1, 3);
}
void k_SymSetOptions() {
    RET(A32(0), 1);
}
void k_SymGetOptions() {
    RET(0, 0);
}
void k_SymLoadModule() {
    RET(0, 6);
}
void k_StackWalk() {
    RET(0, 9);
}
void k_SymFunctionTableAccess() {
    RET(0, 2);
}
void k_SymGetModuleInfo() {
    RET(0, 3);
}
void k_SymGetSymFromAddr() {
    RET(0, 4);
}
void k_SymGetLineFromAddr() {
    RET(0, 4);
}

} // namespace

} // namespace sfcop::hle::kernel32rt

namespace k = sfcop::hle::kernel32rt;

extern "C" const win32hle_shim sfcop_hle_kernel32rt[] = {
    // threads
    {"CreateThread", k::k_CreateThread},
    {"ExitThread", k::k_ExitThread},
    {"ResumeThread", k::k_ResumeThread},
    {"SetThreadPriority", k::k_SetThreadPriority},
    {"GetCurrentThread", k::k_GetCurrentThread},
    {"GetCurrentThreadId", k::k_GetCurrentThreadId},
    {"GetThreadContext", k::k_GetThreadContext},
    // synchronisation
    {"CreateEventA", k::k_CreateEventA},
    {"CreateMutexA", k::k_CreateMutexA},
    {"CreateSemaphoreA", k::k_CreateSemaphoreA},
    {"OpenMutexA", k::k_OpenMutexA},
    {"SetEvent", k::k_SetEvent},
    {"ResetEvent", k::k_ResetEvent},
    {"ReleaseMutex", k::k_ReleaseMutex},
    {"ReleaseSemaphore", k::k_ReleaseSemaphore},
    {"WaitForSingleObject", k::k_WaitForSingleObject},
    {"WaitForMultipleObjects", k::k_WaitForMultipleObjects},
    {"Sleep", k::k_Sleep},
    {"InitializeCriticalSection", k::k_InitializeCriticalSection},
    {"DeleteCriticalSection", k::k_DeleteCriticalSection},
    {"EnterCriticalSection", k::k_EnterCriticalSection},
    {"LeaveCriticalSection", k::k_LeaveCriticalSection},
    {"InterlockedIncrement", k::k_InterlockedIncrement},
    {"InterlockedDecrement", k::k_InterlockedDecrement},
    {"InterlockedExchange", k::k_InterlockedExchange},
    // thread-local storage
    {"TlsAlloc", k::k_TlsAlloc},
    {"TlsGetValue", k::k_TlsGetValue},
    {"TlsSetValue", k::k_TlsSetValue},
    // the heap and virtual memory
    {"GetProcessHeap", k::k_GetProcessHeap},
    {"HeapCreate", k::k_HeapCreate},
    {"HeapDestroy", k::k_HeapDestroy},
    {"HeapAlloc", k::k_HeapAlloc},
    {"HeapFree", k::k_HeapFree},
    {"GlobalAlloc", k::k_GlobalAlloc},
    {"GlobalLock", k::k_GlobalLock},
    {"GlobalUnlock", k::k_GlobalUnlock},
    {"GlobalHandle", k::k_GlobalHandle},
    {"GlobalFree", k::k_GlobalFree},
    {"GlobalReAlloc", k::k_GlobalReAlloc},
    {"GlobalMemoryStatus", k::k_GlobalMemoryStatus},
    {"LocalFree", k::k_LocalFree},
    {"VirtualAlloc", k::k_VirtualAlloc},
    {"VirtualFree", k::k_VirtualFree},
    {"VirtualQuery", k::k_VirtualQuery},
    {"VirtualQueryEx", k::k_VirtualQueryEx},
    {"VirtualLock", k::k_VirtualLock},
    // exceptions
    {"RaiseException", k::k_RaiseException},
    {"RtlUnwind", k::k_RtlUnwind},
    {"SetUnhandledExceptionFilter", k::k_SetUnhandledExceptionFilter},
    {"UnhandledExceptionFilter", k::k_UnhandledExceptionFilter},
    {"SetErrorMode", k::k_SetErrorMode},
    {"IsBadReadPtr", k::k_IsBadReadPtr},
    {"IsBadWritePtr", k::k_IsBadWritePtr},
    {"IsBadCodePtr", k::k_IsBadCodePtr},
    {k::kNestedFrameRow, k::k_NestedFrameHandler}, // the dispatcher's own frame, not an import
    // time
    {"GetTickCount", k::k_GetTickCount},
    {"QueryPerformanceCounter", k::k_QueryPerformanceCounter},
    {"QueryPerformanceFrequency", k::k_QueryPerformanceFrequency},
    {"GetSystemTime", k::k_GetSystemTime},
    {"GetLocalTime", k::k_GetLocalTime},
    {"GetSystemTimeAsFileTime", k::k_GetSystemTimeAsFileTime},
    {"FileTimeToLocalFileTime", k::k_FileTimeToLocalFileTime},
    {"LocalFileTimeToFileTime", k::k_LocalFileTimeToFileTime},
    {"FileTimeToSystemTime", k::k_FileTimeToSystemTime},
    {"SystemTimeToFileTime", k::k_SystemTimeToFileTime},
    {"GetTimeZoneInformation", k::k_GetTimeZoneInformation},
    {"timeGetTime", k::k_timeGetTime},
    {"timeSetEvent", k::k_timeSetEvent},
    {"timeKillEvent", k::k_timeKillEvent},
    // the environment and start-up
    {"GetEnvironmentStrings", k::k_GetEnvironmentStrings},
    {"GetEnvironmentStringsW", k::k_GetEnvironmentStringsW},
    {"FreeEnvironmentStringsA", k::k_FreeEnvironmentStringsA},
    {"FreeEnvironmentStringsW", k::k_FreeEnvironmentStringsW},
    {"GetEnvironmentVariableA", k::k_GetEnvironmentVariableA},
    {"SetEnvironmentVariableA", k::k_SetEnvironmentVariableA},
    {"GetStartupInfoA", k::k_GetStartupInfoA},
    {"SetHandleCount", k::k_SetHandleCount},
    {"GetVersion", k::k_GetVersion},
    {"GetVersionExA", k::k_GetVersionExA},
    {"GetSystemInfo", k::k_GetSystemInfo},
    {"GetACP", k::k_GetACP},
    {"GetOEMCP", k::k_GetOEMCP},
    {"IsValidCodePage", k::k_IsValidCodePage},
    {"GetCPInfo", k::k_GetCPInfo},
    {"GetCurrentProcess", k::k_GetCurrentProcess},
    {"ExitProcess", k::k_ExitProcess},
    {"TerminateProcess", k::k_TerminateProcess},
    // module handles
    {"LoadLibraryA", k::k_LoadLibraryA},
    {"GetModuleHandleA", k::k_GetModuleHandleA},
    {"GetProcAddress", k::k_GetProcAddress},
    {"FreeLibrary", k::k_FreeLibrary},
    // DBGHELP
    {"SymInitialize", k::k_SymInitialize},
    {"SymSetOptions", k::k_SymSetOptions},
    {"SymGetOptions", k::k_SymGetOptions},
    {"SymLoadModule", k::k_SymLoadModule},
    {"StackWalk", k::k_StackWalk},
    {"SymFunctionTableAccess", k::k_SymFunctionTableAccess},
    {"SymGetModuleInfo", k::k_SymGetModuleInfo},
    {"SymGetSymFromAddr", k::k_SymGetSymFromAddr},
    {"SymGetLineFromAddr", k::k_SymGetLineFromAddr},
    {nullptr, nullptr},
};
