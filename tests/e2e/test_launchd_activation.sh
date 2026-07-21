#!/bin/sh
# Issue #29 macOS launchd socket activation live E2E.
#
# Real events only: a REAL LaunchAgent plist bootstrapped into the user's
# gui domain under a throwaway label, launchd REALLY binds the pathname UDS
# from the plist <Sockets> dict, and the FIRST real shim connection triggers
# launchd to spawn the daemon (which adopts the fd via the official
# launch_activate_socket API — PV ref:
# https://developer.apple.com/documentation/xpc/launch_activate_socket).
#
# Allowed seams (issue #29 contract): throwaway launchd label, temp HOME and
# temp socket dir as test rigging. Forbidden and not done here: manually
# pre-starting the daemon, mocking launchd, fabricating readiness.
#
# States verified:
#   I_INSTALLED  — service definition installed: launchctl knows the label,
#                  launchd bound the socket (correct type/mode), and the
#                  daemon process has NOT been started yet.
#   I_ACTIVATED  — first client connection triggers on-demand start: the
#                  daemon answers the version handshake + a real MCP
#                  initialize round-trip, and exactly one daemon instance
#                  exists for our socket.
set -eu

case "$(uname -s)" in
Darwin) ;;
*)
    echo "[launchd-e2e] SKIP: launchd socket activation only exists on macOS" >&2
    exit 0
    ;;
esac

BIN=${BIN:-./build/c/codebase-memory-mcp}
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")
[ -x "$BIN" ] || { echo "[launchd-e2e] FAIL: missing binary $BIN" >&2; exit 1; }

TMPDIR_ROOT=${TMPDIR:-/tmp}
CASE_DIR=$(mktemp -d "$TMPDIR_ROOT/cbm-launchd-e2e.XXXXXX")
chmod 700 "$CASE_DIR"
FAIL=0
UID_NUM=$(id -u)
LABEL="dev.codebase-memory.e2e.$$"
SOCK_DIR="$CASE_DIR/runtime"
SOCK="$SOCK_DIR/daemon.sock"
PLIST="$CASE_DIR/$LABEL.plist"
TEST_HOME="$CASE_DIR/home"
mkdir -p "$SOCK_DIR" "$TEST_HOME"
chmod 700 "$SOCK_DIR" "$TEST_HOME"
# Keep the daemon's HTTP UI off inside the temp HOME so the E2E exercises
# only the activation path (config file is the documented UI toggle).
mkdir -p "$TEST_HOME/.cache/codebase-memory-mcp"
printf '{"ui_enabled": false, "ui_port": 9749}\n' \
    >"$TEST_HOME/.cache/codebase-memory-mcp/config.json"

note() { printf '[launchd-e2e] %s\n' "$1"; }
fail() { printf '[launchd-e2e] FAIL: %s\n' "$1" >&2; FAIL=1; }

cleanup() {
    launchctl bootout "gui/$UID_NUM/$LABEL" 2>/dev/null || true
    rm -rf "$CASE_DIR"
}
trap cleanup EXIT INT TERM

daemon_pid_count() {
    # Count live daemon processes serving OUR socket path only.
    pgrep -f -- "codebase-memory-mcp daemon --launchd --socket $SOCK" 2>/dev/null |
        wc -l | tr -d ' '
}

cat >"$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
 "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>$LABEL</string>
    <key>ProgramArguments</key>
    <array>
        <string>$BIN</string>
        <string>daemon</string>
        <string>--launchd</string>
        <string>--socket</string>
        <string>$SOCK</string>
    </array>
    <key>EnvironmentVariables</key>
    <dict>
        <key>HOME</key><string>$TEST_HOME</string>
    </dict>
    <key>Sockets</key>
    <dict>
        <key>Listeners</key>
        <dict>
            <key>SockPathName</key><string>$SOCK</string>
            <key>SockPathMode</key><integer>384</integer>
        </dict>
    </dict>
    <key>StandardOutPath</key><string>$CASE_DIR/daemon.out</string>
    <key>StandardErrorPath</key><string>$CASE_DIR/daemon.err</string>
</dict>
</plist>
EOF

# ── I_INSTALLED: install service definition ─────────────────────────────
note "I_INSTALLED: launchctl bootstrap gui/$UID_NUM $PLIST"
launchctl bootstrap "gui/$UID_NUM" "$PLIST"

launchctl print "gui/$UID_NUM/$LABEL" >"$CASE_DIR/print.installed" 2>&1 ||
    fail "I_INSTALLED: launchctl print does not know $LABEL"

i=0
while [ ! -S "$SOCK" ]; do
    i=$((i + 1))
    [ "$i" -lt 100 ] || { fail "I_INSTALLED: launchd never bound $SOCK"; break; }
    sleep 0.05
done
if [ -S "$SOCK" ]; then
    MODE=$(stat -f '%Lp' "$SOCK")
    [ "$MODE" = "600" ] || fail "I_INSTALLED: socket mode $MODE, want 600"
    note "I_INSTALLED: OK (label registered, socket bound by launchd, mode=$MODE)"
fi
if [ "$(daemon_pid_count)" -ne 0 ]; then
    fail "I_INSTALLED: daemon already running before any connection (not on-demand)"
else
    note "I_INSTALLED: OK (no daemon process before first connection)"
fi

# ── I_ACTIVATED: first connection triggers on-demand start ──────────────
note "I_ACTIVATED: first shim connection through launchd-bound socket"
INIT_MSG='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"launchd-e2e","version":"0"}}}'
set +e
printf '%s' "$INIT_MSG" | "$BIN" --socket "$SOCK" \
    >"$CASE_DIR/activate.out" 2>"$CASE_DIR/activate.err"
rc=$?
set -e
if [ "$rc" -ne 0 ]; then
    fail "I_ACTIVATED: shim exit=$rc (want 0); stderr: $(tail -3 "$CASE_DIR/activate.err" 2>/dev/null)"
fi
grep -q '"jsonrpc"' "$CASE_DIR/activate.out" ||
    fail "I_ACTIVATED: no MCP initialize response on stdout"
grep -q 'daemon.launchd.adopted' "$CASE_DIR/daemon.err" ||
    fail "I_ACTIVATED: daemon log missing launchd fd adoption marker"
COUNT=$(daemon_pid_count)
if [ "$COUNT" -ne 1 ]; then
    fail "I_ACTIVATED: expected exactly 1 daemon instance, got $COUNT"
else
    note "I_ACTIVATED: OK (first connection spawned daemon, MCP round-trip served, single instance)"
fi

# Second connection must reuse the SAME instance (still exactly one).
set +e
printf '%s' "$INIT_MSG" | "$BIN" --socket "$SOCK" \
    >"$CASE_DIR/activate2.out" 2>"$CASE_DIR/activate2.err"
rc2=$?
set -e
[ "$rc2" -eq 0 ] || fail "I_ACTIVATED: second connection exit=$rc2 (want 0)"
COUNT2=$(daemon_pid_count)
[ "$COUNT2" -eq 1 ] || fail "I_ACTIVATED: after second connection expected 1 instance, got $COUNT2"
note "I_ACTIVATED: OK (second connection reused the single instance)"

if [ "$FAIL" -ne 0 ]; then
    echo "[launchd-e2e] FAILURES above; daemon.err tail:" >&2
    tail -20 "$CASE_DIR/daemon.err" >&2 2>/dev/null || true
    exit 1
fi
note "PASS: launchd socket activation live E2E (I_INSTALLED + I_ACTIVATED)"
