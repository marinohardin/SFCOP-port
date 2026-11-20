# SFCOP-port — Implementation Plan

How to build what [DESIGN.md](DESIGN.md) describes, following [CONVENTIONS.md](CONVENTIONS.md), with as much of the work as possible done in parallel by separate agents in separate git worktrees.

---

## 1. Shape of the plan

The work is split into **work packages**. Each one is a single branch, in a single worktree, done by a single agent, touching a set of paths no other package in the same wave touches.

Work packages are grouped into **waves**. Everything in a wave can run at the same time. A wave starts when the wave before it has merged and its gate has passed.

| Wave | Purpose | Packages | Parallel |
| --- | --- | --- | --- |
| 0A | Native build skeleton and the check wiring | 1 (F1) | No |
| 0B | Contracts and the walking skeleton | 8 (C1 to C8) | Yes |
| 1 | Building blocks: one platform component or HLE module each; the first grind batches | 9 (B1 to B8, D) | Yes |
| 2 | Assemblies: native boot to the menu, skirmish, mission modules, the CI matrix | 5 (A1 to A5) | Mostly |
| 3 | Netplay (Phase 3 of the proposal) | 6 (N1 to N6) | Mostly |
| 4 | Dynaverse (Phase 4) | 3 (V1 to V3) | Partly |
| 5 | Mod manager, launcher, release | 2 (M1, L1) | Yes |

The decompilation grind (D) is not a wave: it is a standing stream of pull requests beside every wave from Wave 1 on. Since the split (2026-11-16) the decompilation (SFC2-decomp) owns `op/` and the matched set; this repository takes its work through a bump of the `decomp` pin.

**Critical path:** F1 → C2 (component interfaces) → B1 (window and GPU) → B2 (DirectDraw 4 and Direct3D 3 on it) → A1 (boot to the menu) → A2 (skirmish) → the owner's oracle recording (§2.2) → gate G2. Everything else runs beside it.

Three ideas make the parallelism work:

1. **Contracts first.** Wave 0B writes every type, interface and schema that two packages will share, and freezes them. After that, packages depend on contracts, not on each other's code.
2. **A walking skeleton.** Wave 0B also creates every module as a compiling stub, fully wired, with `sh scripts/check.sh` passing. Later packages fill in files that already exist. Nobody after Wave 0 needs to edit the wiring, the root configuration or a module's public surface.
3. **Disjoint ownership.** Each package lists the paths it owns. An agent edits nothing else.


---

## 2. Before any code

### 2.1 Decisions

The open questions of DESIGN §12, with what each blocks here.

| # | Decision | Status | Blocks |
| --- | --- | --- | --- |
| 12.1 | Licence: GPL-3.0-or-later (`LICENSE`) | Decided 2026-11-20 | Wave 5 release |
| 12.2 | Native client packages may be published as alpha releases while they carry translated code; unsigned until complete, a signed launcher at the end | Decided 2026-11-09 (supersedes 2026-11-03) | L1, release packaging |
| 12.3 | The key prompt and online key check in the native client | Open (owner) | N6, V1 |
| 12.4 | Guest mode or lift the VC6 object | Decided 2026-11-04: lift the object (DESIGN §12.4); B9 builds the lifter | D in the native build |
| 12.5 | Scripting library sources: committed or copied from the install | Open (owner) | A3, C8 (the build copies until decided) |
| 12.6 | Dynaverse mission transport | N1 analysis | V1 |
| 12.7 | 53-bit floating point reproduces the traces | First oracle run on the native build | "functional" in the native build |
| 12.8 | NAT traversal: decided 2026-11-07, the project's own punch (#60) | N1 | N2 |
| 12.9 | The oracle recording machine | Open (owner) | G2 and every "functional" figure |
| 12.10 | Fix defaults | Owner, as `docs/FIXES.md` fills | Nothing before the fix packages |
| 12.11 | One repository | Decided 2026-11-03 | Nothing |

### 2.2 Things only a person can do

