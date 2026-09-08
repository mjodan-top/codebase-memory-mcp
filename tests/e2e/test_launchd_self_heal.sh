#!/bin/sh
# Issue #60 live E2E: after the daemon dies, does it come back BY ITSELF?
#
# The 2026-09-07 outage was not just "the daemon crashed" — it was "and then
# nothing brought it back for ~30h". The machine was running a hand-written
# RunAtLoad + KeepAlive plist, and measurement (2026-09-08) showed launchd
# honours neither key for this job: bootstrap leaves runs=0, and a SIGKILL of a
# job that had been alive 47 minutes never increments runs. Only an explicit
# `launchctl kickstart` starts it.
#
# The fix is not to argue with launchd about KeepAlive; it is to go back to
# socket activation, where recovery is structural: launchd owns the listening
# socket, so the NEXT client connection spawns a fresh daemon whether or not
# the previous one is alive.
#
# Everything here is real: real throwaway LaunchAgent labels bootstrapped into
# the user's gui domain, real pathname UDS bound by launchd, real shim
# subprocesses, a real SIGKILL. Nothing is mocked and readiness is never
# fabricated.
#
# Scenarios:
#   S_SELF_HEAL       — socket-activated (the fix): kill the daemon, and the
#                       next connection must transparently get a NEW daemon.
#   S_RESIDENT_NO_HEAL— the pre-fix shape (RunAtLoad+KeepAlive, no <Sockets>):
#                       kill it and nothing comes back. This is the built-in
#                       counter-example — without it, S_SELF_HEAL passing would
#                       not prove that socket activation is what recovers.
#   S_DEFAULT_PERSISTENT — the daemon's default socket path must NOT sit in
#                       /tmp, which the OS sweeps (that sweep is what broke
#                       bootstrap with error 5 and cost us socket activation).
#   S_INSTALLER_AGREES— install.sh's default socket path must equal the one the
#                       binary resolves on its own. These are two independent
#                       implementations of one contract; drift strands clients.
set -eu

if [ "$(uname -s)" != "Darwin" ]; then
    printf '[selfheal-e2e] SKIP: launchd is macOS-only (this host is %s)\n' "$(uname -s)"
    exit 0
fi

BIN=${BIN:-./build/c/codebase-memory-mcp}
INSTALL_SH=${INSTALL_SH:-scripts/service/install.sh}

