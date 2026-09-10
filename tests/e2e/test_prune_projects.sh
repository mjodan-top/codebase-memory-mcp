#!/bin/sh
# E2E for issue #66: prune_projects — find (and optionally delete) indexed
# projects whose root_path no longer exists on disk.
#
# Motivation (2026-09-09 host audit): 145 of 153 indexed projects were husks of
# deleted worktrees / /tmp test repos. They still resolved, still answered
# search_code from a frozen graph (silently total=0), crowded list_projects and
# ate ~GBs of cache. delete_project only takes one name, so cleaning up meant
# hand-copying names out of list_projects.
#
# Everything here is real: the production binary indexes three real git repos
# into an isolated CBM_CACHE_DIR, two of them are then moved away (same effect
# as a deleted worktree: root_path no longer exists), and the assertions read
# the tool's actual JSON.
set -eu

BIN=${BIN:-./build/c/codebase-memory-mcp}

CASE_DIR=$(mktemp -d "/tmp/cbm-prune-e2e.XXXXXX")
chmod 700 "$CASE_DIR"
FAIL=0

note() { printf '[prune-e2e] %s\n' "$1"; }
fail() { printf '[prune-e2e] FAIL: %s\n' "$1" >&2; FAIL=1; }

cleanup() { rm -rf "$CASE_DIR"; }
trap cleanup EXIT INT TERM

# Isolated cache: the test must never see (or touch) the developer's real
# ~/.cache/codebase-memory-mcp.
export CBM_CACHE_DIR="$CASE_DIR/cache"
export CBM_INDEX_LOG=off
mkdir -p "$CBM_CACHE_DIR"
GRAVE="$CASE_DIR/grave"
mkdir -p "$GRAVE"

mkrepo() {
    # $1 = name. Prints the repo path.
    r="$CASE_DIR/repo-$1"
    mkdir -p "$r"
    git -C "$r" init -q
    printf 'int f_%s(void) { return 1; }\n' "$1" > "$r/a.c"
    git -C "$r" add . >/dev/null
    git -C "$r" -c user.email=e2e@example.com -c user.name=e2e commit -qm init
    printf '%s\n' "$r"
}

index_repo() {
    "$BIN" cli index_repository "{\"repo_path\":\"$1\"}" >/dev/null 2>"$CASE_DIR/index.err" \
        || { fail "index_repository $1: $(cat "$CASE_DIR/index.err")"; return 1; }
}

# prune_projects: $1 = JSON args. Echoes the JSON payload (stdout only; the
# binary logs to stderr).
prune() {
    "$BIN" cli prune_projects "$1" 2>/dev/null
}

count_field() {
    # $1 = json, $2 = key holding an integer
    printf '%s' "$1" | sed -n "s/.*\"$2\":\([0-9-][0-9]*\).*/\1/p" | head -1
}

# ── Fixture ─────────────────────────────────────────────────────────────────
LIVE=$(mkrepo live)
GONE1=$(mkrepo gone1)
GONE2=$(mkrepo gone2)
index_repo "$LIVE"
index_repo "$GONE1"
index_repo "$GONE2"

NDB=$(ls "$CBM_CACHE_DIR"/*.db | wc -l | tr -d ' ')
[ "$NDB" = "3" ] || fail "expected 3 .db files after indexing, got $NDB"

# Simulate two deleted worktrees: their root_path no longer exists.
mv "$GONE1" "$GRAVE/"
mv "$GONE2" "$GRAVE/"

# ── 1. dry_run (default) lists exactly the two stale projects, deletes nothing
OUT=$(prune '{}')
note "dry_run: $OUT"
case "$OUT" in
    *'"dry_run":true'*) ;;
    *) fail "dry_run default should be true: $OUT" ;;
esac
[ "$(count_field "$OUT" count)" = "2" ] || fail "dry_run count != 2: $OUT"
case "$OUT" in
    *"repo-gone1"*) ;; *) fail "gone1 missing from candidates" ;;
esac
case "$OUT" in
    *"repo-gone2"*) ;; *) fail "gone2 missing from candidates" ;;
esac
case "$OUT" in
    *'"candidates":['*'repo-live'*) fail "live project must NEVER be a candidate: $OUT" ;;
esac
case "$OUT" in
    *'"deleted"'*) fail "dry_run must not emit a deleted list" ;;
esac
NDB=$(ls "$CBM_CACHE_DIR"/*.db | wc -l | tr -d ' ')
[ "$NDB" = "3" ] || fail "dry_run must not delete anything (db count $NDB)"

# ── 2. older_than_days keeps freshly indexed (just now) stale projects
OUT=$(prune '{"older_than_days":9999}')
note "older_than_days=9999: $OUT"
[ "$(count_field "$OUT" count)" = "0" ] || fail "older_than_days=9999 should yield 0 candidates: $OUT"
case "$OUT" in
    *'"reason":"newer_than_older_than_days"'*) ;;
    *) fail "skipped entries should carry the age reason: $OUT" ;;
esac

# older_than_days=0 → age filter is a no-op, both are back
OUT=$(prune '{"older_than_days":0}')
[ "$(count_field "$OUT" count)" = "2" ] || fail "older_than_days=0 should yield 2 candidates: $OUT"

# ── 3. dry_run=false deletes exactly the two stale projects
OUT=$(prune '{"dry_run":false}')
note "dry_run=false: $OUT"
case "$OUT" in
    *'"dry_run":false'*) ;; *) fail "dry_run echo wrong: $OUT" ;;
esac
[ "$(count_field "$OUT" count)" = "2" ] || fail "delete count != 2: $OUT"
case "$OUT" in
    *'"failed":[]'*) ;; *) fail "expected empty failed list: $OUT" ;;
esac
FREED=$(count_field "$OUT" freed_bytes)
[ -n "$FREED" ] && [ "$FREED" -gt 0 ] || fail "freed_bytes should be > 0: $OUT"

NDB=$(ls "$CBM_CACHE_DIR"/*.db | wc -l | tr -d ' ')
[ "$NDB" = "1" ] || fail "expected exactly 1 .db after prune, got $NDB: $(ls "$CBM_CACHE_DIR")"
ls "$CBM_CACHE_DIR"/*.db | grep -q 'repo-live' || fail "live project db was removed!"
if ls "$CBM_CACHE_DIR"/*gone* >/dev/null 2>&1; then
    fail "stale db residue left behind: $(ls "$CBM_CACHE_DIR")"
fi

# list_projects must agree: only the live project remains.
LIST=$("$BIN" cli list_projects '{}' 2>/dev/null)
case "$LIST" in
    *repo-gone*) fail "list_projects still shows a pruned project: $LIST" ;;
esac
case "$LIST" in
    *repo-live*) ;; *) fail "list_projects lost the live project: $LIST" ;;
esac

# ── 4. Idempotent: a second prune finds nothing and is not an error
OUT=$(prune '{"dry_run":false}')
[ "$(count_field "$OUT" count)" = "0" ] || fail "second prune should find 0: $OUT"

# ── 5. Bad input is rejected loudly (the CLI prints isError results on stderr)
OUT=$("$BIN" cli prune_projects '{"older_than_days":-1}' 2>&1 || true)
case "$OUT" in
    *'older_than_days must be >= 0'*) ;;
    *) fail "negative older_than_days should be rejected: $OUT" ;;
esac

if [ "$FAIL" -ne 0 ]; then
    note "FAILED"
    exit 1
fi
note "OK"
