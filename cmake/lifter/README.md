# cmake/lifter

The lift of the original game's exe to C at build time (`docs/DESIGN.md` §7.2, §7.4), driven by
`cmake/SfcopLifter.cmake`. Nothing it generates is committed.

| File | What it does |
| --- | --- |
| `setup.sh` | Makes the pcrecomp clone, and each build folder's patched copy of it |
| `pcrecomp_copy.py` | A patched copy named by its patch set, for tools that run without a build folder |
| `patches/` | The patches applied to pcrecomp; `patches/series` lists them in the order they apply |
| `lift.sh` | The lift of the exe, in the lifter container: rtti, disasm, then `run_lift.py` |
| `run_lift.py`, `eh_frames.py` | The lift's driver and its exception-frame table |
| `objlift.cmake` | The claimed units' own VC6 objects, lifted in place of their exe bodies (`tools/objlift/`) |
| `locate.py` | The default install and work folders (`harness/lib/paths.py`) |
| `Dockerfile` | The lifter container |

## pcrecomp: one clone, one patched copy per build folder

pcrecomp is pinned to one revision (`PCRECOMP_REV` in `setup.sh` and `lift.sh`). Two folders hold
it:

- **The clone**, `SFCOP_PCRECOMP_DIR` (by default `$SFCOP_WORK/lifter-hle/pcrecomp`). It is made
  once per machine with `sh cmake/lifter/setup.sh DIR`, the only step that uses the network, and
  is only ever read after that: every checkout, worktree and build folder on the machine shares it.
- **The build folder's copy**, `SFCOP_PCRECOMP_BUILD_DIR` (`<build>/pcrecomp`). At every configure,
  `SfcopLifter.cmake` runs `setup.sh --copy SOURCE DEST STAMP`, which makes a local clone of the
  clone (no network, and its working tree is ignored), checks out the pinned revision and applies
  the patches `patches/series` lists, in order. Everything that compiles or lifts against pcrecomp
  uses the copy: the lift, the objlift step, win32hle and the guest tests.

The copy carries a stamp, `<build>/pcrecomp.stamp`: one hash of the revision, `setup.sh`, and each
listed patch's name and content in series order. While the stamp matches, configure leaves the
copy alone (about 0.3 s), so nothing rebuilds. When a patch, the series or the revision changes,
the copy is made again from scratch (about 1 s, 6.6 MB) and the stamp rewritten; the lift and the
objlift step depend on the stamp, and the lift's own key covers the series and every patch, so
both run again. A patch file, the series and `setup.sh` are configure dependencies: editing one
reconfigures the next build by itself.

So two worktrees whose branches carry different patches build side by side from the one clone,
and a branch that adds a patch never has to touch the clone.

`patches/series` must list every `*.patch` in the folder, each once: a patch missing from the
series, a listed name with no file, or a name listed twice is a configure error naming the file.
To add a patch, add the file and its line in the series, where it applies.

The copy is a plain local clone, not `git clone --shared`: its objects are hard-linked (or copied
across file systems), with no alternates pointing back at the clone. Once made it stands alone,
so moving the clone or running `git gc` in it never breaks a build folder's copy. A configure
after the clone has moved stops at the clone check, which says to run `setup.sh` or configure
again with `-DSFCOP_PCRECOMP_DIR=<where it is now>`. A copy damaged by hand (its commit gone) is
made again at the next configure.

## Copies outside a build folder

`tools/scr2native` (and its `gen_baseline.py`) and the objlift tests run without a build folder.
They make their copy with `pcrecomp_copy.py`, which runs `setup.sh --copy` into
`<root>/pcrecomp-<key>`, where the key is the same patch-set hash as the stamp (16 hex digits).
Checkouts or test runs whose series differ use different folders under one root, side by side,
and never remove each other's. A copy is made in a private folder, with its stamp written inside
it, and renamed into place in one step, so two runs that need the same copy at once never see it
half made. A folder under a copy's name without the stamp inside (left by a crash) is replaced at
the next run; the replacement and the rename happen under a lock file in the root. The roots:

| User | Root |
| --- | --- |
| `tools/scr2native`, `gen_baseline.py` | `<--work>/.pcrecomp` |
| `tests/test_objlift.py` | `$SFCOP_WORK/objlift-tests` |
| `src/app/guest/tests/run_machine_quantum.sh` | its own scratch folder, removed after the run |

scr2native's lift key includes the patch-set hash, so a module is lifted again when the series
changes. A missing clone or a patch that does not apply stops it with exit status 3 and the fix.
Old copies are not removed; delete a root to reclaim the space.
