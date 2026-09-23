#!/bin/sh
# Issue #27 per-state live E2E: stdio -> pathname UDS thin shim.
#
# Every state below is exercised with a REAL shim subprocess, a REAL daemon
# subprocess (except S_VERSION_MISMATCH_FAILCLOSED, which is explicitly
# allowed to use a fake daemon that only implements the handshake — see the
# Issue #27 shams/seams/fakes contract), a REAL pathname UDS socket, REAL
# stdio pipes, and a REAL SIGKILL for the daemon-restart states. No mocked
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

# ── Daemon restart mid-session (reconnect contract) ─────────────────────
# A single REAL shim process drives a whole session through a REAL daemon
# SIGKILL. driver.py plays the MCP host over the shim's real stdio pipes.
cat <<'PYEOF' > "$CASE_DIR/driver.py"
import json, os, select, signal, subprocess, sys, time

mode, bin_path, sock, case_dir = sys.argv[1:5]
log = open(os.path.join(case_dir, mode + ".driver.log"), "w")
env = dict(os.environ)
env["CBM_CACHE_DIR"] = os.path.join(case_dir, "cache")
env["CBM_SHIM_LOG"] = os.path.join(case_dir, mode + ".shim.log")
daemon = None

def start_daemon():
    d = subprocess.Popen([bin_path, "daemon", "--socket", sock], env=env,
                         stdout=open(os.path.join(case_dir, mode + ".daemon.log"), "ab"),
                         stderr=subprocess.STDOUT)
    for _ in range(200):
        if os.path.exists(sock):
            return d
        time.sleep(0.05)
    raise SystemExit("socket never appeared")

def die(msg):
    log.write("FAIL " + msg + "\n"); log.flush()
    print("FAIL " + msg); sys.exit(1)

daemon = start_daemon()
shim = subprocess.Popen([bin_path, "--socket", sock], env=env, stdin=subprocess.PIPE,
                        stdout=subprocess.PIPE, stderr=open(os.path.join(case_dir, mode + ".err"), "wb"))
buf = b""

def send(obj):
    shim.stdin.write((json.dumps(obj) + "\n").encode()); shim.stdin.flush()

def recv(timeout=20):
    global buf
    deadline = time.time() + timeout
    while b"\n" not in buf:
        left = deadline - time.time()
        if left <= 0:
            die("timeout waiting for a response")
        r, _, _ = select.select([shim.stdout], [], [], left)
        if r:
            chunk = os.read(shim.stdout.fileno(), 65536)
            if not chunk:
                die("shim stdout closed (rc=%s)" % shim.poll())
            buf += chunk
    line, buf = buf.split(b"\n", 1)
    log.write("<< " + line.decode() + "\n"); log.flush()
    return json.loads(line)

send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
      "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                 "clientInfo": {"name": "e2e", "version": "0"}}})
if "protocolVersion" not in json.dumps(recv().get("result", {})):
    die("initialize failed")
