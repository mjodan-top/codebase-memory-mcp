#!/bin/sh
# Issue #38 live E2E: daemon log lines carry a leading ISO8601 UTC millisecond
# ts= key, and msg=mcp.request lines for tools/call record project= when the
# call carries a project argument.
#
# Uses a REAL daemon subprocess on its own throwaway pathname UDS socket and a
# REAL shim subprocess over real stdio pipes — same shams/seams contract as
# test_shim_states.sh. Assertions are user-visible content assertions against
# the daemon's captured stderr log (text format, then JSON format).
set -eu

BIN=${BIN:-./build/c/codebase-memory-mcp}
TMPDIR_ROOT=${TMPDIR:-/tmp}
CASE_DIR=$(mktemp -d "$TMPDIR_ROOT/cbm-log38-e2e.XXXXXX")
chmod 700 "$CASE_DIR"
FAIL=0

note() { printf '[log38-e2e] %s\n' "$1"; }
fail() { printf '[log38-e2e] FAIL: %s\n' "$1" >&2; FAIL=1; }

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

TS_RE='^ts=[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}Z '

INIT_MSG='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"e2e","version":"0"}}}'
INITED_MSG='{"jsonrpc":"2.0","method":"notifications/initialized"}'
CALL_NOPROJ='{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"list_projects","arguments":{}}}'
CALL_PROJ='{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"search_graph","arguments":{"project":"log38-demo-project","query":"anything"}}}'

# ── Text format (default) ───────────────────────────────────────────────
note "text format: daemon up, drive initialize + tools/call via real shim"
SOCK="$CASE_DIR/text.sock"
"$BIN" daemon --socket "$SOCK" >"$CASE_DIR/text-daemon.log" 2>&1 &
DAEMON_PID=$!
wait_for_socket "$SOCK"

printf '%s\n%s\n%s\n%s\n' "$INIT_MSG" "$INITED_MSG" "$CALL_NOPROJ" "$CALL_PROJ" \
    | "$BIN" --socket "$SOCK" >"$CASE_DIR/text-shim.out" 2>"$CASE_DIR/text-shim.err" || {
    fail "text: shim round-trip failed (stderr: $(cat "$CASE_DIR/text-shim.err"))"
}

kill -TERM "$DAEMON_PID" 2>/dev/null || true
wait "$DAEMON_PID" 2>/dev/null || true
DAEMON_PID=

LOG="$CASE_DIR/text-daemon.log"
[ -s "$LOG" ] || fail "text: daemon log is empty"

# 1. Every structured log line leads with ts=<ISO8601 UTC ms> at column 0.
if grep -E 'msg=' "$LOG" | grep -Ev "$TS_RE" >/dev/null; then
    fail "text: found msg= line(s) without leading ISO8601 ts=: $(grep -E 'msg=' "$LOG" | grep -Ev "$TS_RE" | head -3)"
fi
grep -Eq "$TS_RE" "$LOG" || fail "text: no line matches leading ts= ISO8601 millisecond pattern"

# 2. mcp.request lines keep the existing keys.
grep -E 'msg=mcp\.request' "$LOG" | grep -q 'method=tools/call' || fail "text: no tools/call mcp.request line"
REQ_LINE=$(grep -E 'msg=mcp\.request' "$LOG" | grep 'tool=search_graph' | head -1)
[ -n "$REQ_LINE" ] || fail "text: no mcp.request line for tool=search_graph"
case "$REQ_LINE" in
*tool=*status=*duration_ms=*) : ;;
*) fail "text: mcp.request line missing tool=/status=/duration_ms=: $REQ_LINE" ;;
esac

# 3. tools/call WITH project argument records project=; without it, omits it.
case "$REQ_LINE" in
*project=*) : ;;
*) fail "text: search_graph call with project arg has no project= key: $REQ_LINE" ;;
esac
NOPROJ_LINE=$(grep -E 'msg=mcp\.request' "$LOG" | grep 'tool=list_projects' | head -1)
[ -n "$NOPROJ_LINE" ] || fail "text: no mcp.request line for tool=list_projects"
case "$NOPROJ_LINE" in
*project=*) fail "text: project-less list_projects call must omit project=: $NOPROJ_LINE" ;;
*) : ;;
esac
# initialize has no project semantics either.
INIT_LINE=$(grep -E 'msg=mcp\.request' "$LOG" | grep 'method=initialize' | head -1)
[ -n "$INIT_LINE" ] || fail "text: no mcp.request line for initialize"
case "$INIT_LINE" in
*project=*) fail "text: initialize must omit project=: $INIT_LINE" ;;
*) : ;;
esac
note "text format: OK"
note "sample: $REQ_LINE"

# ── JSON format (CBM_LOG_FORMAT=json) ───────────────────────────────────
note "json format: daemon with CBM_LOG_FORMAT=json"
JSOCK="$CASE_DIR/json.sock"
CBM_LOG_FORMAT=json "$BIN" daemon --socket "$JSOCK" >"$CASE_DIR/json-daemon.log" 2>&1 &
DAEMON_PID=$!
wait_for_socket "$JSOCK"

printf '%s\n%s\n%s\n' "$INIT_MSG" "$INITED_MSG" "$CALL_PROJ" \
    | "$BIN" --socket "$JSOCK" >"$CASE_DIR/json-shim.out" 2>"$CASE_DIR/json-shim.err" || {
    fail "json: shim round-trip failed (stderr: $(cat "$CASE_DIR/json-shim.err"))"
}

kill -TERM "$DAEMON_PID" 2>/dev/null || true
wait "$DAEMON_PID" 2>/dev/null || true
DAEMON_PID=

JLOG="$CASE_DIR/json-daemon.log"
JTS_RE='^\{"ts":"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}Z"'
if grep -E '"event":' "$JLOG" | grep -Ev "$JTS_RE" >/dev/null; then
    fail "json: found event line(s) without leading \"ts\" key: $(grep -E '"event":' "$JLOG" | grep -Ev "$JTS_RE" | head -3)"
fi
JREQ_LINE=$(grep '"event":"mcp.request"' "$JLOG" | grep '"tool":"search_graph"' | head -1)
[ -n "$JREQ_LINE" ] || fail "json: no mcp.request event for search_graph"
case "$JREQ_LINE" in
*'"project":'*) : ;;
*) fail "json: search_graph call with project arg has no project key: $JREQ_LINE" ;;
esac
note "json format: OK"
note "sample: $JREQ_LINE"

if [ "$FAIL" -ne 0 ]; then
    note "RESULT: FAIL"
    exit 1
fi
note "RESULT: all issue #38 log assertions passed"
exit 0
