#!/usr/bin/env python3
"""近 N 小时 codebase-memory MCP 两个缺口指标（默认 24h）。

A_strict = A 去掉 hook 设计豁免后的真缺口（见 report 注释）：放行帧按线上 hook 重放，
    hook 自己也放行的（仓外/记忆根/≤3 文件页内精定位）不计；被拦后下一步是页内精读
    （sed -n/单文件 grep）不计，只有放弃或换工具继续跨文件扫（git grep/find/rg -r）才计。
A = 应该用但没用 MCP 的比例
    分母：代码检索意图 = grep 帧里判为 code_symbol 的（含被 hook 拦下的）+ MCP 调用数
    分子：code_symbol grep 放行执行的 + 被拦后没转 MCP 的（绕过/放弃）
B = 用了 MCP 但没拿到期望结果的比例
    分母：MCP 调用数
    分子：结果为 project_not_found/root_missing/error/timeout/empty/flood/
          snippet_ambiguous/no_output 的，加上命中后 15 分钟内又用同一关键词跨文件 grep 的

帧来源与去重复用 grep_corpus（三个会话根、fork 重放窗口、call_id 全局去重）。
分类是启发式，输出里附样例，方便人工抽查。
"""
import argparse, collections, json, os, re, sys
from datetime import datetime, timedelta, timezone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import grep_corpus as gc  # noqa: E402
import importlib.util as _ilu  # noqa: E402

_HOOK_PATH = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                          "hooks", "grep-intercept.py")
_spec = _ilu.spec_from_file_location("grep_intercept", _HOOK_PATH)
hook = _ilu.module_from_spec(_spec)
_spec.loader.exec_module(hook)


def hook_verdict(cmd, cwd):
    """用线上 hook 的同一套判定重放一条命令 → 'deny' / 'allow'。

    hook 放行的形态（仓外目标、记忆/会话根、≤3 个具体文件页内精定位、
    远端执行……）是设计上的豁免：索引不覆盖或 MCP 替代不了，不算「该用没用」。
    """
    import shlex
    env = hook.collect_assignments(cmd)
    for seg, head in hook.split_pipeline(cmd):
        if re.match(r"^cd(\s|$)", seg):
            try:
                ct = hook.strip_redirects(shlex.split(seg))
            except ValueError:
                ct = []
            ca = [t for t in ct[1:] if t != "--"]
            cwd = (hook.resolve_target(hook.expand_assigned(ca[0], env), cwd) or "") \
                if len(ca) == 1 else ""
            continue
        if hook.analyze_segment(seg, is_first=head, cwd=cwd, env=env) == "deny":
            return "deny"
    return "allow"


RESCAN_RE = re.compile(r"\bgit\s+grep\b|\bfind\s+\S+.*-name\b|\bag\b|\back\b|os\.walk|glob\.glob|\bfd\s")
PAGE_RE = re.compile(r"\b(sed\s+-n|nl\s|awk\s+'?NR|head\s|tail\s|cat\s)")


def classify_next(nxt_cmd, cwd):
    """被拦后下一条 shell 命令 → rescan（换工具继续跨文件扫 = 真绕过）/
    page_read（对具体文件页内读/单文件 grep = hook 提示里允许的精定位）/ other。"""
    if not nxt_cmd:
        return "other"
    if RESCAN_RE.search(nxt_cmd):
        return "rescan"
    if re.search(r"\b(grep|rg|egrep)\b", nxt_cmd):
        return "rescan" if hook_verdict(nxt_cmd, cwd) == "deny" else "page_read"
    if PAGE_RE.search(nxt_cmd):
        return "page_read"
    return "other"

SHELL = {"exec_command", "shell_command", "shell", "local_shell"}
BAD = ("project_not_found", "root_missing", "error", "timeout", "empty", "flood",
       "snippet_ambiguous", "no_output")


def text_of(out):
    if out is None:
        return ""
    if isinstance(out, str):
        return out
    if isinstance(out, dict):
        c = out.get("content", out)
        return text_of(c) if not isinstance(c, dict) else json.dumps(c, ensure_ascii=False)
    if isinstance(out, list):
        return "\n".join(text_of(x.get("text", x) if isinstance(x, dict) else x) for x in out)
    return str(out)


