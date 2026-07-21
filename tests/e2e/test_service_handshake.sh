#!/bin/sh
# Issue #29: service install/uninstall/upgrade handshake live E2E
# (macOS launchd main path; non-Darwin SKIP exit 0).
#
# Drives the REAL user-facing scripts scripts/service/install.sh and
# scripts/service/uninstall.sh under a temp HOME + throwaway label, then
# walks the full service lifecycle with real launchd events only:
#
#   I_NOT_INSTALLED — no service: shim connection fails closed (exit != 0).
#   I_INSTALLED     — install.sh: launchctl knows the label, socket bound
#                     mode 600, daemon NOT started yet.
#   I_ACTIVATED     — first connection triggers on-demand start: real MCP
#                     initialize round-trip + daemon.launchd.adopted log +
#                     exactly 1 instance.
#   I_UPGRADED      — incompatible "old" shim (via the approved
#                     CBM_SHIM_PROTOCOL_VERSION_OVERRIDE injection seam) is
#                     rejected (exit != 0, VERSION_MISMATCH on stderr);
#                     a current shim still works afterwards (recoverable).
#   I_RESTARTED     — SIGKILL the daemon; launchd still holds the socket;
#                     next connection re-spawns (count back to 1, MCP OK).
#   I_UNINSTALLED   — uninstall.sh: label unknown, plist/socket gone,
#                     connection fails closed again.
#
# Allowed seams (issue #29 contract): throwaway label, temp HOME/socket dir,
# version-number injection for the upgrade scenario. No mocks, no manual
# daemon pre-start, no fabricated readiness.
set -eu

case "$(uname -s)" in
Darwin) ;;
*)
    echo "[svc-e2e] SKIP: this service E2E drives launchd (macOS only)" >&2
    exit 0
    ;;
esac

BIN=${BIN:-./build/c/codebase-memory-mcp}
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")
[ -x "$BIN" ] || { echo "[svc-e2e] FAIL: missing binary $BIN" >&2; exit 1; }

REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
INSTALL_SH="$REPO_ROOT/scripts/service/install.sh"
UNINSTALL_SH="$REPO_ROOT/scripts/service/uninstall.sh"
[ -f "$INSTALL_SH" ] || { echo "[svc-e2e] FAIL: missing $INSTALL_SH" >&2; exit 1; }

TMPDIR_ROOT=${TMPDIR:-/tmp}
CASE_DIR=$(mktemp -d "$TMPDIR_ROOT/cbm-svc-e2e.XXXXXX")
chmod 700 "$CASE_DIR"
FAIL=0
UID_NUM=$(id -u)
LABEL="dev.codebase-memory.e2e-svc.$$"
AGENT_DIR="$CASE_DIR/agents"
SOCK_DIR="$CASE_DIR/runtime"
SOCK="$SOCK_DIR/daemon.sock"
LOG_DIR="$CASE_DIR/logs"
TEST_HOME="$CASE_DIR/home"
mkdir -p "$SOCK_DIR" "$TEST_HOME" "$LOG_DIR"
chmod 700 "$SOCK_DIR" "$TEST_HOME"
# Keep the daemon's HTTP UI off inside the temp HOME so the E2E exercises
# only the activation path (config file is the documented UI toggle).
mkdir -p "$TEST_HOME/.cache/codebase-memory-mcp"
printf '{"ui_enabled": false, "ui_port": 9751}\n' \
    >"$TEST_HOME/.cache/codebase-memory-mcp/config.json"

note() { printf '[svc-e2e] %s\n' "$1"; }
fail() { printf '[svc-e2e] FAIL: %s\n' "$1" >&2; FAIL=1; }

cleanup() {
    launchctl bootout "gui/$UID_NUM/$LABEL" 2>/dev/null || true
    rm -rf "$CASE_DIR"
}
trap cleanup EXIT INT TERM

daemon_pid_count() {
    pgrep -f -- "codebase-memory-mcp daemon --launchd --socket $SOCK" 2>/dev/null |
        wc -l | tr -d ' '
}

INIT_MSG='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"svc-e2e","version":"0"}}}'

shim_roundtrip() {
    # $1 = out file, $2 = err file; returns shim exit code.
    set +e
    printf '%s' "$INIT_MSG" | "$BIN" --socket "$SOCK" >"$1" 2>"$2"
    _rc=$?
    set -e
    return $_rc
}

