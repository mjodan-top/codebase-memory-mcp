#!/bin/sh
# Live E2E: a client that vanishes mid-response must NOT take the daemon down.
#
# Regression for the 2026-09-07 outage: the daemon role installed handlers for
# SIGTERM/SIGINT only, so the default SIGPIPE disposition applied. Every client
# connection is served over an unbuffered (setvbuf _IONBF) stdio stream on the
# accepted socket, so the first write to a socket whose peer had gone away
# terminated the WHOLE daemon process — every other live session with it. The
# machine then sat for ~30h with a stale socket and no working MCP.
#
# Everything here is real: a real production daemon subprocess, a real pathname
# UDS socket, the real ASCII handshake, real MCP Content-Length framing and a
# real close() before the response is read. Nothing is mocked, and the kill is
# NOT racy: the client sends a complete request and closes the socket, so the
# daemon is guaranteed to hit EPIPE when it writes the response back.
set -eu

BIN=${BIN:-./build/c/codebase-memory-mcp}
HS_HEADER=${HS_HEADER:-src/daemon/shim_handshake.h}

# Single source of contract: the wire version comes from the header both roles
# compile against, never from a literal duplicated into this test.
SHIM_VERSION=$(sed -n 's/^#define CBM_SHIM_PROTOCOL_VERSION[ 	]\{1,\}\([0-9]\{1,\}\).*$/\1/p' \
    "$HS_HEADER" | head -1)
[ -n "$SHIM_VERSION" ] || { printf '[dc-e2e] FAIL: cannot read CBM_SHIM_PROTOCOL_VERSION from %s\n' "$HS_HEADER" >&2; exit 1; }

# Deliberately NOT $TMPDIR: on macOS the per-user TMPDIR path is long enough to
# blow the 104-byte sun_path budget once a socket name is appended.
CASE_DIR=$(mktemp -d "/tmp/cbm-dc-e2e.XXXXXX")
chmod 700 "$CASE_DIR"
FAIL=0
DAEMON_PID=""

note() { printf '[dc-e2e] %s\n' "$1"; }
fail() { printf '[dc-e2e] FAIL: %s\n' "$1" >&2; FAIL=1; }

cleanup() {
    if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
        kill -TERM "$DAEMON_PID" 2>/dev/null || true
        wait "$DAEMON_PID" 2>/dev/null || true
    fi
    rm -rf "$CASE_DIR"
}
trap cleanup EXIT INT TERM

SOCK="$CASE_DIR/d.sock"
DAEMON_LOG="$CASE_DIR/daemon.log"

# The client helper: handshake, send one request, then either read the response
# (mode=serve) or close immediately without reading it (mode=abandon).
CLIENT="$CASE_DIR/client.py"
cat > "$CLIENT" <<'PYEOF'
import socket, struct, sys

sock_path, version, mode = sys.argv[1], sys.argv[2], sys.argv[3]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(10)
s.connect(sock_path)

s.sendall(("CBM-SHIM-HELLO %s\n" % version).encode())
hello = b""
while not hello.endswith(b"\n"):
    chunk = s.recv(1)
    if not chunk:
        print("HANDSHAKE_EOF", file=sys.stderr)
        sys.exit(3)
    hello += chunk
if b"OK" not in hello:
    print("HANDSHAKE_REJECTED %r" % hello, file=sys.stderr)
    sys.exit(4)

if mode == "abandon":
    # tools/list yields a multi-kilobyte response, so the daemon is still
    # writing long after we are gone — but even a single write suffices: the
    # request below is complete, so the daemon WILL produce a response.
    body = b'{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}'
    s.sendall(b"Content-Length: %d\r\n\r\n" % len(body) + body)
    # Discard anything already queued and drop the peer hard: RST rather than
    # an orderly FIN, so the daemon's next write is guaranteed to fail.
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    s.close()
    print("ABANDONED")
    sys.exit(0)

body = b'{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"dc-e2e","version":"0"}}}'
s.sendall(b"Content-Length: %d\r\n\r\n" % len(body) + body)
buf = b""
while b"\r\n\r\n" not in buf:
    chunk = s.recv(4096)
    if not chunk:
        print("RESPONSE_EOF", file=sys.stderr)
        sys.exit(5)
    buf += chunk
header, _, rest = buf.partition(b"\r\n\r\n")
length = 0
for line in header.split(b"\r\n"):
    if line.lower().startswith(b"content-length:"):
        length = int(line.split(b":", 1)[1])
while len(rest) < length:
    chunk = s.recv(4096)
    if not chunk:
        break
    rest += chunk
