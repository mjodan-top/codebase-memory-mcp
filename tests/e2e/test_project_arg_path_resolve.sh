#!/bin/sh
# Live E2E (#65): `project` given as an absolute path must resolve to the
# indexed project of the SAME git repo, even when the project was indexed
# through an alias symlink and the caller stands in a sibling worktree.
#
# Motivation (7-day MCP review, 2026-09-02→09-09): 50% of misses inside the
# daemon window were agents passing "/Users/.../run-solo-company" as `project`
# while the index was keyed on an alias — every query answered "project not
# found or not indexed" and the session fell back to grep. Separately the
# not-found hint listed 153 projects of which 145 had a deleted root, so the
# hint itself was noise.
#
# Everything here is real: a real git repo, a real `git worktree add`, a real
# alias symlink, a real index in an isolated CBM_CACHE_DIR, and the real
# `cli` entry point of the built binary. Nothing is mocked.
set -eu

BIN=${BIN:-./build/c/codebase-memory-mcp}
TMPDIR_ROOT=${TMPDIR:-/tmp}
CASE_DIR=$(mktemp -d "$TMPDIR_ROOT/cbm-projarg.XXXXXX")
chmod 700 "$CASE_DIR"
FAIL=0

note() { printf '[proj-arg] %s\n' "$1"; }
fail() { printf '[proj-arg] FAIL: %s\n' "$1" >&2; FAIL=1; }

cleanup() { rm -rf "$CASE_DIR"; }
trap cleanup EXIT INT TERM

export CBM_CACHE_DIR="$CASE_DIR/cache"
mkdir -p "$CBM_CACHE_DIR"

# Resolve BIN to an absolute path — the test cds around.
case "$BIN" in /*) ;; *) BIN="$(pwd)/$BIN" ;; esac
[ -x "$BIN" ] || { echo "binary not found: $BIN" >&2; exit 1; }

# ── 1. real repo + alias + worktree ─────────────────────────────────────
REPO="$CASE_DIR/repo"
mkdir -p "$REPO"
git -C "$REPO" init -q
git -C "$REPO" config user.email t@t.local
git -C "$REPO" config user.name t
cat > "$REPO/lib.py" <<'PY'
def marker_needle_fn():
    return 42
PY
git -C "$REPO" add lib.py
git -C "$REPO" commit -q -m init

ALIAS="$CASE_DIR/aliases/repo-active"
mkdir -p "$(dirname "$ALIAS")"
ln -s "$REPO" "$ALIAS"

WT="$CASE_DIR/wt-feature"
git -C "$REPO" worktree add -q "$WT" -b feature

# A second, unrelated repo that is NOT indexed → must stay not-found.
OTHER="$CASE_DIR/other"
mkdir -p "$OTHER"
git -C "$OTHER" init -q
echo 'x = 1' > "$OTHER/a.py"

# A stale project: indexed, then its root deleted → must NOT appear in hint.
STALE="$CASE_DIR/stale-repo"
mkdir -p "$STALE"
git -C "$STALE" init -q
echo 'def gone(): pass' > "$STALE/g.py"

# ── 2. index via alias path (what cbm-wt alias-init does) ──────────────
"$BIN" cli index_repository --repo-path "$ALIAS" --mode full >"$CASE_DIR/idx1.json" 2>"$CASE_DIR/idx1.err" \
    || { cat "$CASE_DIR/idx1.err" >&2; fail "index_repository via alias failed"; }
"$BIN" cli index_repository --repo-path "$STALE" --mode full >"$CASE_DIR/idx2.json" 2>"$CASE_DIR/idx2.err" \
    || { cat "$CASE_DIR/idx2.err" >&2; fail "index_repository stale repo failed"; }
rm -rf "$STALE"

LIST=$("$BIN" cli list_projects 2>/dev/null)
ALIAS_PROJ=$(printf '%s' "$LIST" | python3 -c '
import sys, json
d = json.load(sys.stdin)
for p in d.get("projects", []):
    if p.get("root_path", "").endswith("/repo") or p.get("root_path", "").endswith("repo-active"):
        print(p["name"]); break
')
[ -n "$ALIAS_PROJ" ] || fail "alias-indexed project missing from list_projects: $LIST"
note "alias project name = $ALIAS_PROJ"

search() { # $1 = project arg
    "$BIN" cli search_code --project "$1" --pattern marker_needle_fn 2>/dev/null
}
hits() { printf '%s' "$1" | python3 -c 'import sys,json
try:
    d=json.load(sys.stdin)
except Exception:
    print(-1); sys.exit()
print(d.get("total_grep_matches", -1) if "error" not in d else -1)'; }

# ── 3a. absolute root path of the alias target ─────────────────────────
OUT=$(search "$REPO")
N=$(hits "$OUT")
if [ "$N" -ge 1 ]; then note "OK (a) project=<abs root> → $N hit(s)"; else fail "(a) abs root not resolved: $OUT"; fi

# ── 3b. sibling worktree path of the same repo ─────────────────────────
OUT=$(search "$WT")
N=$(hits "$OUT")
if [ "$N" -ge 1 ]; then note "OK (b) project=<worktree path> → $N hit(s)"; else fail "(b) worktree path not resolved: $OUT"; fi

# ── 3b'. a subdirectory inside the worktree also resolves ─────────────
mkdir -p "$WT/sub/dir"
OUT=$(search "$WT/sub/dir")
N=$(hits "$OUT")
if [ "$N" -ge 1 ]; then note "OK (b') project=<subdir in worktree> → $N hit(s)"; else fail "(b') subdir not resolved: $OUT"; fi

# ── 3c. unrelated repo → still not found; hint excludes stale project ──
# The cli prints tool errors as JSON on stderr with rc=1 — capture both.
OUT=$("$BIN" cli search_code --project "$OTHER" --pattern marker_needle_fn 2>&1 | grep -v '^ts=' || true)
printf '%s' "$OUT" | grep -q 'not found or not indexed' \
    && note "OK (c) unrelated path still reports not-found" \
    || fail "(c) unrelated path did not report not-found: $OUT"
printf '%s' "$OUT" | grep -q 'stale-repo' \
    && fail "(c) hint still advertises stale project (root deleted): $OUT" \
    || note "OK (c) hint omits project whose root was deleted"
printf '%s' "$OUT" | grep -q "$ALIAS_PROJ" \
    && note "OK (c) hint still lists the live project" \
    || fail "(c) hint dropped the live project: $OUT"

# ── 3d. bare name path still works (regression guard) ──────────────────
OUT=$(search "$ALIAS_PROJ")
N=$(hits "$OUT")
if [ "$N" -ge 1 ]; then note "OK (d) project=<name> → $N hit(s)"; else fail "(d) name lookup regressed: $OUT"; fi

git -C "$REPO" worktree remove --force "$WT" 2>/dev/null || true

if [ "$FAIL" -ne 0 ]; then
    echo "[proj-arg] FAILED" >&2
    exit 1
fi
note "PASS"
