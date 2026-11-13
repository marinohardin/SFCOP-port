// The launcher's steps file (`--launcher-steps`).
#include "app/launcher/LauncherSteps.h"

#include <doctest/doctest.h>

namespace sfcop::app::launcher {

TEST_CASE("a steps file reads every kind of step, skipping blanks and comments") {
    const ParsedSteps parsed = parseLauncherSteps("# the first run\r\n"
                                                  "wait welcome\n"
                                                  "\n"
                                                  "  type /games/Orion Pirates  \n"
                                                  "press use-path\n"
                                                  "wait working\n"
                                                  "frames 30\n"
                                                  "shot shots/working.bmp\n"
                                                  "choose /games/Orion Pirates\n"
                                                  "close");
    REQUIRE(parsed.error.empty());
    REQUIRE(parsed.steps.size() == 8);
    CHECK(parsed.steps[0].kind == StepKind::kWait);
    CHECK(parsed.steps[0].screen == Screen::kWelcome);
    CHECK(parsed.steps[0].line == 2);
    CHECK(parsed.steps[1].kind == StepKind::kType);
    CHECK(parsed.steps[1].text == "/games/Orion Pirates");
    CHECK(parsed.steps[1].line == 4);
    CHECK(parsed.steps[2].kind == StepKind::kPress);
    CHECK(parsed.steps[2].action == Action::kUseTypedPath);
    CHECK(parsed.steps[3].screen == Screen::kWorking);
    CHECK(parsed.steps[4].kind == StepKind::kFrames);
    CHECK(parsed.steps[4].frames == 30);
    CHECK(parsed.steps[5].kind == StepKind::kShot);
    CHECK(parsed.steps[5].text == "shots/working.bmp");
    CHECK(parsed.steps[6].kind == StepKind::kChoose);
    CHECK(parsed.steps[6].text == "/games/Orion Pirates");
    CHECK(parsed.steps[7].kind == StepKind::kClose);
    CHECK(parsed.steps[7].line == 10);
}

TEST_CASE("every button a steps file can press has its name") {
    const ParsedSteps parsed = parseLauncherSteps(
        "press use-path\npress cancel\npress play\npress check\npress change\npress back\n"
        "press another\npress retry\npress quit\n");
    REQUIRE(parsed.error.empty());
    const Action expected[] = {Action::kUseTypedPath, Action::kCancel,     Action::kPlay,
                               Action::kCheck,        Action::kChangeGame, Action::kBackToGame,
                               Action::kChooseAnother, Action::kTryAgain,  Action::kQuit};
    REQUIRE(parsed.steps.size() == 9);
    for (std::size_t i = 0; i < 9; ++i) {
        CHECK(parsed.steps[i].action == expected[i]);
    }
}

TEST_CASE("a wrong step names its line and keeps no steps") {
    CHECK(parseLauncherSteps("wait welcome\nwait closing\n").error ==
          "line 2: wait takes welcome, working, result or ready, not 'closing'");
    CHECK(parseLauncherSteps("frames 0\n").error ==
          "line 1: frames takes a count from 1 to 100000, not '0'");
    CHECK(parseLauncherSteps("frames 100001\n").error ==
          "line 1: frames takes a count from 1 to 100000, not '100001'");
    CHECK(parseLauncherSteps("frames -3\n").error ==
          "line 1: frames takes a count from 1 to 100000, not '-3'");
    CHECK(parseLauncherSteps("choose\n").error == "line 1: choose needs a path");
    CHECK(parseLauncherSteps("shot\n").error == "line 1: shot needs a path");
    CHECK(parseLauncherSteps("press fire\n").error ==
          "line 1: press takes use-path, cancel, play, check, change, back, another, retry or "
          "quit, not 'fire'");
    CHECK(parseLauncherSteps("close now\n").error == "line 1: unknown step 'close now'");
    const ParsedSteps parsed = parseLauncherSteps("wait welcome\njump\n");
    CHECK(parsed.error == "line 2: unknown step 'jump'");
    CHECK(parsed.steps.empty());
}

TEST_CASE("an empty type clears the path field") {
    const ParsedSteps parsed = parseLauncherSteps("type\n");
    REQUIRE(parsed.error.empty());
    REQUIRE(parsed.steps.size() == 1);
    CHECK(parsed.steps[0].text.empty());
}

} // namespace sfcop::app::launcher
