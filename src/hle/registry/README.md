# src/hle/registry: ADVAPI32 on Store's registry

The game's nine ADVAPI32 imports (`surface.tsv`): the registry calls it makes and the event log it
opens. The values live in `registry.tsv` in the per-user folder, which Store's host backend reads
once and writes atomically on every change (its header line gives the form). Nothing is read from
or written to a host registry, on any platform.

| File | What |
|---|---|
| `Registry.h`, `Registry.cpp` | The module: key handles, the three value types, the event log |
| `RegistryBridge.h`, `RegistryBridge.cpp` | The module as the C shim calls it, one per process, behind a lock |
| `RegistryGuest.c` | The guest shims, `sfcop_hle_registry[]` (built only with the lift) |
| `RegistryTest.cpp` | Every row through `StoreMock`; the round trip through a fresh Store on the same folder |
| `RegistryBridgeTest.cpp` | The bridge, on a folder of the test's own |

## Behaviour

- **Key names.** A key is stored under the predefined key's name and the path the game gives,
  `HKEY_LOCAL_MACHINE\SOFTWARE\...`; keys and value names compare without ASCII case. The handles
  the game holds are the module's own numbers; the predefined keys (`0x80000000` to `0x80000006`)
  need no opening.
- **Values.** `REG_DWORD` (exactly 4 bytes), `REG_SZ` (stored up to its NUL; read back with it, and
  its size counts it) and `REG_BINARY`. Any other type is `ERROR_INVALID_PARAMETER`.
  `RegQueryValueExA` gives the type, and with a buffer too small `ERROR_MORE_DATA` and the size
  needed, writing nothing.
- **`RegDeleteKeyA`** deletes the subtree, where Windows NT refuses a key with subkeys: Store has no
  way to list a key's subkeys.
- **Errors.** A missing key or value is `ERROR_FILE_NOT_FOUND`; a closed or unknown handle
  `ERROR_INVALID_HANDLE`; a subkey starting with `\` `ERROR_BAD_PATHNAME`; a Store failure
  `ERROR_REGISTRY_IO_FAILED` (nothing changed). The Reg* rows leave `GetLastError` alone.
- **Event log (stubs).** `RegisterEventSourceA` gives a handle, `ReportEventA` accepts a report for
  an open source and keeps nothing (a report could hold what DESIGN §9 keeps out of logs), and
  `DeregisterEventSource` closes the handle.

Until the guest runtime attaches the module to the client's per-user folder, the shim attaches it
on its first call to the folder the client uses without `--data` (`src/app/DataDir.h`).

The keys the original game names: `SOFTWARE\Quicksilver\Debug`, the CD-key keys and the matchmaking
service's key. Tests use made-up keys and values only.
