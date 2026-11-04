# kernel32rt: the run-time rows of KERNEL32, WINMM's timers and DBGHELP

The Win32 calls the game's C runtime and its threads live on: threads, events, mutexes,
semaphores and waits, critical sections, thread-local storage, the heap and virtual memory,
exceptions, time and the multimedia timers, the environment, module handles and the start-up
queries. Files, paths and `.ini` rows are `../kernel/`'s; this module is everything in KERNEL32
that is not about files.

| File | What it holds |
|---|---|
| `Kernel32rt.h` | Win32 constants, `Answer` (a value and the last error it sets), `Object` |
| `Sync.*` | `CreateThread`'s thread, events, mutexes, semaphores and waits on `Threads` |
| `CriticalSections.*` | A `CRITICAL_SECTION` by its guest address, as a recursive `Threads` mutex |
| `ThreadLocal.*` | `TlsAlloc`, `TlsGetValue`, `TlsSetValue` on `Threads`' slots |
| `Time.*` | Tick count, performance counter, `SYSTEMTIME` and `FILETIME` on `Clock` |
| `Timers.*` | `timeSetEvent` and `timeKillEvent`: one thread per timer |
| `VirtualMemory.*` | `VirtualAlloc`, `VirtualFree`, `VirtualQuery`: regions and the state of each page |
| `Dispatcher.*` | `RaiseException`'s and `RtlUnwind`'s walk over the guest's `fs:[0]` chain |
| `Modules.*` | DLLs the host provides itself (the game's `OSSpecific95/98.dll`) |
| `HostMemory*` | Whether a host range is mapped (`IsBadReadPtr`), per platform |
| `Runtime.*` | The one instance the rows share, attached to `Threads` and `Clock` by the guest host |
| `Kernel32rtGuest.cpp` | The shims, one per row, exported as `sfcop_hle_kernel32rt[]` |
| `*Test.cpp` | The doctests, through `ThreadsMock`, `ClockMock` and fakes of the guest |

## How it answers

- **Threads.** A `CreateThread` thread is a `Threads` thread that enters the guest's start routine
  through pcrecomp's `hle_call_guest`, which gives it its own guest stack and thread block. Its host
  stack is at least 16 MB (`kGuestThreadHostStackBytes`): lifted code keeps every guest call on it.
  `ExitThread` gives the machine up and jumps back to the start of the thread's body.
  `GetCurrentThreadId` is pcrecomp's id for the thread, the one `CreateThread` returned.
- **Kernel objects.** A thread, event, mutex or semaphore is an entry of win32hle's handle table of
  type 26 (`../README.md`, "Handle types") holding an `Object`; `CloseHandle`, which `../kernel/`
  answers and hands on, releases it through the closer registered here. A row given a handle of
  another type runs win32hle's own row of that name. `Runtime::WaitableOf()` gives the `Threads`
  event behind a handle, for DirectInput's `SetEventNotification`.
- **Blocking.** A wait, `Sleep`, entering an owned critical section and stopping a timer give the
  machine lock up for their length, so the thread they wait for can run guest code.
- **Heap.** One system allocator in guest memory (pcrecomp's `hle_heap_*`) answers the process
  heap, every `HeapCreate` heap and the `Global`/`Local` blocks; it replaces SmartHeap. A handle of a
  `Global` block is its address. `HeapDestroy` leaves the heap's blocks allocated.
- **Virtual memory.** Regions are reserved in the guest's 4 GB block (pcrecomp's `hle_vm_*`); the
  state and protection of each 4 KB page are kept and reported, not enforced. A page outside every
  region is described as the host maps it (the image and the heap as committed, the rest as free).
- **Exceptions.** `RaiseException` builds the `EXCEPTION_RECORD` and `CONTEXT` below the caller's
  stack and walks the chain (`Dispatcher`); the catch itself is entered by the guest host
  (`src/app/guest/`). A dispatch no frame takes runs `UnhandledExceptionFilter` (the game's filter
  if it set one, else a report on stderr) and ends the process with exit status 1, as Windows ends a
  thread whose exception reaches its outermost frame. `IsBad*Ptr` ask the host's mapping.
- **Time.** `GetTimeZoneInformation` gives the bias in force and no daylight rule
  (`TIME_ZONE_ID_UNKNOWN`): `Clock` gives the offset, not the zone's rules.
- **The environment** is private: `PATH` and `WINDIR` as any Windows has them; nothing of the host's
  environment reaches the game. `GetEnvironmentStringsW` is refused as on Windows 95 and 98, so the
  C runtime reads the narrow block.
- **Version.** Windows XP Service Pack 3 (5.1.2600, NT), one Pentium-class processor, 256 MB.
- **DBGHELP.** The game walks its own stack for a crash report. Lifted code has no x86 frames to
  walk: `StackWalk` finds no frame and the symbol rows find nothing, so the report is written
  without a call stack.

## Rows left to pcrecomp's win32hle

The import rows of this module's groups that still run win32hle's code, because the
row is shared with another module or is pure text conversion: `GetCommandLineA` (the command line
the host sets), `GetLastError` and `SetLastError` (every module's last error), `GetStdHandle` and
`SetStdHandle` (win32hle's `ReadFile` and `WriteFile` own the standard handles), the code-page
conversions and locale rows (`MultiByteToWideChar`, `WideCharToMultiByte`, `LCMapString*`,
`GetStringType*`, `CompareString*`, `GetLocaleInfo*`, `EnumSystemLocalesA`, `IsValidLocale`,
`GetUserDefaultLCID`), and OLE32's three rows (win32hle's `CoCreateInstance` already asks the class
factories a COM module registers with `hle_com_register_class`). The guest host logs each the first
time it is called.

## Tests

`sfcop_tests` (the doctests) and `sh scripts/check.sh`. The shims are compiled only with the lift;
the guest tests (`src/app/guest/README.md`) run them under a lifted program.
