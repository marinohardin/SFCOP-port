// Threads' thread start on Windows: the C runtime's _beginthreadex with the stack size asked for,
// its handle closed at once (the thread runs detached; Threads keeps its own handle object).

#include "platform/Threads/Threads.h"

#include <process.h>
#include <windows.h>

namespace sfcop::platform::threadshost {
namespace {

unsigned __stdcall runBody(void *raw) {
    std::unique_ptr<std::function<void()>> body(static_cast<std::function<void()> *>(raw));
    (*body)();
    return 0;
}

} // namespace

bool startHostThread(std::function<void()> body, std::size_t stackBytes) {
    auto owned = std::make_unique<std::function<void()>>(std::move(body));
    const std::uintptr_t thread = _beginthreadex(nullptr, static_cast<unsigned>(stackBytes),
                                                 runBody, owned.get(), 0, nullptr);
    if (thread == 0) {
        return false;
    }
    owned.release(); // runBody owns it now
    CloseHandle(reinterpret_cast<HANDLE>(thread));
    return true;
}

} // namespace sfcop::platform::threadshost
