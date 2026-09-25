#!/usr/bin/env python3
"""mcp-gap-24h 的 A_strict 判定单测：hook 豁免重放 + 被拦后下一步分类。"""
import importlib.util, os, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("gap", os.path.join(HERE, "mcp-gap-24h.py"))
g = importlib.util.module_from_spec(spec)
spec.loader.exec_module(g)

REPO = os.path.dirname(os.path.dirname(HERE))
OUT = tempfile.mkdtemp(prefix="gap-outside-")  # 不在任何 git 仓内

CASES = [
    # (fn, args, expect)
    (g.hook_verdict, ("grep -rn setStatus src/", REPO), "deny"),
    (g.hook_verdict, ("grep -n -A4 esweb168 ~/.ssh/config", REPO), "allow"),
    (g.hook_verdict, (f"cd {OUT} && grep -n 'def query' a.py", REPO), "allow"),
    (g.hook_verdict, ("grep -n handleTool src/mcp/mcp.c", REPO), "allow"),
    (g.classify_next, ("sed -n 560,612p src/pipeline/pipeline_incremental.c", REPO), "page_read"),
    (g.classify_next, ("grep -n hasParent /tmp/x.ts | head", REPO), "page_read"),
    (g.classify_next, ("git grep -n setStatus", REPO), "rescan"),
    (g.classify_next, ("grep -rn setStatus src/", REPO), "rescan"),
    (g.classify_next, ("find . -name '*.go' | xargs grep -l Foo", REPO), "rescan"),
    (g.classify_next, ("git show 116ed742 -- scripts/dev/ci-run", REPO), "other"),
    (g.classify_next, ("", REPO), "other"),
]

CASES += [
    (g.mcp_result, ("search_code", '{"results":[{"node":"stream_idle_timeout","qualified_name":"x"}],"total_grep_matches":3}'), ("hit", "total=3")),
    (g.mcp_result, ("index_repository", "tool call error: tool call failed for `codebase-memory/index_repository`\n\nCaused by: timed out awaiting tools/call after 300s"), None),
]


def _kind(fn, args):
    r = fn(*args)
    return r[0] if fn is g.mcp_result else r


fail = 0
for fn, args, want in CASES:
    got = fn(*args)
    if fn is g.mcp_result:
        got, want = got[0], (want[0] if want else "timeout")
    ok = got == want
    fail += not ok
    print(f"[{'ok' if ok else 'FAIL'}] {fn.__name__}{args[:1]} -> {got} (want {want})")
print(f"{len(CASES) - fail}/{len(CASES)} passed")
sys.exit(1 if fail else 0)