def mcp_result(tool, out):
    o = text_of(out)
    if not o:
        return "no_output", ""
    low = o.lower()[:3000]
    # Anchor failure shapes to the server's error envelope ({"error": "..."}),
    # not to substrings anywhere in the payload: a search that *hits* a symbol
    # named e.g. cbm_watcher_root_missing_errno was being scored as a miss.
    err = re.search(r'"error"\s*:\s*"([^"]{0,120})', low[:400])
    err = err.group(1) if err else ""
    if err.startswith("root_missing"):
        return "root_missing", o[:160]
    if "project" in err and "not found" in err:
        return "project_not_found", o[:200]
    # Timeout = transport/tool-call failure envelope or server error, never a
    # substring of the payload (a hit on DEFAULT_STREAM_IDLE_TIMEOUT_MS was
    # being scored as a timeout, #98).
    head = low[:400]
    if ("tool call error" in head or "tool call failed" in head or err) and \
            ("timed out" in head or "timeout" in err):
        return "timeout", o[:160]
    if isinstance(out, dict) and out.get("error"):
        return "error", o[:200]
    if low.startswith("error") or '"error"' in low[:120]:
        return "error", o[:200]
    if tool == "get_code_snippet":
        if "suggestion" in low or "ambiguous" in low or "did you mean" in low:
            return "snippet_ambiguous", o[:160]
        return ("hit" if len(o) > 300 else "empty"), f"len={len(o)}"
    m = re.search(r'"total(?:_grep_matches|_results)?"\s*:\s*(\d+)', o)
    n = int(m.group(1)) if m else None
    if n is None and re.search(r'"(results|nodes|matches)"\s*:\s*\[\s*\]', o):
        n = 0
    if n == 0:
        return "empty", ""
    if n is not None and n >= 200:
        return "flood", f"total={n}"
    if n is None:
        return ("hit" if len(o) > 300 else "empty"), f"len={len(o)}"
    return "hit", f"total={n}"


def idents(s):
    return {w.lower() for w in re.findall(r"[A-Za-z_][A-Za-z0-9_]{4,}", str(s))}


STOP = idents("grep search project limit pattern query include exclude users work solo "
              "src projects shells coder mode compact python3 print head tail sort")


def load_session(path, since, until, seen):
    """返回该会话的有序事件列表（shell 与 MCP 调用，带输出）。"""
    ev, outs = [], {}
    cwd, meta_ts, forked = "", None, False
    try:
        fh = open(path, errors="replace")
    except OSError:
        return [], ""
    with fh:
        for line in fh:
            if ('"function_call' not in line and '"session_meta"' not in line
                    and '"turn_context"' not in line and '"forked_history_ref"' not in line
                    and '"tool_search_call"' not in line):
                continue
            try:
                d = json.loads(line)
            except Exception:
                continue
            t = d.get("type")
            p = d.get("payload") or {}
            if t == "session_meta":
                cwd = p.get("cwd", "") or cwd
                meta_ts = gc.parse_ts(d.get("timestamp", ""))
                if "forked_history_ref" in line:
                    forked = True
                continue
            if t == "forked_history_ref" or "forked_history_ref" in line:
                forked = True
            if t == "turn_context":
                cwd = p.get("cwd", "") or cwd
                continue
            if t != "response_item" or not isinstance(p, dict):
                continue
            pt = p.get("type")
            if pt == "function_call_output":
                outs[p.get("call_id")] = p.get("output")
                continue
            if pt not in ("function_call", "tool_search_call"):
                continue
            ts = gc.parse_ts(d.get("timestamp", ""))
            if ts is None or ts < since or ts > until:
                continue
            if forked and meta_ts and abs((ts - meta_ts).total_seconds()) <= gc.REPLAY_BURST_S:
                continue
            cid = p.get("call_id")
            if cid:
                if cid in seen:
                    continue
                seen.add(cid)
            args = p.get("arguments")
            if isinstance(args, str):
                try:
                    args = json.loads(args)
                except Exception:
                    args = {"_raw": args}
            ev.append({"ts": ts, "cid": cid, "t": pt, "name": p.get("name") or pt,
                       "ns": p.get("namespace") or "", "args": args or {}, "cwd": cwd})
    for e in ev:
        e["out"] = outs.get(e["cid"])
    return ev, cwd


