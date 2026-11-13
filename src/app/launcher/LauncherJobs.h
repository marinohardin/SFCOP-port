// The launcher's jobs on a worker thread (Launcher.h, Jobs). The import library's calls block
// for seconds to minutes, so each runs on a thread of its own; its progress callback, which runs
// on that thread once per MiB, only stores the latest progress, and its cancel callback reads an
// atomic flag the window's thread sets. The window's thread takes what arrived once a frame and
// hands it to the Launcher, so the Launcher only ever runs on the window's thread.
#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "app/Import.h"
#include "app/ImportFiles.h"
#include "app/launcher/Launcher.h"

namespace sfcop::app::launcher {

// The two calls of the import library, behind an interface so that the tests can stand in.
class Library {
public:
    virtual ~Library() = default;
    virtual ImportReport importGame(const ImportRequest &request,
                                    const ImportCallbacks &callbacks) = 0;
    virtual ImportReport verifyImportedGame(const std::string &gameDir,
                                            const InstallManifest &manifest,
                                            const ImportCallbacks &callbacks) = 0;
};

// The library on `files` (hostImportFiles() for the real file system).
class FilesLibrary final : public Library {
public:
    explicit FilesLibrary(ImportFiles &files) : files_(files) {}
    ImportReport importGame(const ImportRequest &request,
                            const ImportCallbacks &callbacks) override;
    ImportReport verifyImportedGame(const std::string &gameDir, const InstallManifest &manifest,
                                    const ImportCallbacks &callbacks) override;

private:
    ImportFiles &files_;
};

// What a job sent since the last take().
struct JobEvents {
    // The latest progress only: the window draws one frame at a time.
    std::optional<ImportProgress> progress;
    // Set once, when the job has ended; its thread has then been joined.
    std::optional<ImportReport> finished;
};

class WorkerJobs final : public Jobs {
public:
    explicit WorkerJobs(Library &library) : library_(library) {}
    // Cancels a running job and waits for it.
    ~WorkerJobs() override;
    WorkerJobs(const WorkerJobs &) = delete;
    WorkerJobs &operator=(const WorkerJobs &) = delete;

    // Ignored while a job runs, or while a finished job's report waits to be taken: the Launcher
    // never asks for two.
    void startImport(const ImportRequest &request) override;
    void startCheck(const std::string &gameDir, const InstallManifest &manifest) override;
    void cancel() override;

    // On the window's thread.
    JobEvents take();
    bool running() const;

private:
    template <typename Call> void start(Call call);
    void join();

    Library &library_;
    std::thread thread_;
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> running_{false};
    mutable std::mutex mutex_;
    JobEvents pending_;
};

} // namespace sfcop::app::launcher
