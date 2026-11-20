# Headless boot results (TEMPORARY: the boot platform leaves with Wave 1)

How often the headless boot reaches the CD-key prompt, for `sfcop` and for the P1-2 spike's own
host binary, and why the boots that stop there stop. Every file here was written by
`tools/boot_probe.sh` or `tools/install_scan.py`; the commands are at the end.

## What decides the outcome

1. **The wall clock at start.** The game seeds its random numbers from the clock, and its first
   draws pick one of 32 space backgrounds, `assets/models/space/space00.mod` to `space31.mod`, which
   the boot loads before the CD-key prompt. The pick advances by one background about every five
   minutes of clock time and wraps after 32. At a given clock time the outcome is the same on every
   run (`ten_runs_at_1538_*.tsv`, `recheck_1528_sfcop.tsv`).
2. **The background.** On the install these runs used, 12 of the 32 backgrounds stop the boot:
   - 10 raise a guest C++ exception (`tErrInfo`) that nothing catches, because the boot platform
     has no guest exception handling until Wave 1 B6;
   - `space01` and `space23` fault, reading guest address 4.

   The other 20 reach the prompt.
3. **The install.** Each of the 12 names a damaged file. None of the 20 does. `install_scan_space.txt`
   lists the damaged files in `assets/models/space/`: empty files, and BMPs shorter than their own
   header says. The whole install has 104 such files (26 empty, 78 truncated BMPs).

| Background | Outcome | Damaged file it names |
| --- | --- | --- |
| space01 | fault | `_SF_11.bmp` (truncated) |
| space02 | throw | `_SF_20.bmp`, `_SF_22.bmp` (truncated) |
| space03 | throw | `_SF_28.bmp` (truncated) |
| space09 | throw | `SpTx256-B.BMP` (truncated) |
| space10 | throw | `_SF_65.bmp` (truncated) |
| space11 | throw | `_SF_67.bmp` (truncated) |
| space20 | throw | `B_03.bmp` (truncated), `B_04.bmp` (empty) |
| space23 | fault | none of its textures; the model itself is 3,153 bytes, where the original disc's file list gives 4,372 |
| space24 | throw | the model itself, `space24.mod` (empty) |
| space26 | throw | `_SF_11.bmp` (truncated) |
| space29 | throw | `spacetex02.bmp` (truncated), `spacetex04.bmp` (empty) |
| space31 | throw | `_NSF_04.bmp` (truncated) |

A model names its textures as plain strings (`strings -n 3 space20.mod`).

## sfcop against the spike

The two binaries agree at every one of the 91 clock times of `clock_sweep_*.tsv`:
- sfcop reached the prompt where the spike stayed alive to the watchdog (the spike's lift has
  no mark at the prompt);
- both threw at the same times;
- both faulted at the same times.

The copy into `src/app/boot/` changed nothing here.

| File | Runs | sfcop | Spike |
| --- | --- | --- | --- |
| `clock_sweep_*.tsv` | 91 each, clock 15:00 to 18:00 every 2 min, watchdog 8 s | 57 prompt, 1 alive, 27 throw, 6 fault | 58 alive, 27 throw, 6 fault |
| `recheck_1528_sfcop.tsv` | 3, clock 15:28, watchdog 15 s | 3 prompt (the sweep's one `alive`: the prompt came after its 8 s watchdog, with two sweeps running) | - |
| `ten_runs_*.tsv` | 10 each, the real clock, run at the same time (16:29:34 to 16:29:50) | 10 throw (`space31`) | 10 throw |
| `ten_runs_at_1538_*.tsv` | 10 each, clock 15:38 (`space21`) | 10 prompt, at 5.85 to 6.43 s | 10 alive |

In the sweeps, the prompt came 5.5 to 7.1 s after start.

The clock column is the local time of day of the machine that ran the probes. The spike's
exit codes are its own (3 for a throw, 4 at the watchdog). sfcop's follow DESIGN §6.8: 0 at the
prompt, 1 otherwise.

## Reproduce

From the repository root, with `GAME` set to the install directory. Each run gets a fresh clone
of `GAME`. The spike's binary is built by the Phase 1 spike's `run_all.sh` (since removed)
into `$SFCOP_WORK/lifter-hle/build/sfcop`. Epoch 1793746800 is 15:00 on the day the probes ran.

```sh
tools/boot_probe.sh -s "$GAME" -w 8 -t 1793746800 -T 120 -o src/app/boot/results/clock_sweep_sfcop.tsv 91 -- \
    ./build/macos-arm64/sfcop --game @GAME@ --headless --watchdog @WATCHDOG@
tools/boot_probe.sh -s "$GAME" -w 8 -t 1793746800 -T 120 -o src/app/boot/results/clock_sweep_spike.tsv 91 -- \
    "$SFCOP_WORK/lifter-hle/build/sfcop" --game @GAME@ --run --headless --watchdog @WATCHDOG@
tools/boot_probe.sh -s "$GAME" -w 12 -t 1793749080 -o src/app/boot/results/ten_runs_at_1538_sfcop.tsv 10 -- \
    ./build/macos-arm64/sfcop --game @GAME@ --headless --watchdog @WATCHDOG@
tools/boot_probe.sh -s "$GAME" -w 15 -t 1793748480 -o src/app/boot/results/recheck_1528_sfcop.tsv 3 -- \
    ./build/macos-arm64/sfcop --game @GAME@ --headless --watchdog @WATCHDOG@
tools/boot_probe.sh -s "$GAME" -w 12 -o src/app/boot/results/ten_runs_sfcop.tsv 10 -- \
    ./build/macos-arm64/sfcop --game @GAME@ --headless --watchdog @WATCHDOG@
python3 tools/install_scan.py "$GAME" --under Assets/Models/space > src/app/boot/results/install_scan_space.txt
```

The spike's `ten_runs_at_1538` and `ten_runs` files use the spike's command line from the second
line. The `ten_runs` pair was run at the same moment, in two shells.
