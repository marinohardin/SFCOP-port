# src/app: the client's front door

What runs before the game: the command line, the per-user data folder, finding the install and
verifying it. DESIGN §5.1, §5.3, §6.6, §6.8 and §8 define the behavior.

| File | What it does |
| --- | --- |
| `CommandLine.h` | Parses the flags of DESIGN §6.8 from one table, which also generates `--help`. A caller can add its own flags (`ExtraFlag`), which are listed and passed through unchecked. An unknown flag, a missing or malformed value, a flag given twice, `--windowed` with `--fullscreen`, or `--oracle` without `--input` (or the reverse) is bad input, with the flag named |
| `ExitCodes.h` | 0 success, 1 the game started and then failed, 2 bad input, 3 install not found or refused, 4 oracle mismatch |
| `Install.h` | Reads the hash list (sha256sum's format), picks out the install's entries, finds the install (`--install`, else the remembered path, else the caller's and the platform's usual folders, else asks once) and verifies every listed file by SHA-256, read in 1 MiB pieces with a progress callback. The first missing, unreadable or different file, in list order, is refused by name with exit 3, and the message says to reinstall and re-patch to 2.5.5.2 |
| `DataDir.h` | The per-user data folder on each platform (`--data` overrides it), its subfolders `settings/`, `saves/`, `replays/`, `logs/` and `mods/`, created on first use, and `install.txt`, the install path remembered after the first verified run |
| `Startup.h` | All of the above in order, for `main()` |
| `Sha256.h` | SHA-256, fed in pieces |
| `HostPlatform.h` | The platform and its environment, one file per platform, passed in so every platform's rules are tested everywhere |
| `Files.h` | **Temporary.** The few file operations these need, until the Store component's header (`src/platform/Store/Store.h`) lands; the integrator replaces it with Store's at gate G0. `FilesMock.h` holds the in-memory test double |

The install is only ever read. Nothing here writes outside the data folder.

## Wiring into `main.cpp` (not done yet)

`src/app/main.cpp` and the build configuration belong to F1, and the integrator wires this
package in. Until then the client calls none of it. Two edits are needed.

**1. The build configuration does not define `SFCOP_HASH_LIST` yet.** F1's build must add this
line to `cmake/sfcop_build_config.h.in` (configured with `@ONLY`):

```c
#define SFCOP_HASH_LIST "@CMAKE_SOURCE_DIR@/analysis/results/retail_hashes.sha256"
```

**2. `main.cpp` hands its command line to `startClient`.** Its own flag parsing and
`printUsage` go away. Its temporary boot-platform flags move into `kBootFlags`, so `--help` lists
them and `startClient` hands them back unchanged in `start.options.passThrough`:

```cpp
#include "Startup.h"

namespace {

// The temporary boot platform's own flags, until Wave 1.
const std::vector<sfcop::app::ExtraFlag> kBootFlags = {
    {"--game", "DIR", "a writable copy of the install (default: the build's copy)"},
    {"--trace", "", "log every Win32 call on stderr"},
    {"--silent", "", "no sound"},
    {"--shots", "DIR", "write screenshots into DIR"},
    {"--shot-every", "S", "seconds between screenshots (default 5)"},
    {"--dry-run", "", "map and bind the exe, then stop"},
};

} // namespace

int main(int argc, char **argv) {
    const sfcop::app::Startup start = sfcop::app::startClient(
        argc, argv, sfcop::app::consoleStartup(SFCOP_HASH_LIST, kBootFlags, {SFCOP_INSTALL_DIR}));
    if (start.exitCode) {
        return *start.exitCode;
    }
    // The boot platform's arguments are then built from `start`, as main.cpp builds them from
    // its own parsing today:
    //   start.options.passThrough   --game DIR sets the copy; --dry-run drops --run; the rest
    //                               (--trace, --silent, --shots DIR, --shot-every S) pass on.
    //                               --shot-every's value is not checked here, so keep isCount()
    //   start.options.headless      "--headless"
    //   start.options.watchdogSeconds > 0
    //                               "--watchdog" and the number
    //   start.installDir            the verified install, in place of SFCOP_INSTALL_DIR in the
    //                               check that --game is not the install itself
    ...
}
```

`consoleStartup` tries the build's install (`SFCOP_INSTALL_DIR`) after the remembered one and
before the platform's usual folders. When it finds none, it asks on the terminal, except with
`--headless`. It prints to `std::cout` and `std::cerr`. `start.dataDir` is the per-user data
folder, already created with its subfolders.

## Hash list

The repository's list has its root above the install: it also names the server kit. The install's
entries are those under the folder of the `StarFleetOP.exe` entry with the most entries beneath
it, taken relative to that folder.

## Tests

`*Test.cpp` beside each unit, run by `ctest --preset macos-arm64`. The invented fixture install
is `tests/fixtures/install/` with its list `tests/fixtures/install.sha256`; none of it is a
retail file. One test verifies the real install against the repository's list and is skipped
unless `SFCOP_INSTALL_DIR` names an install.