case "$BIN" in
/*) ;;
*) BIN="$(pwd)/$BIN" ;;
esac
[ -x "$BIN" ] || { printf '[selfheal-e2e] FAIL: BIN not executable: %s\n' "$BIN" >&2; exit 1; }

# Short parent, deliberately not $TMPDIR: macOS per-user TMPDIR is long enough
# to blow the 104-byte sun_path budget once a socket name is appended.
CASE_DIR=$(mktemp -d "/tmp/cbm-selfheal.XXXXXX")
chmod 700 "$CASE_DIR"
UID_NUM=$(id -u)
SUFFIX=$(basename "$CASE_DIR" | tr -cd 'A-Za-z0-9')
HEAL_LABEL="dev.codebase-memory.selfheal-$SUFFIX"
RES_LABEL="dev.codebase-memory.resident-$SUFFIX"
FAIL=0

note() { printf '[selfheal-e2e] %s\n' "$1"; }
fail() { printf '[selfheal-e2e] FAIL: %s\n' "$1" >&2; FAIL=1; }

bootout_label() {
    launchctl print "gui/$UID_NUM/$1" >/dev/null 2>&1 || return 0
    launchctl bootout "gui/$UID_NUM/$1" >/dev/null 2>&1 || true
    # bootout is asynchronous while the job runs; wait (bounded) for launchd to
    # actually forget the label.
    i=0
    while launchctl print "gui/$UID_NUM/$1" >/dev/null 2>&1; do
        i=$((i + 1))
        [ "$i" -lt 100 ] || return 0
        sleep 0.05
    done
}

cleanup() {
    bootout_label "$HEAL_LABEL"
    bootout_label "$RES_LABEL"
    rm -rf "$CASE_DIR"
}
trap cleanup EXIT INT TERM

INIT_MSG='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"selfheal-e2e","version":"0"}}}'

# The daemon for a given socket path — the path is unique per scenario, so this
# never matches another test's or the machine's real daemon.
daemon_pid_for() {
    pgrep -f "daemon .*--socket $1\$" 2>/dev/null | head -1 || true
}

wait_pid_gone() {
    i=0
    while kill -0 "$1" 2>/dev/null; do
        i=$((i + 1))
        [ "$i" -lt 100 ] || return 1
        sleep 0.05
    done
    return 0
}

wait_for_socket() {
    i=0
    while [ ! -S "$1" ]; do
        i=$((i + 1))
        [ "$i" -lt 200 ] || return 1
        sleep 0.05
    done
    return 0
}

write_activated_plist() {
    # label, sock, home, plist, logdir
    cat >"$4" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
 "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>$1</string>
    <key>ProgramArguments</key>
    <array>
        <string>$BIN</string>
        <string>daemon</string>
        <string>--launchd</string>
        <string>--socket</string>
        <string>$2</string>
    </array>
    <key>EnvironmentVariables</key>
    <dict>
        <key>HOME</key><string>$3</string>
    </dict>
    <key>Sockets</key>
    <dict>
        <key>Listeners</key>
        <dict>
            <key>SockPathName</key><string>$2</string>
            <key>SockPathMode</key><integer>384</integer>
        </dict>
    </dict>
    <key>StandardOutPath</key><string>$5/daemon.out</string>
    <key>StandardErrorPath</key><string>$5/daemon.err</string>
</dict>
</plist>
EOF
}

write_resident_plist() {
    # The pre-fix shape: no <Sockets>, relies on RunAtLoad + KeepAlive.
    cat >"$4" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
 "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>$1</string>
    <key>ProgramArguments</key>
    <array>
        <string>$BIN</string>
        <string>daemon</string>
        <string>--socket</string>
        <string>$2</string>
    </array>
    <key>EnvironmentVariables</key>
    <dict>
        <key>HOME</key><string>$3</string>
    </dict>
    <key>RunAtLoad</key><true/>
    <key>KeepAlive</key><true/>
    <key>StandardOutPath</key><string>$5/daemon.out</string>
    <key>StandardErrorPath</key><string>$5/daemon.err</string>
</dict>
</plist>
EOF
}

# ── S_SELF_HEAL ─────────────────────────────────────────────────────────
note "S_SELF_HEAL: socket-activated service must survive losing its daemon"
HEAL_HOME="$CASE_DIR/heal-home"
HEAL_SOCKDIR="$CASE_DIR/hs"
HEAL_LOGS="$CASE_DIR/heal-logs"
mkdir -p "$HEAL_HOME" "$HEAL_SOCKDIR" "$HEAL_LOGS"
chmod 700 "$HEAL_SOCKDIR"
HEAL_SOCK="$HEAL_SOCKDIR/d.sock"
HEAL_PLIST="$CASE_DIR/$HEAL_LABEL.plist"
write_activated_plist "$HEAL_LABEL" "$HEAL_SOCK" "$HEAL_HOME" "$HEAL_PLIST" "$HEAL_LOGS"

launchctl bootstrap "gui/$UID_NUM" "$HEAL_PLIST" ||
    { fail "S_SELF_HEAL: launchctl bootstrap failed"; exit 1; }
wait_for_socket "$HEAL_SOCK" || { fail "S_SELF_HEAL: launchd never bound $HEAL_SOCK"; exit 1; }

# First connection: launchd spawns the daemon on demand.
set +e
printf '%s\n' "$INIT_MSG" | "$BIN" --socket "$HEAL_SOCK" \
    >"$CASE_DIR/heal1.out" 2>"$CASE_DIR/heal1.err"
rc1=$?
set -e
[ "$rc1" -eq 0 ] || fail "S_SELF_HEAL: first connection exit=$rc1 (want 0): $(cat "$CASE_DIR/heal1.err")"
grep -q 'protocolVersion' "$CASE_DIR/heal1.out" ||
    fail "S_SELF_HEAL: first connection got no initialize result"
PID1=$(daemon_pid_for "$HEAL_SOCK")
[ -n "$PID1" ] || { fail "S_SELF_HEAL: no daemon process after first connection"; exit 1; }
note "S_SELF_HEAL: first connection spawned daemon pid=$PID1"

# Now destroy it the way the outage did — unsurvivably, no clean shutdown.
kill -KILL "$PID1"
wait_pid_gone "$PID1" || { fail "S_SELF_HEAL: daemon pid=$PID1 did not die"; exit 1; }
note "S_SELF_HEAL: daemon killed (SIGKILL), pid $PID1 gone"

# THE ASSERTION: plain client connections must get service again within a
# bounded time. No kickstart, no human, no supervisor noticing.
#
# Why "bounded" and not "the very next connection": launchd needs a moment to
# reap the killed job, and the connection that TRIGGERS the respawn can itself
# time out while the fresh daemon is still booting (measured 2026-09-08: at +2s
# launchd already reports state=spawn but the shim gives up with HS_TIMEOUT; at
# +4s the connection is served by the new pid and runs went 1 -> 2). That first
# failed attempt is a real, honest cost of this design — recorded here rather
# than hidden — but it is a retry, not an outage: contrast S_RESIDENT_NO_HEAL,
# where nothing EVER comes back.
HEAL_DEADLINE=20
rc2=1
attempt=0
elapsed=0
while [ "$elapsed" -lt "$HEAL_DEADLINE" ]; do
    attempt=$((attempt + 1))
    set +e
    printf '%s\n' "$INIT_MSG" | "$BIN" --socket "$HEAL_SOCK" \
        >"$CASE_DIR/heal2.out" 2>"$CASE_DIR/heal2.err"
    rc2=$?
    set -e
    [ "$rc2" -eq 0 ] && break
    sleep 2
    elapsed=$((elapsed + 2))
done
if [ "$rc2" -ne 0 ]; then
    fail "S_SELF_HEAL: still unserved ${HEAL_DEADLINE}s and $attempt attempts after the kill (last exit=$rc2) — the service did NOT heal itself: $(cat "$CASE_DIR/heal2.err")"
else
    note "S_SELF_HEAL: service restored on attempt $attempt (~${elapsed}s after the kill)"
    grep -q 'protocolVersion' "$CASE_DIR/heal2.out" ||
        fail "S_SELF_HEAL: reconnect served no initialize result"
    PID2=$(daemon_pid_for "$HEAL_SOCK")
    if [ -z "$PID2" ]; then
        fail "S_SELF_HEAL: reconnect answered but no daemon process is running"
    elif [ "$PID2" = "$PID1" ]; then
        fail "S_SELF_HEAL: pid unchanged ($PID2) — the old process cannot have been killed"
    else
        note "S_SELF_HEAL: OK — next connection was served by a NEW daemon pid=$PID2 (was $PID1), no human involved"
    fi
fi
bootout_label "$HEAL_LABEL"

# ── S_RESIDENT_NO_HEAL (built-in counter-example) ───────────────────────
# Without this, S_SELF_HEAL alone would not show that socket activation is what
# provides recovery. If this scenario ever starts healing, launchd's behaviour
# changed and this whole fix should be re-evaluated — so it failing is
# informative, not noise.
note "S_RESIDENT_NO_HEAL: the pre-fix RunAtLoad+KeepAlive shape must NOT recover"
RES_HOME="$CASE_DIR/res-home"
RES_SOCKDIR="$CASE_DIR/rs"
RES_LOGS="$CASE_DIR/res-logs"
mkdir -p "$RES_HOME" "$RES_SOCKDIR" "$RES_LOGS"
chmod 700 "$RES_SOCKDIR"
RES_SOCK="$RES_SOCKDIR/d.sock"
RES_PLIST="$CASE_DIR/$RES_LABEL.plist"
write_resident_plist "$RES_LABEL" "$RES_SOCK" "$RES_HOME" "$RES_PLIST" "$RES_LOGS"

launchctl bootstrap "gui/$UID_NUM" "$RES_PLIST" ||
    { fail "S_RESIDENT_NO_HEAL: bootstrap failed"; exit 1; }
# RunAtLoad is not honoured here (measured), so start it explicitly — the point
# of this scenario is what happens AFTER it dies, not how it starts.
launchctl kickstart "gui/$UID_NUM/$RES_LABEL" >/dev/null 2>&1 || true
if wait_for_socket "$RES_SOCK"; then
    RPID=$(daemon_pid_for "$RES_SOCK")
    if [ -n "$RPID" ]; then
        note "S_RESIDENT_NO_HEAL: resident daemon pid=$RPID (started via kickstart)"
        kill -KILL "$RPID"
        wait_pid_gone "$RPID" || fail "S_RESIDENT_NO_HEAL: pid=$RPID did not die"
        # Give any supervisor a generous window to act.
        sleep 6
        RPID2=$(daemon_pid_for "$RES_SOCK")
        if [ -n "$RPID2" ]; then
            fail "S_RESIDENT_NO_HEAL: something DID restart it (pid=$RPID2) — launchd behaviour differs from the 2026-09-08 measurement; re-evaluate #60"
        else
            note "S_RESIDENT_NO_HEAL: OK — 6s after the kill nothing came back (this is the outage shape)"
        fi
    else
        note "S_RESIDENT_NO_HEAL: SKIP — could not start the resident daemon to kill it"
    fi
else
    note "S_RESIDENT_NO_HEAL: SKIP — resident plist never produced a socket (consistent with RunAtLoad being ignored)"
fi
bootout_label "$RES_LABEL"

# ── S_DEFAULT_PERSISTENT ────────────────────────────────────────────────
# Read the binary's OWN default path (no --socket): the shim journal records the
# socket it resolved. Redirect the cache dir so we never touch real logs.
note "S_DEFAULT_PERSISTENT: the built-in default socket path must not live in /tmp"
JOURNAL_CACHE="$CASE_DIR/jcache"
mkdir -p "$JOURNAL_CACHE"
set +e
printf '%s\n' '{}' | env CBM_CACHE_DIR="$JOURNAL_CACHE" "$BIN" \
    >"$CASE_DIR/default.out" 2>"$CASE_DIR/default.err"
set -e
DEFAULT_SOCK=$(sed -n 's/.*[[:space:]]socket=\([^[:space:]]*\).*/\1/p' \
    "$JOURNAL_CACHE/logs/shim.log" 2>/dev/null | tail -1)