| Task | How | Why only a person | Needed by | Status |
| --- | --- | --- | --- | --- |
| Create the private repository and apply its settings | `gh repo create marinohardin/SFCOP-port --private --source . --push`, then the settings commands in `references/hosting-github.md` §1 of the workflow skill (default branch `develop`, squash merges, `contract-change` label). The integrator shows the commands; the owner says yes | Changes the remote | Before the first pull request | done (2026-11-03 for the combined repository; SFCOP-port at the split, 2026-11-16) |
| Register the SFCOP runner | Download the runner into `$SFCOP_ROOT/tools/actions-runner`; `./config.sh --url https://github.com/marinohardin/SFCOP-port --token <registration token> --name marino-macbook-sfcop --labels sfcop-mac`; `./svc.sh install && ./svc.sh start`. Check with `gh api repos/marinohardin/SFCOP-port/actions/runners`. The two existing runners on this Mac belong to another repository and cannot serve this one. Script: `$SFCOP_ROOT/tools/register-sfcop-runner.sh`. The runner must live on the internal disk (`~/gitdir/actions-runner-sfcop`): a launchd service cannot open external volumes under macOS privacy rules | Runs as the owner's user on their machine | Before the first `check` job | done for the combined repository (2026-11-03); for SFCOP-port, check with the `gh api` command |
| Switch on the git hooks in the main checkout | `sh .githooks/install.sh` from the repo root; then every worktree inherits them | Sets git configuration under the owner's user; agents never run `git config` | Before the next commit | done (2026-11-03: probe commit on `develop` refused) |
| Install the Claude GitHub app and its token secret | `/install-github-app` in an interactive `claude` terminal; decline the offered workflows (the templates are in place) | Signs in as the owner and grants an app | Before the first automated review | open |
| Move Docker's disk image to the external drive and cap it | Docker Desktop, Settings, Resources, Advanced: Disk image location on `/Volumes/External`, virtual disk limit 64 GB | GUI-only setting on the owner's machine | Soon; the internal disk is nearly full | open |
| Record the oracle trace | On a Windows PC or another Wine build, following `oracle/README.md`; commit the trace under `oracle/expected/` (the integrator opens the pull request) | Needs a machine the agents do not have and the owner's own key | Gate G2; every "functional" figure | open (DESIGN §12.9) |
| Set the decompilation's read token | A fine-grained token with read access to the contents of `marinohardin/SFC2-decomp`, then `gh secret set DECOMP_READ_TOKEN --repo marinohardin/SFCOP-port`; removed once the decompilation is public | A secret of the owner's account | Every CI job that checks out `decomp/` | open |
| Set the review token | `claude setup-token`, then `gh secret set CLAUDE_CODE_OAUTH_TOKEN --repo marinohardin/SFCOP-port` | A secret of the owner's account | Automated reviews; until then sub-agents review | open |
| Decide DESIGN §12.1, §12.3, §12.5, §12.10 | In conversation with the integrator | Owner's calls | See §2.1 | open |
| Choose and pay for hosting under 5 USD a month | Phase 3; the integrator proposes the cheapest shape | Money and an account | N3, N4 | open |
| Enter the game's CD key once | `oracle/launch_game.sh`, type it into the game's prompt, quit; the script saves the game's stored form privately | A licence credential agents must not handle | Before the replay oracle can record | done (2026-11-03) |

### 2.3 What is committed

`.gitignore` excludes the owner's local configuration, `.agent/`, dependencies, build output and editor files. Run `git status` before a commit and confirm that only intended files are listed.

---

## 3. Working model

### 3.1 Worktrees

Nothing is ever worked on in the main checkout, or on `main` or `develop` directly. Each work package gets its own worktree, on a branch cut from `develop`.

Worktrees are created by Claude Code, not by hand: start the session with its worktree option, or have the integrator spawn the agent with worktree isolation. They live inside the repository under `.claude/worktrees/`, which git ignores and the project's tools skip. Creating them this way matters for two reasons: they are cut from the current `develop` on the remote, and the git-ignored files named in `.worktreeinclude` are copied in. A worktree made with a bare `git worktree add` gets neither.

