"""Lift placed object code to C with pcrecomp, through the code paths cmake/lifter/run_lift.py
uses for the exe: generate.true_extent for a body's extent, linear_disassemble_function,
eh32.resume_points for catch landing pads, and lift_function_linear with
lift32.Lifter(precise_carry=True, precise_sbb=True).

pcrecomp lifts from a code buffer and its start address, so no PE is needed: each placed code
section is its own buffer, relocated, at the address it was placed at. Needs capstone, which the
lifter container (cmake/lifter/Dockerfile) has.
"""
import os
import re
import sys

NOT_LIFTED_RE = re.compile(r'RECOMP_ICALL\(0x([0-9A-F]{8})u\);[^\n]*/\* not lifted \*/')


class LiftError(Exception):
    pass


def load_pcrecomp(tools_dir):
    """Import pcrecomp's lifter modules from <pcrecomp>/tools. Returns a namespace."""
    for sub in ('lift', 'pe', 'disasm'):
        p = os.path.join(tools_dir, sub)
        if p not in sys.path:
            sys.path.insert(0, p)
    try:
        import eh32
        from capstone import CS_ARCH_X86, CS_MODE_32, Cs
        from generate import linear_disassemble_function, lift_function_linear, true_extent
        from lift32 import Lifter
    except ImportError as e:
        raise LiftError(f'cannot import pcrecomp\'s lifter from {tools_dir} ({e}); run objlift in '
                        'the lifter container (tools/objlift/objlift.sh) or with a Python that '
                        'has capstone') from None

    class Ns:
        pass
    ns = Ns()
    ns.eh32, ns.Cs, ns.Lifter = eh32, Cs, Lifter
    ns.CS_ARCH_X86, ns.CS_MODE_32 = CS_ARCH_X86, CS_MODE_32
    ns.true_extent, ns.linear = true_extent, linear_disassemble_function
    ns.lift_function_linear = lift_function_linear
    return ns


class Body:
    """One lifted function."""
    __slots__ = ('va', 'name', 'c', 'section', 'claimed')

    def __init__(self, va, name, c, section, claimed):
        self.va, self.name, self.c, self.section, self.claimed = va, name, c, section, claimed


def lift_sections(pcr, sections, images, entries_of, read, lifted, iat_map, enter_hook):
    """Lift every entry of every placed code section.

    sections:   the placed code sections (placement.InputSection with va set)
    images:     section -> relocated bytes
    entries_of: section -> sorted entry addresses in it
    read:       read(va, n) -> bytes or None, over the placed sections and the exe
    lifted:     every address a direct call may name as sub_XXXXXXXX
    enter_hook: va -> C statement to put before RECOMP_ENTER, or ''
    Returns [Body], sorted by address. Raises LiftError for a body the lifter cannot handle.
    """
    md = pcr.Cs(pcr.CS_ARCH_X86, pcr.CS_MODE_32)
    md.detail = True
    lifter = pcr.Lifter(iat_map=iat_map, lifted=None if lifted is None else set(lifted),
                        precise_carry=True, precise_sbb=True)
    out = []
    for sec in sections:
        code = images[sec]
        start, end = sec.va, sec.va + len(code)
        entries = set(entries_of[sec])
        for addr in sorted(entries):
            name = 'sub_%08X' % addr
            reached = set()
            body_end, _clean = pcr.true_extent(md, code, start, addr, end, entries, reached=reached)
            lo = min(reached) if reached else addr
            insns, leaders = (pcr.linear(md, code, start, lo, body_end, reached=reached)
                              if body_end > addr else ([], None))
            if leaders is not None:
                leaders.add(addr)
            resume = None
            if insns:
                head = [i for i in insns if i.address >= addr][:pcr.eh32.PROLOGUE_WINDOW]
                resume = pcr.eh32.resume_points(md, read, head)
            try:
                c = (pcr.lift_function_linear(lifter, name, insns, leaders, addr, eh_resume=resume)
                     if insns else 'void %s(void) { }\n' % name)
            except Exception as e:  # noqa: BLE001 -- reported with the address, never hidden
                raise LiftError(f'pcrecomp could not lift {name} ({sec.label}): '
                                f'{type(e).__name__}: {e}') from None
            if 'UNIMPLEMENTED' in c:
                raise LiftError(f'{name} ({sec.label}) has instructions pcrecomp does not lift: '
                                + ', '.join(sorted(set(re.findall(r'UNIMPLEMENTED: (\S+)', c)))))
            missing = NOT_LIFTED_RE.findall(c)
            if missing:
                raise LiftError(f'{name} ({sec.label}) calls ' + ', '.join(f'0x{m}' for m in
                                sorted(set(missing))) + ', which the exe lift has no body for')
            hook = enter_hook(addr)
            if hook:
                enter = 'RECOMP_ENTER(0x%08Xu);' % addr
                if enter not in c:
                    raise LiftError(f'{name}: no RECOMP_ENTER line to hook')
                c = c.replace(enter, hook + ' ' + enter, 1)   # before: g_cur_func is the caller
            out.append(Body(addr, name, c, sec, sec.mode == 'claimed'))
    return sorted(out, key=lambda b: b.va)
