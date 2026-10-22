#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Maurizio Cammalleri
#
# Run a program under watch: the one guard of the tests, measures and
# sabotages of Meri, and of the projects that build on it (they call this
# file, they do not copy it).
#
#   sorvegliato.sh [-t SECONDS] [-f MIB] [-d DIR] [-m MIB] [-g GIB] --
#                  PROGRAM [ARGUMENTS...]
#
# - it refuses to start (exit 3) if the work directory (-d, tmp/meri in
#   the home directory of the user by default) holds more than -m MiB
#   (2048), or if its disk has less than -g GiB free (20);
# - the program has a process group of its own (setsid) and a limit to
#   the size of the files it writes (-f, 256 MiB by default; ulimit -f);
# - past the time limit (-t, 300 s by default) the whole group gets TERM,
#   then KILL: exit 124, and "sorvegliato: tempo scaduto" on stderr;
# - when the guard exits, even if interrupted, the whole group is killed;
#   if anything of it is left, it says so (exit 125).
# Standard input and output go to the program; the exit code is its own.
# The messages on stderr are read by the scripts that call this one: they
# stay as they are, and nothing of the shell's own (a job killed by a
# signal) gets among them. Nothing comes from the environment: the
# directory and the thresholds are options (another session could export
# a variable), the home directory is read from the user database.
set -u

LIMIT=300
FILE_MIB=256
TMP_DIR="$(getent passwd "$(id -u)" | cut -d: -f6)/tmp/meri"
TMP_MIB=2048
FREE_GIB=20
while [ $# -gt 0 ]; do
    case "$1" in
    -t) LIMIT="$2"; shift 2 ;;
    -f) FILE_MIB="$2"; shift 2 ;;
    -d) TMP_DIR="$2"; shift 2 ;;
    -m) TMP_MIB="$2"; shift 2 ;;
    -g) FREE_GIB="$2"; shift 2 ;;
    --) shift; break ;;
    *) echo "sorvegliato: opzione sconosciuta: $1" >&2; exit 2 ;;
    esac
done
[ $# -gt 0 ] || { echo "sorvegliato: manca il programma" >&2; exit 2; }

case "$TMP_DIR" in
/*) ;;
*) echo "sorvegliato: -d vuole un percorso assoluto: $TMP_DIR" >&2; exit 2 ;;
esac
used=$(du -sm "$TMP_DIR" 2>/dev/null | awk '{print $1}')
# the free space of the disk of the directory (of its first ancestor that
# exists, if it does not yet)
on="$TMP_DIR"
while [ ! -e "$on" ]; do on=$(dirname "$on"); done
free=$(df -Pk "$on" | awk 'NR == 2 {print int($4 / 1048576)}')
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
# 2>/dev/null: the shell's notice of a job ended by a signal ("Annullato
# (core dump creato)" for SIGABRT) is not the program's; its status is
wait -n -p first "$pid" "$clock" 2>/dev/null
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
