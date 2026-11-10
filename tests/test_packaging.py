"""packaging/check_package.py: the release packages' no-retail-file check (packaging/README.md).

Every tree and archive here is invented in a temporary folder; nothing is a retail file.
"""

from __future__ import annotations

import hashlib
import os
import subprocess
import sys
import tarfile
import zipfile

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "packaging"))
import check_package  # noqa: E402

RETAIL = b"invented bytes standing in for a retail file\n"
SCRIPT = os.path.join(ROOT, "packaging", "check_package.py")


def make_tree(root, layout):
    for relative in sorted(layout):
        path = os.path.join(root, relative)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as stream:
            stream.write(f"invented contents of {relative}\n")


@pytest.fixture
def hashes(tmp_path):
    listing = tmp_path / "hashes.sha256"
    listing.write_text(hashlib.sha256(RETAIL).hexdigest() + "  Test Install/StarFleetOP.exe\n")
    return str(listing)


def refusals(tree, hashes, kind):
    return check_package.tree_refusals(str(tree), check_package.check_bundle.read_hashes(hashes),
                                       check_package.LAYOUTS[kind])


@pytest.mark.parametrize("kind", ["linux", "appdir", "windows"])
def test_a_clean_tree_passes(tmp_path, hashes, kind):
    make_tree(tmp_path / "pkg", check_package.LAYOUTS[kind])
    assert refusals(tmp_path / "pkg", hashes, kind) == []


@pytest.mark.parametrize("kind,relative,data,reason", [
    ("linux", "share/game/StarFleetOP.exe", RETAIL,
     "a file of the original game (its hash is in the list)"),
    ("linux", "missions/Met_10Patrol/Met_10Patrol.so", b"x\n",
     "a library that is not the client's (a mission module?)"),
    ("linux", "lib/libextra.so.1", b"x\n", "a library that is not the client's (a mission module?)"),
    ("windows", "missions/Met_10Patrol.dll", b"x\n",
     "a library that is not the client's (a mission module?)"),
    ("windows", "Assets/Scripts/Met_10Patrol.scr", b"x\n", "a compiled mission script"),
    ("appdir", "usr/share/notes.txt", b"x\n", "not one of the client's files"),
])
def test_each_kind_of_stray_file_is_refused(tmp_path, hashes, kind, relative, data, reason):
    tree = tmp_path / "pkg"
    make_tree(tree, check_package.LAYOUTS[kind])
    (tree / relative).parent.mkdir(parents=True, exist_ok=True)
    (tree / relative).write_bytes(data)
    assert refusals(tree, hashes, kind) == [f"{relative}: {reason}"]


def test_a_retail_file_under_a_client_name_and_a_missing_file_are_refused(tmp_path, hashes):
    tree = tmp_path / "pkg"
    make_tree(tree, check_package.LAYOUTS["windows"])
    (tree / "SDL3.dll").write_bytes(RETAIL)
    (tree / "README.txt").unlink()
    assert refusals(tree, hashes, "windows") == [
        "SDL3.dll: a file of the original game (its hash is in the list)",
        "README.txt: missing",
    ]


def test_a_link_in_a_tree_is_refused(tmp_path, hashes):
    tree = tmp_path / "pkg"
    make_tree(tree, check_package.LAYOUTS["linux"])
    os.symlink("/opt/game", tree / "gameroot")
    assert refusals(tree, hashes, "linux") == ["gameroot: a link; the package holds plain files only"]


def write_tar(path, tree, top):
    with tarfile.open(path, "w:gz") as archive:
        archive.add(tree, arcname=top)


def test_a_clean_tar_gz_passes_and_the_command_says_so(tmp_path, hashes):
    tree = tmp_path / "SFCOP-0.1.0-alpha-linux-x64"
    make_tree(tree, check_package.LAYOUTS["linux"])
    asset = tmp_path / "SFCOP-0.1.0-alpha-linux-x64.tar.gz"
    write_tar(asset, tree, tree.name)
    run = subprocess.run([sys.executable, "-I", SCRIPT, str(asset), hashes],
                         capture_output=True, text=True)
    assert run.returncode == 0, run.stderr
    assert "only the client's own files, none in the hash list" in run.stdout


