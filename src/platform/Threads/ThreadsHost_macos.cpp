// Threads' thread start on macOS: a detached POSIX thread with the stack size asked for (the
// default for a secondary thread is 512 KB, less than the game's lifted code may need).

#include "platform/Threads/Threads.h"

#include <pthread.h>
#include <unistd.h>

namespace sfcop::platform::threadshost {
namespace {

void *runBody(void *raw) {
    std::unique_ptr<std::function<void()>> body(static_cast<std::function<void()> *>(raw));
    (*body)();
    return nullptr;
}

} // namespace

bool startHostThread(std::function<void()> body, std::size_t stackBytes) {
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) {
        return false;
    }
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    if (stackBytes != 0) {
        // A size that is not a page multiple is refused (EINVAL) on some systems and would leave
        // the thread on the default stack: round it up, and a refusal is the start's failure.
        const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
        const std::size_t rounded = (stackBytes + page - 1) / page * page;
        if (rounded < stackBytes || pthread_attr_setstacksize(&attributes, rounded) != 0) {
            pthread_attr_destroy(&attributes);
            return false;
        }
    }
    auto owned = std::make_unique<std::function<void()>>(std::move(body));
    pthread_t thread;
    const bool started = pthread_create(&thread, &attributes, runBody, owned.get()) == 0;
    pthread_attr_destroy(&attributes);
    if (started) {
        owned.release(); // runBody owns it now
    }
    return started;
}

} // namespace sfcop::platform::threadshost
