#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""codebase-memory-mcp 采用度统计：代码定位替代率 S / 命令级代理 s。

口径来源：/tmp/mcp-adoption-metrics-design.html（2026-07-22 设计稿 §1.2/§1.3）。
只读数据源：
  1. daemon 日志 ~/Library/Logs/codebase-memory-mcp/daemon.err（ts= 行）
  2. 会话 jsonl ~/.codex/sessions/**.jsonl 与 ~/.coder/sessions/**.jsonl（同名文件去重）

分类规则（机械可执行）：
  - MCP 定位：tools/call 且 tool ∈ SEARCH_TOOLS；jsonl 侧同名工具帧；
    exec 里的 `codebase-memory-mcp cli <搜索类子命令>` 直调。
  - legacy 扫射：grep/rg 跨文件（-r / glob / 多文件 / 目录目标）且 pattern 像标识符、
    目标在代码目录；sed 同类跨文件扫。
  - 豁免（两边都不计）：页内精定位（单文件 grep / sed -n 'X,Yp'）；
    非代码文本目标（.log/.toml/.json/.md/...）；握手管理面
    （list_projects/index_status/tools/list/initialize）；管道过滤 `| grep`。
  - hook 拦截（2026-07-23 新增）：function_call_output 含
    "blocked by PreToolUse hook" 的 legacy 尝试帧＝被拦、未真执行，
    单列 legacy_denied 桶，不计 s/S 的 legacy 分母（denied 是纪律在起效，
    不是 agent 真跑了 grep）。

事件级 S：同会话内相邻检索调用间隔 ≤120s 归并为一个事件，
按事件内首个跨文件定位动作判 MCP 主导 / legacy 主导。
"""
import argparse
import glob
import json
import os
import re
import shlex
import sys
from collections import Counter, defaultdict
from datetime import datetime, timedelta, timezone

# ---------------- 口径常量（改口径先改设计稿再改这里） ----------------
SEARCH_TOOLS = {"search_graph", "search_code", "get_code_snippet",
                "trace_path", "get_architecture"}
MGMT_TOOLS = {"list_projects", "index_status", "index_repository",
              "tools/list", "initialize"}
# jsonl 侧 MCP 工具帧可能的命名形态：裸名 / mcp__ 前缀 / 含 codebase
MCP_FRAME_RE = re.compile(r"(?:^|__)(search_graph|search_code|get_code_snippet|"
                          r"trace_path|get_architecture|list_projects|index_status|"
                          r"index_repository|delete_project|list_family_snapshots|"
                          r"ingest_traces|manage_adr|get_graph_schema|get_tool_schema)$")
CLI_SEARCH_RE = re.compile(r"codebase-memory-mcp\s+cli\s+(search_graph|search_code|"
                           r"get_code_snippet|trace_path|get_architecture)")
CLI_ANY_RE = re.compile(r"codebase-memory-mcp\s+cli\s+(\S+)")

NONCODE_EXT = {".log", ".toml", ".json", ".jsonl", ".md", ".txt", ".yaml", ".yml",
               ".lock", ".err", ".out", ".csv", ".html", ".plist", ".cfg", ".ini",
               ".service", ".env"}
NONCODE_PATH_HINTS = ("daemon.err", "/Library/Logs/", "/sessions/", "/tmp/",
                      "/var/log", ".codex", ".coder", "AGENTS.md", "MEMORY.md",
                      "/memory/", "node_modules")
CODE_EXT = {".c", ".h", ".cc", ".cpp", ".hpp", ".rs", ".go", ".py", ".mjs", ".js",
            ".ts", ".tsx", ".jsx", ".java", ".rb", ".sh", ".swift", ".m", ".kt",
            ".cs", ".php", ".lua", ".zig", ".sql"}
EPISODE_GAP_S = 120  # 事件归并窗口（设计稿 §1.1）

# grep/rg 里"pattern 像标识符"：≥3 个 word 字符起步（可含 | 交替、\b 等）
IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]{2,}")

WORD_SPLIT_RE = re.compile(r"(?<![|&])[;\n]|&&|\|\||(?<!\|)\|(?!\|)")


def parse_ts(s):
    try:
        return datetime.fromisoformat(s.replace("Z", "+00:00"))
    except Exception:
        return None


# ---------------- daemon.err ----------------
DAEMON_KV_RE = re.compile(r"(\w+)=(\S+)")


def parse_daemon(path, since):
    rows = []
    if not os.path.exists(path):
        return rows
    for line in open(path, errors="replace"):
        if "msg=mcp.request" not in line or "ts=" not in line:
            continue
        kv = dict(DAEMON_KV_RE.findall(line))
        ts = parse_ts(kv.get("ts", ""))
        if ts is None or ts < since:
            continue
        rows.append({
            "ts": ts,
            "method": kv.get("method", ""),
            "tool": kv.get("tool", ""),
            "project": kv.get("project", ""),
            "status": kv.get("status", ""),
            "duration_ms": int(kv.get("duration_ms", "0") or 0),
        })
    return rows


# ---------------- 命令分类 ----------------
def _tokens(seg):
    try:
        return shlex.split(seg, posix=True)
    except ValueError:
        return seg.split()


def _has_glob(tok):
    return any(ch in tok for ch in "*?[")


# 重定向 token：`2>/dev/null`、`>x`、`2>&1`、`<x` 不是文件参数；
# 裸 `>` / `2>` / `<` 的目标是下一个 token，一并跳过。
_REDIR_RE = re.compile(r"^\d*(?:>>|>|<)\S*$")


def _strip_redirects(toks):
    """剥离重定向 token（含裸符号+下一个目标 token），其余原样保序返回。

    >>> _strip_redirects(["grep", "-n", "close", "store.mjs", "2>/dev/null"])
    ['grep', '-n', 'close', 'store.mjs']
    >>> _strip_redirects(["grep", "foo", "f.md", ">", "out.txt"])
    ['grep', 'foo', 'f.md']
    >>> _strip_redirects(["grep", "foo", "f.py", "2>&1"])
    ['grep', 'foo', 'f.py']
    """
    out = []
    skip = False
    for t in toks:
        if skip:
            skip = False
            continue
        if _REDIR_RE.match(t):
            if re.match(r"^\d*(?:>>|>|<)$", t):
                skip = True
            continue
        out.append(t)
    return out


def _path_kind(tok, cwd=""):
    """返回 'code' / 'noncode' / 'dir' / 'unknown'。"""
    t = tok.rstrip("/")
    low = tok.lower()
    if any(h.lower() in low for h in NONCODE_PATH_HINTS):
        return "noncode"
    base = os.path.basename(t)
    _, ext = os.path.splitext(base)
    if ext in NONCODE_EXT:
        return "noncode"
    if ext in CODE_EXT:
        return "code"
    if ext == "" or tok.endswith("/"):
        return "dir"
    return "unknown"


GREP_NOFILE_OK = {"-e", "--regexp", "-f", "--file", "-m", "--max-count", "-A", "-B",
                  "-C", "-g", "--glob", "--iglob", "-t", "--type", "-T", "--type-not",
                  "--include", "--exclude", "--exclude-dir", "--color", "-d"}


def classify_search_cmd(seg, piped_before):
    """对一段（已按 ; && || | 切开的）命令分类。
    返回 (kind, detail) ；kind ∈ {legacy, exempt_page, exempt_noncode,
    exempt_filter, sed_page, none}"""
    toks = _strip_redirects(_tokens(seg))
    if not toks:
        return ("none", "")
    # 跳过 env 赋值 / timeout / cd x && 已切开
    i = 0
    while i < len(toks) and ("=" in toks[i] and not toks[i].startswith("-")):
        i += 1
    while i < len(toks) and toks[i] in ("timeout", "command", "nice", "xargs", "sudo"):
        i += 1
        if i < len(toks) and toks[i - 1] == "timeout" and re.match(r"^\d", toks[i]):
            i += 1
    if i >= len(toks):
        return ("none", "")
    prog = os.path.basename(toks[i])
    args = toks[i + 1:]
    if prog not in ("grep", "egrep", "fgrep", "rg", "sed"):
        return ("none", "")

    if prog == "sed":
        files = [a for a in args if not a.startswith("-")
                 and not re.match(r"^-?\d*[,\d]*[pd]$", a)
                 and not re.search(r"^['\"]?\d+(,\d+)?p", a)
                 and "/" in a or ("." in a and not a.startswith("-") and len(a) > 2 and not re.match(r"^\d", a))]
        files = [a for a in args if ("/" in a or "." in a)
                 and not a.startswith("-") and not re.match(r"^\d+(,\$?\d*)?[pd]?$", a)
                 and not re.match(r"^['\"]", a) and not re.search(r"[sy]/", a)]
        code_files = [f for f in files if _path_kind(f) == "code"]
        if len(code_files) >= 2 or any(_has_glob(f) for f in code_files):
            return ("legacy", "sed 跨文件: " + seg.strip()[:120])
        return ("sed_page", seg.strip()[:120])

    # grep / rg
    recursive = any(a in ("-r", "-R", "--recursive") for a in args) or prog == "rg"
    is_filter = piped_before  # `... | grep x`：过滤器
    pattern = None
    files = []
    skip_next = False
    for j, a in enumerate(args):
        if skip_next:
            skip_next = False
            continue
        if a.startswith("-"):
            if a in GREP_NOFILE_OK:
                skip_next = True
            continue
        if pattern is None:
            pattern = a
        else:
            files.append(a)
    if is_filter and not files:
        return ("exempt_filter", seg.strip()[:120])
    if pattern is None or not IDENT_RE.search(pattern):
        return ("none", "")
    kinds = [_path_kind(f) for f in files]
    if files and all(k == "noncode" for k in kinds):
        return ("exempt_noncode", seg.strip()[:120])
    multi = len(files) >= 2 or any(_has_glob(f) for f in files) or any(k == "dir" for k in kinds)
    if prog == "rg" and not files:
        # rg 默认递归当前目录
        multi = True
    if recursive or multi:
        # 目标含代码目录/代码文件才算 legacy；混合目标按 legacy 从严
        if files and all(k == "noncode" for k in kinds):
            return ("exempt_noncode", seg.strip()[:120])
        return ("legacy", seg.strip()[:160])
    if len(files) == 1:
        k = kinds[0]
        if k == "noncode":
            return ("exempt_noncode", seg.strip()[:120])
        return ("exempt_page", seg.strip()[:120])
    return ("none", "")


def classify_exec(cmd):
    """把一条 exec 命令串切段分类，返回 [(kind, detail), ...]（不含 none）。

    FP 修复回归（重定向剥离 / $VAR / glob-noncode / 带值标志）：

    >>> classify_exec("grep -n close services/matter-service/store.mjs 2>/dev/null")[0][0]
    'exempt_page'
    >>> classify_exec("grep -i -h postreview $MEM/MEMORY.md 2>/dev/null")[0][0]
    'exempt_noncode'
    >>> classify_exec("grep -l -i postreview $MEM/*.md 2>/dev/null")[0][0]
    'exempt_noncode'
    >>> classify_exec('grep -n "NONCODE_PATH_HINTS" -A 6 scripts/metrics/mcp-adoption.py')[0][0]
    'exempt_page'
    >>> classify_exec("grep -rn setStatus src/ 2>/dev/null")[0][0]
    'legacy'
    >>> classify_exec("rg -n GoalBudget")[0][0]
    'legacy'
    """
    out = []
    m = CLI_SEARCH_RE.search(cmd)
    if m:
        out.append(("mcp_cli", m.group(0)))
    elif CLI_ANY_RE.search(cmd):
        out.append(("mcp_cli_mgmt", CLI_ANY_RE.search(cmd).group(0)))
    # 按分隔符切段，记录每段前是否是管道
    segs = []
    buf, prev_sep = "", ""
    i, n = 0, len(cmd)
    while i < n:
        two = cmd[i:i + 2]
        ch = cmd[i]
        if two in ("&&", "||"):
            segs.append((buf, prev_sep)); buf, prev_sep = "", two; i += 2; continue
        if ch in ";\n":
            segs.append((buf, prev_sep)); buf, prev_sep = "", ch; i += 1; continue
        if ch == "|":
            segs.append((buf, prev_sep)); buf, prev_sep = "", "|"; i += 1; continue
        buf += ch; i += 1
    segs.append((buf, prev_sep))
    for seg, sep in segs:
        if not seg.strip():
            continue
        kind, detail = classify_search_cmd(seg, piped_before=(sep == "|"))
        if kind != "none":
            out.append((kind, detail))
    return out


# ---------------- 会话解析 ----------------
def iter_session_files(roots, since):
    seen = set()
    out = []
    for root in roots:
        for f in glob.glob(os.path.join(root, "*", "*", "*", "*.jsonl")):
            base = os.path.basename(f)
            if base in seen:
                continue
            if os.path.getmtime(f) < since.timestamp():
                continue
            seen.add(base)
            out.append(f)
    # 按会话开始时间（文件名内嵌）升序：保证 call_id 去重时保留原始帧的真实时间戳
    out.sort(key=os.path.basename)
    return out


REPLAY_BURST_S = 10  # fork/clone 创建时会把父历史以创建时刻整体重放写入，需跳过


def parse_session(path, since, until, seen_call_ids):
    """返回该会话的检索调用时间线与计数。

    坑（2026-07-22 实测）：fork/clone 会话把父历史重放写入自身 jsonl，
    重放帧共享 fork 创建时刻的时间戳 → 不跳过会把父历史的 grep 按分身数
    重复计入。两道防线：① forked 文件跳过创建时刻 ±REPLAY_BURST_S 的帧；
    ② 跨文件按 call_id 全局去重（seen_call_ids，按会话开始时间升序喂入）。
    """
    calls = []       # (ts, kind, detail)  kind: mcp/legacy
    counters = Counter()
    cwd = ""
    sid = os.path.basename(path)
    meta_ts = None
    forked = False
    pending_legacy = {}   # call_id -> [calls 下标]，等 output 判 hook 拦截
    denied_idx = set()    # 被 hook 拦截的 calls 下标（终局剔除）
    for line in open(path, errors="replace"):
        if '"function_call"' not in line and '"function_call_output"' not in line \
           and '"turn_context"' not in line \
           and '"session_meta"' not in line and '"forked_history_ref"' not in line:
            continue
        try:
            d = json.loads(line)
        except Exception:
            continue
        t = d.get("type")
        if t == "session_meta":
            cwd = d.get("payload", {}).get("cwd", "") or cwd
            meta_ts = parse_ts(d.get("timestamp", ""))
            continue
        if t == "forked_history_ref":
            forked = True
            continue
        if t == "turn_context":
            cwd = d.get("payload", {}).get("cwd", "") or cwd
            continue
        if t != "response_item":
            continue
        p = d.get("payload", {})
        if p.get("type") == "function_call_output":
            cid = p.get("call_id")
            idxs = pending_legacy.pop(cid, None)
            if idxs and "blocked by PreToolUse hook" in str(p.get("output", ""))[:400]:
                counters["legacy_denied"] += len(idxs)
                counters["legacy"] -= len(idxs)
                denied_idx.update(idxs)
            continue
        if p.get("type") != "function_call":
            continue
        ts = parse_ts(d.get("timestamp", ""))
        if ts is None or ts < since or ts > until:
            continue
        if forked and meta_ts is not None \
           and abs((ts - meta_ts).total_seconds()) <= REPLAY_BURST_S:
            counters["skipped_replay"] += 1
            continue
        cid = p.get("call_id")
        if cid:
            if cid in seen_call_ids:
                counters["skipped_dup"] += 1
                continue
            seen_call_ids.add(cid)
        name = p.get("name", "")
        if MCP_FRAME_RE.search(name) or "codebase" in name.lower():
            bare = name.split("__")[-1]
            if bare in SEARCH_TOOLS:
                counters["mcp_frame"] += 1
                calls.append((ts, "mcp", f"[MCP工具帧] {bare}"))
            else:
                counters["mcp_mgmt_frame"] += 1
            continue
        if name == "tool_search":
            counters["tool_search"] += 1
            continue
        if name not in ("exec_command", "shell_command"):
            continue
        try:
            a = json.loads(p.get("arguments", "") or "{}")
        except Exception:
            continue
        cmd = a.get("cmd") or a.get("command") or ""
        if not cmd:
            continue
        for kind, detail in classify_exec(cmd):
            if kind == "mcp_cli":
                counters["mcp_cli"] += 1
                calls.append((ts, "mcp", f"[cli直调] {detail}"))
            elif kind == "mcp_cli_mgmt":
                counters["mcp_cli_mgmt"] += 1
            elif kind == "legacy":
                counters["legacy"] += 1
                calls.append((ts, "legacy", detail))
                if cid:
                    pending_legacy.setdefault(cid, []).append(len(calls) - 1)
            else:
                counters[kind] += 1
    if denied_idx:
        calls = [c for i, c in enumerate(calls) if i not in denied_idx]
    return {"sid": sid, "cwd": cwd, "calls": sorted(calls), "counters": counters}


# ---------------- 事件归并 ----------------
def build_episodes(calls):
    eps = []
    cur = []
    for c in calls:
        if cur and (c[0] - cur[-1][0]).total_seconds() > EPISODE_GAP_S:
            eps.append(cur)
            cur = []
        cur.append(c)
    if cur:
        eps.append(cur)
    return eps


def pctl(xs, q):
    if not xs:
        return 0
    xs = sorted(xs)
    k = max(0, min(len(xs) - 1, int(round(q * (len(xs) - 1)))))
    return xs[k]


def main():
    ap = argparse.ArgumentParser(description="codebase-memory-mcp 采用度统计")
    ap.add_argument("--hours", type=float, default=24)
    ap.add_argument("--daemon-log", default=os.path.expanduser(
        "~/Library/Logs/codebase-memory-mcp/daemon.err"))
    ap.add_argument("--sessions-root", action="append", default=None,
                    help="可多次；默认 ~/.codex/sessions 与 ~/.coder/sessions")
    ap.add_argument("--samples", type=int, default=5, help="事件级归并样例数")
    ap.add_argument("--json", action="store_true", help="输出机器可读 JSON")
    args = ap.parse_args()

    until = datetime.now(timezone.utc)
    since = until - timedelta(hours=args.hours)
    roots = args.sessions_root or [os.path.expanduser("~/.codex/sessions"),
                                   os.path.expanduser("~/.coder/sessions")]

    # 1) daemon 侧
    drows = parse_daemon(args.daemon_log, since)
    d_search = [r for r in drows if r["method"] == "tools/call"
                and r["tool"] in SEARCH_TOOLS]
    d_mgmt = [r for r in drows if r["method"] in ("initialize", "tools/list")
              or r["tool"] in MGMT_TOOLS]
    d_other = [r for r in drows if r not in d_search and r not in d_mgmt]
    d_err = [r for r in d_search if r["status"] != "ok"]

    # 2) 会话侧
    sessions = []
    seen_call_ids = set()
    for f in iter_session_files(roots, since):
        s = parse_session(f, since, until, seen_call_ids)
        if s["calls"] or s["counters"]:
            sessions.append(s)

    N_mcp_jsonl = sum(s["counters"]["mcp_frame"] + s["counters"]["mcp_cli"]
                      for s in sessions)
    N_legacy = sum(s["counters"]["legacy"] for s in sessions)
    N_denied = sum(s["counters"]["legacy_denied"] for s in sessions)
    # 命令级 s：分子 = daemon 检索 tools/call + cli 直调（cli 不落 daemon 日志）
    n_cli = sum(s["counters"]["mcp_cli"] for s in sessions)
    N_mcp = len(d_search) + n_cli
    s_val = N_mcp / (N_mcp + N_legacy) if (N_mcp + N_legacy) else None

    # 按 project（cwd basename）/ 按会话
    by_proj = defaultdict(lambda: Counter())
    for s in sessions:
        proj = os.path.basename(s["cwd"].rstrip("/")) or "?"
        c = s["counters"]
        by_proj[proj]["mcp"] += c["mcp_frame"] + c["mcp_cli"]
        by_proj[proj]["legacy"] += c["legacy"]

    # slot 加载成功率：只对"发生过代码定位活动"的会话统计
    active = [s for s in sessions if s["calls"]]
    slot_loaded = [s for s in active
                   if s["counters"]["mcp_frame"] or s["counters"]["mcp_cli"]
                   or s["counters"]["mcp_mgmt_frame"]]

    # 事件级 S
    all_eps = []
    for s in sessions:
        for ep in build_episodes(s["calls"]):
            first = ep[0][1]
            all_eps.append({"sid": s["sid"], "cwd": s["cwd"], "n": len(ep),
                            "lead": first, "calls": ep,
                            "fallback": first == "mcp" and any(c[1] == "legacy" for c in ep)})
    E_mcp = sum(1 for e in all_eps if e["lead"] == "mcp")
    E_leg = sum(1 for e in all_eps if e["lead"] == "legacy")
    S_val = E_mcp / (E_mcp + E_leg) if (E_mcp + E_leg) else None

    # 根因桶
    def bucket(s):
        c = s["counters"]
        has_mcp = c["mcp_frame"] or c["mcp_cli"] or c["mcp_mgmt_frame"]
        if c["legacy"] and not has_mcp:
            return "惯性未拉帧"
        if c["legacy"] and has_mcp:
            return "零命中回退/混用"
        return "-"

    top_legacy = sorted(sessions, key=lambda s: -s["counters"]["legacy"])[:8]
    top_legacy = [s for s in top_legacy if s["counters"]["legacy"]]

    report = {
        "window": {"since": since.isoformat(), "until": until.isoformat(),
                   "hours": args.hours},
        "s_cmd_level": {"value": s_val, "N_mcp": N_mcp,
                        "N_mcp_daemon_search": len(d_search), "N_mcp_cli": n_cli,
                        "N_legacy": N_legacy, "N_legacy_denied": N_denied},
        "S_episode_level": {"value": S_val, "E_mcp": E_mcp, "E_legacy": E_leg,
                            "episodes": len(all_eps)},
        "daemon": {"rows": len(drows), "search_calls": len(d_search),
                   "mgmt": len(d_mgmt), "other": len(d_other),
                   "errors": len(d_err),
                   "by_tool": dict(Counter(r["tool"] for r in d_search)),
                   "p50_ms": pctl([r["duration_ms"] for r in d_search], .5),
                   "p95_ms": pctl([r["duration_ms"] for r in d_search], .95)},
        "reconcile": {"jsonl_mcp_frames": N_mcp_jsonl - n_cli,
                      "daemon_search_calls": len(d_search),
                      "note": "jsonl 帧 ≤ daemon（父会话历史裁剪/fork 会漏帧）；cli 直调只在 jsonl 侧"},
        "slot": {"active_sessions": len(active), "loaded": len(slot_loaded),
                 "rate": (len(slot_loaded) / len(active)) if active else None},
        "by_project": {k: {"mcp": v["mcp"], "legacy": v["legacy"],
                           "s": v["mcp"] / (v["mcp"] + v["legacy"])
                           if (v["mcp"] + v["legacy"]) else None}
                       for k, v in sorted(by_proj.items(),
                                          key=lambda kv: -(kv[1]["mcp"] + kv[1]["legacy"]))
                       if v["mcp"] + v["legacy"]},
        "top_legacy_sessions": [
            {"sid": s["sid"], "cwd": s["cwd"],
             "legacy": s["counters"]["legacy"],
             "legacy_denied": s["counters"]["legacy_denied"],
             "exempt_noncode": s["counters"]["exempt_noncode"],
             "exempt_page": s["counters"]["exempt_page"],
             "mcp": s["counters"]["mcp_frame"] + s["counters"]["mcp_cli"],
             "bucket": bucket(s)} for s in top_legacy],
        "exempt_totals": {
            "page": sum(s["counters"]["exempt_page"] + s["counters"]["sed_page"] for s in sessions),
            "noncode": sum(s["counters"]["exempt_noncode"] for s in sessions),
            "filter": sum(s["counters"]["exempt_filter"] for s in sessions)},
        "dedup": {"skipped_replay": sum(s["counters"]["skipped_replay"] for s in sessions),
                  "skipped_dup_call_id": sum(s["counters"]["skipped_dup"] for s in sessions)},
    }

    if args.json:
        print(json.dumps(report, ensure_ascii=False, indent=2, default=str))
    else:
        w = report
        def fmt(v):
            return "n/a" if v is None else f"{v*100:.1f}%"
        print(f"# MCP 采用度基线（近 {args.hours:g}h：{since:%m-%d %H:%M} → {until:%m-%d %H:%M} UTC）")
        sc = w["s_cmd_level"]
        print(f"\n## 命令级 s = {fmt(sc['value'])}  "
              f"(N_mcp={sc['N_mcp']} [daemon {sc['N_mcp_daemon_search']} + cli {sc['N_mcp_cli']}], "
              f"N_legacy={sc['N_legacy']}, hook拦截剔除 {sc['N_legacy_denied']})")
        Se = w["S_episode_level"]
        print(f"## 事件级 S = {fmt(Se['value'])}  "
              f"(E_mcp={Se['E_mcp']}, E_legacy={Se['E_legacy']}, 共 {Se['episodes']} 事件)")
        sl = w["slot"]
        print(f"## slot 加载率 = {fmt(sl['rate'])}  ({sl['loaded']}/{sl['active_sessions']} 个有检索活动的会话)")
        d = w["daemon"]
        print(f"\n## daemon 侧：{d['rows']} 行，检索 tools/call {d['search_calls']}"
              f"（错误 {d['errors']}），管理面 {d['mgmt']}；"
              f"p50={d['p50_ms']}ms p95={d['p95_ms']}ms；按工具 {d['by_tool']}")
        r = w["reconcile"]
        print(f"## 对账：jsonl MCP 帧 {r['jsonl_mcp_frames']} vs daemon 检索 {r['daemon_search_calls']}（{r['note']}）")
        print(f"## 豁免剔除：页内精定位 {w['exempt_totals']['page']}，"
              f"非代码文本 {w['exempt_totals']['noncode']}，管道过滤 {w['exempt_totals']['filter']}")
        print(f"## 去重：fork 重放跳过 {w['dedup']['skipped_replay']} 帧，"
              f"call_id 重复跳过 {w['dedup']['skipped_dup_call_id']} 帧")
        print("\n## 按 project（cwd）")
        for k, v in w["by_project"].items():
            print(f"  {k:40s} mcp={v['mcp']:<4d} legacy={v['legacy']:<4d} s={fmt(v['s'])}")
        print("\n## legacy 扫射 Top 会话")
        for t in w["top_legacy_sessions"]:
            print(f"  legacy={t['legacy']:<3d} denied={t['legacy_denied']:<3d} "
                  f"mcp={t['mcp']:<3d} 桶={t['bucket']:8s} "
                  f"豁免(非代码/页内)={t['exempt_noncode']}/{t['exempt_page']} "
                  f"cwd={os.path.basename(t['cwd']) or '?'} {t['sid'][:60]}")
        # 事件样例
        print(f"\n## 事件级归并样例（窗口 {EPISODE_GAP_S}s，最多 {args.samples} 个）")
        shown = 0
        for e in sorted(all_eps, key=lambda x: -x["n"]):
            if shown >= args.samples:
                break
            if e["n"] < 2:
                continue
            shown += 1
            print(f"  ▸ 事件#{shown} [{'MCP主导' if e['lead']=='mcp' else 'legacy主导'}]"
                  f"{' +回退' if e['fallback'] else ''} {e['n']} 调用，"
                  f"cwd={os.path.basename(e['cwd']) or '?'}，会话 {e['sid'][:48]}")
            for ts, kind, detail in e["calls"][:6]:
                print(f"      {ts:%H:%M:%S} [{kind}] {detail[:110]}")
            if e["n"] > 6:
                print(f"      ... 共 {e['n']} 条")
    return 0


if __name__ == "__main__":
    sys.exit(main())