def test_a_tar_gz_with_a_retail_file_fails_naming_it(tmp_path, hashes):
    tree = tmp_path / "SFCOP-0.1.0-alpha-linux-arm64"
    make_tree(tree, check_package.LAYOUTS["linux"])
    (tree / "StarFleetOP.exe").write_bytes(RETAIL)
    asset = tmp_path / "pkg.tar.gz"
    write_tar(asset, tree, tree.name)
    run = subprocess.run([sys.executable, "-I", SCRIPT, str(asset), hashes],
                         capture_output=True, text=True)
    assert run.returncode == 1
    assert run.stderr == ("check_package: refused: StarFleetOP.exe: a file of the original game "
                          "(its hash is in the list)\n")


def test_a_tar_gz_with_a_link_or_two_folders_is_refused(tmp_path, hashes):
    tree = tmp_path / "top"
    make_tree(tree, check_package.LAYOUTS["linux"])
    os.symlink("bin/sfcop", tree / "run")
    asset = tmp_path / "links.tar.gz"
    write_tar(asset, tree, "top")
    assert check_package.asset_refusals(str(asset), check_package.check_bundle.read_hashes(hashes)) == [
        "top/run: a link or special file; the package holds plain files only"]
    os.unlink(tree / "run")
    asset = tmp_path / "two.tar.gz"
    with tarfile.open(asset, "w:gz") as archive:
        archive.add(tree, arcname="top")
        archive.add(tree / "README.txt", arcname="stray.txt")
    assert check_package.asset_refusals(str(asset), check_package.check_bundle.read_hashes(hashes)) == [
        "two.tar.gz: holds ['stray.txt', 'top'], not one folder"]


def test_a_zip_is_checked_as_the_windows_tree(tmp_path, hashes):
    tree = tmp_path / "SFCOP-0.1.0-alpha-windows-x64"
    make_tree(tree, check_package.LAYOUTS["windows"])
    asset = tmp_path / "SFCOP-0.1.0-alpha-windows-x64.zip"
    with zipfile.ZipFile(asset, "w") as archive:
        for relative in sorted(check_package.LAYOUTS["windows"]):
            archive.write(tree / relative, f"{tree.name}/{relative}")
        archive.writestr(f"{tree.name}/missions/Met_1Scout.dll", b"invented\n")
    assert check_package.asset_refusals(str(asset), check_package.check_bundle.read_hashes(hashes)) == [
        "missions/Met_1Scout.dll: a library that is not the client's (a mission module?)"]


def test_an_asset_that_is_itself_a_retail_file_is_refused(tmp_path, hashes):
    asset = tmp_path / "SFCOP-0.1.0-alpha-windows-x64.zip"
    asset.write_bytes(RETAIL)
    assert check_package.asset_refusals(str(asset), check_package.check_bundle.read_hashes(hashes)) == [
        "SFCOP-0.1.0-alpha-windows-x64.zip: a file of the original game (its hash is in the list)"]


# About 35 seconds of hdiutil on a loaded Mac, so PR CI leaves it out; the release workflow runs
# this file with SFCOP_TEST_DMG=1 before it builds anything.
@pytest.mark.skipif(sys.platform != "darwin" or not os.environ.get("SFCOP_TEST_DMG"),
                    reason="hdiutil is macOS's; set SFCOP_TEST_DMG=1 (the release workflow does)")
def test_a_dmg_holds_the_app_and_the_applications_link_only(tmp_path, hashes):
    staging = tmp_path / "staging"
    app = staging / "SFCOP.app"
    make_tree(app, check_bundle_expected())
    os.symlink("/Applications", staging / "Applications")
    (staging / "Extra.txt").write_text("invented\n")
    asset = tmp_path / "test.dmg"
    subprocess.run(["hdiutil", "create", "-quiet", "-fs", "HFS+", "-format", "UDZO",
                    "-volname", "SFCOP", "-srcfolder", str(staging), str(asset)], check=True)
    assert check_package.asset_refusals(str(asset), check_package.check_bundle.read_hashes(hashes)) == [
        "Extra.txt: not SFCOP.app or the Applications link"]


