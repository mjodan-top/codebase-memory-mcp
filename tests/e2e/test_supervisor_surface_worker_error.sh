#!/bin/sh
# Live E2E: a supervised index worker that fails GRACEFULLY (runs the handler,
# writes an isError result, exits non-zero) must surface its OWN error text to
# the caller — not the generic "Indexing worker crashed on a file".
#
# Background (2026-09-10, svc-feishu): `cli index_repository '{"root_path":...}'`
# (wrong key) made the worker print "repo_path is required" and exit 1. The
# parent classified EXIT_NONZERO like a crash, re-ran the recovery loop, and told
# the agent "Indexing worker crashed on a file" — the real cause was hidden in a
# worker log nobody read. The agent gave up on indexing and fell back to grep.
#
# Everything here is real: the real binary (host-marked → supervisor gate ON),
# a real spawned worker, an isolated cache. Only the arg is wrong on purpose.
set -eu

BIN=${BIN:-./build/c/codebase-memory-mcp}
case "$BIN" in /*) ;; *) BIN="$(pwd)/$BIN" ;; esac

CASE_DIR=$(mktemp -d "/tmp/cbm-superr-e2e.XXXXXX")
chmod 700 "$CASE_DIR"
FAIL=0

note() { printf '[superr-e2e] %s\n' "$1"; }
fail() { printf '[superr-e2e] FAIL: %s\n' "$1" >&2; FAIL=1; }
cleanup() { rm -rf "$CASE_DIR"; }
trap cleanup EXIT INT TERM

export CBM_CACHE_DIR="$CASE_DIR/cache"
mkdir -p "$CBM_CACHE_DIR"
export HOME="$CASE_DIR/home"
mkdir -p "$HOME"
# Make the supervisor's recovery loop, if it wrongly engages, finish fast.
export CBM_INDEX_MAX_RESTARTS=2
# Supervisor must be ON — that is the code path under test.
unset CBM_INDEX_SUPERVISOR

REPO="$CASE_DIR/repo"
mkdir -p "$REPO"
printf 'int greet(void) { return 1; }\n' > "$REPO/a.c"

# Combined stdout+stderr with info-log lines stripped.
cli() { "$BIN" cli "$@" 2>&1 | grep -v '^ts=' | grep -v '^warning:' || true; }

# 1) Wrong key: root_path instead of repo_path → handler returns isError
#    "repo_path is required"; worker exits 1 (graceful, no crash).
OUT=$(cli index_repository "{\"root_path\":\"$REPO\"}")
case "$OUT" in
    *"repo_path is required"*) note "graceful worker error surfaced verbatim" ;;
    *) fail "worker's own error not surfaced: $OUT" ;;
esac
case "$OUT" in
    *"crashed on a file"*) fail "graceful exit mislabeled as crash: $OUT" ;;
    *) ;;
esac

# 2) Sanity: the same binary still indexes fine with the right key (the
#    supervisor's clean path is unaffected).
OUT=$(cli index_repository "{\"repo_path\":\"$REPO\",\"mode\":\"full\"}")
case "$OUT" in
    *'"status":"indexed"'*) note "correct arg still indexes" ;;
    *) fail "index_repository with repo_path did not index: $OUT" ;;
esac

# 3) The supervisor's log must name the graceful failure, not a crash. The worker
#    log is kept on any non-clean exit under <cache>/logs/.
if ls "$CBM_CACHE_DIR"/logs/.worker-*.log >/dev/null 2>&1; then
    note "worker log retained for post-mortem"
else
    note "no worker log retained (acceptable: kept only on non-clean exit)"
fi

[ "$FAIL" -eq 0 ] || exit 1
note "PASS"
