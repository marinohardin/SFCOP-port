"""eh_frames.py: the static map the landing pads are built from.

Synthetic x86 bytes, hand-assembled, in a fake image; expected addresses are
written out. Needs capstone and pefile (the lifter-eh venv has both):

    $LIFTER/venv/bin/python -m pytest spike/lifter/eh/tests
"""
import os
import struct
import sys

import pytest

pytest.importorskip('capstone')
pytest.importorskip('pefile')
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'scripts'))
import eh_frames  # noqa: E402
from capstone import CS_ARCH_X86, CS_MODE_32, Cs  # noqa: E402


class FakeImage(eh_frames.Image):
    """The Image interface over a dict of bytes, without a PE file."""

    def __init__(self, chunks, text=(0x1000, 0x9000)):
        self.mem = {}
        for va, raw in chunks.items():
            self.mem.update({va + i: b for i, b in enumerate(raw)})
        self.text_va, self.text_end = text
        self.md = Cs(CS_ARCH_X86, CS_MODE_32)
        self.md.detail = True

    def read(self, va, n):
        return bytes(self.mem.get(va + i, 0xCC) for i in range(n))


H = bytes.fromhex


def test_continuation_is_the_immediate_eax_holds_at_ret():
    img = FakeImage({0x1100: H('b850100000' 'c3')})                   # mov eax, 0x1050; ret
    assert eh_frames.funclet_continuations(img, 0x1100) == [0x1050]


def test_continuation_survives_instructions_scheduled_before_the_ret():
    # mov eax,[ebp-14h]; mov ecx,[eax+4]; mov eax,0x1335; mov [ebp-14h],ecx; ret
    img = FakeImage({0x1100: H('8b45ec' '8b4804' 'b835130000' '894dec' 'c3')})
    assert eh_frames.funclet_continuations(img, 0x1100) == [0x1335]


def test_eax_overwritten_before_ret_is_not_a_continuation():
    img = FakeImage({0x1100: H('b850100000' '8b45f8' 'c3')})          # mov eax,imm; mov eax,[ebp-8]; ret
    assert eh_frames.funclet_continuations(img, 0x1100) == []


def test_both_arms_of_a_branch_are_followed():
    # test ecx,ecx; je +6; mov eax,0x1050; ret; mov eax,0x1060; ret
    img = FakeImage({0x1100: H('85c9' '7406' 'b850100000' 'c3' 'b860100000' 'c3')})
    assert eh_frames.funclet_continuations(img, 0x1100) == [0x1050, 0x1060]


def test_arms_that_load_different_continuations_and_share_a_ret_give_both():
    # 0x1100 test ecx,ecx; je 0x110B; mov eax,0x1050; jmp 0x1110
    # 0x110B mov eax,0x1060
    # 0x1110 mov [ebp-14h],ecx; ret
    img = FakeImage({0x1100: H('85c9' '7407' 'b850100000' 'eb05' 'b860100000' '894dec' 'c3')})
    assert eh_frames.funclet_continuations(img, 0x1100) == [0x1050, 0x1060]


def test_a_catch_that_only_rethrows_has_no_continuation_and_counts_the_throw():
    # push 0; push 0; call 0x2000 (the throw helper, noreturn)
    call = 0x2000 - (0x1104 + 5)
    img = FakeImage({0x1100: H('6a00' '6a00') + b'\xE8' + struct.pack('<i', call)})
    throws = [0]
    assert eh_frames.funclet_continuations(img, 0x1100, noreturn={0x2000}, throws=throws) == []
    assert throws == [1]


def test_scope_table_reads_except_and_finally_entries_and_stops_at_garbage():
    table = struct.pack('<iII', -1, 0x1200, 0x1210) + struct.pack('<iII', 0, 0, 0x1220) \
        + struct.pack('<iII', 7, 0x1230, 0x1240)                          # enclosing 7 > index: not an entry
    img = FakeImage({0x8000: table})
    assert eh_frames.scope_table(img, 0x8000) == [
        {'enclosing': -1, 'filter': 0x1200, 'handler': 0x1210},
        {'enclosing': 0, 'filter': 0, 'handler': 0x1220},
    ]


def test_a_push_matched_inside_another_instruction_is_not_a_prologue():
    # 0x1000: push ebp; mov ebp,esp; push -1; push 0x3000 (the real push, at 0x1005)
    # 0x1100: mov eax, 0x00300068 -- the bytes 68 00 30 00 sit inside it, at 0x1101
    img = FakeImage({0x1000: H('558bec6aff6800300000'), 0x1100: H('b868003000' 'c3')})
    assert eh_frames.at_prologue(img, 0x1000, 0x1005)
    assert not eh_frames.at_prologue(img, 0x1100, 0x1101)
    assert not eh_frames.at_prologue(img, 0x1000, 0x1000 + eh_frames.PROLOGUE_BYTES + 1)


def test_prologue_kinds():
    img = FakeImage({0x1000: H('558bec6aff'), 0x1100: H('6aff6800300000'), 0x1200: H('b800300000')})
    assert eh_frames.prologue_kind(img, 0x1000, 0x1005) == ('ebp', 5)
    assert eh_frames.prologue_kind(img, 0x1100, 0x1102) == ('fpo', 2)
    assert eh_frames.prologue_kind(img, 0x1200, 0x1200) == ('other', 0)


def test_landing_pads_join_cxx_and_seh_owners():
    frames = {
        'cxx': [{'owners': [{'function': 0x1000}, {'function': 0x1400}],
                 'try_blocks': [{'catches': [{'continuations': [0x1050]}, {'continuations': []}]}]}],
        'seh': [{'function': 0x2000, 'scopes': [{'filter': 0x2100, 'handler': 0x2110},
                                                {'filter': 0, 'handler': 0x2120}]}],
    }
    pads, owners = eh_frames.landing_pads(frames)
    assert pads == {0x1000: {0x1050}, 0x1400: {0x1050}, 0x2000: {0x2110}}
    assert owners == {0x1050: {0x1000, 0x1400}, 0x2110: {0x2000}}
