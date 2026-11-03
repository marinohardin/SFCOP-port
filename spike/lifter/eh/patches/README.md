# Patches to pcrecomp (at `ef3f0da`)

`run_eh.sh setup` applies these to `work/lifter-eh/pcrecomp`, on top of the spike's
`spike/lifter/patches/pcrecomp-tib-hook.patch` (already in the copied tree).

| Patch | What | Upstreamable |
|---|---|---|
| `pcrecomp-eh-landing-pads.patch` | `generate.lift_function_linear` emits the landing pad (`RECOMP_EH_ENTER`, the `_ljump` dispatch and `RECOMP_EH_LEAVE` on every exit) whenever `eh_resume` is non-empty, not only when a continuation lies inside the body. The pad's default case already tail-calls an address it has no label for, so a continuation that lifted as an entry of its own resumes correctly. | Yes. It changes nothing for existing callers whose continuations are all in the body, and native32's `eh32.c` needs it as much as this runtime does. |

Not patched, and worth reporting upstream with the patch:

- `tools/lift/eh32.py` `funclet_returns` takes a continuation only when `mov eax, imm32` is the
  instruction right before `ret`. VC6 at `/O2` schedules stores between them
  (`mov eax, offset $L; mov [ebp-14h], ecx; ret`), so it misses those continuations, and it
  recognises only the `push ebp; mov ebp, esp` prologue, not `mov eax, stub; call __EH_prolog`.
  `../scripts/eh_frames.py` does both (eax tracked until written; `__EH_prolog` call sites).
