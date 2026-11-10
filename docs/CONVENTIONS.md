# SFCOP-decomp — Conventions

The standard every change is held to. `DESIGN.md` says what the system does; this says what the code looks like. The automated review reads this and `AGENTS.md`.

---

## 1. Toolchain

| Tool | Convention |
| --- | --- |
| Languages | `game/`: C++ that compiles with VC6 SP4 (the byte check and the 32-bit build) and with clang in C++17 mode (the native build, §4.1); `src/`, `server/`, `tools/`: C++17; tooling: Python 3.11 or later, standard library only unless `third_party/` says otherwise |
| Compilers | VC6 SP4 in the `sfcop-msvc6` container (`toolchain/`); Apple clang on macOS; clang or MSVC on Windows; clang on Linux. Versions in `toolchain/VERSIONS.md` |
| Build | CMake 3.28 or later with Ninja for the native build, the tools and the tests (`cmake --preset <platform>`); `toolchain/` scripts for the VC6 side; `-j4` on the owner's machine |
| Package manager | None. `third_party/` holds pinned copies or pinned FetchContent declarations; Python tooling uses a venv under `work/` with `PIP_CACHE_DIR` there |
| Tests | `python3 -m pytest tests` for tooling; CTest with doctest for C++ (§6); the harness and the oracle through `sh scripts/check.sh` |
| Format | clang-format with the repository's `.clang-format` for `src/`, `server/` and `tools/` only; `game/` keeps the original's style and is never reformatted |
| Lint | None beyond the compiler at `-Wall -Wextra -Werror` for `src/`; `game/` builds with warnings allowed |
| CI | GitHub Actions on the self-hosted runner `sfcop-mac`; the same `scripts/check.sh` as locally |

Versions are pinned in a file in the repository, never "latest".

## 2. Repository layout

```
game/                 the decompiled original, one file per original translation unit at its original path
  Source/...          SFC2_5\Source: the game proper
  Meta/...            the Dynaverse client
  Common/...          Taldren's utility layer (SharedR)
  Switch/...          nSwitch
  dmp3d/, Tiger/      the inherited engine and platform libraries, shared with Empires at War (DESIGN §7.5)
  unsorted/           units whose original file is not known yet (harness/SRC_LAYOUT.md)
  port/               types.h and the few headers the two compile modes need; nothing else
src/
  hle/                the Win32 surface (surface.tsv) implemented on the components: ddraw4/, d3d3/, dinput/, miles/, ds3d/, dplay/, winsock/, kernel/, registry/
  platform/           Window, Gpu, Input, Audio, Transport, Store, Clock, Threads; one directory each, interface in <Name>.h
  net/                PROTOCOL.md, envelope, rendezvous, relay client
  fixes/              fixes.h, switches.def, and overrides/ with one unit per fixed game unit
  app/                main(), command line, install discovery, the guest runtime glue
server/               sfcop-lobby, sfcop-relay, directory and auth stand-ins; one directory each with its own tests
tools/                scr2native, the lifter driver, build helpers
third_party/          pinned dependencies and INVENTORY.md; integrator only
harness/, oracle/, toolchain/, analysis/, ghidra/   the decompilation's judges and evidence (hotspots where marked)
scripts/              check.sh, progress.py, the small scripts the documents name
tests/                Python tests for the tooling
docs/                 DESIGN.md, CONVENTIONS.md, PLAN.md, FIXES.md
spike/                Phase 0 and 1 spikes, frozen; read, never extended
```

What may include what: `game/` includes only `game/`, by paths from the `game/` root (§4.1); `src/hle/` includes `src/platform/` headers and the `game/` headers a layout needs; `src/platform/` includes `third_party/` and the C++ standard library, never Win32; `src/app/` includes anything in `src/`; `server/` includes `src/net/` and `third_party/`; `tools/` includes `src/` where it must. Generated files (lifted code, objects, traces under test, build directories) live under `work/` or `build/`, never in git.

## 3. Naming

Files

