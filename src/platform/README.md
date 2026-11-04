# src/platform: the platform components

The components the HLE modules in `src/hle/` translate the Win32 surface onto (DESIGN §6.2,
§7.1). Each is an abstract C++ class in `src/platform/<Name>/<Name>.h`; the game never sees them,
and they know nothing of Win32.

| Component | Serves | Header |
| --- | --- | --- |
| `Window` | the game's window, display modes, the OS event loop, the frame cap | `Window/Window.h` |
| `Gpu` | DirectDraw 4 surfaces and Direct3D 3 fixed-function drawing, present | `Gpu/Gpu.h` |
| `Input` | DirectInput keyboard and mouse: state and a timestamped event queue | `Input/Input.h` |
| `Audio` | the Miles sample API and DirectSound/DirectSound3D: clips, voices, 3D, streams | `Audio/Audio.h` |
| `Transport` | Winsock UDP and TCP sockets; `Rendezvous` for Phase 3 | `Transport/Transport.h` |
| `Store` | install files (read-only), the per-user directory, atomic writes, the registry store | `Store/Store.h` |
| `Clock` | millisecond ticks, the high-resolution counter, the wall clock, sleep | `Clock/Clock.h` |
| `Threads` | threads, events, mutexes, semaphores, waits, thread-local slots | `Threads/Threads.h` |

`Core/` holds what they share: `Result.h` (the one `Error` enum and `Result<T>`), `Handle.h`
(typed handles) and `MockLog.h` (the call recorder the mocks use; tests only).

## Rules

- **No third-party type in a header.** No SDL, miniaudio or operating-system type, and no Win32
  header, appears in any header here. A backend keeps them in its own source file.
- **Expected failures are values.** Every method returns `Result<T>` or `Status`; nothing throws
  for a failure the caller can expect. Each method's comment lists the errors it may return.
- **Method names avoid the Windows SDK's A/W macro names** (`CreateEvent`, `CreateWindow`,
  `PlaySound`, `CreateDirectory`, ...), so that a Windows backend can include `<windows.h>`.
  For the same reason, write `(std::min)(a, b)` and `(std::max)(a, b)` in any header here.
- Each method's comment states its inputs, outputs, errors and threading; a header's top
  comment states the component's threading rule. That comment is the contract a backend is
  tested against.
- These headers are contracts (`.github/hotspots.txt`): changing one is a contract change.

## Files per component

| File | What |
| --- | --- |
| `<Name>.h` | The interface, and `make<Name>Stub()` |
| `<Name>Mock.h` | A test double that records calls (`MockLog`) and returns programmed values |
| `<Name>Stub.cpp` | The stub backend: compiles everywhere, every method returns `kUnsupported` |
| `<Name>Test.cpp` | doctest: a caller driven through the mock, and the stub |
| `<Name>_<platform>.cpp` | A real backend for one platform (Wave 1) |

The build collects these by name (`cmake/SfcopSources.cmake`): every `.cpp` here except tests
goes into `sfcop_src`, `*Test.cpp` into `sfcop_tests`, and `*_macos.*`, `*_windows.*`,
`*_linux.*` only on that platform. Nothing here needs a CMake edit.

## Adding a backend (CONVENTIONS §8)

1. Read the component's header; it is the contract. If the backend needs a change to it, that is
   a contract change, made by the integrator first.
2. Write the backend in `<Name>/<Name><Backend>_<platform>.cpp` (or without a platform suffix if
   it builds everywhere, as SDL-based backends do), with a factory function declared in a header
   of its own beside it, so the interface header does not change.
3. Test every method through the interface (`<Name><Backend>Test.cpp`): its success and each
   error the contract lists. No real clock, network or display in a unit test; a test that needs
   one is marked and skipped where it is absent.
4. A backend that brings a dependency goes through `third_party/INVENTORY.md` (integrator).

## How the components meet

- `Window::PumpEvents()` is the only reader of the operating system's event queue. It queues
  window events and delivers key, mouse and focus events to the `InputSink` attached with
  `Window::AttachInput()`. Every `Input` is an `InputSink`, so any `Input` backend works with
  any `Window` backend; the window also applies the relative mouse mode the sink asks for.
- `Gpu::Attach(Window&)` binds the renderer to the window; a `Gpu` backend accepts only a
  `Window` of its own family. `Gpu` runs on the main thread, the one that pumps the window.
  `Gpu::Present()` applies the frame cap stored in the `Window`.
- Nothing calls back into the game from a backend thread. `Audio` records ended voices and
  finished stream buffers in a queue the caller drains with `PollCompletion()` on its own thread.
- A periodic timer is built from `Threads` and `Clock` (see the top of `Threads/Threads.h`).
- Event timestamps (`InputEvent::timeMs`, `WindowEvent::timeMs`) are on the `Clock::Ticks()`
  scale.
- `Store` is the only code that knows the install and per-user paths (DESIGN §6.6); where the
  per-user directory is on each platform is decided outside this directory.
