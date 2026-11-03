# SFCOP-decomp — Conventions

The standard every change is held to. `DESIGN.md` says what the system does; this says what the code looks like. The automated review reads this and `AGENTS.md`.

---

## 1. Toolchain

| Tool | Convention |
| --- | --- |
| Language and version | C and C++ for the port, Python 3 for tooling |
| Package manager | none (a pip venv under work/ for Python tooling; CMake for the port once it exists) |
| Build | none |
| Tests | python3 -m pytest tests |
| Lint and format | none |
| CI | GitHub Actions on a self-hosted runner |

Versions are pinned in a file in the repository, never "latest".

## 2. Repository layout

<!-- A tree of the top-level folders with one line each. Say where shared code goes and what may import what. -->

## 3. Naming

<!-- Files, directories, identifiers, branches. One table per kind. -->

## 4. Code style

<!-- What the formatter does not decide: comments, error handling, logging, when a function is too long. -->

## 5. Dependencies

<!-- Where they are declared, who may add one (the integrator), how versions are chosen. -->

## 6. Testing

- Tests sit in `tests/` for the Python tooling; for the port, beside the code or where the plan phase decides.
- There is no line-coverage threshold. Progress is measured by `scripts/progress.py` as percent of original code bytes that are byte-exact, functionally verified, or still lifted, plus percent of functions named; no pull request may lower the exact or functional figure for any function, compared by original address.
- Every new feature and every bug fix comes with tests. A bug fix comes with a test that fails without the fix.
- Test names say what is guaranteed, not what function is called.
- No network, no real external services, no real clock in unit tests. Pass time in or fake it.
- No real personal or confidential data in fixtures. Fixtures are plainly invented.
- Expected values are written out, not computed by calling the code under test.

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

<!-- One per recurring task ("Adding a module", "Adding a command"): the files to touch, the tests to add, the document to update. -->
