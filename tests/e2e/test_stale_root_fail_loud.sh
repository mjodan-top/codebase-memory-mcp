#!/bin/sh
# Live E2E (#65): a project whose indexed root directory has been deleted must
# fail LOUD, not return a clean empty result.
#
# Background: the 2026-09 seven-day review found that a third of the MCP misses
# in the outage window were queries against an alias whose worktree had been
# removed. index_status still said "ready", search_code returned total=0 — byte
# identical to "the code has no such symbol" — and the agent concluded the
# symbol did not exist and fell back to grep.
#
# Everything here is real: a real git repo, a real index, a real rm -rf of the
# root, and the real CLI entry (`codebase-memory-mcp cli <tool> <json>`) which
# runs the same tool handlers the MCP server does, against an isolated cache.
set -eu

BIN=${BIN:-./build/c/codebase-memory-mcp}
case "$BIN" in /*) ;; *) BIN="$(pwd)/$BIN" ;; esac

CASE_DIR=$(mktemp -d "/tmp/cbm-stale-e2e.XXXXXX")
chmod 700 "$CASE_DIR"
FAIL=0

note() { printf '[stale-e2e] %s\n' "$1"; }
fail() { printf '[stale-e2e] FAIL: %s\n' "$1" >&2; FAIL=1; }
cleanup() { rm -rf "$CASE_DIR"; }
trap cleanup EXIT INT TERM

# Isolated cache so the test never sees (or touches) the developer's projects.
export CBM_CACHE_DIR="$CASE_DIR/cache"
mkdir -p "$CBM_CACHE_DIR"
export HOME="$CASE_DIR/home"
mkdir -p "$HOME"

REPO="$CASE_DIR/repo"
mkdir -p "$REPO"
# The indexer records the resolved path (/tmp -> /private/tmp on macOS).
REPO_REAL=$(cd "$REPO" && pwd -P)
cat > "$REPO/hello.py" <<'PY'
def greet_stale_root():
    return "hello"
PY
git -C "$REPO" init -q
git -C "$REPO" -c user.email=e2e@example.com -c user.name=e2e add -A
git -C "$REPO" -c user.email=e2e@example.com -c user.name=e2e commit -qm init

# The CLI prints isError results on stderr and normal results on stdout; the
# assertions below look at both, with the info-log lines stripped.
cli() { "$BIN" cli "$@" 2>&1 | grep -v '^ts=' | grep -v '^warning:' || true; }

OUT=$(cli index_repository "{\"repo_path\":\"$REPO\",\"mode\":\"full\"}")
PROJECT=$(printf '%s' "$OUT" | sed -n 's/.*"project":"\([^"]*\)".*/\1/p' | head -1)
[ -n "$PROJECT" ] || { fail "index_repository did not return a project name: $OUT"; exit 1; }
note "indexed project=$PROJECT"

# Sanity: live root, real hit.
OUT=$(cli search_code "{\"project\":\"$PROJECT\",\"pattern\":\"greet_stale_root\"}")
case "$OUT" in
    *root_missing*) fail "live root wrongly reported root_missing: $OUT" ;;
    *'"total_grep_matches":0'*) fail "live root returned 0 matches: $OUT" ;;
    *) note "live root: search_code hits" ;;
esac

OUT=$(cli list_projects '{}')
case "$OUT" in
    *"\"name\":\"$PROJECT\""*) note "live root: listed by default" ;;
    *) fail "live root not listed by default: $OUT" ;;
esac
case "$OUT" in
    *'"stale_count":0'*) ;;
    *) fail "live root: stale_count should be 0: $OUT" ;;
esac

# The regression: delete the tree the index describes.
rm -rf "$REPO"
note "root deleted"

for tool_and_args in \
    "search_code {\"project\":\"$PROJECT\",\"pattern\":\"greet_stale_root\"}" \
    "search_graph {\"project\":\"$PROJECT\",\"pattern\":\"greet_stale_root\"}" \
    "get_code_snippet {\"project\":\"$PROJECT\",\"qualified_name\":\"greet_stale_root\"}" \
    "query_graph {\"project\":\"$PROJECT\",\"query\":\"MATCH (n) RETURN n LIMIT 1\"}" \
    "get_architecture {\"project\":\"$PROJECT\"}" \
    "detect_changes {\"project\":\"$PROJECT\"}"
do
    tool=${tool_and_args%% *}
    args=${tool_and_args#* }
    OUT=$(cli "$tool" "$args" || true)
    case "$OUT" in
        *'"error":"root_missing"'*)
            case "$OUT" in
                *"\"root_path\":\"$REPO_REAL\""*) note "$tool -> root_missing (root_path echoed)" ;;
                *) fail "$tool: root_missing without the root_path: $OUT" ;;
            esac ;;
        *) fail "$tool did not fail loud after root deletion: $OUT" ;;
    esac
done

OUT=$(cli index_status "{\"project\":\"$PROJECT\"}")
case "$OUT" in
    *'"status":"root_missing"'*) note "index_status -> status=root_missing" ;;
    *) fail "index_status still claims ready/empty: $OUT" ;;
esac

OUT=$(cli list_projects '{}')
case "$OUT" in
    *"\"name\":\"$PROJECT\""*) fail "stale project still listed by default: $OUT" ;;
    *) note "list_projects default hides the stale project" ;;
esac
case "$OUT" in
    *'"stale_count":1'*) note "list_projects stale_count=1" ;;
    *) fail "list_projects stale_count != 1: $OUT" ;;
esac
case "$OUT" in
    *stale_hint*) ;;
    *) fail "list_projects missing stale_hint: $OUT" ;;
esac

OUT=$(cli list_projects '{"include_stale":true}')
case "$OUT" in
    *"\"name\":\"$PROJECT\""*) note "list_projects include_stale=true shows it" ;;
    *) fail "include_stale=true did not list the stale project: $OUT" ;;
esac

[ "$FAIL" -eq 0 ] || exit 1
note "PASS"