s.close()
sys.stdout.write(rest[:length].decode("utf-8", "replace"))
PYEOF

wait_for_socket() {
    i=0
    while [ ! -S "$1" ]; do
        i=$((i + 1))
        [ "$i" -lt 200 ] || { fail "socket $1 never appeared"; return 1; }
        sleep 0.05
    done
    return 0
}

count_event() {
    # Count occurrences of a msg= event name in the daemon log.
    grep -c "msg=$1" "$DAEMON_LOG" 2>/dev/null || true
}

note "starting real daemon on $SOCK"
"$BIN" daemon --socket "$SOCK" >"$DAEMON_LOG" 2>&1 &
DAEMON_PID=$!
wait_for_socket "$SOCK" || exit 1

# ── Baseline: one well-behaved session, so the "normal" counter is non-zero ──
if ! python3 "$CLIENT" "$SOCK" "$SHIM_VERSION" serve > "$CASE_DIR/serve0.out" 2>"$CASE_DIR/serve0.err"; then
    fail "baseline session failed: $(cat "$CASE_DIR/serve0.err")"
fi
grep -q 'protocolVersion' "$CASE_DIR/serve0.out" || fail "baseline session got no initialize result"

# ── The regression: abandon the connection mid-response, three times over ────
# Three rounds, not one: a single event cannot show that the counter is
# countable over a window, and a daemon that dies on the first one can never
# reach the third.
ROUNDS=3
i=0
while [ "$i" -lt "$ROUNDS" ]; do
    i=$((i + 1))
    if ! python3 "$CLIENT" "$SOCK" "$SHIM_VERSION" abandon >"$CASE_DIR/abandon$i.out" 2>"$CASE_DIR/abandon$i.err"; then
        fail "round $i: abandoning client failed to even connect: $(cat "$CASE_DIR/abandon$i.err")"
    fi
    # Give the daemon a moment to attempt the write and log the outcome.
    sleep 0.5
    if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
        fail "round $i: DAEMON DIED after a client disconnected mid-response (the 2026-09-07 regression)"
        note "daemon log tail:"
        tail -5 "$DAEMON_LOG" >&2 || true
        exit 1
    fi
    note "round $i: client abandoned, daemon still alive (pid $DAEMON_PID)"
done

# ── The daemon must still be SERVING, not merely not-dead ───────────────────
if ! python3 "$CLIENT" "$SOCK" "$SHIM_VERSION" serve > "$CASE_DIR/serve1.out" 2>"$CASE_DIR/serve1.err"; then
    fail "post-disconnect session failed: $(cat "$CASE_DIR/serve1.err")"
fi
grep -q 'protocolVersion' "$CASE_DIR/serve1.out" ||
    fail "daemon survived but no longer answers initialize"

# ── Observability (AGENTS.md §5): the broken branch must be countable ───────
BROKEN=$(count_event daemon.client_stream_broken)
DONE=$(count_event daemon.client_stream_done)
note "events: client_stream_broken=$BROKEN client_stream_done=$DONE"

[ "$BROKEN" -ge "$ROUNDS" ] ||
    fail "expected >= $ROUNDS daemon.client_stream_broken events, got $BROKEN (the failure is invisible in the log)"
[ "$DONE" -ge 2 ] ||
    fail "expected >= 2 daemon.client_stream_done events, got $DONE (cannot compute a broken/total ratio)"

# "Why did it degrade?" must be answerable from the log alone: the event has to
# carry the actual errno and its text, not just a bare event name.
if ! grep -q 'msg=daemon.client_stream_broken.*errno=[0-9]\{1,\}.*error=' "$DAEMON_LOG"; then
    fail "daemon.client_stream_broken carries no errno/error payload"
    grep 'client_stream_broken' "$DAEMON_LOG" | head -3 >&2 || true
fi
if grep -q 'msg=daemon.client_stream_broken.*errno=0' "$DAEMON_LOG"; then
    fail "daemon.client_stream_broken logged errno=0 — the error was lost before it was recorded"
fi

# Every event line must be timestamped, otherwise "how many in the last N
# hours" is unanswerable no matter how many events there are.
if ! grep 'msg=daemon.client_stream_broken' "$DAEMON_LOG" | head -1 | grep -q 'ts='; then
    fail "daemon.client_stream_broken lines have no ts= field (not aggregatable by time window)"
fi

if [ "$FAIL" -eq 0 ]; then
    note "PASS: daemon survived $ROUNDS mid-response client disconnects, kept serving, and logged each one"
    exit 0
fi
printf '[dc-e2e] FAILED\n' >&2
exit 1
