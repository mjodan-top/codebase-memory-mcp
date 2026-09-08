#!/bin/sh
# Live E2E: the shim's connect-outcome journal must make daemon loss
# mechanically discoverable from logs alone.
#
# Motivation (2026-09-07 incident): the daemon was killed by SIGPIPE and stayed
# dead ~30 hours. Every session's shim failed closed correctly, but the only
# trace was one stderr line inside that session, and the daemon's own log had
# stopped growing. No persistent record could answer "how many sessions lost
# the daemon, since when, why". This test pins the fix: both the failing and
# the healthy outcome land in a persistent append-only file, as parseable
# key=value lines that can be counted per event name.
#
# Everything here is real: a real shim subprocess, a real daemon subprocess, a
# real pathname UDS socket, a real stale-socket inode, real files on disk.
# Nothing is mocked and no log line is fabricated.
set -eu

BIN=${BIN:-./build/c/codebase-memory-mcp}
TMPDIR_ROOT=${TMPDIR:-/tmp}
CASE_DIR=$(mktemp -d "$TMPDIR_ROOT/cbm-shim-journal.XXXXXX")
chmod 700 "$CASE_DIR"
FAIL=0

note() { printf '[shim-journal] %s\n' "$1"; }
fail() { printf '[shim-journal] FAIL: %s\n' "$1" >&2; FAIL=1; }
# Confirmation lines must never print once something has already failed —
# a run that reports both FAIL and OK for the same check is unreadable.
note_if_clean() {
    if [ "$FAIL" -eq 0 ]; then
        note "$1"
    fi
}

cleanup() {
    [ -n "${DAEMON_PID:-}" ] && kill -0 "$DAEMON_PID" 2>/dev/null && kill -TERM "$DAEMON_PID" 2>/dev/null
    [ -n "${DAEMON_PID:-}" ] && wait "$DAEMON_PID" 2>/dev/null || true
    rm -rf "$CASE_DIR"
}
trap cleanup EXIT INT TERM

wait_for_socket() {
    path=$1
    i=0
    while [ ! -S "$path" ]; do
        i=$((i + 1))
        [ "$i" -lt 200 ] || { fail "socket $path never appeared"; return 1; }
        sleep 0.05
    done
    return 0
}

INIT_MSG='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"journal-e2e","version":"0"}}}'

JOURNAL="$CASE_DIR/shim.log"

# Count records whose msg= field equals $1, exactly (not a substring match).
count_event() {
    [ -f "$JOURNAL" ] || { echo 0; return 0; }
    awk -v want="$1" '{
        for (i = 1; i <= NF; i++) {
            if ($i == "msg=" want) { n++; break }
        }
    } END { print n + 0 }' "$JOURNAL"
}

# Print the value of key $2 from the last record whose msg= field is $1.
field_of_last() {
    [ -f "$JOURNAL" ] || return 0
    awk -v want="$1" -v key="$2" '{
        hit = 0
        for (i = 1; i <= NF; i++) if ($i == "msg=" want) hit = 1
        if (!hit) next
        for (i = 1; i <= NF; i++) {
            eq = index($i, "=")
            if (eq > 0 && substr($i, 1, eq - 1) == key) val = substr($i, eq + 1)
        }
    } END { print val }' "$JOURNAL"
}

# Every record must carry the fields an aggregator needs, and a UTC timestamp
# it can bucket by; a line missing any of them is not aggregatable.
assert_records_parseable() {
    [ -f "$JOURNAL" ] || { fail "journal file $JOURNAL does not exist"; return 1; }
    bad=$(awk '{
        ts = ""; msg = ""; state = ""; sock = ""; pid = ""
        for (i = 1; i <= NF; i++) {
            eq = index($i, "=")
            if (eq <= 0) continue
            k = substr($i, 1, eq - 1); v = substr($i, eq + 1)
            if (k == "ts") ts = v
            else if (k == "msg") msg = v
            else if (k == "state") state = v
            else if (k == "socket") sock = v
            else if (k == "pid") pid = v
        }
        if (ts !~ /^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$/) { print NR; next }
        if (msg == "" || state == "" || sock == "" || pid !~ /^[0-9]+$/) { print NR }
    }' "$JOURNAL")
    if [ -n "$bad" ]; then
        fail "journal lines not aggregatable (line numbers: $(echo "$bad" | tr '\n' ' '))"
        return 1
    fi
    return 0
}