The agent's first steps in a new worktree are to rename its branch to the work package's name and install:

```bash
git branch -m wp/<ID>-<slug>
```


The git hooks in `.githooks/`, switched on once per clone with `sh .githooks/install.sh`, refuse a commit or a push made directly on `main` or `develop`. Remove the worktree after its branch merges.

**Small packages** are one branch and one pull request into `develop`. **Large packages** use a long-running branch, `feature/<id>-<slug>`, cut from `develop`; the agent opens a series of small pull requests into that branch, and the feature branch is merged into `develop` in a single pull request when the package's acceptance line is met. The integrator decides which packages are large when handing out briefs.

### 3.2 Rules for every agent

1. **Read first:** the DESIGN sections listed for the package, and all of CONVENTIONS.
2. **Edit only owned paths.** If the work seems to need a change elsewhere, stop and raise a contract change (§3.4).
3. **Do not touch hotspot files** (§3.3).
4. **Do not add a dependency** that is not already declared for the package. If one is needed, raise it.
5. **Commits are small, atomic and readable.** Each pull request is squash-merged, so its title is a Conventional Commits subject.
6. **Done means:** the acceptance line is met; `sh scripts/check.sh` passes; tests exist at the bar in CONVENTIONS; nothing outside the owned paths changed; the pull request description says what, why and how it was tested.
7. **Stubs are replaced, not worked around.** If a dependency is still a stub, code against its contract and test with a mock of it.
8. **Keep `.agent/CONTINUITY.md` current** in the worktree, as `AGENTS.md` requires. It is never committed.

### 3.3 Hotspot files

Changed only by the integrator, after Wave 0. The list is `.github/hotspots.txt`, in the form the `hotspots` check reads; `CODEOWNERS` names the owner for the same files. Change the two together.

### 3.4 Contract changes

Contracts will be wrong in places. When an agent finds one:

1. It stops work on the affected part and describes the change needed: which type or signature, and why.
2. The integrator makes the change in a small pull request that touches only hotspot files, labeled `contract-change`, and merges it.
3. Agents whose packages consume that contract bring `develop` in (a merge, not a rebase).

Additive changes are cheap. Anything else is worth a second look at the design.

### 3.5 Integrator

One person, or one lead agent, per wave:

- Starts packages, hands each agent its brief (§7).
- Reads every review before a merge, runs fix rounds, and merges finished pull requests in any order, or tells the owner the order.
- Owns hotspot files and contract changes.
- Runs the wave's gate before starting the next wave.

Within a wave there is no required order. In practice, start the packages on the critical path and the largest ones first, and run as many at once as the integrator can review and the machine can carry.

---

## 4. Hosting setup and automation

The repository is private on GitHub's free plan under `marinohardin`. The free plan offers no
branch protection or required checks on a private repository, so the git hooks (`.githooks/`,
installed once per clone) and the integrator's discipline stand in: `main` and `develop` refuse
direct commits and pushes, every change arrives through a pull request from a worktree branch,
and the `hotspots` check fails any work-branch pull request that touches a protected path
without the `contract-change` label.

The jobs that build or read the retail install run on a self-hosted runner on the owner's
MacBook Pro (`runs-on: [self-hosted, sfcop-mac]`): `check` (`sh scripts/check.sh`), `build`, the
`oracle` and the release. `plan`, `hotspots` and the automated review read only this repository
and run on GitHub-hosted `ubuntu-latest`. The runner is one process registered to this repository
with the label `sfcop-mac`, limited to one job at a time, with its work folder on the external
drive. Docker, Python 3 and the VC6 SP4 compiler image are already on the machine, so the setup
action only checks for them; nothing is installed by CI. The jobs that check out `decomp/` read
the private decompilation with the `DECOMP_READ_TOKEN` secret (§2.2). There is no deploy: this
project ships no service.

