#!/bin/sh
# Issue #29 Linux user systemd socket activation live E2E.
#
# Real events only: a REAL transient user systemd socket+service unit pair
# under a throwaway name, systemd REALLY binds the pathname UDS from the
# .socket unit, and the FIRST real shim connection triggers systemd to spawn
# the daemon, which receives the fd per the sd_listen_fds contract
# (LISTEN_PID/LISTEN_FDS, first fd == 3 — PV ref:
# https://www.freedesktop.org/software/systemd/man/latest/sd_listen_fds.html)
# and adopts it via cbm_uds_owner_adopt().
#
# Allowed seams (issue #29 contract): throwaway unit name, temp HOME and
# temp socket dir as test rigging. Forbidden and not done here: manually
# pre-starting the daemon, mocking systemd, fabricating readiness.
#
# States verified:
#   I_INSTALLED  — service definition installed: systemd knows the units,
#                  systemd bound the socket (correct type/mode), and the
#                  daemon process has NOT been started yet.
#   I_ACTIVATED  — first client connection triggers on-demand start: the
#                  daemon answers a real MCP initialize round-trip, and
#                  exactly one daemon instance exists for our socket.
#
# Cleanup is strict: both transient units are stopped and their files
# removed on exit (systemctl --user only; no system-level state touched).
set -eu

case "$(uname -s)" in
Linux) ;;
*)
    echo "[systemd-e2e] SKIP: systemd socket activation only exists on Linux" >&2
    exit 0
    ;;
esac
if ! systemctl --user is-system-running >/dev/null 2>&1; then
    echo "[systemd-e2e] FAIL: user systemd unavailable (fail-closed, not skipping silently)" >&2
    exit 1
fi

BIN=${BIN:-./build/c/codebase-memory-mcp}
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")
[ -x "$BIN" ] || { echo "[systemd-e2e] FAIL: missing binary $BIN" >&2; exit 1; }

TMPDIR_ROOT=${TMPDIR:-/tmp}
CASE_DIR=$(mktemp -d "$TMPDIR_ROOT/cbm-systemd-e2e.XXXXXX")
chmod 700 "$CASE_DIR"
FAIL=0
UNIT="cbm-e2e-$$"
UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
SOCK_DIR="$CASE_DIR/runtime"
SOCK="$SOCK_DIR/daemon.sock"
TEST_HOME="$CASE_DIR/home"
mkdir -p "$SOCK_DIR" "$TEST_HOME" "$UNIT_DIR"
chmod 700 "$SOCK_DIR" "$TEST_HOME"
# Keep the daemon's HTTP UI off inside the temp HOME so the E2E exercises
# only the activation path (config file is the documented UI toggle).
mkdir -p "$TEST_HOME/.cache/codebase-memory-mcp"
printf '{"ui_enabled": false, "ui_port": 9750}\n' \
    >"$TEST_HOME/.cache/codebase-memory-mcp/config.json"

note() { printf '[systemd-e2e] %s\n' "$1"; }
fail() { printf '[systemd-e2e] FAIL: %s\n' "$1" >&2; FAIL=1; }

cleanup() {
    systemctl --user stop "$UNIT.socket" "$UNIT.service" 2>/dev/null || true
    rm -f "$UNIT_DIR/$UNIT.socket" "$UNIT_DIR/$UNIT.service"
    systemctl --user daemon-reload 2>/dev/null || true
    systemctl --user reset-failed "$UNIT.service" 2>/dev/null || true
    rm -rf "$CASE_DIR"
}
trap cleanup EXIT INT TERM

daemon_pid_count() {
    # Count live daemon processes serving OUR socket path only.
    pgrep -f -- "codebase-memory-mcp daemon --systemd --socket $SOCK" 2>/dev/null |
        wc -l | tr -d ' '
}

cat >"$UNIT_DIR/$UNIT.socket" <<EOF
[Unit]
Description=cbm issue29 systemd activation E2E socket ($$)
[Socket]
ListenStream=$SOCK
SocketMode=0600
EOF

cat >"$UNIT_DIR/$UNIT.service" <<EOF
[Unit]
Description=cbm issue29 systemd activation E2E service ($$)
Requires=$UNIT.socket
[Service]
Type=simple
Environment=HOME=$TEST_HOME
ExecStart=$BIN daemon --systemd --socket $SOCK
StandardOutput=append:$CASE_DIR/daemon.out
StandardError=append:$CASE_DIR/daemon.err
EOF

# ── I_INSTALLED: install service definition ─────────────────────────────
note "I_INSTALLED: systemctl --user start $UNIT.socket"
systemctl --user daemon-reload
systemctl --user start "$UNIT.socket"

systemctl --user status "$UNIT.socket" >"$CASE_DIR/status.installed" 2>&1 ||
    fail "I_INSTALLED: systemctl --user status does not know $UNIT.socket"

i=0
while [ ! -S "$SOCK" ]; do
    i=$((i + 1))
    [ "$i" -lt 100 ] || { fail "I_INSTALLED: systemd never bound $SOCK"; break; }
    sleep 0.05
done
if [ -S "$SOCK" ]; then
    MODE=$(stat -c '%a' "$SOCK")
    [ "$MODE" = "600" ] || fail "I_INSTALLED: socket mode $MODE, want 600"
    note "I_INSTALLED: OK (unit active, socket bound by systemd, mode=$MODE)"
fi
if [ "$(daemon_pid_count)" -ne 0 ]; then
    fail "I_INSTALLED: daemon already running before any connection (not on-demand)"
else
    note "I_INSTALLED: OK (no daemon process before first connection)"
fi

# ── I_ACTIVATED: first connection triggers on-demand start ──────────────
note "I_ACTIVATED: first shim connection through systemd-bound socket"
INIT_MSG='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"systemd-e2e","version":"0"}}}'
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
grep -q 'daemon.systemd.adopted' "$CASE_DIR/daemon.err" ||
    fail "I_ACTIVATED: daemon log missing systemd fd adoption marker"
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

# The socket inode must still be systemd's (daemon never unlinks it): stop
# the service and confirm the path is still there for re-activation.
systemctl --user stop "$UNIT.service" 2>/dev/null || true
[ -S "$SOCK" ] || fail "inode: socket path vanished after service stop (daemon must not unlink)"
note "inode: OK (socket survives service stop; systemd owns re-activation)"

if [ "$FAIL" -ne 0 ]; then
    echo "[systemd-e2e] FAILURES above; daemon.err tail:" >&2
    tail -20 "$CASE_DIR/daemon.err" >&2 2>/dev/null || true
    exit 1
fi
note "PASS: systemd socket activation live E2E (I_INSTALLED + I_ACTIVATED)"