def check_bundle_expected():
    bundle = check_package.check_bundle
    return bundle.EXPECTED - {bundle.SIGNATURE}


PACK = os.path.join(ROOT, "packaging", "pack.py")


@pytest.mark.parametrize("kind,form,suffix", [("linux", "tar", ".tar.gz"), ("windows", "zip", ".zip")])
def test_pack_writes_the_same_archive_twice_and_it_passes_the_check(tmp_path, hashes, kind, form,
                                                                    suffix):
    tree = tmp_path / f"SFCOP-0.1.0-alpha-{kind}-x64"
    make_tree(tree, check_package.LAYOUTS[kind])
    first, second = tmp_path / f"one{suffix}", tmp_path / f"two{suffix}"
    for out in (first, second):
        subprocess.run([sys.executable, "-I", PACK, form, str(tree), str(out)], check=True)
    assert first.read_bytes() == second.read_bytes()
    assert check_package.asset_refusals(str(first), check_package.check_bundle.read_hashes(hashes)) == []


def test_pack_keeps_the_executable_bit_and_one_top_folder(tmp_path):
    tree = tmp_path / "SFCOP-0.1.0-alpha-linux-arm64"
    make_tree(tree, check_package.LAYOUTS["linux"])
    os.chmod(tree / "sfcop", 0o755)
    out = tmp_path / "pkg.tar.gz"
    subprocess.run([sys.executable, "-I", PACK, "tar", str(tree), str(out)], check=True)
    with tarfile.open(out) as archive:
        modes = {member.name: member.mode for member in archive.getmembers() if member.isfile()}
        tops = {member.name.split("/")[0] for member in archive.getmembers()}
    assert tops == {"SFCOP-0.1.0-alpha-linux-arm64"}
    assert modes["SFCOP-0.1.0-alpha-linux-arm64/sfcop"] == 0o755
    assert modes["SFCOP-0.1.0-alpha-linux-arm64/README.txt"] == 0o644


def test_pack_refuses_a_tree_with_a_link(tmp_path):
    tree = tmp_path / "SFCOP-0.1.0-alpha-windows-arm64"
    make_tree(tree, check_package.LAYOUTS["windows"])
    os.symlink("sfcop.exe", tree / "game.exe")
    run = subprocess.run([sys.executable, "-I", PACK, "zip", str(tree), str(tmp_path / "x.zip")],
                         capture_output=True, text=True)
    assert run.returncode != 0
    assert "game.exe: not a plain file or folder" in run.stderr


ICON = os.path.join(ROOT, "packaging", "linux", "write_icon.py")


def test_the_linux_icon_is_the_png_inside_the_icns(tmp_path):
    out = tmp_path / "sfcop.png"
    subprocess.run([sys.executable, "-I", ICON, str(out)], check=True)
    data = out.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    assert data in (open(os.path.join(ROOT, "packaging", "macos", "SFCOP.icns"), "rb").read())


def invented(path, text):
    path.write_text(text)
    return str(path)


def test_windows_make_tree_lays_out_the_folder_the_check_expects(tmp_path):
    out = tmp_path / "SFCOP-0.1.0-alpha-windows-arm64"
    run = subprocess.run(
        ["sh", os.path.join(ROOT, "packaging", "windows", "make_tree.sh"), str(out),
         invented(tmp_path / "sfcop.exe", "invented exe\n"), invented(tmp_path / "a.dll", "two\n"),
         invented(tmp_path / "b.dll", "three\n"), ROOT, "0.1.0-alpha", "arm64"],
        capture_output=True, text=True, env={**os.environ, "SFCOP_PYTHON": sys.executable})
    assert run.returncode == 0, run.stderr
    assert sorted(os.listdir(out)) == sorted(check_package.LAYOUTS["windows"])
    readme = (out / "README.txt").read_bytes()
    assert readme.startswith(b"SFCOP 0.1.0-alpha for Windows arm64\r\n")
    assert b"\n" not in readme.replace(b"\r\n", b"")