| Kind | Convention | Example |
| --- | --- | --- |
| Decompiled unit | The original path and file name, drive and project root dropped | `game/Common/DynamicAllocator.cpp` |
| Unknown-origin unit | `game/unsorted/<Class>.cpp`, or `fn_<page>.cpp` for free functions by 4 KB page | `game/unsorted/tShipYard.cpp` |
| New C++ source | One class or component per file, named after it, UpperCamelCase | `src/platform/Audio/Audio.h`, `src/hle/miles/Miles.cpp` |
| Tests | `<Unit>Test.cpp` beside the code for C++; `tests/test_<topic>.py` for Python | `src/net/EnvelopeTest.cpp` |
| Documents and data | `UPPER.md` for the documents the workflow names, lowercase for data | `docs/FIXES.md`, `src/hle/surface.tsv` |

Identifiers

| Kind | Convention | Example |
| --- | --- | --- |
| Original classes, functions, members | The original identifier, exactly, once evidence gives it (a symbol, RTTI, an assertion string, an SDK match); `fn_<address>` until then | `tDynamicAllocator::Instance`, `fn_006FC690` |
| Names that exist only in a leaked or unlicensed source | Never used; a descriptive name of this project's own instead | |
| New classes and types in `src/`, `server/`, `tools/` | UpperCamelCase, no prefix | `Transport`, `Envelope` |
| New functions and methods | UpperCamelCase for methods, lowerCamelCase for free functions and locals | `Audio::PlayVoice`, `openInstall()` |
| Constants and enumerators | `k` prefix UpperCamelCase, as the original does | `kMaxPlayers` |
| Namespaces | `sfcop::<component>` for new code; the original's `n*` namespaces untouched | `sfcop::hle` |
| Fix switches | lower_snake_case, verb first | `fix_shield_overflow` |
| Macros | `SFCOP_` prefix | `SFCOP_FIX(fix_shield_overflow)` |

Branches follow §7.1. Commit scopes are the top-level directory or the component: `feat(hle/dinput): ...`, `fix(game/Switch): ...`, `docs(design): ...`.

## 4. Code style

### 4.1 Decompiled code (`game/`)

