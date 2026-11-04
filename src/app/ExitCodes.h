// The native client's exit codes (DESIGN §6.8).
#pragma once

namespace sfcop::app {

enum ExitCode : int {
    kExitSuccess = 0,
    // Not in §6.8: a run that started and then failed (a boot fault, a watchdog that expired
    // before its mark). The boot platform already returns it.
    kExitFailure = 1,
    // An unknown flag, a missing or malformed value, or flags that cannot go together.
    kExitBadInput = 2,
    // No install found, or a file of the install missing or differing from 2.5.5.2.
    kExitInstallRefused = 3,
    // The replay oracle's trace differs from the expected one.
    kExitOracleMismatch = 4,
};

// One line per code, for `--help`.
struct ExitCodeText {
    ExitCode code;
    const char *meaning;
};

inline constexpr ExitCodeText kExitCodeTexts[] = {
    {kExitSuccess, "success"},
    {kExitFailure, "the game started and then failed"},
    {kExitBadInput, "bad input on the command line"},
    {kExitInstallRefused, "install not found or verification failed"},
    {kExitOracleMismatch, "oracle mismatch"},
};

} // namespace sfcop::app