def test_linux_make_tree_lays_out_the_tree_the_check_expects(tmp_path):
    out = tmp_path / "SFCOP-0.1.0-alpha-linux-x64"
    run = subprocess.run(
        ["sh", os.path.join(ROOT, "packaging", "linux", "make_tree.sh"), str(out),
         invented(tmp_path / "sfcop", "invented elf\n"), invented(tmp_path / "s2", "two\n"),
         invented(tmp_path / "s3", "three\n"), ROOT, "0.1.0-alpha", "x64"],
        capture_output=True, text=True,
        env={**os.environ, "SFCOP_PYTHON": sys.executable,
             "SFCOP_LLVM_DIR": str(tmp_path / "no-llvm")})
    assert run.returncode == 0, run.stderr
    assert "the needed libraries were not checked" in run.stderr
    hashes = check_package.check_bundle.read_hashes(
        os.path.join(ROOT, "analysis", "results", "retail_hashes.sha256"))
    assert check_package.tree_refusals(str(out), hashes, check_package.LAYOUTS["linux"]) == []
    assert os.access(out / "sfcop", os.X_OK) and os.access(out / "bin" / "sfcop", os.X_OK)
    assert (out / "README.txt").read_text().startswith("SFCOP 0.1.0-alpha for Linux x64\n")
    assert "glibc 2.31 or later" in (out / "README.txt").read_text()


NOTES = os.path.join(ROOT, "packaging", "release_notes.py")


def run_notes(tmp_path, dist, hashes, lift="true", outcomes="", reasons=()):
    notes, assets = tmp_path / "notes.md", tmp_path / "assets.txt"
    run = subprocess.run(
        [sys.executable, "-I", NOTES, "--dist", str(dist), "--version", "0.1.0-alpha",
         "--lift", lift, "--commit", "0123456789abcdef0123", "--outcomes", outcomes,
         "--hash-list", hashes, "--notes", str(notes), "--assets", str(assets)]
        + [arg for reason in reasons for arg in ("--reason", reason)],
        capture_output=True, text=True)
    return run, notes.read_text() if notes.exists() else "", \
        assets.read_text().split() if assets.exists() else []


def test_release_notes_list_the_checked_assets_and_name_the_missing_ones(tmp_path, hashes):
    dist = tmp_path / "dist"
    tree = dist / "SFCOP-0.1.0-alpha-linux-x64"
    make_tree(tree, check_package.LAYOUTS["linux"])
    subprocess.run([sys.executable, "-I", PACK, "tar", str(tree),
                    str(dist / "SFCOP-0.1.0-alpha-linux-x64.tar.gz")], check=True)
    run, notes, assets = run_notes(
        tmp_path, dist, hashes,
        outcomes="macos-arm64=success linux-x64=success linux-arm64=failure "
                 "windows-x64=skipped windows-arm64=skipped",
        reasons=("windows-x64=the invented reason", "windows-arm64=", "linux-x64=unused"))
    assert run.returncode == 0, run.stderr
    assert assets == [str(dist / "SFCOP-0.1.0-alpha-linux-x64.tar.gz")]
    assert "| `SFCOP-0.1.0-alpha-linux-x64.tar.gz` | Linux x86-64" in notes
    assert "- SFCOP-0.1.0-alpha-macos-arm64.dmg: not built (the macos-arm64 step: success)" in notes
    assert "- SFCOP-0.1.0-alpha-windows-x64.zip: not built: the invented reason" in notes
    assert "- SFCOP-0.1.0-alpha-windows-arm64.zip: not built (the windows-arm64 step: skipped)" in notes
    assert "- SFCOP-0.1.0-alpha-linux-arm64.tar.gz: not built (the linux-arm64 step: failure)" in notes
    assert ("- SFCOP-0.1.0-alpha-linux-x64.AppImage: not built (optional; the tar.gz is the Linux "
            "package)") in notes
    assert "linux-arm64.AppImage" not in notes  # its step failed; the tar.gz line says so