# ── Failure side 1: daemon absent ───────────────────────────────────────
note "daemon absent: shim must journal the reason, not just its own stderr"
ABSENT_SOCK="$CASE_DIR/absent.sock"
set +e
printf '%s\n' "$INIT_MSG" | CBM_SHIM_LOG="$JOURNAL" timeout 10 "$BIN" --socket "$ABSENT_SOCK" \
    >"$CASE_DIR/absent.out" 2>"$CASE_DIR/absent.err"
set -e

if [ "$(count_event shim.connect_failed)" -ne 1 ]; then
    fail "daemon absent: expected exactly 1 shim.connect_failed record, got $(count_event shim.connect_failed)"
fi
state=$(field_of_last shim.connect_failed state)
if [ "$state" != "connect.daemon_absent" ]; then
    fail "daemon absent: expected state=connect.daemon_absent, got state=$state"
fi
if [ -z "$(field_of_last shim.connect_failed errno)" ]; then
    fail "daemon absent: record carries no errno — the reason would be unrecoverable from logs"
fi
if [ "$(field_of_last shim.connect_failed socket)" != "$ABSENT_SOCK" ]; then
    fail "daemon absent: record does not name the socket it failed on"
fi
note_if_clean "daemon absent: OK ($(head -1 "$JOURNAL" 2>/dev/null))"

# ── Failure side 2: stale socket inode (the actual 2026-09-07 shape) ────
note "stale socket: bound-but-nobody-listening inode, as left behind by a dead daemon"
STALE_SOCK="$CASE_DIR/stale.sock"
python3 - "$STALE_SOCK" <<'PYEOF'
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind(sys.argv[1])
s.close()
PYEOF
set +e
printf '%s\n' "$INIT_MSG" | CBM_SHIM_LOG="$JOURNAL" timeout 10 "$BIN" --socket "$STALE_SOCK" \
    >"$CASE_DIR/stale.out" 2>"$CASE_DIR/stale.err"
set -e

if [ "$(count_event shim.connect_failed)" -ne 2 ]; then
    fail "stale socket: expected 2 cumulative shim.connect_failed records, got $(count_event shim.connect_failed)"
fi
state=$(field_of_last shim.connect_failed state)
if [ "$state" != "connect.stale_socket" ]; then
    fail "stale socket: expected state=connect.stale_socket, got state=$state"
fi
note_if_clean "stale socket: OK ($(tail -1 "$JOURNAL" 2>/dev/null))"

# ── Healthy side: a real daemon must produce a countable success record ──
note "real daemon: the healthy outcome must be countable too, else no rate exists"
SOCK="$CASE_DIR/daemon.sock"
"$BIN" daemon --socket "$SOCK" >"$CASE_DIR/daemon.log" 2>&1 &
DAEMON_PID=$!
wait_for_socket "$SOCK"

set +e
printf '%s\n' "$INIT_MSG" | CBM_SHIM_LOG="$JOURNAL" timeout 20 "$BIN" --socket "$SOCK" \
    >"$CASE_DIR/attached.out" 2>"$CASE_DIR/attached.err"
rc_attached=$?
set -e
if [ "$rc_attached" -ne 0 ]; then
    fail "real daemon: shim exited $rc_attached (stderr: $(cat "$CASE_DIR/attached.err"))"
fi
if [ "$(count_event shim.connect_ok)" -ne 1 ]; then
    fail "real daemon: expected 1 shim.connect_ok record, got $(count_event shim.connect_ok)"
fi
if [ "$(field_of_last shim.connect_ok state)" != "attached" ]; then
    fail "real daemon: expected state=attached on the success record, got $(field_of_last shim.connect_ok state)"
