# src/hle/user32: USER32 and COMDLG32

The game's windows, messages, mouse and keyboard, and dialogs (DESIGN §4.7, §5.6), on the `Window`
and `Input` components and the gdi32 module. The game makes one top-level window of its own class,
subclasses it, runs a `PeekMessageA` loop while it is active (a blocking `GetMessageA` loop while it
is not), reads the cursor with `GetCursorPos` every frame, and draws everything through DirectDraw.

| File | What |
| --- | --- |
| `User32Defs.h` | The USER32 constants the module uses |
| `Keys.h`, `.cpp` | USB usage ID to virtual key and set-1 scan code; the characters of a US layout; `MapVirtualKeyA`, `VkKeyScanA` |
| `WindowManager.h`, `.cpp` | Classes, windows and their rectangles, long values, focus and activation, invalid regions, menus, the clipboard, hooks, the cursor |
| `MessageQueue.h`, `.cpp` | The thread's queue: platform events become messages; `PeekMessageA`'s order; `TranslateMessage`; key state |
| `Dialog.h`, `.cpp` | Dialog templates, the button, edit and static controls, keyboard and mouse, drawing with GDI |
| `User32.h`, `.cpp` | The rows: sending and dispatching, creation and destruction messages, `DefWindowProcA`, modal dialogs, painting, metrics; the Presenter on `Gpu` |
| `User32Host.cpp` | `AttachHost()`, how the runtime gives the module its components; `DeliverEvent()`, the one way a platform event reaches the queue |
| `User32Marshal.h`, `.cpp` | The structure layouts the rows read and write (MSG, RECT, POINT, WNDCLASSA, PAINTSTRUCT, DEVMODEA), `SystemParametersInfoA`'s answers and `wsprintfA`'s format, as pure functions on byte images; tested without the lift |
| `User32Guest.cpp` | The guest shims, exporting `sfcop_hle_user32[]` (the USER32 and COMDLG32 rows); built only with the lift |
| `*Test.cpp`, `User32TestRig.h` | doctests through `WindowMock`, `InputMock` and a fake of the game's procedures |

## Behaviour

- **The pump.** Each `PeekMessageA`, each turn of `GetMessageA`'s wait and of a modal dialog's loop
  runs the host's pump (`Host::pump`), on the game's main thread only. The host's pump is the one
  drain of the `Window` (`src/app/guest/GuestPump.h`): it reads each event, hands it to
  `DeliverEvent()` (which reaches `User32::OnWindowEvent` and `MessageQueue::Feed`), then does its
  DirectInput and screenshot work. This module never calls `Window::PumpEvents()` itself, so no event
  is read twice or lost between two drains. Events the pump reads before the first USER32 row makes
  the module are kept (256 at most) and delivered when it exists.
- **Threads.** The queue is one, the main thread's. `PeekMessageA` on another thread reads it without
  pumping. `GetMessageA` on another thread takes nothing and never pumps: it waits, giving the machine
  up, until the host is shutting down (`Host::stopping`), and then returns `WM_QUIT`.
- **An early close.** A close request that arrives before any window exists is remembered, and the
  first window made gets the `WM_CLOSE`.