def test_release_notes_refuse_a_package_with_a_retail_file(tmp_path, hashes):
    dist = tmp_path / "dist"
    tree = dist / "SFCOP-0.1.0-alpha-windows-x64"
    make_tree(tree, check_package.LAYOUTS["windows"])
    (tree / "StarFleetOP.exe").write_bytes(RETAIL)
    subprocess.run([sys.executable, "-I", PACK, "zip", str(tree),
                    str(dist / "SFCOP-0.1.0-alpha-windows-x64.zip")], check=True)
    run, _, assets = run_notes(tmp_path, dist, hashes)
    assert run.returncode == 1
    assert "refused: StarFleetOP.exe: a file of the original game" in run.stderr
    assert assets == []


def write_source(dist, extra=None):
    top = dist / "SFCOP-0.1.0-alpha-source"
    (top / "src").mkdir(parents=True)
    (top / "src" / "main.cpp").write_text("int main() { return 0; }\n")
    for relative, data in (extra or {}).items():
        (top / relative).parent.mkdir(parents=True, exist_ok=True)
        (top / relative).write_bytes(data)
    with tarfile.open(dist / "SFCOP-0.1.0-alpha-source.tar.gz", "w:gz") as archive:
        archive.add(top, arcname=top.name)
    import shutil
    shutil.rmtree(top)


def test_without_the_lift_the_draft_is_the_source_archive(tmp_path, hashes):
    dist = tmp_path / "dist"
    dist.mkdir()
    write_source(dist)
    run, notes, assets = run_notes(tmp_path, dist, hashes, lift="false")
    assert run.returncode == 0, run.stderr
    assert assets == [str(dist / "SFCOP-0.1.0-alpha-source.tar.gz")]
    assert "This release is the source only" in notes
    assert "Not in this release" not in notes


def test_a_source_archive_is_scanned_file_by_file(tmp_path, hashes):
    dist = tmp_path / "dist"
    dist.mkdir()
    write_source(dist, {"tests/fixtures/StarFleetOP.exe": RETAIL,
                        "Assets/Scripts/Met_1Scout.scr": b"invented\n"})
    run, _, assets = run_notes(tmp_path, dist, hashes, lift="false")
    assert run.returncode == 1
    assert ("SFCOP-0.1.0-alpha-source/Assets/Scripts/Met_1Scout.scr: a compiled mission script"
            in run.stderr)
    assert ("SFCOP-0.1.0-alpha-source/tests/fixtures/StarFleetOP.exe: a file of the original game"
            in run.stderr)
    assert assets == []


def test_an_appimage_that_is_a_retail_file_is_refused(tmp_path, hashes):
    dist = tmp_path / "dist"
    dist.mkdir()
    (dist / "SFCOP-0.1.0-alpha-linux-x64.AppImage").write_bytes(RETAIL)
    run, _, assets = run_notes(tmp_path, dist, hashes)
    assert run.returncode == 1
    assert "refused: SFCOP-0.1.0-alpha-linux-x64.AppImage: a file of the original game" in run.stderr
    assert assets == []


def test_an_unreadable_archive_is_refused_not_a_crash(tmp_path, hashes):
    asset = tmp_path / "SFCOP-0.1.0-alpha-linux-x64.tar.gz"
    asset.write_bytes(b"not a gzip file\n")
    found = check_package.asset_refusals(str(asset), check_package.check_bundle.read_hashes(hashes))
    assert len(found) == 1 and found[0].startswith(
        "SFCOP-0.1.0-alpha-linux-x64.tar.gz: cannot be read as its kind of package")


