#!/bin/sh
# True-process pathname UDS singleton/lifecycle/peer acceptance test for Issue #25.
# No sleeps: readiness and liveness are observed through bounded probes.
set -eu

OWNER=${OWNER:-./build/cbm-uds-owner}
TMPDIR_ROOT=${TMPDIR:-/tmp}
CASE_DIR=$(mktemp -d "$TMPDIR_ROOT/cbm-uds-owner.XXXXXX")
chmod 700 "$CASE_DIR"
SOCKET_PATH="$CASE_DIR/daemon.sock"
OWNER1_LOG="$CASE_DIR/owner1.log"
OWNER2_LOG="$CASE_DIR/owner2.log"
OWNER1_PID=
FOREIGN_PID=

cleanup() {
    if [ -n "${OWNER1_PID:-}" ] && kill -0 "$OWNER1_PID" 2>/dev/null; then
        kill -TERM "$OWNER1_PID" 2>/dev/null || true
        i=0
        while kill -0 "$OWNER1_PID" 2>/dev/null && [ "$i" -lt 200 ]; do i=$((i + 1)); done
        if kill -0 "$OWNER1_PID" 2>/dev/null; then kill -KILL "$OWNER1_PID" 2>/dev/null || true; fi
        wait "$OWNER1_PID" 2>/dev/null || true
    fi
    if [ -n "${FOREIGN_PID:-}" ] && kill -0 "$FOREIGN_PID" 2>/dev/null; then
        kill -TERM "$FOREIGN_PID" 2>/dev/null || true
        wait "$FOREIGN_PID" 2>/dev/null || true
    fi
    rm -rf "$CASE_DIR"
}
trap cleanup EXIT INT TERM

wait_ready() {
    log=$1 pid=$2 label=$3
    i=0
    while ! grep -q '^READY ' "$log" 2>/dev/null; do
        if ! kill -0 "$pid" 2>/dev/null; then
            wait "$pid" || rc=$?
            printf '%s exited before readiness: rc=%s\n' "$label" "${rc:-0}" >&2
            cat "$log" >&2
            exit 1
        fi
        i=$((i + 1))
        [ "$i" -lt 200000 ] || { printf '%s readiness probe exhausted\n' "$label" >&2; exit 1; }
    done
}

assert_state_order() {
    log=$1; shift
    last=0
    for state in "$@"; do
        line=$(grep -n "^STATE $state$" "$log" | tail -1 | cut -d: -f1)
        [ -n "$line" ] || { printf 'missing state %s in %s\n' "$state" "$log" >&2; cat "$log" >&2; exit 1; }
        [ "$line" -gt "$last" ] || { printf 'state %s out of order in %s\n' "$state" "$log" >&2; exit 1; }
        last=$line
    done
}

"$OWNER" --socket "$SOCKET_PATH" >"$OWNER1_LOG" 2>&1 &
OWNER1_PID=$!
wait_ready "$OWNER1_LOG" "$OWNER1_PID" owner1
[ "$(stat -f '%Lp' "$CASE_DIR" 2>/dev/null || stat -c '%a' "$CASE_DIR")" = 700 ]
[ "$(stat -f '%Lp' "$SOCKET_PATH" 2>/dev/null || stat -c '%a' "$SOCKET_PATH")" = 600 ]
"$OWNER" --probe "$SOCKET_PATH" >"$CASE_DIR/probe.log" 2>&1
while ! grep -q '^PEER_OK ' "$OWNER1_LOG" 2>/dev/null; do
    kill -0 "$OWNER1_PID" 2>/dev/null || { cat "$OWNER1_LOG" >&2; exit 1; }
done
assert_state_order "$OWNER1_LOG" D_STARTING D_BIND_CLAIM D_LISTENING D_SERVING

set +e
"$OWNER" --socket "$SOCKET_PATH" >"$OWNER2_LOG" 2>&1
OWNER2_RC=$?
set -e
[ "$OWNER2_RC" -ne 0 ] || { printf 'second owner unexpectedly succeeded\n' >&2; exit 1; }
grep -q '^STATE D_SINGLETON_LOSER$' "$OWNER2_LOG"
kill -0 "$OWNER1_PID" 2>/dev/null || { printf 'first owner died after rejection\n' >&2; exit 1; }

kill -TERM "$OWNER1_PID"
wait "$OWNER1_PID"
OWNER1_PID=
[ ! -e "$SOCKET_PATH" ] || { printf 'socket survived graceful SIGTERM\n' >&2; exit 1; }
assert_state_order "$OWNER1_LOG" D_STARTING D_BIND_CLAIM D_LISTENING D_SERVING D_DRAINING D_EXITED

# SIGKILL leaves a stale pathname; the next owner must safely reclaim it.
"$OWNER" --socket "$SOCKET_PATH" >"$CASE_DIR/killed.log" 2>&1 &
OWNER1_PID=$!
wait_ready "$CASE_DIR/killed.log" "$OWNER1_PID" pre-kill-owner
kill -KILL "$OWNER1_PID"
wait "$OWNER1_PID" 2>/dev/null || true
OWNER1_PID=
[ -S "$SOCKET_PATH" ] || { printf 'SIGKILL did not leave stale socket\n' >&2; exit 1; }