# ── I_NOT_INSTALLED: fail-closed before any install ─────────────────────
set +e
printf '%s' "$INIT_MSG" | "$BIN" --socket "$SOCK" \
    >"$CASE_DIR/pre.out" 2>"$CASE_DIR/pre.err"
rc=$?
set -e
if [ "$rc" -eq 0 ]; then
    fail "I_NOT_INSTALLED: shim exit=0 against a non-existent service (want fail-closed)"
else
    note "I_NOT_INSTALLED: OK (no service: shim exit=$rc, fail-closed)"
fi
[ ! -S "$SOCK" ] || fail "I_NOT_INSTALLED: socket exists before install"

# ── I_INSTALLED: install.sh registers the service ───────────────────────
if ! HOME="$TEST_HOME" sh "$INSTALL_SH" \
    --bin "$BIN" --socket "$SOCK" --label "$LABEL" \
    --prefix "$AGENT_DIR" --log-dir "$LOG_DIR" \
    >"$CASE_DIR/install.out" 2>&1; then
    fail "I_INSTALLED: install.sh failed: $(tail -3 "$CASE_DIR/install.out")"
fi
PLIST="$AGENT_DIR/$LABEL.plist"
[ -f "$PLIST" ] || fail "I_INSTALLED: install.sh did not write $PLIST"

# Double-install must be refused (no half-overwrite).
if HOME="$TEST_HOME" sh "$INSTALL_SH" \
    --bin "$BIN" --socket "$SOCK" --label "$LABEL" \
    --prefix "$AGENT_DIR" --log-dir "$LOG_DIR" \
    >"$CASE_DIR/install2.out" 2>&1; then
    fail "I_INSTALLED: second install.sh with same label succeeded (want explicit refusal)"
fi

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
fi
if [ "$(daemon_pid_count)" -ne 0 ]; then
    fail "I_INSTALLED: daemon already running before any connection (not on-demand)"
fi
[ "$FAIL" -ne 0 ] ||
    note "I_INSTALLED: OK (label known, socket bound mode=600, daemon not started, re-install refused)"

# ── I_ACTIVATED: first connection triggers on-demand start ──────────────
if ! shim_roundtrip "$CASE_DIR/activate.out" "$CASE_DIR/activate.err"; then
    fail "I_ACTIVATED: shim exit != 0; stderr: $(tail -3 "$CASE_DIR/activate.err")"
fi
grep -q '"jsonrpc"' "$CASE_DIR/activate.out" ||
    fail "I_ACTIVATED: no MCP initialize response on stdout"
grep -q 'daemon.launchd.adopted' "$LOG_DIR/daemon.err" ||
    fail "I_ACTIVATED: daemon log missing launchd fd adoption marker"
COUNT=$(daemon_pid_count)
if [ "$COUNT" -ne 1 ]; then
    fail "I_ACTIVATED: expected exactly 1 daemon instance, got $COUNT"
else
    note "I_ACTIVATED: OK (first connection spawned daemon, MCP round-trip served, adopted log present, instances=1)"
fi

# ── I_UPGRADED: incompatible old shim rejected, current shim recovers ────
set +e
printf '%s' "$INIT_MSG" |
    CBM_SHIM_PROTOCOL_VERSION_OVERRIDE=2 "$BIN" --socket "$SOCK" \
        >"$CASE_DIR/old.out" 2>"$CASE_DIR/old.err"
rc_old=$?
set -e
if [ "$rc_old" -eq 0 ]; then
    fail "I_UPGRADED: incompatible shim (override=2) exit=0 (want rejection)"
fi
grep -q 'VERSION_MISMATCH' "$CASE_DIR/old.err" ||
    fail "I_UPGRADED: stderr missing VERSION_MISMATCH marker: $(tail -3 "$CASE_DIR/old.err")"
if shim_roundtrip "$CASE_DIR/recover.out" "$CASE_DIR/recover.err" &&
    grep -q '"jsonrpc"' "$CASE_DIR/recover.out"; then
    note "I_UPGRADED: OK (old shim rejected exit=$rc_old with VERSION_MISMATCH, current shim still served)"
else
    fail "I_UPGRADED: current shim broken after mismatch rejection"
fi