def test_check_temporaries_go_beside_the_asset_when_no_folder_is_named(tmp_path, hashes, monkeypatch):
    monkeypatch.delenv("SFCOP_CHECK_TMP", raising=False)
    seen = []
    real = check_package.tempfile.mkdtemp
    monkeypatch.setattr(check_package.tempfile, "mkdtemp",
                        lambda **kw: seen.append(kw["dir"]) or real(**kw))
    tree = tmp_path / "out" / "SFCOP-0.1.0-alpha-windows-x64"
    make_tree(tree, check_package.LAYOUTS["windows"])
    subprocess.run([sys.executable, "-I", PACK, "zip", str(tree), str(tmp_path / "out" / "w.zip")],
                   check=True)
    assert check_package.asset_refusals(str(tmp_path / "out" / "w.zip"),
                                        check_package.check_bundle.read_hashes(hashes)) == []
    assert seen == [str(tmp_path / "out")]
    assert not [name for name in os.listdir(tmp_path / "out") if name.startswith(".check-package-")]


TAG = os.path.join(ROOT, "packaging", "release_tag.sh")


@pytest.mark.parametrize("event,tag,expected", [
    ("workflow_dispatch", "", "TAG=v0.1.0-alpha\nSFCOP_VERSION=0.1.0-alpha\n"),
    ("workflow_dispatch", "v0.1.0-alpha.r1-test",
     "TAG=v0.1.0-alpha.r1-test\nSFCOP_VERSION=0.1.0-alpha.r1-test\n"),
    ("push", "v0.1.0-alpha", "TAG=v0.1.0-alpha\nSFCOP_VERSION=0.1.0-alpha\n"),
    ("push", "v0.1.0-alpha-rc1", "TAG=v0.1.0-alpha-rc1\nSFCOP_VERSION=0.1.0-alpha-rc1\n"),
])
def test_release_tags_that_are_accepted(tmp_path, event, tag, expected):
    version = tmp_path / "VERSION"
    version.write_text("0.1.0-alpha\n")
    run = subprocess.run(["sh", TAG, event, tag, str(version)], capture_output=True, text=True)
    assert (run.returncode, run.stdout) == (0, expected), run.stderr


@pytest.mark.parametrize("event,tag,why", [
    ("workflow_dispatch", "v0.2.0", "does not start with v plus the VERSION file"),
    ("workflow_dispatch", "0.1.0-alpha", "is not v followed by"),
    ("workflow_dispatch", "v0.1.0-alpha/../../tmp", "is not v followed by"),
    ("workflow_dispatch", "v0.1.0-alpha x", "is not v followed by"),
    ("workflow_dispatch", "v0.1.0-alpha.", "does not start with v plus the VERSION file"),
    ("workflow_dispatch", "v0.1.0-alphabet", "does not start with v plus the VERSION file"),
    ("push", "v0.1.1", "does not start with v plus the VERSION file"),
    ("push", "", "a push without a tag"),
])
def test_release_tags_that_are_refused(tmp_path, event, tag, why):
    version = tmp_path / "VERSION"
    version.write_text("0.1.0-alpha\n")
    run = subprocess.run(["sh", TAG, event, tag, str(version)], capture_output=True, text=True)
    assert run.returncode == 1 and run.stdout == ""
    assert why in run.stderr


sys.path.insert(0, os.path.join(ROOT, "packaging"))
import scrub  # noqa: E402


def test_scrub_replaces_the_lift_inputs_as_given_and_as_real_paths(tmp_path):
    real = tmp_path / "real" / "Test Install"
    real.mkdir(parents=True)
    os.symlink(tmp_path / "real", tmp_path / "alias")
    given = str(tmp_path / "alias" / "Test Install")
    environ = {"SFCOP_INSTALL_DIR": given + "/", "SFCOP_PCRECOMP_DIR": "/opt/test/pcrecomp"}
    text = (f"error: {given}/StarFleetOP.exe\nnote: {os.path.realpath(given)}/Assets\n"
            f"cd {tmp_path / 'alias'}\nfrom /opt/test/pcrecomp/runtime\n")
    assert scrub.scrub(text, environ) == ("error: <install>/StarFleetOP.exe\nnote: <install>/Assets\n"
                                          "cd <install-parent>\nfrom <pcrecomp>/runtime\n")


