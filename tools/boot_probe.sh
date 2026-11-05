#!/bin/sh
# boot_probe.sh: run the headless boot N times and write one TSV row per run.
#
#   tools/boot_probe.sh [-o OUT.tsv] [-l LOGDIR] [-s SRC] [-w S] [-t EPOCH [-T STEP]] N -- COMMAND [ARGS...]
#
# COMMAND runs once per run with its stdout and stderr in LOGDIR/run_<i>.out|.err. In ARGS,
# @GAME@ is replaced by the game directory and @WATCHDOG@ by -w S (default 30). With -s SRC,
# each run gets a fresh APFS clone of SRC (cp -Rc), so no run sees what an earlier one wrote;
# without -s, @GAME@ is not allowed.
#
# With -t EPOCH (macOS), run i starts with the time of day the game reads at EPOCH + (i - 1) * STEP
# seconds (-T, default 0): tools/fakeclock.c, which shifts time() and gettimeofday() but not the
# clocks that waits use, is built into LOGDIR and loaded into COMMAND. The game seeds
# its random numbers from the clock, and the first draws pick the space background the boot
# loads, so a time of day reproduces a boot exactly.
#
# Columns: run, clock, outcome, at_s, exit, background, last_line.
#   clock      the local time of day the run started at, as the run saw it
#   outcome    prompt  the boot reached the CD-key prompt ([mark] ... (CD-key prompt) at T s)
#              fault   a host signal (=== fault: ... at T s ===)
#              throw   a guest C++ throw reached RaiseException
#              alive   reached the watchdog with none of the above (a build without the prompt mark)
#              other   anything else (exited early, no watchdog line)
#   at_s       the time the log gives for the outcome, or "-"
#   background the space model the boot opened ([file] line, sfcop only), or "-"
#   last_line  the last log line that does not start with a space (the indirect-call trace does)
#
# Example, from the repository root:
#   tools/boot_probe.sh -s "$GAME" -o probe.tsv 10 -- ./build/macos-arm64/sfcop --game @GAME@ \
#       --headless --watchdog @WATCHDOG@
set -eu

out=; logs=; src=; wd=30; epoch=; step=0
usage() {
    echo "usage: tools/boot_probe.sh [-o OUT.tsv] [-l LOGDIR] [-s SRC] [-w S] [-t EPOCH [-T STEP]] N -- COMMAND [ARGS...]" >&2
    exit 2
}
while getopts o:l:s:w:t:T: opt; do
    case $opt in
    o) out=$OPTARG ;;
    l) logs=$OPTARG ;;
    s) src=$OPTARG ;;
    w) wd=$OPTARG ;;
    t) epoch=$OPTARG ;;
    T) step=$OPTARG ;;
    *) usage ;;
    esac
done
shift $((OPTIND - 1))
[ $# -ge 3 ] || usage
n=$1; shift
[ "$1" = "--" ] || usage
shift
for v in "$n" "$wd" "$step" ${epoch:+"$epoch"}; do
    case $v in '' | *[!0-9]*) usage ;; esac
done
[ -z "$src" ] || [ -d "$src" ] || { echo "boot_probe: no directory $src" >&2; exit 2; }
if [ -z "$src" ]; then
    for a in "$@"; do case $a in *@GAME@*) echo "boot_probe: @GAME@ needs -s SRC" >&2; exit 2 ;; esac; done
fi

work=${SFCOP_WORK:-}
if [ -z "$work" ]; then
    root=$(cd "$(dirname "$0")/.." && pwd)
    case $root in */.claude/worktrees/*) root=${root%/.claude/worktrees/*} ;; esac
    work=$(dirname "$root")/work
fi
[ -n "$logs" ] || logs=$work/F1/probe/$(date +%Y%m%dT%H%M%S)
[ -n "$out" ] || out=$logs/probe.tsv
mkdir -p "$logs" "$(dirname "$out")"
scratch=$logs/game

fake=
if [ -n "$epoch" ]; then
    fake=$logs/fakeclock.dylib
    cc -dynamiclib -o "$fake" "$(dirname "$0")/fakeclock.c"
fi

# Fills in the placeholders and runs the command. A function's "$@" is its own, so the
# template in the caller's "$@" stays as it was.
run_once() {
    first=1
    for a in "$@"; do
        if [ "$first" = 1 ]; then set --; first=0; fi
        set -- "$@" "$(printf '%s' "$a" | sed -e "s|@GAME@|$scratch|g" -e "s|@WATCHDOG@|$wd|g")"
    done
    if [ -n "$fake" ]; then
        SFCOP_FAKE_CLOCK_OFFSET=$offset DYLD_INSERT_LIBRARIES=$fake "$@"
    else
        "$@"
    fi
}

printf 'run\tclock\toutcome\tat_s\texit\tbackground\tlast_line\n' > "$out"
i=1
while [ "$i" -le "$n" ]; do
    if [ -n "$src" ]; then
        rm -rf "$scratch"
        cp -Rc "$src" "$scratch"
        chmod -R u+w "$scratch"
    fi
    offset=0
    if [ -n "$epoch" ]; then
        when=$((epoch + (i - 1) * step))
        offset=$((when - $(date +%s)))
        clock=$(date -r "$when" +%H:%M:%S)
    else
        clock=$(date +%H:%M:%S)
    fi
    rc=0
    run_once "$@" > "$logs/run_$i.out" 2> "$logs/run_$i.err" || rc=$?
    err=$logs/run_$i.err
    at=-
    if grep -aq '(CD-key prompt) at' "$err"; then
        outcome=prompt
        at=$(grep -a '(CD-key prompt) at' "$err" | head -1 | sed 's/.* at \([0-9.]*\) s.*/\1/')
    elif grep -aq '^=== fault' "$err"; then
        outcome=fault
        at=$(grep -a '^=== fault' "$err" | head -1 | sed 's/.* at \([0-9.]*\) s.*/\1/')
    elif grep -aq '^\[throw\] RaiseException' "$err"; then
        outcome=throw
    elif grep -aq 'watchdog' "$err"; then
        outcome=alive
    else
        outcome=other
    fi
    bg=$(grep -a '^\[file\] ' "$err" | grep -aio 'space[0-9]*\.mod' | head -1 || true)
    last=$(grep -av '^[[:space:]]' "$err" | grep -av '^$' | tail -1 | tr '\t' ' ' | cut -c1-200 || true)
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$i" "$clock" "$outcome" "$at" "$rc" "${bg:--}" "$last" >> "$out"
    printf 'run %s at %s: %s %s (exit %s) %s\n' "$i" "$clock" "$outcome" "$at" "$rc" "${bg:--}"
    i=$((i + 1))
done
[ -z "$src" ] || rm -rf "$scratch"
echo "boot_probe: $out"
