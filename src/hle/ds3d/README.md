# DirectSound and DirectSound3D

The `DSOUND.DLL`, `IDirectSound`, `IDirectSoundBuffer`, `IDirectSound3DBuffer` and
`IDirectSound3DListener` rows of `src/hle/surface.tsv` (72), on the Audio component
(`src/platform/Audio/`).

| File | What it is |
| --- | --- |
| `DsTypes.h`, `DsTypes.cpp` | Result codes, flags and structures as values; DirectSound's scales converted to Audio's |
| `DirectSound.h`, `DirectSound.cpp` | The device: cooperative level, caps, the primary buffer, secondary buffers and duplicates, the listener |
| `SoundBuffer.h`, `SoundBuffer.cpp` | A buffer: Lock and Unlock into an Audio clip, play, stop, status, position, volume, pan, frequency |
| `Buffer3D.h`, `Buffer3D.cpp` | A buffer's 3D parameters onto `Audio::SetVoice3D`, immediate or deferred |
| `Listener.h`, `Listener.cpp` | The listener onto `Audio::SetListener`, immediate or deferred; `CommitDeferredSettings` |
| `Ds3dGuest.cpp` | The shims, `sfcop_hle_ds3d[]`: `DirectSoundCreate` by name and as `dsound.dll#1`, and every method; built only with the lift |
| `DirectSoundTest.cpp`, `SoundBufferTest.cpp`, `Buffer3DTest.cpp`, `ListenerTest.cpp` | Every entry through `AudioMock`, success and error paths; `Buffer3DTest.cpp` also plays a 3D buffer through the real mixer |
| `Ds3dShimTest.cpp` | Every row has a shim, and the shim purges the bytes the row gives |
| `SelfTest.cpp` | The spike's DirectSound3D self-test against the module on the real mixer |

## How it plays

A secondary buffer's bytes are one Audio clip, played by the buffer's own voice. The game writes
them through the pointers `Lock` returns (memory the guest can address, from the shims' allocator)
and `Unlock` copies the written ranges into the clip. Duplicates share the bytes and the clip, each
with its own voice. The primary buffer is the output: its volume is the master gain.

Volume is in hundredths of a decibel (gain `10^(v/2000)`). Pan is DirectSound's law: the far side
is attenuated by the pan in hundredths of a decibel and the near side stays at unity, so a
centred buffer is at unity on both sides; the Audio component takes the same number over 10000. A 3D buffer is mono and is placed in the world
from its creation; `DS3DMODE_HEADRELATIVE` places it relative to the listener and
`DS3DMODE_DISABLE` returns it to its 2D pan. A change made with `DS3D_DEFERRED` reads back at once
and is heard after `CommitDeferredSettings`.

A buffer slower than the 1000 Hz the Audio component's clips start at plays a 1000 Hz clip at its
own rate. Methods that need a control the buffer was not created with return
`DSERR_CONTROLUNAVAIL`.

## Self-test

The spike's DirectSound3D self-test counted each result it asserted (13) and each call's stack
purge (13). `SelfTest.cpp` keeps both, in the same order with the same expected values. **26
checks**, all passing:

```sh
build/sfcop_tests -tc="the spike's DirectSound3D self-test*"
```

## Known differences

- The 3D distance law is miniaudio's inverse model, `min / (min + rolloff (d - min))`. It equals
  DirectSound3D's `(min / d) ^ rolloff` only at a rolloff factor of 1.
- An immediate 3D `Set` made after a deferred one applies the merged parameters at once, including
  the deferred ones (`Buffer3D.cpp`, `Listener.cpp`); DirectSound keeps them waiting for
  `CommitDeferredSettings`.
- `DSBSTATUS_LOCSOFTWARE` is never set in a buffer's status.
- The Miles and DirectSound modules each own a separate `AudioMiniaudio`, and both discard the
  completions they poll; nothing reads completions yet.
- The test shims are checked as text, as described in `src/hle/miles/README.md`; the shim files
  refer to `sfcop_mss_silent` from the temporary boot platform.
