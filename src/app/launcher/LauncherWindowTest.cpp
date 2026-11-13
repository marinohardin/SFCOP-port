// The launcher's window on SDL's dummy video driver, so that it runs without a display: it draws,
// plays a steps file, reports what the steps did and, whatever the way out, leaves the video
// subsystem as it found it for the game's own window.
#include "app/launcher/LauncherWindow.h"

#include <SDL3/SDL.h>
#include <doctest/doctest.h>

#include <memory>
#include <sstream>
#include <string>

#include "app/FilesMock.h"
#include "app/ImportFilesMock.h"

namespace sfcop::app::launcher {

namespace {

// The video driver for one test, reset however the test ends.
struct VideoDriver {
    explicit VideoDriver(const char *name) { SDL_SetHint(SDL_HINT_VIDEO_DRIVER, name); }
    ~VideoDriver() { SDL_ResetHint(SDL_HINT_VIDEO_DRIVER); }
    VideoDriver(const VideoDriver &) = delete;
    VideoDriver &operator=(const VideoDriver &) = delete;
};

struct DummyVideo : VideoDriver {
    DummyVideo() : VideoDriver("dummy") {}
};

struct WindowRig {
    testing::MemoryFileSystem files;
    testing::ControlledImportFiles importFiles;
    LauncherRequest request;

    WindowRig() {
        request.dataDir = "/home/TestCaptain/.local/share/SFCOP";
        request.gameDir = "/home/TestCaptain/.local/share/SFCOP/game";
        request.hashListPath = "/sfcop/data/retail_hashes.sha256";
        request.platform = Platform::kMacos;
        request.env = [](const std::string &) { return std::optional<std::string>(); };
        request.files = &files;
        request.importFiles = &importFiles;
        files.dirs.insert(request.dataDir);
    }

