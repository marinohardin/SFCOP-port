# src/hle/dinput: DirectInput

The game's DirectInput (DESIGN §5.7, §6.1): `DINPUT.DLL`'s `DirectInputCreateA`, `IDirectInputA` and
`IDirectInputDeviceA`, serving the system keyboard on the `Input` component and signalling through
`Threads`. The game asks for nothing else: it creates DirectInput with version 0x0700, creates
`GUID_SysKeyboard`, sets the `c_dfDIKeyboard` format it links, sets the cooperative level 0x16
(non-exclusive, foreground, no Windows key) and acquires (the rows' notes in `surface.tsv`). The
mouse comes from window messages (USER32), not from here.

| File | What |
| --- | --- |
| `DInputDefs.h` | Result codes, flags, GUIDs and the host forms of the game's structures |
| `KeyTable.h`, `.cpp` | USB usage ID (the key identity of `Input.h`) to `DIK_` code and key name, 119 keys |
| `KeyboardHub.h`, `.cpp` | What a DirectInput object and its devices share: the components, one lock, the pump from Input's queue |
| `DirectInput.h`, `.cpp` | `IDirectInputA` |
| `KeyboardDevice.h`, `.cpp` | `IDirectInputDeviceA` for the keyboard |
| `DinputBridge.h`, `.cpp` | The module as C functions for the shims, and `setPlatform()` for the runtime |
| `DinputGuest.c` | The guest shims, exporting `sfcop_hle_dinput[]`; built only with the lift |
| `*Test.cpp`, `DinputTestUtil.h` | doctests, one or more per entry, through `InputMock` and `ThreadsMock` |

## Behaviour

- **Immediate data.** `GetDeviceState` lays out `Input::GetKeyboard()` by the data format: with
  `c_dfDIKeyboard`, 256 bytes indexed by `DIK_` code, 0x80 for a key that is down. While the window
  has no focus every key reads up (`Input.h`).
- **Buffered data.** `GetDeviceData` returns records made from Input's event queue: each press and
  each release, in the order the window delivered them, with Input's timestamp (the `GetTickCount`
  scale) and Input's sequence number. Auto-repeats are not records (DirectInput reports changes of
  state). A press and release between two reads are both returned. `DI_BUFFEROVERFLOW` when the
  device's buffer (`DIPROP_BUFFERSIZE`) or Input's own queue dropped events since the last read that
  removed records. Events that arrive while the device is not acquired are not buffered.
- **One reader of Input's queue.** The module drains Input's queue on every device read and in
  `DirectInput::Pump()`, and hands each key event to every acquired keyboard; mouse events in the
  queue are discarded (nothing else reads Input's queue: the window queues its own events for USER32).
- **Notification.** `SetEventNotification` takes a game event `HANDLE`; the runtime resolves it to a
  `Threads` event (`Platform::waitableOfHandle`, from the kernel module's handle table), which is set
  whenever an acquired keyboard changes. So that it is set without the game reading first, the
  runtime calls `sfcop_dinput_pump()` after each `Window::PumpEvents()`.
- **Components.** The runtime calls `sfcop::hle::dinput::setPlatform()` with the window's `Input`,
  `Threads` and the handle lookup before the game runs. Until it does, objects work on an `Input`
  that no window feeds (every key reads up) and a `Threads` stub, so the game still boots.

## Differences from the spike's module (`src/app/boot/dinput.c`)

| | Spike | This module |
| --- | --- | --- |
| Source of keys | SDL2's keyboard state | The `Input` component, fed by the window |
| Buffered records | State compared at each read: a press and release between two reads are lost; records within one read in `DIK_` order | Every press and release, in event order |
| Sequence numbers | Per device, from 1 | Input's, shared by every device |
| Reading an unbuffered device | `DIERR_NOTACQUIRED` | `DIERR_NOTBUFFERED` (0x80040207), as DirectInput |
| `GetDeviceState` size | Any, padded or cut | The data format's size, else `DIERR_INVALIDPARAM`, as DirectInput |
| `SetEventNotification` | Accepted, never signalled | A `Threads` event, signalled |
| `EnumObjects`, `GetObjectInfo` | Nothing enumerated; `DIERR_OBJECTNOTFOUND` | The keys, with names; by offset or ID |
| Interfaces | 2A and 7A slots on both objects | The 8 and 18 slots of the surface; the game never asks for more |

## Acceptance: the oracle driver's key script

Done when (B3): the oracle driver's key script gives the same command sequence as the spike. The
driver (`oracle/driver/oracle_input.c`) is a Win32 program that types into the game's window inside
Wine; it cannot run against the native build yet, and `oracle/driver/skirmish.steps` has no `key`
steps yet. So the comparison is at the level of `DIK_` records, in `KeyScriptTest.cpp`:

- The script, in the driver's steps form (`key <vk>` presses a key and releases it 50 ms later;
  `sleep <ms>` waits):

  ```
  window 120 Starfleet Command
  sleep 1000
  key 0x1B
  sleep 300
  key 0x41
  sleep 300
  key 0x20
  sleep 300
  key 0x70
  sleep 300
  key 0x26
  sleep 300
  key 0x0D
  sleep 500
  ```

- Replayed into the SDL3 `Input` backend (`makeInput()`) as the window would deliver it, and read
  with a buffered `GetDeviceData` once every 16 ms frame through this module:

  `01+ 01- 1E+ 1E- 39+ 39- 3B+ 3B- C8+ C8- 1C+ 1C-` (Esc, A, Space, F1, Up, Enter; `+` press, `-`
  release)

- The spike's behaviour for the same script and frames, modelled from its header comment (state
  compared at each read): the same sequence, `01+ 01- 1E+ 1E- 39+ 39- 3B+ 3B- C8+ C8- 1C+ 1C-`.

- Where they differ: two keys tapped inside one frame (Esc 10 to 20 ms, A 30 to 40 ms, read at
  100 ms) give `01+ 01- 1E+ 1E-` here and nothing through the spike.

Run: `ctest --test-dir <build>` or `<build>/sfcop_tests --source-file='*dinput*'`. When the native
client can run the driver, the same script goes into `oracle/driver/` and this comparison moves to
the replay oracle.
