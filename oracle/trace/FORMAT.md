# Oracle trace format (format_version 1)

A trace is the replay oracle's result for one mission played with one input script: a short header,
then one hash per tick. A trace recorded on the original game is the expected value; a build under
test passes when its trace has the same hash on every tick (DESIGN §5.10, §6.5). There is no
tolerance: two traces either agree on every tick or the first differing tick is reported.

`mklog.py` makes a trace from the oracle mission's log; `compare.py` compares two traces.

## File

- Encoding: ASCII. Any byte above 0x7F is an error.
- Line endings: LF only. A CR anywhere is an error.
- Every line, the last included, ends with LF. No blank lines, no comments, no trailing blanks.
- Each line is a key and a value separated by exactly one TAB.

```
format_version	1
mission	OracleSkirmish
input	skirmish
exe_sha256	<64 lowercase hex digits>
build	original 2.5.5.2
recorded	2026-11-03T18:00:00Z
0	<hash of tick 0>
1	<hash of tick 1>
...
```

## Header

The first six lines, each key exactly once, in this order:

| Key | Value | Compared |
| --- | --- | --- |
| `format_version` | Decimal integer. This document describes `1`; any other value is rejected | yes |
| `mission` | The oracle mission's name: its directory under `oracle/scr/` (`OracleSkirmish`). Letters, digits, `_` and `-` | yes |
| `input` | The input script's name: its steps file under `oracle/driver/` without `.steps` (`skirmish`). Letters, digits, `_` and `-` | yes |
| `exe_sha256` | SHA-256 of the executable that produced the trace, 64 lowercase hex digits | no |
| `build` | What produced the trace, for people: `original 2.5.5.2`, `delinked <commit>`, `native <preset> <commit>`. 1 to 200 printable ASCII characters, no leading or trailing space | no |
| `recorded` | When the trace was made, UTC, `YYYY-MM-DDTHH:MM:SSZ` | no |

"Compared" fields must be equal in the two traces, or the comparison stops before any tick is read.
The others describe which build made the trace and when; they differ between the original and a
build under test by design, so they are checked for form only.

## Ticks

After the header, at least one line `<tick><TAB><hash>`:

- `tick` is a decimal integer without sign or leading zeros. The first is `0` and each is one more
  than the one before.
- `hash` is the SHA-256 of that tick's record (below), 64 lowercase hex digits.

A tick is one oracle sample: the mission samples every 1/32 game turn (`kOracleSampleTurns` in
`oracle/scr/OracleSkirmish/OracleConfig.h`). The last tick is the mission's end record, so a trace
of `n` samples has `n + 1` tick lines and the reason the mission ended is part of the comparison.

## The per-tick record

The oracle mission (`oracle/scr/OracleSkirmish/OracleRecorder.cpp`) writes its log,
`OracleTrace.txt`, next to the game executable. `mklog.py` turns that log into the tick lines:

1. **Normalise.** Split the log at LF. Remove one CR at the end of a line (the game writes in text
   mode, so lines end in CRLF); any other CR is an error. Remove trailing spaces and TABs. Drop lines
   that start with `#`: they hold run-specific metadata (addresses, entity IDs, seeds) that is not
   behaviour. A blank line, or a line that is not LF-terminated, is an error.
   This is stricter than `oracle/normalize_trace.sh`, which removes every CR and keeps blank lines:
   the recorder never writes a blank line or a CR inside a line, so either means the log is damaged,
   and the trace is refused rather than hashed.
2. **Check the recorder version.** The log's first line must be `# oracle-trace v1`. Another
   version means the record layout below has changed, and with it this document and
   `format_version`.
3. **Tag.** Every remaining line is one of the three records below; its second field is the sample
   it belongs to. Tags never decrease from one line to the next. The `END` record is the last line
   and appears once.
4. **Group.** The record of tick `t` is every line tagged `t`, in log order, each followed by one LF.
   Ticks run from `0` to the sample number in `END`. A tick with no lines has the empty record, whose
   SHA-256 is `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`.
