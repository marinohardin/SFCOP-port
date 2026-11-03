#!/bin/sh
# Points this clone at the hooks in this folder. Run it once after cloning, or wire it to the
# package manager's install step so it runs on every install.
#
# The setting is stored in the clone's shared configuration, so it covers every worktree.
# Each worktree then runs the hooks of the commit it has checked out.

if git rev-parse --git-dir >/dev/null 2>&1; then
  git config core.hooksPath .githooks
fi