fi
note_if_clean "real daemon: OK ($(tail -1 "$JOURNAL" 2>/dev/null))"

# ── The aggregation claim itself ────────────────────────────────────────
note "aggregability: every record parses and the failure rate is computable"
assert_records_parseable || true
failed=$(count_event shim.connect_failed)
ok=$(count_event shim.connect_ok)
total=$((failed + ok))
if [ "$total" -ne 3 ]; then
    fail "aggregability: expected 3 outcome records total, got $total"
fi
# This is the number a periodic log read has to be able to produce: the share
# of shim starts that could not reach the daemon. Guard the division so a
# zero-record regression reports the missing records, not an awk error.
if [ "$total" -gt 0 ]; then
    rate=$(awk -v f="$failed" -v t="$total" 'BEGIN { printf "%.0f", (f * 100) / t }')
    if [ "$rate" -ne 67 ]; then
        fail "aggregability: computed failure rate ${rate}% != expected 67% (2 of 3)"
    else
        note "aggregability: OK (failed=$failed ok=$ok rate=${rate}%)"
    fi
else
    fail "aggregability: no outcome records at all — the failure rate is not computable"
fi

# ── Default location (not just the test override) ───────────────────────
note "default location: journal must land under the cache dir with no override set"
DEFAULT_CACHE="$CASE_DIR/cache"
mkdir -p "$DEFAULT_CACHE"
set +e
printf '%s\n' "$INIT_MSG" | CBM_CACHE_DIR="$DEFAULT_CACHE" timeout 10 "$BIN" \
    --socket "$CASE_DIR/absent2.sock" >/dev/null 2>&1
set -e
if [ ! -s "$DEFAULT_CACHE/logs/shim.log" ]; then
    fail "default location: expected a record at $DEFAULT_CACHE/logs/shim.log"
else
    note "default location: OK ($(tail -1 "$DEFAULT_CACHE/logs/shim.log"))"
fi

# ── Off switch ──────────────────────────────────────────────────────────
note "off switch: CBM_SHIM_LOG=off must write nothing and change nothing else"
before=$(wc -l <"$JOURNAL")
set +e
printf '%s\n' "$INIT_MSG" | CBM_SHIM_LOG=off timeout 10 "$BIN" --socket "$CASE_DIR/absent3.sock" \
    >"$CASE_DIR/off.out" 2>"$CASE_DIR/off.err"
rc_off=$?
set -e
after=$(wc -l <"$JOURNAL")
if [ "$before" -ne "$after" ]; then
    fail "off switch: journal grew from $before to $after lines while disabled"
fi
if [ "$rc_off" -ne 71 ]; then
    fail "off switch: fail-closed exit code changed to $rc_off (expected 71)"
fi
if ! grep -q 'daemon_absent' "$CASE_DIR/off.err"; then
    fail "off switch: stderr diagnostic disappeared along with the journal"
fi
note_if_clean "off switch: OK (exit=$rc_off, journal unchanged at $after lines)"

# ── Journal failure must never break the shim (fail-open) ───────────────
note "fail-open: an unwritable journal path must not alter exit code or stdout"
set +e
printf '%s\n' "$INIT_MSG" | CBM_SHIM_LOG="$CASE_DIR/nonexistent-dir/deep/shim.log" timeout 10 "$BIN" \
    --socket "$CASE_DIR/absent4.sock" >"$CASE_DIR/failopen.out" 2>"$CASE_DIR/failopen.err"
rc_failopen=$?
set -e
if [ "$rc_failopen" -ne 71 ]; then
    fail "fail-open: expected fail-closed exit 71, got $rc_failopen"
fi
if [ -s "$CASE_DIR/failopen.out" ]; then
    fail "fail-open: stdout must stay empty (MCP transport), got: $(cat "$CASE_DIR/failopen.out")"
fi
note_if_clean "fail-open: OK (exit=$rc_failopen, stdout empty)"

if [ "$FAIL" -ne 0 ]; then
    printf '[shim-journal] RESULT: FAIL\n' >&2
    exit 1
fi
printf '[shim-journal] RESULT: PASS\n'
