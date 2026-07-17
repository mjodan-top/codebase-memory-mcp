#!/bin/sh
# Issue #27 per-state live E2E: stdio -> pathname UDS thin shim.
#
# Every state below is exercised with a REAL shim subprocess, a REAL daemon
# subprocess (except S_VERSION_MISMATCH_FAILCLOSED, which is explicitly
# allowed to use a fake daemon that only implements the handshake — see the
# Issue #27 shams/seams/fakes contract), a REAL pathname UDS socket, REAL
# stdio pipes, and a REAL SIGKILL for the midstream-loss state. No mocked
# sockets, no fabricated success, no raw text written to stdout.
set -eu

BIN=${BIN:-./build/c/codebase-memory-mcp}
TMPDIR_ROOT=${TMPDIR:-/tmp}
CASE_DIR=$(mktemp -d "$TMPDIR_ROOT/cbm-shim-e2e.XXXXXX")
chmod 700 "$CASE_DIR"
FAIL=0

note() { printf '[shim-e2e] %s\n' "$1"; }
fail() { printf '[shim-e2e] FAIL: %s\n' "$1" >&2; FAIL=1; }

cleanup() {
    [ -n "${PERM_DIR:-}" ] && chmod 700 "$PERM_DIR" 2>/dev/null || true
    [ -n "${PERM_DAEMON_PID:-}" ] && kill -0 "$PERM_DAEMON_PID" 2>/dev/null && kill -TERM "$PERM_DAEMON_PID" 2>/dev/null
    [ -n "${PERM_DAEMON_PID:-}" ] && wait "$PERM_DAEMON_PID" 2>/dev/null || true
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

INIT_MSG='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"e2e","version":"0"}}}'

# ── S_DAEMON_ABSENT_FAILCLOSED ─────────────────────────────────────────
note "S_DAEMON_ABSENT_FAILCLOSED: connect with no daemon running"
ABSENT_SOCK="$CASE_DIR/absent.sock"
set +e
printf '{}' | "$BIN" --socket "$ABSENT_SOCK" >"$CASE_DIR/absent.out" 2>"$CASE_DIR/absent.err"
rc=$?
set -e
if [ "$rc" -ne 71 ]; then
    fail "S_DAEMON_ABSENT_FAILCLOSED: expected exit 71, got $rc"
fi
if [ -s "$CASE_DIR/absent.out" ]; then
    fail "S_DAEMON_ABSENT_FAILCLOSED: stdout must be empty, got: $(cat "$CASE_DIR/absent.out")"
fi
grep -q 'daemon_absent' "$CASE_DIR/absent.err" || fail "S_DAEMON_ABSENT_FAILCLOSED: missing structured stderr diagnostic"
note "S_DAEMON_ABSENT_FAILCLOSED: OK (exit=$rc, stdout empty, stderr structured)"

# ── S_STALE_SOCKET_FAILCLOSED ───────────────────────────────────────────
note "S_STALE_SOCKET_FAILCLOSED: bound-but-nobody-listening socket inode"
STALE_SOCK="$CASE_DIR/stale.sock"
python3 - "$STALE_SOCK" <<'PYEOF'
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind(sys.argv[1])
s.close()
PYEOF
set +e
printf '{}' | "$BIN" --socket "$STALE_SOCK" >"$CASE_DIR/stale.out" 2>"$CASE_DIR/stale.err"
rc=$?
set -e
if [ "$rc" -ne 72 ]; then
    fail "S_STALE_SOCKET_FAILCLOSED: expected exit 72, got $rc"
fi
if [ -s "$CASE_DIR/stale.out" ]; then
    fail "S_STALE_SOCKET_FAILCLOSED: stdout must be empty"
fi
grep -q 'stale_socket' "$CASE_DIR/stale.err" || fail "S_STALE_SOCKET_FAILCLOSED: missing structured stderr diagnostic"
note "S_STALE_SOCKET_FAILCLOSED: OK (exit=$rc)"
rm -f "$STALE_SOCK"

# ── S_PERMISSION_DENIED_FAILCLOSED ──────────────────────────────────────
note "S_PERMISSION_DENIED_FAILCLOSED: real daemon socket behind a non-searchable parent directory"
PERM_DIR="$CASE_DIR/permission"
mkdir "$PERM_DIR"
chmod 700 "$PERM_DIR"
PERM_SOCK="$PERM_DIR/daemon.sock"
"$BIN" daemon --socket "$PERM_SOCK" >"$CASE_DIR/permission-daemon.log" 2>&1 &
PERM_DAEMON_PID=$!
wait_for_socket "$PERM_SOCK"
chmod 000 "$PERM_DIR"
set +e
printf '{}' | "$BIN" --socket "$PERM_SOCK" >"$CASE_DIR/permission.out" 2>"$CASE_DIR/permission.err"
rc_permission=$?
set -e
chmod 700 "$PERM_DIR"
if [ "$rc_permission" -ne 73 ]; then
    fail "S_PERMISSION_DENIED_FAILCLOSED: expected exit 73, got $rc_permission (stderr: $(cat "$CASE_DIR/permission.err"))"
fi
if [ -s "$CASE_DIR/permission.out" ]; then
    fail "S_PERMISSION_DENIED_FAILCLOSED: stdout must be empty"
fi
grep -q 'permission_denied' "$CASE_DIR/permission.err" || fail "S_PERMISSION_DENIED_FAILCLOSED: missing structured stderr diagnostic"
kill -TERM "$PERM_DAEMON_PID" 2>/dev/null || true
wait "$PERM_DAEMON_PID" 2>/dev/null || true
PERM_DAEMON_PID=
note "S_PERMISSION_DENIED_FAILCLOSED: OK (exit=$rc_permission)"

# ── Real daemon for the remaining states ────────────────────────────────
SOCK="$CASE_DIR/daemon.sock"
"$BIN" daemon --socket "$SOCK" >"$CASE_DIR/daemon.log" 2>&1 &
DAEMON_PID=$!
wait_for_socket "$SOCK"
note "daemon.pid=$DAEMON_PID socket=$SOCK"

# ── S_CONNECTING / S_NEGOTIATING / S_ATTACHED ───────────────────────────
note "S_CONNECTING/S_NEGOTIATING/S_ATTACHED: real shim -> real daemon initialize"
printf '%s\n' "$INIT_MSG" | timeout 5 "$BIN" --socket "$SOCK" >"$CASE_DIR/attached.out" 2>"$CASE_DIR/attached.err"
rc=$?
if [ "$rc" -ne 0 ]; then
    fail "S_ATTACHED: expected exit 0, got $rc (stderr: $(cat "$CASE_DIR/attached.err"))"
fi
if ! grep -q '"protocolVersion"' "$CASE_DIR/attached.out"; then
    fail "S_ATTACHED: stdout did not contain a valid initialize response"
fi
python3 -c "import json,sys; json.loads(open(sys.argv[1]).read().splitlines()[0])" "$CASE_DIR/attached.out" \
    || fail "S_ATTACHED: stdout line is not valid JSON-RPC"
kill -0 "$DAEMON_PID" 2>/dev/null || fail "S_ATTACHED: daemon must still be running after a clean session"
note "S_ATTACHED: OK (exit=$rc, valid MCP JSON on stdout, daemon survives)"

# ── Structural / process / behavioural "never per-process fallback" proof ──
note "three-layer never-per-process-fallback proof"
# 1. Structural: the shim's own translation unit does not reference server
#    construction symbols (grep on the compiled object's symbol table).
if command -v nm >/dev/null 2>&1; then
    SHIM_OBJ=$(find . -name 'mcp_shim.o' 2>/dev/null | head -1)
    if [ -n "$SHIM_OBJ" ]; then
        if nm "$SHIM_OBJ" 2>/dev/null | grep -q 'cbm_mcp_server_new'; then
            fail "structural proof: mcp_shim.o references cbm_mcp_server_new"
        else
            note "structural proof: mcp_shim.o has no cbm_mcp_server_new reference"
        fi
    else
        note "structural proof: mcp_shim.o not found (build/link mode without per-TU .o); relying on source-level grep instead"
        grep -q 'mcp/mcp.h' src/daemon/mcp_shim.c && fail "structural proof: mcp_shim.c includes mcp/mcp.h" || note "structural proof: mcp_shim.c does not include mcp/mcp.h"
    fi
fi
# 2. Process: before/after a fail-closed attempt, no new MCP owner process
#    appears in the process table.
BEFORE_COUNT=$(pgrep -f "$BIN daemon" | wc -l | tr -d ' ')
set +e
printf '{}' | "$BIN" --socket "$CASE_DIR/absent-again.sock" >/dev/null 2>/dev/null
set -e
AFTER_COUNT=$(pgrep -f "$BIN daemon" | wc -l | tr -d ' ')
if [ "$BEFORE_COUNT" != "$AFTER_COUNT" ]; then
    fail "process proof: daemon-role process count changed ($BEFORE_COUNT -> $AFTER_COUNT) after a fail-closed shim attempt"
else
    note "process proof: daemon-role process count unchanged ($BEFORE_COUNT) after fail-closed attempt"
fi
# 3. Behavioural: retrying the same absent path still fails the same way
#    (a silently self-started server would instead start succeeding).
set +e
printf '{}' | "$BIN" --socket "$CASE_DIR/absent-again.sock" >/dev/null 2>"$CASE_DIR/retry.err"
rc2=$?
set -e
if [ "$rc2" -ne 71 ]; then
    fail "behavioural proof: retry against the same absent path did not fail closed again (rc=$rc2)"
else
    note "behavioural proof: retry against the same absent path still fails closed (rc=$rc2)"
fi

# ── S_MIDSTREAM_LOST_FAILCLOSED ─────────────────────────────────────────
note "S_MIDSTREAM_LOST_FAILCLOSED: SIGKILL a dedicated daemon mid-session (does not touch the shared daemon used by other states)"
MID_SOCK="$CASE_DIR/mid-daemon.sock"
"$BIN" daemon --socket "$MID_SOCK" >"$CASE_DIR/mid-daemon.log" 2>&1 &
MID_DAEMON_PID=$!
wait_for_socket "$MID_SOCK"
rm -f "$CASE_DIR/mid.fifo"
mkfifo "$CASE_DIR/mid.fifo"
(
    set +e
    exec 9<>"$CASE_DIR/mid.fifo"
    "$BIN" --socket "$MID_SOCK" <&9 >"$CASE_DIR/mid.out" 2>"$CASE_DIR/mid.err" &
    SHIM_PID=$!
    printf '%s\n' "$INIT_MSG" >&9
    i=0
    while [ ! -s "$CASE_DIR/mid.out" ] && [ "$i" -lt 100 ]; do i=$((i + 1)); sleep 0.05; done
    kill -KILL "$MID_DAEMON_PID" 2>/dev/null
    wait "$SHIM_PID" 2>/dev/null
    echo "$?" > "$CASE_DIR/mid.rc"
    exit 0
)
wait "$MID_DAEMON_PID" 2>/dev/null || true
MID_RC=$(cat "$CASE_DIR/mid.rc" 2>/dev/null || echo -1)
if [ "$MID_RC" != "76" ]; then
    fail "S_MIDSTREAM_LOST_FAILCLOSED: expected exit 76, got $MID_RC"
fi
grep -q 'midstream_loss' "$CASE_DIR/mid.err" || fail "S_MIDSTREAM_LOST_FAILCLOSED: missing structured stderr diagnostic"
if ! grep -q '"protocolVersion"' "$CASE_DIR/mid.out"; then
    fail "S_MIDSTREAM_LOST_FAILCLOSED: stdout should still contain the pre-kill valid initialize response"
fi
note "S_MIDSTREAM_LOST_FAILCLOSED: OK (exit=$MID_RC)"

# ── S_VERSION_MISMATCH_FAILCLOSED ───────────────────────────────────────
# Allowed fake (per Issue #27 shams/seams/fakes): a minimal daemon that only
# implements the handshake wire format with a bumped protocol version, never
# the real MCP framing.
note "S_VERSION_MISMATCH_FAILCLOSED: fake daemon advertising an incompatible protocol version"
FAKE_SOCK="$CASE_DIR/fake.sock"
python3 - "$FAKE_SOCK" > "$CASE_DIR/fake_daemon.log" 2>&1 &
FAKE_PID=$!
cat <<'PYEOF' > "$CASE_DIR/fake_daemon.py"
import socket, sys, os
path = sys.argv[1]
if os.path.exists(path):
    os.unlink(path)
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind(path)
s.listen(1)
print("FAKE_READY", flush=True)
conn, _ = s.accept()
data = b""
while not data.endswith(b"\n"):
    chunk = conn.recv(1)
    if not chunk:
        break
    data += chunk
# Reply with a mismatched version but verdict=OK to also probe the client's
# independent peer_version re-check (never trust the daemon's self-reported
# verdict alone).
conn.sendall(b"CBM-DAEMON-HELLO 999 OK\n")
conn.close()
PYEOF
kill "$FAKE_PID" 2>/dev/null || true
wait "$FAKE_PID" 2>/dev/null || true
python3 "$CASE_DIR/fake_daemon.py" "$FAKE_SOCK" > "$CASE_DIR/fake_daemon.log" 2>&1 &
FAKE_PID=$!
i=0
while [ ! -S "$FAKE_SOCK" ] && [ "$i" -lt 100 ]; do i=$((i + 1)); sleep 0.05; done
set +e
printf '{}' | timeout 5 "$BIN" --socket "$FAKE_SOCK" >"$CASE_DIR/mismatch.out" 2>"$CASE_DIR/mismatch.err"
rc3=$?
set -e
kill "$FAKE_PID" 2>/dev/null || true
wait "$FAKE_PID" 2>/dev/null || true
if [ "$rc3" -ne 74 ]; then
    fail "S_VERSION_MISMATCH_FAILCLOSED: expected exit 74, got $rc3"
fi
if [ -s "$CASE_DIR/mismatch.out" ]; then
    fail "S_VERSION_MISMATCH_FAILCLOSED: stdout must be empty"
fi
grep -q 'handshake_failed' "$CASE_DIR/mismatch.err" || fail "S_VERSION_MISMATCH_FAILCLOSED: missing structured stderr diagnostic"
note "S_VERSION_MISMATCH_FAILCLOSED: OK (exit=$rc3), and the client independently rejected a self-reported OK at a mismatched version"

# ── S_EXITED ─────────────────────────────────────────────────────────────
note "S_EXITED: stdin EOF cleanly tears down the UDS side and closes stdout"
set +e
: | timeout 5 "$BIN" --socket "$SOCK" >"$CASE_DIR/exited.out" 2>"$CASE_DIR/exited.err"
rc4=$?
set -e
if [ "$rc4" -ne 0 ]; then
    fail "S_EXITED: expected clean exit 0 on stdin EOF, got $rc4"
fi
kill -0 "$DAEMON_PID" 2>/dev/null || fail "S_EXITED: daemon must still be alive (only the session closed)"
note "S_EXITED: OK (exit=$rc4, daemon still serving)"

if [ "$FAIL" -ne 0 ]; then
    echo "[shim-e2e] one or more states FAILED" >&2
    exit 1
fi
echo "[shim-e2e] all 9 states PASSED"
