# oracle/driver: steps files and the programs that play them

A steps file is a menu path written once and played by two drivers: `oracle_input.exe` into the
original under Wine, and the native client's own player (`sfcop --input`, `src/app/input/`). The
vocabulary and what each driver does with each step are in `src/app/input/README.md`, "Steps";
`oracle_input.c`'s header lists the Wine side.

| File | What |
| --- | --- |
| `oracle_input.c`, `build.sh` | The Wine driver: a Win32 program built by VC6, run in the game's prefix (`oracle/README.md`) |
| `skirmish.steps` | The Wine path towards the oracle skirmish, in the game's display pixels (800x600, the 640x480 menu centred); partly calibrated |
| `probe/probe.steps` | Wine only: lists the windows the driver can see |
| `menu.steps` | Native: boot to the main menu ("Select Game") and click SINGLE PLAYER, which opens `Setup`; in the game's display pixels |
| `run_native.sh` | Plays a steps file into the native client with a prepared data folder |

## The native runner

    SFCOP_ORACLE_DATA=<prepared data folder> oracle/driver/run_native.sh

runs `sfcop --data <folder> --windowed --input oracle/driver/menu.steps --watchdog 90` (90 s, not 30, until the boot time is fixed), and
`--headless`, `--fullscreen`, `--steps FILE`, `--watchdog S` and `-- <more sfcop flags>` change
it. The data folder must already hold a stored key, so the boot goes past the key prompt; agents
never make, read or list one. Its path comes from `SFCOP_ORACLE_DATA`, or from the first line of
the file `SFCOP_ORACLE_CONFIG` names (default `~/.config/sfcop/oracle-data`), both outside git. The
script starts with `set +x`, never prints the path, and reads nothing under it; without it, it
stops with exit 2. `SFCOP_BIN` picks the client (default `build/macos-arm64/sfcop`) and
`SFCOP_INSTALL` its `--install`.

The exit is the client's: 0 when the watchdog finds its goal mark (with a stored key, the main
menu, "Select Game"), 1 when not, 2 for a bad steps file or a wait in it that ran out.

## menu.steps

Waits for the `Select Game` mark (the entry of `tState_Namespace_Setup::tState_SelectGame`, the
main menu), lets the menu settle, then clicks SINGLE PLAYER at (237, 232) and waits for the screen
it opens: the `Setup` mark, the Single Player screen (the entry of
`tState_Namespace_Setup::tState_Setup`). A click that does not take ends the run with exit 2
after 10 s. The menus run in an
800x600 display mode with the 640x480 menu centred, the same space as the Wine driver's client
area, so the coordinates are the ones `skirmish.steps` has. `waitmark` is the native player's: the
Wine driver refuses the line, so this file is for the native client only.