def cmd_of(e):
    a = e["args"]
    c = a.get("cmd") or a.get("command") or a.get("_raw") or ""
    return " ".join(map(str, c)) if isinstance(c, list) else str(c)


def is_mcp(e):
    return e["ns"].startswith("mcp__codebase") or e["name"].startswith("mcp__codebase")


def analyze(hours, until=None):
    now = until or datetime.now(timezone.utc)
    since = now - timedelta(hours=hours)
    seen = set()
    A_rows, deny_rows, mcp_rows = [], [], []
    for f in gc.iter_session_files(since):
        ev, _ = load_session(f, since, now, seen)
        sid = os.path.basename(f)[-14:-6]
        for i, e in enumerate(ev):
            if e["name"] in SHELL:
                c = cmd_of(e)
                if not re.search(r"\b(grep|rg|egrep|fgrep)\b", c):
                    continue
                fcwd = e["args"].get("workdir") or e["cwd"]
                bucket, reason, _ = gc.classify_intent(c, fcwd)
                if bucket != "code_symbol":
                    continue
                o = text_of(e["out"])
                blocked = "blocked by PreToolUse hook" in o[:400]
                nxt, nxt_cmd = "", ""
                if blocked:
                    nxt = "none"
                    for n in ev[i + 1:i + 6]:
                        if is_mcp(n) or n["t"] == "tool_search_call":
                            nxt = "mcp"
                            break
                        if n["name"] in SHELL:
                            if "blocked by PreToolUse" in text_of(n["out"])[:400]:
                                continue
                            nxt = "shell_bypass"
                            nxt_cmd = cmd_of(n)[:600]
                            break
                row = {"sid": sid, "ts": e["ts"], "cwd": fcwd, "cmd": c[:4000],
                       "reason": reason, "blocked": blocked, "next": nxt, "next_cmd": nxt_cmd,
                       "exempt": (not blocked) and hook_verdict(c, fcwd) == "allow",
                       "next_kind": classify_next(nxt_cmd, fcwd) if nxt == "shell_bypass" else nxt}
                (deny_rows if blocked else A_rows).append(row)
            elif is_mcp(e):
                tool = e["name"].split("__")[-1]
                k, d = mcp_result(tool, e["out"])
                a = e["args"]
                q = (a.get("pattern") or a.get("query") or a.get("qualified_name")
                     or a.get("function_name") or a.get("name_pattern") or "")
                if k == "hit":
                    qt = idents(q) - STOP
                    for n in ev[i + 1:i + 8]:
                        if (n["ts"] - e["ts"]).total_seconds() > 900:
                            break
                        if n["name"] in SHELL:
                            nc = cmd_of(n)
                            if (qt & idents(nc) and re.search(r"\b(rg|grep)\b", nc)
                                    and "blocked by PreToolUse" not in text_of(n["out"])[:400]
                                    and gc.classify_intent(nc, n["cwd"])[0] == "code_symbol"):
                                k, d = "grep_after_hit", nc[:120]
                                break
                mcp_rows.append({"sid": sid, "ts": e["ts"], "tool": tool, "k": k, "d": d,
                                 "project": a.get("project"), "q": str(q)[:80]})
    return since, now, A_rows, deny_rows, mcp_rows