The trade-off: a job runs as the owner's user, keeps its work folder between jobs, and can reach
everything the owner can. That is accepted because the machine already holds the owner's
credentials, every pull request is the owner's own or an agent's under the owner's account, and
the repository is private. It stops holding the day the repository is made public or accepts
outside pull requests; before that, move the self-hosted jobs to GitHub-hosted runners (change their `runs-on`) or add a dedicated machine. The runner only works while the Mac is awake; a
job that queues for a day fails and is rerun.

The automated review runs Claude Code on a GitHub-hosted runner, so it does not see the owner's
settings, memory or plugins; its token is the `CLAUDE_CODE_OAUTH_TOKEN` repository secret. Review turns are capped at 40.

---

## 5. Dependencies between waves

| Package | Consumes |
| --- | --- |
| C1 to C8 | F1 (the build and the check) |
| B1 Window and GPU backends | C2 |
| B2 DirectDraw 4 and Direct3D 3 HLE | C1, C2 (the `Gpu` and `Window` headers), B1 at link time only |
| B3 Input | C1, C2, B1's `Window` event feed at run time (`AttachInput`) |
| B4 Audio | C1, C2 |
| B5 Store, kernel and registry HLE | C1, C2, C4 (paths) |
| B6 Guest runtime | C2 (`Clock`, `Threads`), C3's override table |
| B7 Winsock on Transport | C1, C2 |
| B8 DirectPlay 4 HLE (LAN) | C1, B7's `Transport` header |
| B9 Object lifter | C3 (the manifest and the override table), the harness (the VC6 object of a unit), F1's lift step |
| D grind | The decompilation (SFC2-decomp) owns `op/` and the matched set; B9 for the native build |
| A1 Boot to the menu | B1 to B6 |
| A2 Skirmish | A1, B3, B4, the oracle trace |
| A3 Mission modules and scr2native | B5, C3, C8 |
| A4 Oracle on the native build | C5, A1 |
| A5 CI matrix | F1, A1 |
| N1 to N6 | A2, B7, B8 |
| V1 to V3 | N1, N3, B7 |
| M1, L1 | A2 |

A package consumes a contract through the file that declares it (§6 of DESIGN), never through another package's implementation; until that implementation lands it codes against the header and tests with the mock.

---

## 6. Waves


### 6.1 Wave 0A (one agent, serial)

| ID | Work package | Owns | Produces | Done when |
| --- | --- | --- | --- | --- |
| F1 | Native build skeleton and check wiring | `CMakeLists.txt`, `CMakePresets.json`, `cmake/`, `src/app/main.cpp` (stub), `third_party/` (SDL3, miniaudio, doctest pinned, `INVENTORY.md`), `toolchain/VERSIONS.md`, `scripts/check.sh`, `scripts/port_footprint.py`, `.clang-format`, `.github/workflows/*` (the matrix job) | A native target that configures and builds on macOS arm64 with the lifted bodies generated from the install into the build directory (the P1-2 host as the starting point), stub targets for Windows x64 and Linux x64 that cross-configure; `check.sh` running pytest, the hotspots consistency check, `progress.py --gate` and the harness `--all --base` against the base branch, the `PORT:` and `SFCOP_FIX` counter, and the oracle step when a trace exists | A fresh clone with the install present builds `sfcop` and runs it to the CD-key prompt headless; `sh scripts/check.sh` passes on `develop` and fails on a fixture that lowers a matched-set score; the three CI presets configure |

### 6.2 Wave 0B (parallel)

