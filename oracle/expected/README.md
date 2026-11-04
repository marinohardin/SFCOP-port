# Expected oracle traces

The replay oracle's expected values: traces recorded on the original game, one per mission and input
script. A build under test passes the oracle when it produces the same trace, tick for tick
(DESIGN §5.10, §6.5). The format is `oracle/trace/FORMAT.md`.

## Where traces live

```
oracle/expected/<mission>/<input>.trace
```

`<mission>` is the oracle mission's directory under `oracle/scr/` (`OracleSkirmish`); `<input>` is the
driver's steps file under `oracle/driver/` without `.steps` (`skirmish`). Both are repeated in the
trace's header, and the comparator refuses a pair whose `mission` or `input` differ.

This directory is a hotspot: only the integrator changes it, in a pull request of its own.

## Recording a trace

On a machine that can run the original game (Windows, or a Wine build that gets past start-up), with
the install verified against `analysis/results/retail_hashes.sha256`:

1. The mission and its steps file are committed under `oracle/scr/` and `oracle/driver/` first.
2. Follow `oracle/README.md` to set up and run `oracle/oracle.sh <label>`. It plays the mission twice
   and leaves `oracle/traces/<label>-1/` and `<label>-2/`, each with the mission's log
   (`trace.raw.txt`) and `run.txt`, whose `exe` line is the executable's SHA-256.
3. Make a candidate trace of each run in a work directory outside the repository (`<work>` below,
   for example `work/oracle/candidates/`), never straight into `oracle/expected/`:

   ```sh
   python3 oracle/trace/mklog.py oracle/traces/<label>-1/trace.raw.txt \
       --mission OracleSkirmish --input skirmish \
       --exe-sha256 <the exe line of run.txt> --build "original 2.5.5.2" \
       -o <work>/run1.trace
   ```

   and the same for `<label>-2` into `<work>/run2.trace`. `--recorded` defaults to the current UTC
   time.
4. Keep the trace only if both hold:
   - the two runs agree: `python3 oracle/trace/compare.py <work>/run1.trace <work>/run2.trace` exits
     0. A recording that does not repeat on the original cannot judge anything else;
   - the mission finished: the log's `END` line says `max_samples` or `one_side_gone`, not `aborted`.

   Only then move `<work>/run1.trace` to `oracle/expected/OracleSkirmish/skirmish.trace`.
5. Commit the trace alone (`docs/CONVENTIONS.md` §8, "Recording an oracle trace"):
   `test(oracle): record OracleSkirmish/skirmish`. A re-recording says why in the commit body. The
   raw log and screenshots stay on the recording machine; only the `.trace` file is committed.

## How the check uses them

When a build under test exists (the delinked executable, or the native build), `scripts/check.sh`
plays each mission and input with it, turns each log into a trace with `mklog.py` under a build
directory that mirrors this one, and runs

```sh
python3 oracle/trace/compare.py --tree oracle/expected <actual traces root>
```

which compares every `<mission>/<input>.trace` here with its twin. Exit 0 passes. Exit 4 means a
tick differs; the first differing tick is printed for each trace. Exit 2 means a trace is missing or
malformed, or a header field that must agree does not. With no build under test, or no trace here,
the step is not run.