"$OWNER" --socket "$SOCKET_PATH" >"$CASE_DIR/recovered.log" 2>&1 &
OWNER1_PID=$!
wait_ready "$CASE_DIR/recovered.log" "$OWNER1_PID" recovered-owner
assert_state_order "$CASE_DIR/recovered.log" D_STARTING D_BIND_CLAIM D_STALE_RECLAIMED D_LISTENING
"$OWNER" --probe "$SOCKET_PATH" >"$CASE_DIR/recovered-probe.log" 2>&1
kill -TERM "$OWNER1_PID"
wait "$OWNER1_PID"
OWNER1_PID=
[ ! -e "$SOCKET_PATH" ] || { printf 'recovered socket survived SIGTERM\n' >&2; exit 1; }

# Kernel peer credentials are enforced. A deliberately different policy UID rejects this real peer.
REJECT_UID=$(( $(id -u) + 1 ))
"$OWNER" --socket "$SOCKET_PATH" --expected-uid "$REJECT_UID" >"$CASE_DIR/peer-reject.log" 2>&1 &
OWNER1_PID=$!
wait_ready "$CASE_DIR/peer-reject.log" "$OWNER1_PID" peer-reject-owner
set +e
"$OWNER" --probe "$SOCKET_PATH" >"$CASE_DIR/rejected-probe.log" 2>&1
PEER_PROBE_RC=$?
set -e
[ "$PEER_PROBE_RC" -ne 0 ] || { printf 'wrong-policy UID probe unexpectedly succeeded\n' >&2; exit 1; }
while ! grep -q '^ACCEPT_REJECTED .*error=Permission denied' "$CASE_DIR/peer-reject.log" 2>/dev/null; do
    kill -0 "$OWNER1_PID" 2>/dev/null || { cat "$CASE_DIR/peer-reject.log" >&2; exit 1; }
done
kill -TERM "$OWNER1_PID"
wait "$OWNER1_PID"
OWNER1_PID=


# An active pathname UDS that does not use our companion lock must not be unlinked.
"$OWNER" --foreign-listener "$SOCKET_PATH" >"$CASE_DIR/foreign.log" 2>&1 &
FOREIGN_PID=$!
i=0
while ! grep -q '^FOREIGN_READY ' "$CASE_DIR/foreign.log" 2>/dev/null; do
    kill -0 "$FOREIGN_PID" 2>/dev/null || { cat "$CASE_DIR/foreign.log" >&2; exit 1; }
    i=$((i + 1))
    [ "$i" -lt 200000 ] || { printf 'foreign listener readiness probe exhausted\n' >&2; exit 1; }
done
set +e
"$OWNER" --socket "$SOCKET_PATH" >"$CASE_DIR/active-foreign-owner.log" 2>&1
ACTIVE_FOREIGN_RC=$?
set -e
[ "$ACTIVE_FOREIGN_RC" -ne 0 ] || { printf 'owner replaced an active foreign listener\n' >&2; exit 1; }
[ -S "$SOCKET_PATH" ] || { printf 'owner unlinked an active foreign listener\n' >&2; exit 1; }
kill -0 "$FOREIGN_PID" 2>/dev/null || { printf 'active foreign listener died\n' >&2; exit 1; }
kill -TERM "$FOREIGN_PID"
wait "$FOREIGN_PID"
FOREIGN_PID=
[ ! -e "$SOCKET_PATH" ] || rm -f "$SOCKET_PATH"

# A stale socket may be reclaimed, but an unrelated filesystem object must never be removed.
printf 'do-not-delete\n' >"$SOCKET_PATH"
set +e
"$OWNER" --socket "$SOCKET_PATH" >"$CASE_DIR/regular-file-owner.log" 2>&1
REGULAR_FILE_RC=$?
set -e
[ "$REGULAR_FILE_RC" -ne 0 ] || { printf 'owner replaced a non-socket pathname\n' >&2; exit 1; }
[ "$(cat "$SOCKET_PATH")" = "do-not-delete" ] || { printf 'owner modified non-socket pathname\n' >&2; exit 1; }
rm -f "$SOCKET_PATH"

printf 'STATES: ' && grep '^STATE ' "$OWNER1_LOG" | tr '\n' ' ' && printf '\n'
printf 'PROBE: ' && cat "$CASE_DIR/probe.log"
printf 'SECOND_OWNER: ' && tr '\n' ' ' <"$OWNER2_LOG" && printf '\n'
printf 'STALE_RECLAIM: ' && grep '^STATE ' "$CASE_DIR/recovered.log" | tr '\n' ' ' && printf '\n'
printf 'PEER_REJECT: ' && grep '^ACCEPT_REJECTED ' "$CASE_DIR/peer-reject.log" | tr '\n' ' ' && printf '\n'
printf 'ACTIVE_FOREIGN_REJECTED: ' && tr '\n' ' ' <"$CASE_DIR/active-foreign-owner.log" && printf '\n'
printf 'NON_SOCKET_REJECTED: ' && tr '\n' ' ' <"$CASE_DIR/regular-file-owner.log" && printf '\n'
printf 'PASS true-process UDS lifecycle, peer UID, probe, singleton and stale reclaim\n'
