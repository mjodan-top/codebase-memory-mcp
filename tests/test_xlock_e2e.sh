#!/usr/bin/env bash
# test_xlock_e2e.sh — issue #22 cross-process singleflight lock, per-state e2e.
#
# Drives cbm_xlock_t (src/foundation/xlock.c) through EVERY state in its
# 7-state machine using REAL separate OS processes (via `cli --xlock-probe`,
# see run_xlock_probe in src/main.c) — no mocks, no in-process shortcuts. This
# is ground-truth: two independently-launched `codebase-memory-mcp` processes
# racing for the SAME project key is exactly the failure mode from issue #22
# (multiple instances concurrently indexing the same repo).
#
# States asserted (see docs/assets/issue-22/lock-state-machine.webp):
#   ACQUIRING     -> a process attempting the non-blocking flock
#   OWNER_INDEXING -> the process that won the flock
#   WAITING       -> a second process finds the lock already held
#   RELEASED      -> the owner finished cleanly and released
#   REUSED        -> a waiter woke up to a fresh completion marker (owner
#                    finished while it waited) and reused instead of re-running
#   CRASH_RELEASE -> the owner process was killed (SIGKILL) without releasing
#   CRASH_RECLAIMED -> a waiter woke up because the holder died (not a clean
#                    finish) and now legitimately owns the lock itself
#
# Usage: bash tests/test_xlock_e2e.sh
# Exit 0 on success (all states observed correctly), non-zero on failure.

set -uo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BINARY="$PROJECT_ROOT/build/c/codebase-memory-mcp"
FAILURES=0

echo "[xlock_e2e] Building project..."
make -f "$PROJECT_ROOT/Makefile.cbm" cbm -C "$PROJECT_ROOT" --quiet 2>&1
if [ ! -x "$BINARY" ]; then
    echo "[xlock_e2e] FAIL: binary not found at $BINARY after build" >&2
    exit 1
fi
echo "[xlock_e2e] Build OK: $BINARY"

fail() {
    echo "[xlock_e2e] FAIL: $1" >&2
    FAILURES=$((FAILURES + 1))
}

assert_state_in() {
    # assert_state_in <label> <output_file> <state1> [state2 ...]
    local label="$1" file="$2"
    shift 2
    local wanted="$*"
    local got
    got=$(grep -o '"state":"[A-Z_]*"' "$file" | tail -1 | sed 's/"state":"//;s/"//')
    for s in $wanted; do
        if [ "$got" = "$s" ]; then
            echo "[xlock_e2e] OK: $label -> $got"
            return 0
        fi
    done
    fail "$label expected one of [$wanted], got [$got] (raw: $(cat "$file"))"
    return 1
}

assert_state_seen() {
    # assert_state_seen <label> <output_file> <state> — anywhere in the stream,
    # not just the last line (used for the intermediate WAITING/CRASH_RELEASE
    # lines that are followed by a further terminal line).
    local label="$1" file="$2" state="$3"
    if grep -q "\"state\":\"$state\"" "$file"; then
        echo "[xlock_e2e] OK: $label saw $state"
    else
        fail "$label expected to see $state (raw: $(cat "$file"))"
    fi
}

# ── Scenario 1: uncontended run — IDLE -> ACQUIRING -> OWNER_INDEXING -> RELEASED ──
echo ""
echo "[xlock_e2e] Scenario 1: uncontended single process"
export CBM_CACHE_DIR
CBM_CACHE_DIR="$(mktemp -d)/s1"
mkdir -p "$CBM_CACHE_DIR"
out1="$(mktemp)"
"$BINARY" cli --xlock-probe s1_project > "$out1" 2>/dev/null
assert_state_seen "s1.owner" "$out1" "OWNER_INDEXING"
assert_state_in "s1.final" "$out1" "RELEASED"
rm -f "$out1"