def report(since, now, A_rows, deny_rows, mcp_rows, samples=8):
    tz8 = timezone(timedelta(hours=8))
    n_mcp = len(mcp_rows)
    miss_deny = [r for r in deny_rows if r["next"] != "mcp"]
    a_num = len(A_rows) + len(miss_deny)
    a_den = len(A_rows) + len(deny_rows) + n_mcp
    kc = collections.Counter(r["k"] for r in mcp_rows)
    b_num = sum(v for k, v in kc.items() if k != "hit")
    pct = lambda a, b: f"{100 * a / b:.1f}%" if b else "n/a"  # noqa: E731
    # A_strict：剔除 hook 设计豁免的放行（仓外/记忆根/页内精定位），分子只留
    # 「非豁免放行」+「被拦后放弃（none）或换工具继续跨文件扫（rescan）」。
    # page_read / other 是 hook 提示允许的精定位或与目标无关的下一步，不计缺口。
    a_nonexempt = [r for r in A_rows if not r["exempt"]]
    s_miss = [r for r in deny_rows if r["next_kind"] in ("rescan", "none")]
    s_num = len(a_nonexempt) + len(s_miss)
    s_den = len(a_nonexempt) + len(deny_rows) + n_mcp
    out = {
        "window_bj": f"{since.astimezone(tz8):%m-%d %H:%M} → {now.astimezone(tz8):%m-%d %H:%M}",
        "A": {"pct": pct(a_num, a_den), "num": a_num, "den": a_den,
              "grep_allowed": len(A_rows), "denied": len(deny_rows),
              "denied_then_not_mcp": len(miss_deny), "mcp_calls": n_mcp,
              "allowed_reason": dict(collections.Counter(r["reason"] for r in A_rows).most_common(10)),
              "deny_next": dict(collections.Counter(r["next"] for r in deny_rows))},
        "A_strict": {"pct": pct(s_num, s_den), "num": s_num, "den": s_den,
                     "allowed_exempt_by_hook": len(A_rows) - len(a_nonexempt),
                     "allowed_nonexempt": len(a_nonexempt),
                     "deny_next_kind": dict(collections.Counter(r["next_kind"] for r in deny_rows))},
        "B": {"pct": pct(b_num, n_mcp), "num": b_num, "den": n_mcp,
              "by_kind": dict(kc.most_common()),
              "by_tool": {t: dict(collections.Counter(r["k"] for r in mcp_rows if r["tool"] == t))
                          for t in sorted({r["tool"] for r in mcp_rows})}},
    }
    print(json.dumps(out, ensure_ascii=False, indent=1))
    print("\n-- A 样例（放行的 code_symbol grep）")
    for r in A_rows[-samples:]:
        print(f"  {r['ts'].astimezone(tz8):%m-%d %H:%M} {r['sid']} [{r['reason']}] {r['cmd'][:150]!r}")
    print("-- A_strict 样例（非豁免放行 / 被拦后放弃或换工具重扫）")
    for r in (a_nonexempt + s_miss)[-samples:]:
        print(f"  {r['ts'].astimezone(tz8):%m-%d %H:%M} {r['sid']} kind={r.get('next_kind') or 'allowed'} "
              f"{r['cmd'][:110]!r} -> {r['next_cmd'][:110]!r}")
    print("-- A 样例（被拦后没转 MCP）")
    for r in miss_deny[-samples:]:
        print(f"  {r['ts'].astimezone(tz8):%m-%d %H:%M} {r['sid']} next={r['next']} {r['cmd'][:150]!r}")
    for k in BAD + ("grep_after_hit",):
        rs = [r for r in mcp_rows if r["k"] == k]
        if rs:
            print(f"-- B {k} ({len(rs)})")
            for r in rs[-samples:]:
                print(f"  {r['ts'].astimezone(tz8):%m-%d %H:%M} {r['sid']} {r['tool']} "
                      f"project={r['project']} q={r['q']!r} {str(r['d'])[:110]!r}")
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--hours", type=float, default=24)
    ap.add_argument("--json-out", help="把汇总追加写入 JSONL（用于 24h 迭代对比）")
    args = ap.parse_args()
    res = analyze(args.hours)
    out = report(*res)
    if args.json_out:
        out["generated_utc"] = res[1].isoformat()
        with open(args.json_out, "a", encoding="utf-8") as f:
            f.write(json.dumps(out, ensure_ascii=False) + "\n")


if __name__ == "__main__":
    main()
