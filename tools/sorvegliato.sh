#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Maurizio Cammalleri
#
# Run a program under watch: the one guard of the tests, measures and
# sabotages of Meri, and of the projects that build on it (they call this
# file, they do not copy it).
#
#   sorvegliato.sh [-t SECONDS] [-f MIB] -- PROGRAM [ARGUMENTS...]
#
# - it refuses to start (exit 3) if the work directory ($SORV_TMP,
#   ~/tmp/meri by default) holds more than 2 GiB, or if the disk has less
#   than 20 GiB free;
# - the program has a process group of its own (setsid) and a limit to
#   the size of the files it writes (-f, 256 MiB by default; ulimit -f);
# - past the time limit (-t, 300 s by default) the whole group gets TERM,
#   then KILL: exit 124, and "sorvegliato: tempo scaduto" on stderr;
# - when the guard exits, even if interrupted, the whole group is killed;
#   if anything of it is left, it says so (exit 125).
# Standard input and output go to the program; the exit code is its own.
# The messages on stderr are read by the scripts that call this one: they
# stay as they are. Thresholds changed only to try the guard out:
# SORV_TMP_MIB, SORV_FREE_GIB.
set -u

LIMIT=300
FILE_MIB=256
while [ $# -gt 0 ]; do
    case "$1" in
    -t) LIMIT="$2"; shift 2 ;;
    -f) FILE_MIB="$2"; shift 2 ;;
    --) shift; break ;;
    *) echo "sorvegliato: opzione sconosciuta: $1" >&2; exit 2 ;;
    esac
done
[ $# -gt 0 ] || { echo "sorvegliato: manca il programma" >&2; exit 2; }

TMP_DIR="${SORV_TMP:-$HOME/tmp/meri}"
TMP_MIB="${SORV_TMP_MIB:-2048}"
FREE_GIB="${SORV_FREE_GIB:-20}"
used=$(du -sm "$TMP_DIR" 2>/dev/null | awk '{print $1}')
free=$(df -Pk "$HOME" | awk 'NR == 2 {print int($4 / 1048576)}')
if [ "${used:-0}" -gt "$TMP_MIB" ]; then
    echo "sorvegliato: $TMP_DIR occupa ${used} MiB (massimo $TMP_MIB):" \
        "non parto" >&2
    exit 3
fi
if [ "$free" -lt "$FREE_GIB" ]; then
    echo "sorvegliato: ${free} GiB liberi sul disco (minimo $FREE_GIB):" \
        "non parto" >&2
    exit 3
fi

# the program, leader of a process group of its own, with the limit to
# its files (standard input passed on explicitly: a command started with &
# in a script would get /dev/null; no core dumps)
setsid bash -c 'ulimit -c 0; ulimit -f "$1"; shift; exec "$@"' sorvegliato \
    $((FILE_MIB * 1024)) "$@" <&0 &
pid=$!

kill_group() {
    kill -TERM -- "-$pid" 2>/dev/null
    sleep 1
    kill -KILL -- "-$pid" 2>/dev/null
}
trap 'kill_group' EXIT
trap 'exit 130' INT TERM HUP

# the clock: a sleep that is a direct child, stopped by its own pid (which
# always works). The guard before it, a group of its own with a mark in
# /dev/shm, outlived the program when the program ended before its setsid;
# when its time ran out it marked as timed out a pid since reused by
# another program, and sent TERM and KILL to a group of that number: false
# timeouts between 28 Sep and 4 Oct 2026 (a program ended in a second,
# "300 s"), 1 970 marks left behind. Fixed in Prabanta on 2 Oct 2026, here
# on 4 Oct; one file from then on.
sleep "$LIMIT" &
clock=$!
wait -n -p first "$pid" "$clock"
st=$?
disown "$clock" 2>/dev/null # reaped already, or its end goes unannounced
if [ "${first:-}" = "$clock" ]; then
    kill_group
    wait "$pid" 2>/dev/null
    echo "sorvegliato: tempo scaduto ($LIMIT s): $*" >&2
    st=124
else
    kill -KILL "$clock" 2>/dev/null
fi
# anything left of the group? (children the program left behind)
if pgrep -g "$pid" >/dev/null 2>&1; then
    kill -KILL -- "-$pid" 2>/dev/null
    echo "sorvegliato: processi rimasti nel gruppo $pid, uccisi: $*" >&2
    [ "$st" -eq 0 ] && st=125
fi
trap - EXIT
exit "$st"