CHECK_PATHS = os.path.join(ROOT, "packaging", "check_paths.py")


def test_check_paths_counts_the_build_folders_and_never_prints_them(tmp_path):
    build, source = tmp_path / "build", tmp_path / "Invented Checkout"
    build.mkdir()
    source.mkdir()
    install = "/invented/Test Install"
    (build / "CMakeCache.txt").write_text(f"CMAKE_HOME_DIRECTORY:INTERNAL={source}\n"
                                          f"SFCOP_INSTALL_DIR:PATH={install}\n")
    leaky = tmp_path / "sfcop"
    leaky.write_bytes(b"\0" + f"{source}/src/app/main.cpp".encode() + b"\0" + install.encode()
                      + b"\0" + f"{source}/x.h".encode())
    clean = tmp_path / "libSDL3.so.0"
    clean.write_bytes(b"\0./src/app/main.cpp\0")
    env = {**os.environ, "HOME": "/invented-home/testcaptain"}
    run = subprocess.run([sys.executable, CHECK_PATHS, str(build), str(leaky), str(clean)],
                         capture_output=True, text=True, env=env)
    assert run.returncode == 1
    assert "sfcop: the checkout: 2" in run.stdout
    assert "sfcop: the install: 1" in run.stdout
    assert "libSDL3.so.0: the checkout: 0" in run.stdout
    assert str(source) not in run.stdout + run.stderr
    assert install not in run.stdout + run.stderr
    ok = subprocess.run([sys.executable, CHECK_PATHS, str(build), str(clean)],
                        capture_output=True, text=True, env=env)
    assert ok.returncode == 0, ok.stdout


def test_check_paths_also_finds_the_folders_above_the_install_the_home_and_the_disk(tmp_path):
    build = tmp_path / "build"
    build.mkdir()
    (build / "CMakeCache.txt").write_text(
        "CMAKE_HOME_DIRECTORY:INTERNAL=/invented-disk/TestDisk/src/sfcop/wt/one\n"
        "SFCOP_INSTALL_DIR:PATH=/invented-disk/TestDisk/src/sfcop/inputs/Test Install\n")
    binary = tmp_path / "sfcop"
    binary.write_bytes(b"\0/invented-disk/TestDisk/elsewhere/x.c\0/invented-home/testcaptain/y\0"
                       b"/invented-disk/TestDisk/src/sfcop/inputs/other\0")
    run = subprocess.run([sys.executable, CHECK_PATHS, str(build), str(binary)],
                         capture_output=True, text=True,
                         env={**os.environ, "HOME": "/invented-home/testcaptain"})
    assert run.returncode == 1
    # Both disk strings are under the checkout's top folder; above the install are "inputs/" and
    # the folder it shares with the checkout, each named once.
    assert "sfcop: the checkout's top folder: 2" in run.stdout
    assert "sfcop: the home folder: 1" in run.stdout
    assert "sfcop: a folder above install: 2" in run.stdout
    assert "sfcop: the checkout: 0" in run.stdout
    assert "TestDisk" not in run.stdout + run.stderr


def leaky_build(tmp_path):
    build = tmp_path / "build"
    build.mkdir()
    (build / "CMakeCache.txt").write_text("CMAKE_HOME_DIRECTORY:INTERNAL=/invented-disk/TestDisk/src\n")
    binary = tmp_path / "sfcop"
    binary.write_bytes(b"\0/invented-disk/TestDisk/src/main.cpp\0")
    return build, binary


