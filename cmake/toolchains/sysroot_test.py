"""Tests for sysroot.py: python3 -m pytest cmake/toolchains"""

import hashlib
import importlib.util
import os
import pathlib

_spec = importlib.util.spec_from_file_location(
    "sysroot", pathlib.Path(__file__).with_name("sysroot.py"))
sysroot = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(sysroot)


def make_tree(root: pathlib.Path) -> None:
    (root / "usr/lib").mkdir(parents=True)
    (root / "lib").mkdir()
    (root / "lib/libc.so.6").write_bytes(b"elf")
    (root / "usr/lib/libc.so").write_bytes(b"GROUP ( /lib/libc.so.6 )")
    os.symlink("/lib/libc.so.6", root / "usr/lib/libc-abs.so")


def test_tree_lines_list_files_and_links_by_path(tmp_path):
    make_tree(tmp_path)
    (tmp_path / ".sfcop-pin").write_text("stamp\n")
    assert sysroot.tree_lines(str(tmp_path)) == [
        "F lib/libc.so.6 " + hashlib.sha256(b"elf").hexdigest(),
        "F usr/lib/libc.so " + hashlib.sha256(b"GROUP ( /lib/libc.so.6 )").hexdigest(),
        "L usr/lib/libc-abs.so /lib/libc.so.6",
    ]


def test_tree_hash_ignores_timestamps_and_stamps(tmp_path):
    one, two = tmp_path / "one", tmp_path / "two"
    make_tree(one)
    make_tree(two)
    os.utime(two / "lib/libc.so.6", (0, 0))
    (two / ".sfcop-pin").write_text("UNSET\n")
    assert sysroot.tree_hash(str(one)) == sysroot.tree_hash(str(two))


def test_tree_hash_changes_with_one_byte(tmp_path):
    one, two = tmp_path / "one", tmp_path / "two"
    make_tree(one)
    make_tree(two)
    (two / "lib/libc.so.6").write_bytes(b"elF")
    assert sysroot.tree_hash(str(one)) != sysroot.tree_hash(str(two))


def test_relink_makes_absolute_links_point_inside_the_tree(tmp_path):
    make_tree(tmp_path)
    assert sysroot.relink(str(tmp_path)) == 1
    link = tmp_path / "usr/lib/libc-abs.so"
    assert os.readlink(link) == "../../lib/libc.so.6"
    assert link.read_bytes() == b"elf"