    LauncherChoice run(const std::string &steps, std::string &messages) {
        ParsedSteps parsed = parseLauncherSteps(steps);
        REQUIRE(parsed.error.empty());
        WindowOptions options;
        options.steps = parsed.steps;
        options.stepsName = "steps.txt";
        std::ostringstream err;
        const LauncherChoice choice = runLauncherWindow(request, options, err);
        messages = err.str();
        return choice;
    }
};

} // namespace

TEST_CASE("the launcher's window quits from Welcome and gives the video subsystem back") {
    DummyVideo video;
    WindowRig rig;
    std::string messages;
    CHECK(rig.run("wait welcome\nframes 3\npress quit\n", messages) == LauncherChoice::kQuit);
    CHECK(SDL_WasInit(SDL_INIT_VIDEO) == 0);
    CHECK(messages == "sfcop: the launcher's window is closed\n");
    CHECK(rig.files.written.empty());
}

TEST_CASE("nothing of the launcher's window is left for the game's window to meet") {
    DummyVideo video;
    WindowRig rig;
    std::string messages;
    // Events the launcher left unread would be the game's first: a press, a drop, a close.
    REQUIRE(rig.run("wait welcome\nframes 5\nclose\n", messages) == LauncherChoice::kQuit);
    CHECK(SDL_WasInit(SDL_INIT_VIDEO) == 0);
    REQUIRE(SDL_InitSubSystem(SDL_INIT_VIDEO));
    int windows = -1;
    SDL_Window **open = SDL_GetWindows(&windows);
    SDL_free(open);
    CHECK(windows == 0);
    SDL_PumpEvents();
    SDL_Event event;
    int left = 0;
    while (SDL_PollEvent(&event)) {
        const bool fromLauncher =
            (event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST) ||
            (event.type >= SDL_EVENT_KEY_DOWN && event.type <= SDL_EVENT_TEXT_INPUT) ||
            (event.type >= SDL_EVENT_MOUSE_MOTION && event.type <= SDL_EVENT_MOUSE_WHEEL) ||
            (event.type >= SDL_EVENT_DROP_FILE && event.type <= SDL_EVENT_DROP_POSITION) ||
            event.type == SDL_EVENT_QUIT ||
            (event.type >= SDL_EVENT_RENDER_TARGETS_RESET &&
             event.type <= SDL_EVENT_RENDER_DEVICE_LOST);
        left += fromLauncher ? 1 : 0;
    }
    CHECK(left == 0);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

TEST_CASE("the launcher's window shows a refused import and quits from its result") {
    DummyVideo video;
    WindowRig rig;
    std::string messages;
    // The manifest beside the hash list cannot be read, so the import is refused as the client's
    // fault before any folder is read.
    CHECK(rig.run("wait welcome\nchoose /games/nowhere\nwait result\npress quit\n", messages) ==
          LauncherChoice::kQuit);
    CHECK(rig.importFiles.changes == 0);
    CHECK(SDL_WasInit(SDL_INIT_VIDEO) == 0);
}

TEST_CASE("a step the screen cannot do ends the launcher with the line named") {
    DummyVideo video;
    WindowRig rig;
    std::string messages;
    CHECK(rig.run("wait welcome\n\npress play\n", messages) == LauncherChoice::kFailed);
    CHECK(messages.find("sfcop: steps.txt line 3: the screen shows no such button\n") !=
          std::string::npos);
    CHECK(SDL_WasInit(SDL_INIT_VIDEO) == 0);
}

TEST_CASE("with no video driver the launcher says no window can be opened") {
    VideoDriver driver("no-such-driver");
    WindowRig rig;
    std::string messages;
    CHECK(rig.run("press quit\n", messages) == LauncherChoice::kNoWindow);
    CHECK(messages.find("sfcop: the launcher's window cannot open: ") == 0);
    CHECK(SDL_WasInit(SDL_INIT_VIDEO) == 0);
}

TEST_CASE("a folder picker that answers before it returns does not freeze the launcher") {
    // SDL calls back on the calling thread when it cannot show a picker at all; the call that
    // opened it must not still hold the inbox's lock then.
    const auto inbox = std::make_shared<PickerInbox>();
    int shown = 0;
    openFolderPicker(inbox, [&](SDL_DialogFileCallback callback, void *userdata) {
        ++shown;
        SDL_SetError("no folder picker here");
        callback(userdata, nullptr, -1);
    });
    CHECK(shown == 1);
    CHECK_FALSE(inbox->open);
    REQUIRE(inbox->failed.has_value());
    CHECK(*inbox->failed == "no folder picker here");
    CHECK(inbox.use_count() == 1);
}

TEST_CASE("a second press of the folder button while the picker is open opens no other") {
    const auto inbox = std::make_shared<PickerInbox>();
    int shown = 0;
    void *pending = nullptr;
    SDL_DialogFileCallback answer = nullptr;
    const ShowFolderPicker show = [&](SDL_DialogFileCallback callback, void *userdata) {
        ++shown;
        answer = callback;
        pending = userdata;
    };
    openFolderPicker(inbox, show);
    openFolderPicker(inbox, show);
    CHECK(shown == 1);
    const char *const chosen[] = {"/games/Orion", nullptr};
    answer(pending, chosen, -1);
    CHECK_FALSE(inbox->open);
    REQUIRE(inbox->chosen.has_value());
    CHECK(*inbox->chosen == "/games/Orion");
    openFolderPicker(inbox, show);
    CHECK(shown == 2);
    answer(pending, nullptr, -1);
}

TEST_CASE("the steps are read from their file only when the launcher opens") {
    std::ostringstream err;
    const std::optional<WindowOptions> none = readWindowOptions("", err);
    REQUIRE(none.has_value());
    CHECK(none->steps.empty());
    CHECK(err.str().empty());

    const std::string missing = "/nonexistent/sfcop-l2b/launcher.steps";
    CHECK_FALSE(readWindowOptions(missing, err).has_value());
    CHECK(err.str() == "sfcop: cannot read the launcher steps " + missing + "\n");

    // The client's launcher reads them when it is called, not when it is made: an unreadable file
    // ends that launcher as a failed one, and a run that never opens it never reads it.
    DummyVideo video;
    WindowRig rig;
    const auto launch = windowLauncher(missing);
    CHECK(launch(rig.request) == LauncherChoice::kFailed);
    CHECK(SDL_WasInit(SDL_INIT_VIDEO) == 0);
}

} // namespace sfcop::app::launcher