@pytest.mark.parametrize("actions, code, said", [
    ("", 0, "a package for local tests only"),
    ("true", 1, "not allowed in GitHub Actions"),
])
def test_the_build_paths_bypass_is_refused_in_github_actions(tmp_path, actions, code, said):
    build, binary = leaky_build(tmp_path)
    env = {key: value for key, value in os.environ.items() if key != "GITHUB_ACTIONS"}
    env.update(SFCOP_ALLOW_BUILD_PATHS="1", HOME="/invented-home/testcaptain")
    if actions:
        env["GITHUB_ACTIONS"] = actions
    run = subprocess.run([sys.executable, CHECK_PATHS, str(build), str(binary)],
                         capture_output=True, text=True, env=env)
    assert run.returncode == code
    assert said in run.stderr


def test_check_paths_finds_toolchain_folders_on_the_checkouts_disk_only(tmp_path):
    build = tmp_path / "build"
    build.mkdir()
    (build / "CMakeCache.txt").write_text(
        "CMAKE_HOME_DIRECTORY:INTERNAL=/invented-disk/TestDisk/src/sfcop\n"
        "CMAKE_C_COMPILER:FILEPATH=/invented-disk/TestDisk/tools/llvm/1.0/bin/clang\n"
        "CMAKE_CXX_COMPILER:FILEPATH=/usr/bin/clang++\n")
    binary = tmp_path / "sfcop"
    binary.write_bytes(b"\0/invented-disk/TestDisk/tools/llvm/1.0/bin/x\0/usr/bin/xdg-open\0"
                       b"/invented/tools/sysroots/x.h\0")
    env = {key: value for key, value in os.environ.items() if key != "SFCOP_ALLOW_BUILD_PATHS"}
    env.update(HOME="/invented-home/testcaptain", SFCOP_TOOLS_DIR="/invented/tools")
    run = subprocess.run([sys.executable, CHECK_PATHS, str(build), str(binary)],
                         capture_output=True, text=True, env=env)
    assert run.returncode == 1
    assert "sfcop: the compiler's folder: 1" in run.stdout
    assert "sfcop: the tools folder: 1" in run.stdout
    assert "/usr/bin" not in run.stdout


def test_fetch_appimage_tools_needs_a_tools_folder(tmp_path):
    env = {key: value for key, value in os.environ.items()
           if key not in ("SFCOP_TOOLS_DIR", "SFCOP_APPIMAGE_TOOLS")}
    run = subprocess.run(["sh", os.path.join(ROOT, "packaging", "linux", "fetch_appimage_tools.sh")],
                         capture_output=True, text=True, env=env)
    assert run.returncode == 2
    assert "set SFCOP_APPIMAGE_TOOLS, or SFCOP_TOOLS_DIR" in run.stderr


def test_linux_make_tree_needs_a_tools_folder(tmp_path):
    env = {key: value for key, value in os.environ.items()
           if key not in ("SFCOP_TOOLS_DIR", "SFCOP_LLVM_DIR")}
    env["SFCOP_PYTHON"] = sys.executable
    run = subprocess.run(
        ["sh", os.path.join(ROOT, "packaging", "linux", "make_tree.sh"),
         str(tmp_path / "SFCOP-0.1.0-alpha-linux-x64"), invented(tmp_path / "sfcop", "invented elf\n"),
         invented(tmp_path / "s2", "two\n"), invented(tmp_path / "s3", "three\n"), ROOT,
         "0.1.0-alpha", "x64"],
        capture_output=True, text=True, env=env)
    assert run.returncode == 2
    assert "set SFCOP_TOOLS_DIR" in run.stderr


def test_a_cross_package_build_needs_the_tools_folder(tmp_path):
    env = {key: value for key, value in os.environ.items() if key != "SFCOP_TOOLS_DIR"}
    run = subprocess.run(["sh", os.path.join(ROOT, "scripts", "dist.sh"), "linux-x64",
                          str(tmp_path / "build")], capture_output=True, text=True, env=env)
    assert run.returncode == 2
    assert "set SFCOP_TOOLS_DIR" in run.stderr
    assert not (tmp_path / "build").exists()