| ID | Work package | Owns | Produces | Done when |
| --- | --- | --- | --- | --- |
| C1 | Win32 surface contract | `src/hle/surface.tsv`, `src/hle/com/` | Every import and COM method the game uses, from `spike/lifter/results/imports_vs_win32hle.tsv`, `dx_inventory.md` and `analysis/netcode/`, with convention and status; vtable structs for every COM interface of DESIGN §6.1 with method order from the public SDK headers; a test that the TSV is complete against the exe's import table | The TSV has a row for all 353 imports and every COM method; `tests/test_surface.py` passes; no row says "unknown" |
| C2 | Platform component interfaces | `src/platform/*/` (headers, mocks, stub backends) | `Window.h`, `Gpu.h`, `Input.h`, `Audio.h`, `Transport.h`, `Store.h`, `Clock.h`, `Threads.h` with a mock each and a stub backend that compiles on all three presets; a doctest per interface exercising the mock | All eight headers build in every preset; each method documented with its contract; mocks used in at least one test |
| C3 | Guest-mode spike (DESIGN §12.4) | `decomp/op/port/`, `decomp/op/Common/DynamicAllocator.cpp` (the unit the harness proof used), `decomp/tools/manifest.py`, `src/app/guest/override.*`, `spike/guest-mode/REPORT.md` | `types.h` with `ptr32<T>` and fixed-width aliases; three matched functions, one a virtual method, compiled exact under VC6 and compiled in guest mode by clang and linked into the native build through a manifest-generated override table; the report with the verdict | The three functions stay exact in the harness and the native build runs them in place of their lifted bodies with the same guest test output; the report says yes, no, or yes-with-constraints and lists each constraint |
| C4 | Application skeleton: command line, install discovery, exit codes | `src/app/` except `guest/` and `main.cpp`'s build glue | `CommandLine`, `Install` (locate, hash-verify against `analysis/results/retail_hashes.sha256`, refuse with the file named), the per-user data directory per platform, exit codes of DESIGN §6.8; doctests with a fixture install | A one-byte-altered fixture is refused naming the file (exit 3); the data directory resolves on all three platforms in tests; `--help` lists every flag of §6.8 |
| C5 | Oracle trace contract and comparator | `oracle/expected/README.md`, `oracle/trace/` (format, `compare.py`), `oracle/scr/` additions | The trace format of DESIGN §6.5, a comparator that reports the first differing tick, and a fixture pair; the hook `check.sh` calls when a trace exists | `python3 -m pytest tests/test_trace.py` passes with a matching pair, a mismatch at a known tick, and a header mismatch; the comparator exits 4 on mismatch |
| C6 | Fix switches contract | `src/fixes/`, `docs/FIXES.md` (contract change), `scripts/port_footprint.py` tests | `fixes.h` with `SFCOP_FIX(name)`, the registry of switches read from the command line and `settings/fixes.gf`, `docs/FIXES.md` with its columns and the first row (the empty-name object fix from P1-2 as a project fix), and the counter's tests | A fix toggles by command line and by file in a doctest; the counter lists every `PORT:` and `SFCOP_FIX` site in a fixture tree with file and line |
| C7 | Game tree skeleton and unit map | `decomp/op/` except `port/` and `Common/DynamicAllocator.cpp`, `decomp/op/UNITS.tsv`, `decomp/harness/units.json` (contract change) | The directory tree from the 1,658 functions with unit evidence in `functions.csv`, `UNITS.tsv` (unit path, address range, evidence, profile), the `unsorted/` scheme, an `op/README.md`, and `units.json` entries with the `sdk-release` profile | Every function in `functions.csv` maps to exactly one unit path or to `unsorted/`; the harness scores a function from a unit the map names; no two units overlap in address |
| C8 | Mission module contract | `src/app/scripts/` (loader interface and a mock), `tools/scr2native/README.md` (design only), `tests/test_module_imports.py` | The module interface of DESIGN §6.4: entry points, version tag, the import allow-list derived from the scripting library's exports and the C runtime, and the refusal path with its message | A fixture module with a foreign import is refused naming it; a conforming fixture loads in the mock; the allow-list is generated, not typed |