5. **Hash.** The tick's hash is the SHA-256 of its record's bytes, written as 64 lowercase hex digits.

### Records (recorder version 1)

Fields are separated by exactly one space. Floats are printed by the game with `%.9g`, which
round-trips a 32-bit float exactly, and are hashed as text. A ship the script owns is named by its
custom ID (`c101`, `c201`), never by entity ID, so the trace does not depend on entity numbering.
The one exception is a target the script does not own, written `id<ship ID>` (field 25 of `S`;
`id-1` when there is no target).

**S**, one per ship per sample, 29 fields:

| # | Field |
| --- | --- |
| 1 | `S` |
| 2 | sample |
| 3 | ship (`c<custom ID>`) |
| 4 | `grid` |
| 5-6 | grid x and y (1-based) |
| 7 | `raw` |
| 8-9 | raw grid x and y |
| 10-14 | raw integers 0-4 |
| 15-19 | raw floats 0-4 |
| 20 | `spd` |
| 21 | speed (integer) |
| 22 | `dmg` |
| 23 | damage percent (float) |
| 24 | `tgt` |
| 25 | target (`c<custom ID>` or `id<ship ID>`) |
| 26 | `dist` |
| 27 | tactical distance to the other side's first ship (float, `-1` if none) |
| 28 | `st` |
| 29 | status bits 0-6 (upper-case hex, no prefix) |

"raw" is the whole reply the game gives to the position request, sentinel-filled beforehand
(integers `2129587950`, floats `-98765.4375`), so a field the game leaves alone shows the sentinel.

**E**, one per event the game delivers to the script, in arrival order, 18 fields:

| # | Field |
| --- | --- |
| 1 | `E` |
| 2 | sample (the number of samples taken when the event arrived) |
| 3 | event bit, 8 upper-case hex digits |
| 4 | ship (`c<custom ID>`, or `-`) |
| 5 | `i` |
| 6-10 | event integers 0-4 (for a damage event, integer 0 is `0` when the hostile ship is known) |
| 11 | `f` |
| 12-16 | event floats 0-4 |
| 17 | `h` |
| 18 | hostile ship for a damage event (`c<custom ID>`), otherwise `-` |

**END**, the last line, 3 fields: `END`, the number of samples taken, and the reason: `max_samples`,
`one_side_gone` or `aborted`.

`mklog.py` checks the kind, the field count, the keyword fields (`grid`, `raw`, `spd`, `dmg`, `tgt`,
`dist`, `st`; `i`, `f`, `h`) and that the sample is a decimal integer; it does not interpret the
values.

## Comparing

`compare.py EXPECTED ACTUAL`:

| Exit | When | Output |
| --- | --- | --- |
| 0 | Every compared header field and every tick agree | `identical: <n> ticks` |
| 4 | A tick differs, or one trace has ticks the other lacks | `first difference at tick <N>: expected <H1>, actual <H2>`; `missing` stands for a hash when that trace ends before tick `N` |
| 2 | A compared header field differs | `header mismatch in field <name>: expected <v1>, actual <v2>` |
| 2 | A file is unreadable or breaks this format | `<EXPECTED or ACTUAL> <path>: line <n>: <what>`, naming the field for a header line |
| 2 | Bad command line | usage |

`compare.py --tree EXPECTED_ROOT ACTUAL_ROOT` compares every `<mission>/<input>.trace` under
`EXPECTED_ROOT` with the file at the same path under `ACTUAL_ROOT`, one result line each. It exits 0
when all agree or `EXPECTED_ROOT` holds no trace, 2 when `EXPECTED_ROOT` is not a directory, any file
is missing or malformed or any header differs, otherwise 4 when any tick differs.

## Changing the format

A change to the file layout, the header, the normalisation, the grouping, the hash or the mission's
record layout is a new `format_version`: this document, `mklog.py`, `compare.py` and every trace under
`oracle/expected/` change together, and the traces are recorded again.