send({"jsonrpc": "2.0", "method": "notifications/initialized"})
send({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
if not recv().get("result", {}).get("tools"):
    die("tools/list empty")

if mode == "inflight":
    # Freeze the daemon so the request is provably written but unanswered,
    # then kill it: the shim must answer id 3 itself, with an error.
    os.kill(daemon.pid, signal.SIGSTOP)
    send({"jsonrpc": "2.0", "id": 3, "method": "tools/call",
          "params": {"name": "list_projects", "arguments": {}}})
    time.sleep(0.3)
os.kill(daemon.pid, signal.SIGKILL)
daemon.wait()

if mode == "inflight":
    r = recv()
    if r.get("id") != 3 or "error" not in r or "restarted" not in r["error"].get("message", ""):
        die("in-flight request did not get a restart error: %r" % r)

if mode == "noreturn":
    rc = shim.wait(timeout=30)
    print("RC %d" % rc)
    sys.exit(0)

time.sleep(0.5)
daemon = start_daemon()
send({"jsonrpc": "2.0", "id": 4, "method": "tools/call",
      "params": {"name": "list_projects", "arguments": {}}})
r = recv()
if r.get("id") != 4 or "result" not in r:
    die("tools/call after restart failed: %r" % r)
shim.stdin.close()
rc = shim.wait(timeout=20)
daemon.send_signal(signal.SIGTERM); daemon.wait(timeout=20)
if buf.strip():
    die("unexpected extra stdout (replayed initialize leaked?): %r" % buf)
print("RC %d" % rc)
PYEOF

journal_count() { # $1=journal $2=event
    [ -f "$1" ] || { echo 0; return 0; }
    awk -v want="msg=$2" '{ for (i = 1; i <= NF; i++) if ($i == want) { n++; break } } END { print n + 0 }' "$1"
}

# (a) daemon SIGKILLed then restarted: the same shim finishes a tools/call.
note "S_RECONNECTED: SIGKILL the daemon, restart it, same shim completes tools/call"
set +e
out=$(timeout 90 python3 "$CASE_DIR/driver.py" restart "$BIN" "$CASE_DIR/re.sock" "$CASE_DIR")
set -e
if [ "$out" != "RC 0" ]; then
    fail "S_RECONNECTED: driver said '$out' (stderr: $(cat "$CASE_DIR/restart.err" 2>/dev/null))"
fi
grep -q 'shim.reconnect_ok' "$CASE_DIR/restart.err" || fail "S_RECONNECTED: missing shim.reconnect_ok stderr diagnostic"
[ "$(journal_count "$CASE_DIR/restart.shim.log" shim.reconnect_ok)" -eq 1 ] || fail "S_RECONNECTED: expected 1 shim.reconnect_ok journal record"
[ "$(journal_count "$CASE_DIR/restart.shim.log" shim.session_lost)" -eq 0 ] || fail "S_RECONNECTED: a recovered session must not journal shim.session_lost"
note "S_RECONNECTED: OK ($out, $(grep 'reconnect_ok' "$CASE_DIR/restart.err"))"

# (b) daemon never returns: fail closed with 76 once the (shortened) budget ends.
note "S_MIDSTREAM_LOST_FAILCLOSED: SIGKILL the daemon, never restart it -> exit 76 after the reconnect budget"
set +e
t0=$(date +%s)
out=$(CBM_SHIM_RECONNECT_TIMEOUT_MS=1500 timeout 60 python3 "$CASE_DIR/driver.py" noreturn "$BIN" "$CASE_DIR/nr.sock" "$CASE_DIR")
t1=$(date +%s)
set -e
if [ "$out" != "RC 76" ]; then
    fail "S_MIDSTREAM_LOST_FAILCLOSED: expected RC 76, driver said '$out'"
fi
grep -q 'midstream_loss' "$CASE_DIR/noreturn.err" || fail "S_MIDSTREAM_LOST_FAILCLOSED: missing structured stderr diagnostic"
[ "$(journal_count "$CASE_DIR/noreturn.shim.log" shim.reconnect_failed)" -eq 1 ] || fail "S_MIDSTREAM_LOST_FAILCLOSED: expected 1 shim.reconnect_failed journal record"
[ "$(journal_count "$CASE_DIR/noreturn.shim.log" shim.session_lost)" -eq 1 ] || fail "S_MIDSTREAM_LOST_FAILCLOSED: expected 1 shim.session_lost journal record"
[ $((t1 - t0)) -lt 20 ] || fail "S_MIDSTREAM_LOST_FAILCLOSED: reconnect budget not honoured ($((t1 - t0))s)"
note "S_MIDSTREAM_LOST_FAILCLOSED: OK ($out in $((t1 - t0))s)"

# (c) request in flight at the moment of the kill: host gets an error for its id.
note "S_INFLIGHT_ERRORED: request unanswered when the daemon dies gets a JSON-RPC error, session continues"
set +e
out=$(timeout 90 python3 "$CASE_DIR/driver.py" inflight "$BIN" "$CASE_DIR/if.sock" "$CASE_DIR")
set -e
if [ "$out" != "RC 0" ]; then
    fail "S_INFLIGHT_ERRORED: driver said '$out' (stderr: $(cat "$CASE_DIR/inflight.err" 2>/dev/null))"
fi
note "S_INFLIGHT_ERRORED: OK ($out)"

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
echo "[shim-e2e] all 11 states PASSED"