if [ -z "$DEFAULT_SOCK" ]; then
    fail "S_DEFAULT_PERSISTENT: could not read the resolved default socket from the shim journal"
else
    note "S_DEFAULT_PERSISTENT: binary resolves default socket = $DEFAULT_SOCK"
    case "$DEFAULT_SOCK" in
    /tmp/*)
        fail "S_DEFAULT_PERSISTENT: default socket is under /tmp ($DEFAULT_SOCK) — the OS sweeps it, which breaks socket activation with bootstrap error 5"
        ;;
    "$HOME"/*)
        note "S_DEFAULT_PERSISTENT: OK — default lives under \$HOME (persistent across reboots)"
        ;;
    *)
        note "S_DEFAULT_PERSISTENT: OK — default is outside /tmp ($DEFAULT_SOCK)"
        ;;
    esac
fi

# ── S_INSTALLER_AGREES ──────────────────────────────────────────────────
note "S_INSTALLER_AGREES: install.sh's default must equal the binary's default"
if [ ! -x "$INSTALL_SH" ]; then
    fail "S_INSTALLER_AGREES: installer not found/executable at $INSTALL_SH"
elif [ -z "${DEFAULT_SOCK:-}" ]; then
    fail "S_INSTALLER_AGREES: skipped — no binary default to compare against"
else
    INST_PREFIX="$CASE_DIR/agents"
    mkdir -p "$INST_PREFIX"
    CONTRACT_LABEL="dev.codebase-memory.contract-$SUFFIX"
    set +e
    "$INSTALL_SH" --bin "$BIN" --label "$CONTRACT_LABEL" --prefix "$INST_PREFIX" \
        --log-dir "$CASE_DIR/contract-logs" --no-load \
        >"$CASE_DIR/install.out" 2>"$CASE_DIR/install.err"
    irc=$?
    set -e
    if [ "$irc" -ne 0 ]; then
        fail "S_INSTALLER_AGREES: install.sh --no-load failed (exit=$irc): $(cat "$CASE_DIR/install.err")"
    else
        INST_SOCK=$(sed -n 's/.*<key>SockPathName<\/key><string>\(.*\)<\/string>.*/\1/p' \
            "$INST_PREFIX/$CONTRACT_LABEL.plist" | head -1)
        if [ -z "$INST_SOCK" ]; then
            fail "S_INSTALLER_AGREES: no SockPathName in the generated plist"
        elif [ "$INST_SOCK" != "$DEFAULT_SOCK" ]; then
            fail "S_INSTALLER_AGREES: installer default '$INST_SOCK' != binary default '$DEFAULT_SOCK' — clients would connect to a socket nobody serves"
        else
            note "S_INSTALLER_AGREES: OK — both sides agree on $INST_SOCK"
        fi
    fi
fi

if [ "$FAIL" -eq 0 ]; then
    note "PASS: killing the daemon no longer needs a human to bring it back"
    exit 0
fi
printf '[selfheal-e2e] FAILED\n' >&2
exit 1
