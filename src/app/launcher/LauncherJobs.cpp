#include "app/launcher/LauncherJobs.h"

#include <utility>

namespace sfcop::app::launcher {

ImportReport FilesLibrary::importGame(const ImportRequest &request,
                                      const ImportCallbacks &callbacks) {
    return app::importGame(request, files_, callbacks);
}

ImportReport FilesLibrary::verifyImportedGame(const std::string &gameDir,
                                              const InstallManifest &manifest,
                                              const ImportCallbacks &callbacks) {
    return app::verifyImportedGame(gameDir, manifest, files_, callbacks);
}

WorkerJobs::~WorkerJobs() {
    cancel();
    join();
}

template <typename Call> void WorkerJobs::start(Call call) {
    if (running_.load()) {
        return;
    }
    {
        // A finished job whose report has not been taken yet: that report is the Launcher's
        // next event, so no new job replaces it.
        const std::lock_guard<std::mutex> lock(mutex_);
        if (pending_.finished) {
            return;
        }
    }
    join(); // a finished job's thread
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        pending_ = JobEvents{};
    }
    cancelled_.store(false);
    running_.store(true);
    thread_ = std::thread([this, call = std::move(call)]() mutable {
        ImportCallbacks callbacks;
        callbacks.progress = [this](const ImportProgress &progress) {
            const std::lock_guard<std::mutex> lock(mutex_);
            pending_.progress = progress;
        };
        callbacks.cancelled = [this] { return cancelled_.load(); };
        ImportReport report = call(callbacks);
        const std::lock_guard<std::mutex> lock(mutex_);
        pending_.finished = std::move(report);
        running_.store(false);
    });
}

void WorkerJobs::startImport(const ImportRequest &request) {
    start([this, request](const ImportCallbacks &callbacks) {
        return library_.importGame(request, callbacks);
    });
}

void WorkerJobs::startCheck(const std::string &gameDir, const InstallManifest &manifest) {
    start([this, gameDir, manifest](const ImportCallbacks &callbacks) {
        return library_.verifyImportedGame(gameDir, manifest, callbacks);
    });
}

void WorkerJobs::cancel() { cancelled_.store(true); }

bool WorkerJobs::running() const { return running_.load(); }

JobEvents WorkerJobs::take() {
    JobEvents events;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        events = std::move(pending_);
        pending_ = JobEvents{};
    }
    if (events.finished) {
        join();
    }
    return events;
}

void WorkerJobs::join() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

} // namespace sfcop::app::launcher
