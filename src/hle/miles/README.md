# Miles: the MSS32.DLL digital-sample API

The 26 `MSS32.DLL` rows of `src/hle/surface.tsv`, on the Audio component (`src/platform/Audio/`).

| File | What it is |
| --- | --- |
| `Miles.h`, `Miles.cpp` | The module: sample handles, formats, volume, pan, rate, user data and the double-buffered playback the game uses, each sample an Audio stream |
| `MilesGuest.cpp` | The shims, `sfcop_hle_miles[]`, by the decorated import names; built only with the lift |
| `MilesTest.cpp` | Every entry through `AudioMock`, success and error paths |
| `MilesShimTest.cpp` | Every row has a shim, and the shim purges the bytes its `@N` gives |
| `SelfTest.cpp` | The spike's Miles self-test against the module on the real mixer |
| `ShimRows.h` | Test helper: reads a `*Guest.cpp` and `surface.tsv` as text (used here and in `src/hle/ds3d/`) |

## How it plays

The game plays every Miles sample double-buffered. `AIL_sample_buffer_ready` names the buffer (0 or
1) to fill next, `AIL_load_sample_buffer` fills it and starts the sample, and a load of zero bytes
marks the end. Each load is converted to 16-bit PCM (8-bit signed or unsigned, IMA ADPCM decoded,
swapped stereo put in order) and queued on the sample's stream, so the game may reuse its memory at
once. A sample is playing while a loaded buffer is still to be heard. A starved sample (both
buffers played, no end marker) reads as done, and plays again when the next buffer is loaded.
Nothing calls back into the game: `AIL_serve` and the status calls drain the Audio completion
queue on the game's thread.

The output opens at 44100 Hz stereo. While the boot host's `sfcop_mss_silent` is set (`--silent`,
`--headless`) it is a null device: the mix runs in real time and nothing is heard.

## Self-test

The spike's self-test counted two kinds of check for its Miles calls: each result it asserted (19)
and each call's stack purge (29). `SelfTest.cpp` keeps both, in the same order with the same
expected values: the results on the module with `AudioMiniaudio` in manual-clock mode, the purges
on `MilesGuest.cpp`'s source. **48 checks**, all passing:

```sh
build/sfcop_tests -tc="the spike's Miles self-test*"
```

The DirectSound3D part (26 checks) is in `src/hle/ds3d/SelfTest.cpp`. The spike's other checks
belong to other modules (DirectInput, the wide-character files, Winsock and the rest): the 210 the
work package brief names is the spike's whole self-test across every module, and the sound subset
is 74 checks (48 Miles, 26 DirectSound3D).

## Limits and known differences

- The 26 rows have no WAVE-file entry, so there is no `WaveTest.cpp` and no `AIL_set_sample_file`.
- The shim tests (`MilesShimTest.cpp`, and the same in `src/hle/ds3d/`) read the shim source as text
  for argument counts and purge sizes; they do not run the shims, which are compiled only with the
  lift. `clang -fsyntax-only` was run over the shim files locally, not in CI.
- `MilesGuest.cpp` and `src/hle/ds3d/Ds3dGuest.cpp` refer to `sfcop_mss_silent`, which the temporary
  boot platform defines today. Whoever replaces that platform must keep providing the symbol.
- Up to 64 samples play at once (`kMaxSamples`); the Audio backend allows 64 streams, one each.
- Known, left for a later round: the strings `a_set_redist_directory` and `a_last_error` hand to
  the guest are leaked, and `a_load_sample_buffer` trusts the byte count the guest gives.