# ── I_RESTARTED: SIGKILL daemon; launchd re-activates on next connection ─
# launchd treats a job that dies within ThrottleInterval (default 10s) of
# its spawn as crash-looping and throttles the re-spawn, which starves the
# reconnect attempts below. Let the daemon genuinely outlive the throttle
# window first (real launchd semantics, measured on this host: a daemon
# killed after >10s uptime is re-spawned immediately on the next
# connection; one killed at ~2s uptime is throttled for ~10s per retry).
note "I_RESTARTED: letting daemon outlive the launchd ThrottleInterval (11s) before SIGKILL"
sleep 11
DPID=$(pgrep -f -- "codebase-memory-mcp daemon --launchd --socket $SOCK" | head -1)
[ -n "$DPID" ] || fail "I_RESTARTED: no daemon pid to kill"
kill -9 "$DPID" 2>/dev/null || fail "I_RESTARTED: SIGKILL failed"
i=0
while kill -0 "$DPID" 2>/dev/null; do
    i=$((i + 1))
    [ "$i" -lt 100 ] || { fail "I_RESTARTED: daemon $DPID survived SIGKILL"; break; }
    sleep 0.05
done
[ -S "$SOCK" ] || fail "I_RESTARTED: socket vanished after SIGKILL (launchd must keep holding it)"
# launchd throttles re-spawn after an abnormal exit (ThrottleInterval,
# default ~10s): retry the REAL connection with a bounded deadline instead
# of faking readiness. First successful round-trip proves re-activation.
restart_ok=0
i=0
while [ "$i" -lt 30 ]; do
    if shim_roundtrip "$CASE_DIR/restart.out" "$CASE_DIR/restart.err"; then
        restart_ok=1
        break
    fi
    i=$((i + 1))
    sleep 1
done
[ "$restart_ok" -eq 1 ] ||
    fail "I_RESTARTED: no successful re-activation within 30s; last stderr: $(tail -3 "$CASE_DIR/restart.err")"
grep -q '"jsonrpc"' "$CASE_DIR/restart.out" ||
    fail "I_RESTARTED: no MCP response after re-activation"
COUNT=$(daemon_pid_count)
if [ "$COUNT" -ne 1 ]; then
    fail "I_RESTARTED: expected exactly 1 daemon instance after re-activation, got $COUNT"
else
    note "I_RESTARTED: OK (SIGKILL'd pid $DPID, socket survived, next connection re-spawned, instances=1)"
fi

# ── I_UNINSTALLED: uninstall.sh removes everything; fail-closed again ────
if ! HOME="$TEST_HOME" sh "$UNINSTALL_SH" --label "$LABEL" --prefix "$AGENT_DIR" \
    >"$CASE_DIR/uninstall.out" 2>&1; then
    fail "I_UNINSTALLED: uninstall.sh failed: $(tail -3 "$CASE_DIR/uninstall.out")"
fi
if launchctl print "gui/$UID_NUM/$LABEL" >/dev/null 2>&1; then
    fail "I_UNINSTALLED: launchctl still knows $LABEL"
fi
[ ! -e "$PLIST" ] || fail "I_UNINSTALLED: plist residue at $PLIST"
[ ! -e "$SOCK" ] || fail "I_UNINSTALLED: socket residue at $SOCK"
set +e
printf '%s' "$INIT_MSG" | "$BIN" --socket "$SOCK" \
    >"$CASE_DIR/post.out" 2>"$CASE_DIR/post.err"
rc=$?
set -e
[ "$rc" -ne 0 ] || fail "I_UNINSTALLED: shim exit=0 after uninstall (want fail-closed)"
# Idempotent second uninstall must succeed and say not-installed.
if ! HOME="$TEST_HOME" sh "$UNINSTALL_SH" --label "$LABEL" --prefix "$AGENT_DIR" \
    >"$CASE_DIR/uninstall2.out" 2>&1; then
    fail "I_UNINSTALLED: second uninstall.sh exited non-zero (want idempotent success)"
fi
grep -q 'not-installed' "$CASE_DIR/uninstall2.out" ||
    fail "I_UNINSTALLED: second uninstall did not report not-installed"
[ "$FAIL" -ne 0 ] ||
    note "I_UNINSTALLED: OK (label gone, no plist/socket residue, fail-closed exit=$rc, re-uninstall idempotent)"

if [ "$FAIL" -ne 0 ]; then
    echo "[svc-e2e] FAILURES above; daemon.err tail:" >&2
    tail -20 "$LOG_DIR/daemon.err" >&2 2>/dev/null || true
    exit 1
fi
note "PASS: service install/uninstall/upgrade handshake live E2E (6 states)"
