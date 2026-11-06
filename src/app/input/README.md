# src/app/input: input scripts

`sfcop --input SCRIPT` plays a steps file into the game through the path real input takes, so the
oracle driver needs no human (DESIGN §5.10). The same file drives the original under Wine
(`oracle/driver/oracle_input.c`) and the native client.

| File | What it does |
| --- | --- |
| `InputScript.h` | Reads a steps file. The first unknown or malformed line refuses the whole file, naming its line; the client then exits 2 |
| `ScriptPlayer.h` | Given the frame and `Clock::Ticks()`, returns the window events due. A click is a move, a left button down and a left button up, each in a frame of its own; a key is its down and its up |
| `ScriptedWindow.h` | The injection: a `Window` decorator around the real window, or the stub one headless |
| `ScriptedGpu.h` | The client's `Gpu`, forwarded, except that attaching it to the wrapper attaches it to the real window: the SDL backend finds the operating system's window by the `Window` object's address, so a Gpu attached to the wrapper would find none |
| `VirtualKeys.h` | The hand-written table from a steps file's Windows virtual key to the USB usage ID `Input` uses |
| `Marks.h` | The marks the game has reached, recorded by the guest host for `waitmark` |

## Steps

One per line; `#` as the first non-blank character makes a comment.

| Step | Native player | Wine driver |
| --- | --- | --- |
| `window <timeout_s> <title>` | ends at once: the player runs only inside the game's message pump, so the game has its window | waits for a visible window with that title |
| `sleep <ms>` | waits by the clock | the same |
| `click <x> <y>` | move, down, up in three frames | `SetCursorPos`, down, up |
| `key <vk>` | down, up in two frames | `keybd_event` down, up |
| `shot <file>` | skipped, with a log line; `--shots` takes the native captures | GDI capture |
| `waitfile <timeout_s> <path>` | waits for the file; a Windows path (`C:\...\Name.txt`) is looked up by its last name in the logs folder (`--logs DIR`, else `logs/` under the data folder), where the game writes its `.txt` and `.log` files, then in the data folder; a relative path is taken from the data folder | waits for the file |
| `list` | skipped, with a log line | prints the windows |
| `waitmark <timeout_s> <mark>` | waits for a mark of the lift, by name (`WinMain`, `CD-key prompt`, `Select Game` the main menu, `Setup` the Single Player screen) or 0x address (any mark `SFCOP_LIFT_EXTRA_MARKS` added) | refused: the driver stops with that line's number |
| `frames <n>` | waits n frames | refused, as above |

`<vk>` is a decimal or `0x` hexadecimal Windows virtual key. The native parser refuses a key its
table (`VirtualKeys.cpp`) lacks, naming the line, which is stricter than the Wine driver: that one
passes any number it reads to `keybd_event`.

A wait that runs out (`window`, `waitfile`, `waitmark`) fails the run: the client prints
`sfcop: input script line N: waited S s for ...; it did not come` and exits 2. Each step is logged
on stderr as it starts: `[input] line 5 at 7412 ms, frame 431: click 157 172`.

**Coordinates** are the game's display pixels: (0, 0) is the top left of the screen the game
draws. The menus run in an 800x600 display mode with the 640x480 menu centred (seen in the native
client's shots); the Wine driver's client area is that same display, so one coordinate means the
same point under both drivers (SINGLE PLAYER is (237, 232) in each). The native window's logical
size is the game's display mode, so the coordinates go to the window unchanged, and USER32 scales
them to the screen.

## How the script enters

`main.cpp` wraps the client's `Window` in a `ScriptedWindow` when `--input` is given, and hands the
wrapper to the guest host as its window. Nothing in `src/app/guest/` or `src/hle/` knows a script
is playing:

1. The guest host's pump calls `PumpEvents()` on the game's main thread. The wrapper pumps the
   real window first (its events are queued and delivered to `Input` as always), counts one frame,
   and asks the player for the events due.
2. Each scripted event is queued after the real ones and delivered to the attached `InputSink`
   (`Input`, which DirectInput reads), as the real window does.
3. The pump then polls: the real events first, then the script's, each going to USER32's message
   queue (`hle::user32::DeliverEvent`), which turns them into the game's messages.

A **frame** is one turn of the game's message pump. `Gpu::Present()` is not counted: headless,
DirectDraw is pcrecomp's own (`src/app/guest/README.md`, "TEMPORARY") and nothing presents, and one
counter for both modes keeps one steps file meaning the same in both.

**Headless**, the inner window is the stub, which fails every call. `PumpEvents()` still plays the
script and succeeds, and `PollEvent()` hands out the script's events, so USER32 sees them; `main.cpp`
attaches `Input` to the wrapper itself (the host attaches it only to a window it opened), so
DirectInput sees them too. USER32 has no window size headless and takes the coordinates unscaled.

**Focus.** A click or key on a window the operating system has not focused (another application in
front) is preceded by a `kFocusGained`, as the Wine driver brings the game's window to the front
before it clicks; USER32 then activates the game's window the way a click would.

## Tests

`InputScriptTest.cpp`, `ScriptPlayerTest.cpp`, `ScriptedWindowTest.cpp` and `ScriptedGpuTest.cpp`,
through `WindowMock`, `InputMock` and `ClockMock`, with every sequence written by hand; run in
`sfcop_tests`.

## Proof runs

On a MacBook Pro (M1 Pro, 120 Hz display), with the lifted build carrying the four marks, the
build's `gameroot` (a copy of a repaired install) and the integrator's test data directory, one
run at a time. Times are the guest's, from the `[mark]` lines.

`oracle/driver/run_native.sh --windowed` (the steps file `oracle/driver/menu.steps`, watchdog
30 s), three runs:

| Run | Select Game | Click | Setup | Exit |
| --- | --- | --- | --- | --- |
| 1 | 22.48 s | 23.12 s | 23.14 s | 0 |
| 2 | 11.49 s | 12.06 s | 12.08 s | 0 |
| 3 | 13.53 s | 14.13 s | 14.15 s | 0 |

**Not met: Setup within 10 s.** These runs reach Setup after 12 to 23 s (headless 13 to 20 s), so
the 10 s clause of the work package is open, pending the integrator's investigation of the boot
time; nothing here fixes it.

A window capture taken at the Select Game line shows the main menu, and one taken at the Setup
line shows the Single Player screen. The 88 mission scripts (`LoadLibraryA` of a `.scr`) load in
0.2 to 0.7 s just before the menu, so they are not what makes the boot long: the time goes before
them, while the main thread runs the game's own loading loop and pumps few frames.

The same with `--headless`, three runs: Select Game at 19.66, 16.64 and 13.25 s, Setup 0.55 s
after each, exit 0.

300 s at the menu with no script, `SFCOP_DDRAW4_STATS=1`: windowed, no fault, exit 0 at the
watchdog, 287 `[ddraw4]` lines, 59 to 120 fps after the menu (the cap is the display's 120 Hz;
nothing sets a lower one). Headless, no fault, exit 0; headless DirectDraw is pcrecomp's own and
prints no rate line.

A fresh, empty `--data` still stops at the CD-key prompt: `tools/boot_probe.sh -s <gameroot> -w 60
-t 1793749080 3 -- <build>/sfcop --install @GAME@ --data <empty dir> --headless --watchdog
@WATCHDOG@` gives the prompt in 3 of 3 (6.07, 5.96, 6.00 s), exit 0.
