# SFCOP-decomp — Implementation Plan

How to build what [DESIGN.md](DESIGN.md) describes, following [CONVENTIONS.md](CONVENTIONS.md), with as much of the work as possible done in parallel by separate agents in separate git worktrees.

---

## 1. Shape of the plan

The work is split into **work packages**. Each one is a single branch, in a single worktree, done by a single agent, touching a set of paths no other package in the same wave touches.

Work packages are grouped into **waves**. Everything in a wave can run at the same time. A wave starts when the wave before it has merged and its gate has passed.

| Wave | Purpose | Packages | Parallel |
| --- | --- | --- | --- |
| 0A | Repository and toolchain | 1 | No |
| 0B | Contracts and a walking skeleton | | Yes |
| 1 | Building blocks | | Yes |
| 2 | Assemblies | | Yes |
| 3 | Integration | | Mostly |
| 4 | Release | 1 | No |

Three ideas make the parallelism work:

1. **Contracts first.** Wave 0B writes every type, interface and schema that two packages will share, and freezes them. After that, packages depend on contracts, not on each other's code.
2. **A walking skeleton.** Wave 0B also creates every module as a compiling stub, fully wired, with `sh scripts/check.sh` passing. Later packages fill in files that already exist. Nobody after Wave 0 needs to edit the wiring, the root configuration or a module's public surface.
3. **Disjoint ownership.** Each package lists the paths it owns. An agent edits nothing else.

<!-- Name the critical path once the packages are known. -->

---

## 2. Before any code

### 2.1 Decisions

| # | Decision | Status | Blocks |
| --- | --- | --- | --- |
| 1 | | | |

### 2.2 Things only a person can do

| Task | How | Why only a person | Needed by | Status |
| --- | --- | --- | --- | --- |
| Create the private repository and apply its settings | `gh repo create marinohardin/SFCOP-decomp --private --source . --push`, then the settings commands in `references/hosting-github.md` §1 of the workflow skill (default branch `develop`, squash merges, `contract-change` label). The integrator shows the commands; the owner says yes | Changes the remote | Before the first pull request | done (2026-11-03: repo created by the owner, settings applied) |
| Register the SFCOP runner | Download the runner into `$SFCOP_ROOT/tools/actions-runner`; `./config.sh --url https://github.com/marinohardin/SFCOP-decomp --token <registration token> --name marino-macbook-sfcop --labels sfcop-mac`; `./svc.sh install && ./svc.sh start`. Check with `gh api repos/marinohardin/SFCOP-decomp/actions/runners`. The two existing runners on this Mac belong to another repository and cannot serve this one. Script: `$SFCOP_ROOT/tools/register-sfcop-runner.sh`. The runner must live on the internal disk (`~/gitdir/actions-runner-sfcop`): a launchd service cannot open external volumes under macOS privacy rules | Runs as the owner's user on their machine | Before the first `check` job | done (2026-11-03: `marino-macbook-sfcop` online, first `check` job passed in 9 s) |
| Switch on the git hooks in the main checkout | `sh .githooks/install.sh` from the repo root; then every worktree inherits them | Sets git configuration under the owner's user; agents never run `git config` | Before the next commit | done (2026-11-03: probe commit on `develop` refused) |
| Install the Claude GitHub app and its token secret | `/install-github-app` in an interactive `claude` terminal; decline the offered workflows (the templates are in place) | Signs in as the owner and grants an app | Before the first automated review | open |
| Move Docker's disk image to the external drive and cap it | Docker Desktop, Settings, Resources, Advanced: Disk image location on `/Volumes/External`, virtual disk limit 64 GB | GUI-only setting on the owner's machine | Soon; the internal disk is nearly full | open |
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

```bash
none
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

Every job runs on a self-hosted runner on the owner's MacBook Pro (`runs-on: [self-hosted,
sfcop-mac]`): the `check` job (`sh scripts/check.sh`), the `hotspots` job, and the automated
pull-request review. Nothing runs on GitHub-hosted runners, so no Actions minutes are consumed.
The runner is one process registered to this repository with the label `sfcop-mac`, limited to
one job at a time, with its work folder on the external drive; the two runners already on the
machine belong to another repository and are not used. Docker, Python 3, Ghidra and the VC6 SP4
compiler image are already on the machine, so the setup action only checks for them; nothing is
installed by CI. There is no deploy: this project ships no service.

The trade-off: a job runs as the owner's user, keeps its work folder between jobs, and can reach
everything the owner can. That is accepted because the machine already holds the owner's
credentials, every pull request is the owner's own or an agent's under the owner's account, and
the repository is private. It stops holding the day the repository is made public or accepts
outside pull requests; before that, move the jobs to GitHub-hosted runners (change `runs-on` in
the three workflows) or add a dedicated machine. The runner only works while the Mac is awake; a
job that queues for a day fails and is rerun.

The automated review runs Claude Code on that runner under its own configuration folder in the
runner's temporary directory, so it does not see the owner's settings, memory or plugins; its
token is the `CLAUDE_CODE_OAUTH_TOKEN` repository secret on a machine already signed in to the
same subscription. Review turns are capped at 40.

---

## 5. Dependencies between waves

<!-- A diagram or table: which packages each package consumes. -->

---

## 6. Waves

<!-- One subsection per wave. Each package is a row: ID, name, owns, produces, consumes, "Done when". Each wave ends with a gate: a short list of checks with the evidence that closes each. -->

### 6.1 Wave 0A (one agent, serial)

| ID | Work package | Produces | Done when |
| --- | --- | --- | --- |
| F1 | Repository and toolchain | The root configuration, every script in CONVENTIONS §1, the checks, the hooks, the coverage threshold, and a placeholder so that `sh scripts/check.sh` passes on an empty project | A fresh clone passes `none` and `sh scripts/check.sh`; a test pull request gets a green check and a review comment; a test pull request from a `wp/` branch that edits a hotspot file fails |

### 6.2 Wave 0B (parallel)

| ID | Work package | Owns | Produces | Done when |
| --- | --- | --- | --- | --- |

**Gate G0.** `sh scripts/check.sh` passes on `develop`. The integrator reads the contract packages end to end against DESIGN, and against each other, because two packages written in parallel will have chosen different names where the design is silent. Contracts are then frozen (§3.4).

---

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