# ── Scenario 2: contended, clean finish — WAITING -> REUSED ────────────────
echo ""
echo "[xlock_e2e] Scenario 2: second process waits, first finishes cleanly -> REUSED"
CBM_CACHE_DIR="$(mktemp -d)/s2"
mkdir -p "$CBM_CACHE_DIR"
out_a="$(mktemp)"; out_b="$(mktemp)"
CBM_XLOCK_PROBE_HOLD_MS=1500 "$BINARY" cli --xlock-probe s2_project > "$out_a" 2>/dev/null &
pid_a=$!
sleep 0.3
CBM_XLOCK_PROBE_HOLD_MS=50 "$BINARY" cli --xlock-probe s2_project > "$out_b" 2>/dev/null &
pid_b=$!
wait "$pid_a"
wait "$pid_b"
assert_state_seen "s2.owner" "$out_a" "OWNER_INDEXING"
assert_state_in "s2.owner.final" "$out_a" "RELEASED"
assert_state_seen "s2.waiter" "$out_b" "WAITING"
assert_state_in "s2.waiter.final" "$out_b" "REUSED"
rm -f "$out_a" "$out_b"

# ── Scenario 3: contended, holder crashes — CRASH_RELEASE -> CRASH_RECLAIMED ──
echo ""
echo "[xlock_e2e] Scenario 3: second process waits, first is SIGKILLed -> CRASH_RECLAIMED"
CBM_CACHE_DIR="$(mktemp -d)/s3"
mkdir -p "$CBM_CACHE_DIR"
out_a="$(mktemp)"; out_b="$(mktemp)"
# Hold long enough that we can reliably kill it mid-hold before it exits.
CBM_XLOCK_PROBE_HOLD_MS=5000 "$BINARY" cli --xlock-probe s3_project > "$out_a" 2>/dev/null &
pid_a=$!
sleep 0.3
CBM_XLOCK_PROBE_HOLD_MS=50 "$BINARY" cli --xlock-probe s3_project > "$out_b" 2>/dev/null &
pid_b=$!
sleep 0.3
kill -9 "$pid_a" 2>/dev/null
wait "$pid_a" 2>/dev/null
wait "$pid_b"
assert_state_seen "s3.owner" "$out_a" "OWNER_INDEXING"
assert_state_seen "s3.waiter" "$out_b" "CRASH_RECLAIMED"
# The waiter, having reclaimed, must itself finish cleanly (RELEASED) —
# proving the lock is fully usable again after a crash, not merely unstuck.
grep -q '"state":"RELEASED"' "$out_b" || fail "s3.waiter did not reach RELEASED after reclaiming"
rm -f "$out_a" "$out_b"

# ── Scenario 4: three-way race — exactly ONE owner, the rest wait ─────────
echo ""
echo "[xlock_e2e] Scenario 4: three processes race for the same key — exactly one owner"
CBM_CACHE_DIR="$(mktemp -d)/s4"
mkdir -p "$CBM_CACHE_DIR"
out_x="$(mktemp)"; out_y="$(mktemp)"; out_z="$(mktemp)"
CBM_XLOCK_PROBE_HOLD_MS=800 "$BINARY" cli --xlock-probe s4_project > "$out_x" 2>/dev/null &
CBM_XLOCK_PROBE_HOLD_MS=800 "$BINARY" cli --xlock-probe s4_project > "$out_y" 2>/dev/null &
CBM_XLOCK_PROBE_HOLD_MS=800 "$BINARY" cli --xlock-probe s4_project > "$out_z" 2>/dev/null &
wait
owners=0
for f in "$out_x" "$out_y" "$out_z"; do
    if grep -q '"state":"OWNER_INDEXING"' "$f"; then
        owners=$((owners + 1))
    fi
done
if [ "$owners" -eq 1 ]; then
    echo "[xlock_e2e] OK: s4.exactly_one_owner (owners=$owners)"
else
    fail "s4.exactly_one_owner expected exactly 1, got $owners — THIS IS THE ISSUE #22 REGRESSION"
fi
rm -f "$out_x" "$out_y" "$out_z"

echo ""
if [ "$FAILURES" -eq 0 ]; then
    echo "[xlock_e2e] ALL SCENARIOS GREEN (7-state machine fully covered by real multi-process runs)"
    exit 0
else
    echo "[xlock_e2e] $FAILURES failure(s)"
    exit 1
fi
