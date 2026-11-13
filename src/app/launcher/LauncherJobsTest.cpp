// The launcher's jobs on their worker thread, over a library that holds each call until it is
// cancelled or let go, so nothing here waits on a clock.
#include "app/launcher/LauncherJobs.h"

#include <doctest/doctest.h>

#include <atomic>
#include <string>
#include <thread>

namespace sfcop::app::launcher {

namespace {

class HeldLibrary final : public Library {
public:
    std::atomic<bool> letGo{false};
    std::atomic<int> calls{0};
    std::string lastGameDir;

    ImportReport importGame(const ImportRequest &request,
                            const ImportCallbacks &callbacks) override {
        lastGameDir = request.gameDir;
        return hold(ImportPhase::kCopying, callbacks);
    }
    ImportReport verifyImportedGame(const std::string &gameDir, const InstallManifest &,
                                    const ImportCallbacks &callbacks) override {
        lastGameDir = gameDir;
        return hold(ImportPhase::kVerifying, callbacks);
    }

private:
    ImportReport hold(ImportPhase phase, const ImportCallbacks &callbacks) {
        ++calls;
        ImportProgress progress;
        progress.phase = phase;
        progress.path = "Assets/first.mod";
        progress.bytesDone = 1;
        progress.bytesTotal = 4;
        callbacks.progress(progress);
        progress.path = "Assets/second.mod";
        progress.bytesDone = 2;
        callbacks.progress(progress);
        ImportReport report;
        while (!letGo.load()) {
            if (callbacks.cancelled()) {
                report.status = ImportStatus::kCancelled;
                return report;
            }
            std::this_thread::yield();
        }
        report.status = phase == ImportPhase::kVerifying ? ImportStatus::kVerified
                                                         : ImportStatus::kImported;
        return report;
    }
};

// Takes what the job sent until it has finished, keeping the last progress seen.
JobEvents untilFinished(WorkerJobs &jobs) {
    JobEvents seen;
    while (!seen.finished) {
        JobEvents events = jobs.take();
        if (events.progress) {
            seen.progress = events.progress;
        }
        seen.finished = events.finished;
        std::this_thread::yield();
    }
    return seen;
}

ImportProgress firstProgress(WorkerJobs &jobs) {
    for (;;) {
        const JobEvents events = jobs.take();
        if (events.progress) {
            return *events.progress;
        }
        std::this_thread::yield();
    }
}

} // namespace

TEST_CASE("an import on the worker reports its latest progress and how it ended") {
    HeldLibrary library;
    WorkerJobs jobs(library);
    ImportRequest request;
    request.gameDir = "/home/TestCaptain/.local/share/SFCOP/game";
    jobs.startImport(request);
    CHECK(jobs.running());
    const ImportProgress first = firstProgress(jobs);
    CHECK(first.phase == ImportPhase::kCopying);
    library.letGo = true;
    const JobEvents seen = untilFinished(jobs);
    REQUIRE(seen.finished);
    CHECK(seen.finished->status == ImportStatus::kImported);
    CHECK_FALSE(jobs.running());
    CHECK(library.lastGameDir == "/home/TestCaptain/.local/share/SFCOP/game");
    // Taken once: nothing more arrives.
    const JobEvents after = jobs.take();
    CHECK_FALSE(after.finished);
    CHECK_FALSE(after.progress);
}

TEST_CASE("cancel stops a running check, which reports that it was cancelled") {
    HeldLibrary library;
    WorkerJobs jobs(library);
    jobs.startCheck("/home/TestCaptain/.local/share/SFCOP/game", InstallManifest{});
    CHECK(firstProgress(jobs).phase == ImportPhase::kVerifying);
    jobs.cancel();
    const JobEvents seen = untilFinished(jobs);
    REQUIRE(seen.finished);
    CHECK(seen.finished->status == ImportStatus::kCancelled);
}

TEST_CASE("a second job is refused while one runs, and a new one starts uncancelled") {
    HeldLibrary library;
    WorkerJobs jobs(library);
    jobs.startImport(ImportRequest{});
    firstProgress(jobs);
    jobs.startCheck("/elsewhere", InstallManifest{}); // ignored: the import still runs
    jobs.cancel();
    CHECK(untilFinished(jobs).finished->status == ImportStatus::kCancelled);
    CHECK(library.calls == 1);
    library.letGo = true;
    jobs.startCheck("/home/TestCaptain/.local/share/SFCOP/game", InstallManifest{});
    CHECK(untilFinished(jobs).finished->status == ImportStatus::kVerified);
    CHECK(library.calls == 2);
}

TEST_CASE("a finished job's report is not lost to a job started before it is taken") {
    HeldLibrary library;
    WorkerJobs jobs(library);
    library.letGo = true;
    jobs.startImport(ImportRequest{});
    while (jobs.running()) {
        std::this_thread::yield();
    }
    jobs.startCheck("/elsewhere", InstallManifest{}); // ignored: the import's report waits
    const JobEvents events = jobs.take();
    REQUIRE(events.finished);
    CHECK(events.finished->status == ImportStatus::kImported);
    CHECK(library.calls == 1);
}

TEST_CASE("the jobs going away cancel a running job and wait for it") {
    HeldLibrary library;
    {
        WorkerJobs jobs(library);
        jobs.startImport(ImportRequest{});
        firstProgress(jobs);
    } // would hang here if the job were not cancelled
    CHECK(library.calls == 1);
}

} // namespace sfcop::app::launcher