- **Coalescing.** A mouse move that finds the last unread posted message to be a move to the same
  window with the same buttons replaces it, as Windows does: a game that does not read its queue for
  seconds (a load) gets one move, not thousands. Buttons and keys are never merged or dropped. The
  queue holds at most 10,000 posted messages (Windows' default limit); `PostMessageA` fails beyond, and the host's own input is dropped there, with a line in the message trace (`posted queue full: message dropped`).
- **Input.** Keys go to the focus window (the active window when nothing in it has the focus) as
  `WM_KEYDOWN`/`WM_KEYUP`, or `WM_SYSKEYDOWN`/`WM_SYSKEYUP` with Alt held and for F10, with the
  scan code, extended and transition bits in `lParam`. `TranslateMessage` makes `WM_CHAR` from a US
  layout; the platform's text events are not used. The mouse goes to the window that captured it or
  the active window, in its client coordinates; the platform reports logical window pixels, which
  are scaled to the screen. `GetCursorPos` is the last mouse event's position on the screen.
  `GetKeyState` answers from the queue, `GetAsyncKeyState` from `Input` now.
- **Activation.** Showing a top-level window activates it (`WM_ACTIVATEAPP`, `WM_ACTIVATE`,
  `WM_SETFOCUS`), except for the `SW_SHOWNOACTIVATE` family. The platform window losing focus
  deactivates the active window and forgets the keys held; regaining it reactivates that window. A
  focus event while a window is already active announces nothing. A mouse button that arrives while
  no window is active (macOS may report the click before the focus) is held, and when the focus
  comes Windows' click-activation sequence follows: `WM_MOUSEACTIVATE`, `WM_ACTIVATEAPP`,
  `WM_ACTIVATE` with `WA_CLICKACTIVE`, `WM_SETFOCUS`, then a `WM_MOUSEMOVE` to the click and the
  button. A held click is dropped when the focus is lost again or when `ShowWindow` activates a
  window, and a held button-up without its down is never kept. The close box posts `WM_CLOSE` to the active window.
- **Tracing input.** With `--trace`, or with `SFCOP_USER32_TRACE=1` alone (the per-call trace
  slows the game to about 56 frames per second, which changes what a click meets), the queue
  logs the first 400 lines of: each platform focus and button event with the queue's clock, the
  queue's reaction to it (posted, held for the activation, dropped), each button message
  `PeekMessageA` hands out, and each change in what `GetKeyState` or `GetAsyncKeyState` answers
  for a mouse button. Kinds and order only: no positions, no text. In play the game reads the
  mouse only through the button messages, drained each frame by a `PeekMessageA` loop; it never
  calls `GetAsyncKeyState`, and `GetKeyState` only for keyboard keys (X4, 2026-11-06).
- **The screen** is the game's display: the platform window's logical size, which follows
  DirectDraw's display mode (640x480 until a window says otherwise), at 32 bits. `SetWindowPos`
  changes only the game's view of its rectangle; the platform window's size is DirectDraw's.
  `EnumDisplaySettingsA` lists 640x480 to 1600x1200 at 16 and 32 bits; `ChangeDisplaySettingsA`
  accepts and changes nothing.
- **Painting.** A shown window is invalid until `BeginPaint`, `ValidateRect`'s equivalent in
  `DefWindowProcA`, or `UpdateWindow` paints it; `WM_PAINT` comes after the posted messages and
  `WM_QUIT`. `GetDC` and `BeginPaint` give a DC on the screen that draws nowhere: the game's frames
  are DirectDraw's.
- **Dialogs.** `DialogBoxIndirectParamA` (the game's "Choose a Display", a template it builds in
  memory) and `MessageBoxA` make a dialog window and its controls, run a modal loop that sends the
  game's dialog procedure `WM_COMMAND` for each button pressed, and draw the dialog with gdi32 onto a
  bitmap that the Presenter writes into DirectDraw's display texture and presents. When nothing can
  be shown (headless, or the Presenter shows nothing because DirectDraw has no primary surface yet)
  the dialog answers with the first enabled push button's id in template order, whatever the
  template's default, sending that button's `WM_COMMAND` first; a dialog with no push button ends
  with 0. `MessageBoxA` answers the same way, except that Retry is pressed at most three times and
  then the Cancel (or Abort) button. Only "dialog answered: button <id>" is logged: never a caption,
  title or text. The game's only dialog is "Choose a Display" (one call site, 0x006AACD1); no run
  made here reaches it, and the CD-key prompt is not a dialog but a screen the game draws through
  DirectDraw.
- **Hooks** are recorded, never called. The game's one hook, a low-level keyboard hook, only keeps
  Alt+Tab, Escape and Ctrl+Escape from Windows' task switcher, which the platform window never passes
  on.
- **Menus** are bookkeeping: `LoadMenuA` gives an empty menu, nothing is drawn. A top-level window
  with a menu has a menu bar's height above its client area, as `AdjustWindowRect` assumes.
- **COMDLG32.** The file dialogs answer cancel; `CommDlgExtendedError` says no error.

## The runtime's part

The runtime calls `sfcop::hle::user32::AttachHost()` once, on the game's main thread, before any guest
code runs, with the `Window`, `Input` and `Gpu`, its own pump, a function naming DirectDraw's
display texture, and one saying the run is ending. The module holds those references for the life of
the runtime's `Run()`. A second call is logged and returns false (the runtime fails the run); a USER32
row that runs before the first call is logged and ends the run. No stubs are made in their place.