- The goal is the original's source, so the style is the original's: its spacing, its idioms, its member order. The matched SDK sources (`Common/`, the scripting library) show that style; follow it.
- Every function carries its `// FUNCTION: STARFLEETOP 0x...` or `// STUB:` line (DESIGN §6.3). Functions appear in address order inside a unit.
- A function VC6 generates from a global object's definition, with no source line of its own, is claimed by annotations stacked directly above that definition (`tINIMgr g_tINIMgr;`), never by a stand-in function: counted upwards from the definition, the dynamic initializer (whose annotation is the one directly above the line), the atexit registration, the atexit destructor (`tools/manifest.py`).
- Includes are paths from the `game/` root: `#include "Common/DynamicAllocator.h"`, with `game/` and `game/port/` the only include directories (owner's decision, 2026-11-04). The original wrote bare file names with a long list of include directories; its include lines are rewritten to this form when a unit is imported, and that rewrite is the one standing exception to "the original's source", since include lines change no code bytes.
- Only what VC6 SP4 compiles: no C++11 or later, no `long long`, no anonymous namespaces where the original had none. When the guest mode is retired the whole tree compiles with clang in C++17 mode with `-fms-extensions`; a construct clang would reject is fixed with a `PORT:` line when it is written, never by forking the file.
- A `PORT:` line is one line, tagged on the line itself: `// PORT: <reason>`. The reasons allowed: 64-bit or pointer-size hygiene, undefined behavior the original relied on, a platform difference the HLE surface cannot absorb. Everything else belongs in `src/`, or in an override unit under `src/fixes/overrides/` behind a fix switch.
- No fix is written into `game/`: no `#if SFCOP_FIX(name)`, no changed line. A fix to a game function is a copy of that function in `src/fixes/overrides/<Dir>/<unit>.cpp`, annotated `// OVERRIDE: STARFLEETOP 0x... fix_name`, in this same style, with the change and a comment on the line that differs (DESIGN §6.9). The byte check never sees it, so no fix changes a function's `FUNCTION:` status.
- Pointer-typed members and `long` keep the original's VC6 types. The native build takes the VC6 object of a unit and lifts it (DESIGN §7.2, §12.4), so `game/` is never compiled in a guest mode; `game/port/types.h` is the fallback route's header and nothing in `game/` depends on it.
- No comment restates the code. A comment records evidence (`// matches SharedR.lib DynamicAllocator.obj`), a judgement (`// the original reads one past the end here; kept, see fix_...`) or a `PORT:` reason.

### 4.2 New code (`src/`, `server/`, `tools/`)

- C++17, `-Wall -Wextra -Werror`, clang-format. RAII everywhere; no raw `new` outside the HLE objects that mirror COM lifetimes, and those are commented as such.
- No C++ exception crosses the Win32 surface in either direction: HLE entry points catch, log and return the Win32 error the game expects. Exceptions inside `src/` are for programming errors only; expected failures return a value.
- Logging through one facility (`sfcop::log`), with a component tag and a level; nothing identifying is logged (DESIGN §9). A log line is a sentence, not a dump.
- A function longer than about 60 lines, or a file longer than about 600, is a sign to split; say so in review rather than enforce a number.
- Every HLE entry point states its calling convention and purges its stack as the original import did; `surface.tsv` is updated in the same commit.
- Platform-specific code lives in a file named for the platform (`Window_macos.mm`, `Store_windows.cpp`) behind the component's one header; no `#ifdef` forests in shared files.

## 5. Dependencies

- Declared in `third_party/INVENTORY.md` (name, version, licence, where used, why) and pinned there and in the build (`FetchContent` with a release URL and its SHA-256, or a commit hash, or a vendored copy). Only the integrator adds, removes or bumps one, in its own pull request labelled `contract-change`.
- Permissive licences only until DESIGN §12.1 is decided: zlib, MIT, BSD, public domain. Nothing derived from the WONAPI or GameSpy sources, ever.
- Current set: SDL3, miniaudio, doctest (tests). NAT traversal is the project's own (DESIGN §12.8). Python tooling uses the standard library; a package it needs is recorded in the venv's requirements file under `tools/` and in the inventory.
- A dependency is taken for a capability the project cannot reasonably write (a GPU abstraction, an audio engine), never for convenience.

## 6. Testing

- Tests sit in `tests/` for the Python tooling; for the port, beside the code or where the plan phase decides.
- There is no line-coverage threshold. Progress is measured by `scripts/progress.py` as percent of original code bytes that are byte-exact, functionally verified, or still lifted, plus percent of functions named; no pull request may lower the exact or functional figure for any function, compared by original address.
- Every new feature and every bug fix comes with tests. A bug fix comes with a test that fails without the fix.
- Test names say what is guaranteed, not what function is called.
- No network, no real external services, no real clock in unit tests. Pass time in or fake it.
- No real personal or confidential data in fixtures. Fixtures are plainly invented.
- Expected values are written out, not computed by calling the code under test.
- C++ tests use doctest through CTest: one `<Unit>Test.cpp` beside the unit, run by `ctest --preset <platform>`; `scripts/check.sh` runs them when the native build is configured on the machine.
- Decompiled code is tested by the harness (byte exact) and the oracle (behavior), not by unit tests; a unit test for game code is written only for an override unit, in both positions of its switch.
- A test that needs the retail install is marked (`pytest.mark.retail`, `TEST_CASE("..." * doctest::skip(!haveInstall()))`) and is skipped, not failed, where the install is absent; CI has it.
- Fixtures are invented: addresses in the game's range, names like `TestCaptain`, addresses from the RFC 5737 ranges, keys in the game's format but made up.

## 7. Git and pull requests

### 7.1 Branches

| Branch | Purpose |
| --- | --- |
| `main` | What is released |
| `develop` | Integrated work. The default branch and the base for all new work |
| `feature/<slug>` | Longer-running work. Receives small pull requests; merged into `develop` when the whole thing is done |
| `feat/<slug>`, `fix/<slug>` | One pull request's worth of work |
| `wp/<id>-<slug>` | A work package from PLAN.md |

- **Never commit to `main` or `develop` directly.** Both accept changes only through pull requests. The hooks in `.githooks/`, switched on once per clone with `sh .githooks/install.sh`, refuse such a commit or push.
- **Never work in the main checkout.** Every change is done in its own git worktree, on its own branch cut from `develop`. Worktrees are created by Claude Code under `.claude/worktrees/` and the branch is then renamed, as PLAN.md describes.
- **Releases** are a pull request from `develop` into `main`, merged with a merge commit, never squashed.

### 7.2 Commits

- Small and atomic. One logical change per commit.
- Conventional Commits with the module as scope. Imperative subject, 72 characters or fewer. The body says why when the diff does not.
- The tests for a change are in the same commit as the change.
- Pull requests are squash-merged: what lands on `develop` is one commit per pull request, named after it.

### 7.3 Pull requests

- Atomic: one concern each. Small enough to review in one sitting, or split, or sent through a `feature/` branch in pieces.
- `sh scripts/check.sh` passes before pushing.
- Opened as a draft; marked ready when the checks pass, which starts the automated review. Never left in draft when handed to the owner. Nobody comments `@claude` to ask for a review.
- The checks are `check` and `hotspots`. A pull request is merged only when both are green. The automated review is advisory: read it, fix what is right, say why when it is not.
- Title: a Conventional Commits subject. Description: what changed, why, how it was tested.
- A pull request that changes behavior described in DESIGN.md or a rule in this document updates that document in the same pull request.
- Never commit generated output, secrets, the owner's local configuration or anything under `.agent/`.

## 8. Checklists

**Decompiling a function**
1. Find it in `ghidra/exports/functions.csv`; take the unit path from its evidence, or `game/unsorted/`.
2. Write it at its address position in the unit with its `// FUNCTION:` line and the original identifiers.
3. `harness/build-and-verify.sh 0x<address>` until exact, or stop at the attempt cap and record it in `difficult_functions.tsv` through the harness.
4. `harness/update-matched-set.sh 0x<address>`; `python3 scripts/progress.py --gate`.
5. Commit: `feat(game/<dir>): decompile <Class>::<Method>`; the matched set change in the same commit.

**Closing a grind batch**
1. Once the batch's pull requests are merged, on a branch cut from `develop`: `python3 scripts/progress.py --markdown --matched-set analysis/results/matched_set.json`, then `python3 tools/progress_chart.py`. Nothing in the output is edited by hand, and a warning from the second script is fixed, not ignored.
2. Commit the progress files, the README badges and the treemap together: `chore(progress): regenerate after <batch>`, with the before and after figures in the body.
3. The progress files are regenerated here and nowhere else: no CI job writes them, and another pull request does so only when it changes how they are computed. Batch pull requests then never conflict over them.

**Adding an HLE entry (a Win32 function or COM method the game calls)**
1. Row in `src/hle/surface.tsv`: module, name, convention, status.
2. Implementation in the module's directory, on the component interface only; calling convention and stack purge stated.
3. A doctest that calls it as the game would (arguments, return, out-parameters) and one for its error path.
4. Commit: `feat(hle/<module>): implement <Name>`.

**Adding a platform component or backend**
1. Interface in `src/platform/<Name>/<Name>.h` first, with a mock in `<Name>Mock.h`; this is a contract change if the header exists already.
2. One backend file per platform; a doctest per method through the interface.
3. `third_party/INVENTORY.md` if it brings a dependency (integrator).

**Adding a fix**
1. Row in `docs/FIXES.md`: name, default, origin, behaviors, tests.
2. The switch declared in `src/fixes/switches.def` (DESIGN §6.9). A `runtime` fix reads it in `src/` with `isEnabled`. A `build` fix is an override unit: copy the function from `game/` into `src/fixes/overrides/<Dir>/<unit>.cpp` with `// OVERRIDE: STARFLEETOP 0x... fix_<name>` on the line before it, make the change, and comment the changed line. `game/` is not touched.
3. A test in each position; the byte check is unchanged, since it never compiles an override.
4. Commit: `fix(fixes): <what> (fix_<name>)`, or `fix(hle/<module>)` for a runtime fix below the surface.

**Adding a message to the network protocol**
1. The message in `src/net/PROTOCOL.md` with its fields, version and the behavior it carries (DESIGN §5.11).
2. Encode and decode with bounds checks; a doctest with a hand-written byte sequence each way and one malformed case.
3. A contract change: the integrator merges the document first.

**Recording an oracle trace**
1. On the recording machine, `oracle/README.md`; the mission and input script committed under `oracle/scr/` first.
2. The trace to `oracle/expected/<mission>/<input>.trace` with its header; `sh scripts/check.sh` must pass against it on the delinked build.
3. Commit: `test(oracle): record <mission>/<input>`. A re-recording says why in the body.