**Gate G0.** `sh scripts/check.sh` passes on `develop` in a fresh worktree and all three presets configure. The integrator reads C1 to C8 against DESIGN §6 and against each other (C2's `Store` paths against C4's, C1's COM structs against B2's needs, C3's verdict against §7.2) and freezes the contracts: `src/hle/surface.tsv`, `src/hle/com/**`, `src/platform/*/*.h`, `game/port/**`, `src/fixes/fixes.h`, `docs/FIXES.md`, `oracle/trace/**`, `src/app/scripts/*.h`, `game/UNITS.tsv` go on the hotspot list. C3's report sets DESIGN §12.4 and, if the answer is no, strikes "the native build" from the grind's acceptance line in §6.3.

### 6.3 Wave 1 (parallel)

| ID | Work package | Owns | Produces | Done when |
| --- | --- | --- | --- | --- |
| B1 | Window and GPU backends | `src/platform/Window/`, `src/platform/Gpu/` (backends) | SDL3 window, modes, events; SDL GPU buffers, textures, pipelines, present; frame cap | A doctest renders a triangle to an offscreen target and reads back the pixels on every preset; the frame cap holds 60 in a 5 s headless run |
| B2 | DirectDraw 4 and Direct3D 3 HLE | `src/hle/ddraw4/`, `src/hle/d3d3/` | Every method of the surface's DirectDraw and Direct3D rows on `Gpu` and `Window`: surfaces, blits, palettes, clippers, the device, viewports, textures, the fixed-function state the game sets | The lifted game draws its loading screen and the key prompt through these modules instead of the spike's path, with a frame diff under 1% against the spike's capture |
| B3 | Input | `src/platform/Input/` (backend), `src/hle/dinput/` | SDL3 keyboard and mouse into `Input`; `IDirectInput7A` and device methods, immediate and buffered, `SetEventNotification` real | The oracle driver's key script produces the same command sequence as on the spike; buffered data test with a hand-written event sequence |
| B4 | Audio | `src/platform/Audio/` (backend), `src/hle/miles/`, `src/hle/ds3d/` | miniaudio engine; the `AIL_*` rows; DS3D buffer and listener applied (distance, pan, cones) | The spike's 210 self-test checks pass against the new modules; attenuation and pan follow distance and angle in a doctest |
| B5 | Store, kernel and registry HLE | `src/platform/Store/` (backend), `src/hle/kernel/`, `src/hle/registry/` | Files, the wide-char calls, the per-user directory, `registry.tsv` with the game's key names and types, atomic writes | A `.gf` round trip is byte-identical; a registry value written through the HLE is read back through the game's path in a test; the install directory is opened read-only |
| B6 | Guest runtime | `src/app/guest/` (except `override.*`), `src/platform/Clock/`, `src/platform/Threads/` | The P1-1 exception dispatcher as a component, the lock-depth leak fixed, divide-by-zero, static TLS, the worker threads, the system allocator in place of SmartHeap | The 14 guest tests pass on all three presets; the game's first throw on the boot path unwinds as on the original; a throw across a guest callback leaves the lock depth unchanged |
| B7 | Winsock on Transport | `src/platform/Transport/` (plain sockets backend), `src/hle/winsock/` | The Winsock rows including `select`, `accept`, `shutdown`, `getpeername`, `__WSAFDIsSet`, on `Transport` | The spike's loopback tests pass; a `select` with a timeout returns on time in a doctest |
| B9 | Object lifter (DESIGN §7.2, §12.4) | `tools/objlift/`, `cmake/lifter/` additions and the lift step's CMake (contract change), `tests/test_objlift.py`, `spike/guest-mode/` (a closing note only) | A lifter for the VC6 COFF objects the harness builds: relocations resolved against the exe's symbols, lifted C in the form the exe lift emits, the override table fed from it; the build step that compiles claimed units in the container and lifts them | The three functions of the C3 spike, compiled by VC6 from `game/` with no guest-mode constructs, enter the native build through their lifted objects and the guest tests give the same output as with the lifted-exe bodies; the harness still scores them exact; a hand-written COFF fixture with every i386 relocation type VC6 emits lifts to the expected C in a test |
| B8 | DirectPlay 4 HLE for LAN | `src/hle/dplay/` | `IDirectPlay4A` and `IDirectPlayLobby3A` methods the inventory lists, over `Transport` with LAN broadcast enumeration; the `tNetMsg` framing left untouched | Two native clients on one machine enumerate and join a session and exchange the inventory's first three message kinds in a test harness |
| D | Grind batches | None here: the decompilation (SFC2-decomp) owns `op/` and the matched set | One pull request per batch in the decompilation; one unit per batch, code shared with Empires at War first (DESIGN §7.5): `Tiger/`, `dmp3d/`, the game proper along the boot path by coverage, then `Common/`, `Switch/` and `Meta/`; inside a unit, functions identical in Empires at War first (owner, 2026-11-08; this replaces the earlier SDK-evidence-first order, so `Common/` waits although its names come easiest) | Each batch: every claimed function exact in the harness; `progress.py --gate` passes; nothing outside the claimed units changed |

**Gate G1.** The native build boots to the key prompt on macOS with every component real (no spike path linked); `surface.tsv` has no row in status `stub` without a line in the pull request that justified it; Windows and Linux presets build and run the headless boot to the first frame; the grind has merged at least one batch and the progress figure moved; at least one decompiled function runs in the native build through B9's lifted object; every worktree of the wave removed.

### 6.4 Wave 2 (mostly parallel)

| ID | Work package | Owns | Produces | Done when |
| --- | --- | --- | --- | --- |
| A1 | Boot to the main menu | `src/app/` glue, `oracle/driver/` additions | The key supplied to the run by an `oracle/` script that never prints it; the menu reached; the frame cap and windowed and fullscreen modes | The boot path's click sequence reaches `Setup` in under 10 s on an M1; no fault in a 5 min idle |
| A2 | Skirmish on the native build | `src/app/`, `src/hle/*` fixes as needed (each its own commit) | A skirmish set up, played by the oracle driver and ended | The oracle's fixed skirmish produces the original's trace tick for tick on macOS; the result recorded by `check.sh` |
| A3 | Mission modules and scr2native | `tools/scr2native/`, `src/app/scripts/` (implementation) | The translator (lift, compile, package, `--check`), the loader implementation, the stock missions through it | Every shipped mission loads through a translated module; the oracle mission translated produces the same trace as the original `.scr` in the 32-bit build |
| A4 | Oracle on all presets | `oracle/` (not `expected/`), `.github/` (contract change) | The oracle step running headless on the Windows and Linux builds in CI | The three presets report the same trace hash for the oracle skirmish |
| A5 | CI matrix | `.github/workflows/` (contract change), `cmake/` toolchain files | Real Windows and Linux builds on the runner through cross-compilers, artifacts kept 7 days, owner-only | Each pull request shows three green build jobs |

**Gate G2.** A2's trace matches the original on macOS and A4 matches on the other two presets; every fix in `docs/FIXES.md` has tests in both positions; the owner has played a skirmish.

### 6.5 Wave 3 (stabilise, then network)

Gate G2 passed with findings: traces differed between two runs of one binary, one mission start in three faulted, the boot took 12 to 33 s and clicks were lost in play. Wave 3 fixes those first (the T, X and K packages, named for their origin: timing, gate finding, kernel module), then starts the network with N1 to N4. N5, N6 and V1 to V3 move to Wave 4.

| ID | Work package | Produces | Status |
| --- | --- | --- | --- |
| T1 | Fixed-step clock for oracle runs | `--fixed-step MS` and the clock under `--oracle`; the Wine driver recording with the same step (§5.10) | merged (#57); run-to-run determinism still open, follow-up T2 (random seeds) |
| X3 | Model loader faults at mission start | The cause: a damaged `pca.mod` in the owner's install, repaired from the unpatched copy; file-layer traces | merged (#53) |
| X4 | Every click reaches the game in play | Button-message and polled-mouse traces in `src/hle/user32/`; the loss did not reproduce | merged (#54) |
| X5 | The spinning lock: slow boot and long frames | A count-based thread quantum in the lifter's host; per-object wake-ups in `src/platform/Threads/` | threads merged (#59); quantum open (#58) |
| K1 | The kernel module reads `fix_hle_named_objects` | The fix switch read by `src/hle/kernel/` and tested both ways | merged (#55) |
| N1 | Protocol documents (contract change) | `src/net/PROTOCOL.md`, `LOBBY_API.md`, `RENDEZVOUS.md`, `NAT_LIBRARY.md`; §12.6 and §12.8 answered | merged (#56) |
| N2 | Transport with NAT traversal and relay fallback | `src/platform/Transport/` backend: the project's own punch and relay client on the `Rendezvous` contract | after this contract change |
| N3 | Lobby service | `server/lobby/`, deploy script, health endpoint | after N1 |
| N4 | Relay service | `server/relay/` | after N1 |

**Gate G3:** two native clients behind symmetric NAT complete a match, with relay fallback under 5 s when traversal is blocked; it closes with N5 in Wave 4. Wave 3's own gate: identical traces across two runs, 20 of 20 mission starts, the menu under 10 s, every click acts, the protocol merged with N2 to N4 passing on a test network.

### 6.6 Waves 4 and 5 (outline)

| ID | Work package | Produces |
| --- | --- | --- |
| N5 | DirectPlay 4 HLE over the new transport | `src/hle/dplay/` session enumeration from the lobby, join through rendezvous |
| N6 | Server list and chooser | Configuration shipped with the port; the `GameBrowser` fed from it |
| V1 | Directory and auth stand-ins | `server/directory/`, `server/auth/`; the client's lookup pointed at them |
| V2 | The server kit under Wine on the host | Deploy and runbook; a campaign reachable |
| V3 | Campaign server reimplementation | `server/campaign/`, with the kit as oracle |
| F2 | Fix override units | A scanner lists the `// OVERRIDE:` annotations under `src/fixes/overrides/` (which tool, now that `tools/manifest.py` is the decompilation's, is decided before F2; DESIGN §6.9); the lift links an override's object in place of the original's when its build switch is on (DESIGN §6.9); built with the first fix to game code |
| M1 | Mod manager | `tools/modman/` or in the launcher |
| L1 | Launcher and release | A signed launcher from the owner's account; the one-command build tool; the tag |

Wave 4 so far (2026-11-10): N5a (DirectPlay over the transport on a LAN, #74), V1 (directory and auth stand-ins, #76), T2 (oracle epoch, #75), S1 (module open-ahead, #81), L1a (the application bundle, #71), R1 (the release pipeline and the five packages, #97) and W1 (the Windows lifted client, #113) are merged. The first release is `v0.1.0-alpha`, cut from `main` after `develop` is merged into it; DESIGN §12.2 records the decision to ship translated code before the decompilation is complete. Still to brief: V1b (the campaign list service), N6, V2, V3, F2, M1 and the signed launcher half of L1.

**Gates G4 and G5.** G4: a logon and one mission against the kit through the stand-ins. G5: the mod manager resolves a two-mod conflict by order; a release is tagged.

## 7. Work package brief

What the integrator gives each agent. The brief is meant to be enough on its own: the integrator copies the package's row into it, so that the agent does not have to open this plan. Everything else the agent needs is in `AGENTS.md`, CONVENTIONS and the DESIGN sections the brief names. The template is `.agent/prompts/work-package-brief.md`.

```
Work package: <ID> <name>
Branch:       wp/<ID>-<slug>, cut from develop (or feature/<ID>-<slug> for a large package)
Worktree:     created by Claude Code under .claude/worktrees/; rename the branch as above
Goal:         <one sentence>
Owns:         <paths>
Read first:   docs/CONVENTIONS.md (all); docs/DESIGN.md §<sections>
Consumes:     <packages and contracts>
Done when:    <the acceptance line from this plan>
Out of scope: anything outside the owned paths
```
