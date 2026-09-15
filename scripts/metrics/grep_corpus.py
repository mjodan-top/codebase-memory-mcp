#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""grep/rg 帧语料抽取 + 意图分类（纯函数，供离线回放基准复用）。

为什么要固化语料：会话 jsonl 会滚动清理、/tmp 样本易失，而「MCP 接管率」
这个指标必须能在修复前后用**同一批输入**复跑对照。因此本脚本把窗口内
所有 grep/rg 帧抽成仓内 JSONL fixture，后续 mcp_takeover_bench.py 只读
fixture，不再依赖原始会话。

数据源（只读）：~/.codex/sessions、~/.coder/sessions、~/.codex-zkf/sessions
下的 rollout-*.jsonl，按 basename 去重（三处同名同内容）。

两道去重防线（沿用 mcp-adoption.py 实证口径）：
  ① fork/clone 会话把父历史以创建时刻整体重放写入 → 跳过 session_meta
     ±REPLAY_BURST_S 内的帧；
  ② 跨文件按 call_id 全局去重（按会话开始时间升序喂入，保留原始帧）。

分类三桶（纯函数 classify_intent，有 golden 单测）：
  code_symbol  找函数/类/符号的定义或调用关系 —— codebase MCP 本该接管
  text_search  日志/配置/文本/页内精定位/仓外目标 —— grep 本就正确
  unknown      判不出意图（pattern 非标识符形态但目标是代码，或目标解析不出）

每帧都带 reason 字段（判据名），任何一条分类结论都能人工机械复核。
"""
from __future__ import annotations

import glob
import json
import os
import re
import shlex
from datetime import datetime, timedelta, timezone

# ---------------- 口径常量 ----------------
REPLAY_BURST_S = 10   # fork 重放帧窗口
SESSION_ROOTS = ("~/.codex/sessions", "~/.coder/sessions", "~/.codex-zkf/sessions")

GREP_PROGS = {"grep", "rg", "egrep", "fgrep"}
REMOTE_WRAPPERS = {"ssh", "mosh", "et", "autossh", "tmux"}
WRAPPERS = {"command", "sudo", "timeout", "xargs", "nice", "env"}

NONCODE_EXT = {
    ".log", ".toml", ".json", ".jsonl", ".md", ".txt", ".yml", ".yaml",
    ".lock", ".cfg", ".ini", ".err", ".out", ".csv", ".html", ".plist",
    ".service", ".env", ".conf", ".xml", ".patch", ".diff",
}
CODE_EXT = {
    ".c", ".h", ".cc", ".cpp", ".hpp", ".hh", ".rs", ".go", ".py", ".mjs",
    ".cjs", ".js", ".ts", ".tsx", ".jsx", ".java", ".rb", ".sh", ".bash",
    ".zsh", ".swift", ".m", ".mm", ".kt", ".cs", ".php", ".lua", ".zig",
    ".sql", ".scala", ".ex", ".exs", ".dart", ".vue", ".svelte",
}
# 目标路径里出现即判非代码（日志/会话档/记忆仓/依赖目录）
NONCODE_PATH_HINTS = (
    "daemon.err", "/library/logs/", "/sessions/", "/var/log", "/logs/",
    ".codex", ".coder", ".claude", "/memory/", "agents.md", "memory.md",
    "node_modules", "subagent-archive", "history.jsonl", "/docs/",
    ".git/", "/target/", "/build/", "/dist/",
)

# pattern「像标识符」：≥3 个 word 字符起步的驼峰/蛇形 token
IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]{2,}")
# 纯自然语言/日志文案特征：含空格且无标识符连接符
PROSE_RE = re.compile(r"^[A-Za-z][A-Za-z ,.'!?:-]{4,}$")
# 明显的定义/调用意图前缀（C/Go/Rust/Py/JS 通用）
DEF_HINT_RE = re.compile(
    r"\b(def|class|func|fn|function|struct|enum|impl|interface|type|"
    r"static|void|int|char|const|export|public|private)\b")

# 带值标志：其后一个 token 是标志的值而非文件/pattern
VALUED_FLAGS = {
    "-e", "--regexp", "-f", "--file", "-m", "--max-count", "-A", "-B", "-C",
    "-g", "--glob", "--iglob", "-t", "--type", "-T", "--type-not",
    "--include", "--exclude", "--exclude-dir", "--color", "-d",
    "--after-context", "--before-context", "--context",
}
REDIR_RE = re.compile(r"^\d*(?:>>|>|<)\S*$")


# ---------------- 引号感知切分（与 hook split_pipeline 同语义） ----------------
def split_pipeline(command: str):
    """quote-aware 两级切分 → [(segment, is_pipeline_head)]。

    `&&` / `;` / `||` 切出的是独立命令组，每组首段都要重新判定；组内 `|`
    切出的非首段是「管道中游过滤」。引号/转义内的分隔符不切——
    `ssh h 'a; grep -r x'` 的 `;` 属 ssh 参数串，切开会把远端 grep 误判成本地。
    未闭合引号 → fail-open：整条按单段返回。
    """
    parts, buf, sep = [], [], ""
    q, esc = None, False
    i, n = 0, len(command)
    while i < n:
        ch = command[i]
        if esc:
            buf.append(ch); esc = False; i += 1; continue
        if q == "'":
            if ch == "'":
                q = None
            buf.append(ch); i += 1; continue
        if q == '"':
            if ch == "\\":
                esc = True
            elif ch == '"':
                q = None
            buf.append(ch); i += 1; continue
        if ch == "\\":
            esc = True; buf.append(ch); i += 1; continue
        if ch in ("'", '"'):
            q = ch; buf.append(ch); i += 1; continue
        if command[i:i + 2] in ("&&", "||"):
            parts.append(("".join(buf), sep)); buf, sep = [], command[i:i + 2]; i += 2; continue
        if ch in ";\n":
            parts.append(("".join(buf), sep)); buf, sep = [], ";"; i += 1; continue
        if ch == "|":
            parts.append(("".join(buf), sep)); buf, sep = [], "|"; i += 1; continue
        buf.append(ch); i += 1
    parts.append(("".join(buf), sep))
    if q is not None:
        return [(command.strip(), True)]
    return [(t.strip(), s != "|") for t, s in parts if t.strip()]


def strip_redirects(toks):
    out, skip = [], False
    for t in toks:
        if skip:
            skip = False
            continue
        if REDIR_RE.match(t):
            if re.match(r"^\d*(?:>>|>|<)$", t):
                skip = True
            continue
        out.append(t)
    return out


def parse_grep_segment(seg: str):
    """把一个 shell 段解析成 grep 调用结构；不是 grep 则返回 None。

    返回 {"prog","flags","pattern","files","recursive","includes"}。
    """
    try:
        toks = shlex.split(seg)
    except ValueError:
        toks = seg.split()
    toks = strip_redirects(toks)
    if not toks:
        return None
    i = 0
    while i < len(toks) and "=" in toks[i] and not toks[i].startswith("-"):
        i += 1                      # 前置 env 赋值
    while i < len(toks) and toks[i].rsplit("/", 1)[-1] in WRAPPERS:
        i += 1
        if i < len(toks) and re.match(r"^\d", toks[i]):
            i += 1                  # timeout 的秒数
    if i >= len(toks):
        return None
    prog = toks[i].rsplit("/", 1)[-1]
    if prog in REMOTE_WRAPPERS:
        return {"prog": prog, "remote": True, "flags": [], "pattern": "",
                "files": [], "recursive": False, "includes": []}
    if prog not in GREP_PROGS:
        return None

    flags, positional, skip = [], [], False
    for a in toks[i + 1:]:
        if skip:
            flags.append(a); skip = False; continue
        if a.startswith("-") and a != "-":
            flags.append(a)
            if a in VALUED_FLAGS:
                skip = True
            continue
        positional.append(a)

    # -e PAT 形式：pattern 在标志值里
    pattern = ""
    for idx, f in enumerate(flags):
        if f in ("-e", "--regexp") and idx + 1 < len(flags):
            pattern = flags[idx + 1]; break
        if f.startswith("--regexp="):
            pattern = f.split("=", 1)[1]; break
    if pattern:
        files = positional
    else:
        pattern = positional[0] if positional else ""
        files = positional[1:]

    recursive = any(re.match(r"^-[a-zA-Z]*[rR]", f) for f in flags) or \
        any(f == "--recursive" for f in flags) or prog == "rg"
    includes = [f.split("=", 1)[1] for f in flags
                if f.startswith("--include=") and "=" in f]
    includes += [f.split("=", 1)[1] for f in flags
                 if f.startswith("--glob=") and "=" in f]
    return {"prog": prog, "remote": False, "flags": flags, "pattern": pattern,
            "files": files, "recursive": recursive, "includes": includes}


# ---------------- 目标性质判定 ----------------
def _glob_prefix_dir(tok: str) -> str:
    m = re.search(r"[*?\[]", tok)
    head = tok[: m.start()] if m else tok
    return head.rsplit("/", 1)[0] if "/" in head else ""


def target_kind(tok: str) -> str:
    """单个目标 token → 'noncode' / 'code' / 'dir' / 'unknown'（纯文本判定，不碰盘）。"""
    t = tok.strip("'\"").lower()
    if not t:
        return "unknown"
    if "$" in t:
        return "unknown"
    if any(h in t for h in NONCODE_PATH_HINTS):
        return "noncode"
    # docs 树 = markdown 文档，非代码（与 hook is_noncode_target 同口径）
    d = t.rstrip("/")
    if d == "docs" or d.endswith("/docs") or t.startswith("docs/"):
        return "noncode"
    base = t.rstrip("/").rsplit("/", 1)[-1]
    _, ext = os.path.splitext(base)
    if ext in NONCODE_EXT:
        return "noncode"
    if ext in CODE_EXT:
        return "code"
    if any(ch in t for ch in "*?["):
        gext = os.path.splitext(base)[1]
        if gext in NONCODE_EXT:
            return "noncode"
        if gext in CODE_EXT:
            return "code"
        return "dir"
    if t in (".", "..") or t.endswith("/") or not ext:
        return "dir"
    return "unknown"


def pattern_looks_symbolic(pattern: str) -> bool:
    """pattern 像「代码符号名」而非自然语言/日志文案。"""
    p = pattern.strip("'\"")
    if not p:
        return False
    idents = IDENT_RE.findall(p)
    if not idents:
        return False
    # 定义关键字前缀（`def foo`、`func NewPoolClient`、`class SessionManager`、
    # `impl Display`）→ 强符号信号。必须先于散文判据：`func NewPoolClient` 含空格
    # 且无标点，会被散文正则误吃（golden 单测实测 3 例）。
    if DEF_HINT_RE.search(p):
        return True
    # 纯散文（含空格、全是普通英文词）→ 文本检索
    if " " in p and not re.search(r"[_(){}\[\]:.<>=*\\|]", p) and PROSE_RE.match(p):
        return False
    # 驼峰 / 蛇形 / 带括号或 :: 的调用形态
    for ident in idents:
        if "_" in ident or re.search(r"[a-z][A-Z]", ident) or len(ident) >= 6:
            return True
    if re.search(r"(::|->|\(\)|\.\w+\()", p):
        return True
    return False


# ---------------- 意图分类（纯函数，有单测） ----------------
BUCKET_WEIGHT = {"text_search": 0, "unknown": 1, "code_symbol": 2}


def classify_intent(command: str, cwd: str = ""):
    """→ (bucket, reason, detail)

    bucket ∈ {code_symbol, text_search, unknown, not_grep}
    reason 是机械判据名，detail 带 pattern/目标，便于人工复核。

    **全段扫描取最重桶**（code_symbol > unknown > text_search）：本机命令
    普遍是 `cd X && grep -n a f.c && grep -rn b src/` 这种多段复合形态，
    只判第一个 grep 段会让后段的跨文件扫射被前段的页内定位/管道过滤盖住，
    系统性低估 MCP 本该接管的量（实测首段口径比全段口径少算约 1/3）。
    同理，链内 `cd <path>` 要更新后续段的 cwd。
    """
    results = []
    cur_cwd = cwd
    for seg, is_head in split_pipeline(command):
        if re.match(r"^cd(\s|$)", seg):
            try:
                ctoks = strip_redirects(shlex.split(seg))
            except ValueError:
                ctoks = []
            cargs = [t for t in ctoks[1:] if t != "--"]
            if len(cargs) == 1 and "$" not in cargs[0]:
                p = os.path.expanduser(cargs[0])
                cur_cwd = p if os.path.isabs(p) else (
                    os.path.normpath(os.path.join(cur_cwd, p)) if cur_cwd else cur_cwd)
            else:
                cur_cwd = ""
            continue
        g = parse_grep_segment(seg)
        if g is None:
            continue
        if g.get("remote"):
            results.append(("text_search", "remote_wrapper", seg[:120]))
            continue
        if not is_head:
            results.append(("text_search", "pipeline_filter", seg[:120]))
            continue
        results.append(_classify_grep(g, cur_cwd))
    if not results:
        return ("not_grep", "no_grep_segment", command[:120])
    results.sort(key=lambda r: -BUCKET_WEIGHT[r[0]])
    return results[0]


def _classify_grep(best, cwd=""):
    """单个 grep 首段结构 → (bucket, reason, detail)。"""
    pat, files = best["pattern"], best["files"]
    detail = f"pat={pat[:60]!r} files={files[:3]}"

    # 1) 无文件参数
    if not files:
        if best["prog"] == "grep" and not best["recursive"]:
            return ("text_search", "stdin_filter", detail)   # 读 stdin
        # rg / grep -r 扫 cwd
        if cwd and any(h in cwd.lower() for h in NONCODE_PATH_HINTS):
            return ("text_search", "cwd_noncode", detail)
        if not pattern_looks_symbolic(pat):
            return ("text_search", "pattern_not_symbolic", detail)
        return ("code_symbol", "recursive_cwd_symbol", detail)

    kinds = [target_kind(f) for f in files]
    # 2) 全部目标是非代码文本
    if kinds and all(k == "noncode" for k in kinds):
        return ("text_search", "targets_noncode", detail)
    # 3) --include 完全限定到非代码扩展名
    inc = best["includes"]
    if inc and all(os.path.splitext(g)[1].lower() in NONCODE_EXT for g in inc):
        return ("text_search", "include_noncode", detail)
    # 4) 单个具体代码文件、非递归 → 页内精定位，MCP 接管不了
    if len(files) == 1 and not best["recursive"] and \
            not any(ch in files[0] for ch in "*?[") and kinds[0] == "code":
        return ("text_search", "page_local", detail)
    # 5) pattern 不像符号 → 文本检索
    if not pattern_looks_symbolic(pat):
        return ("text_search", "pattern_not_symbolic", detail)
    # 6) 目标含代码文件/代码 glob/目录 + 符号形 pattern → MCP 本该接管
    if any(k in ("code", "dir") for k in kinds):
        if inc and all(os.path.splitext(g)[1].lower() in CODE_EXT for g in inc):
            return ("code_symbol", "include_code_symbol", detail)
        return ("code_symbol", "code_targets_symbol", detail)
    return ("unknown", "undetermined_targets", detail)


# ---------------- 会话抽取 ----------------
def parse_ts(s):
    try:
        return datetime.fromisoformat(str(s).replace("Z", "+00:00"))
    except Exception:
        return None


def iter_session_files(since: datetime):
    seen, out = set(), []
    for root in SESSION_ROOTS:
        r = os.path.expanduser(root)
        for f in glob.glob(os.path.join(r, "*", "*", "*", "*.jsonl")):
            b = os.path.basename(f)
            if b in seen:
                continue
            try:
                if os.path.getmtime(f) < since.timestamp():
                    continue
            except OSError:
                continue
            seen.add(b)
            out.append(f)
    out.sort(key=os.path.basename)
    return out


def extract_frames(path, since, until, seen_call_ids):
    """抽取该会话内的 grep/rg 帧（含 hook 拦截标记与输出命中行数）。"""
    frames, pending = [], {}
    sid = os.path.basename(path)
    cwd, meta_ts, forked = "", None, False
    try:
        fh = open(path, errors="replace")
    except OSError:
        return frames
    with fh:
        for line in fh:
            if ('"function_call"' not in line and '"function_call_output"' not in line
                    and '"turn_context"' not in line and '"session_meta"' not in line
                    and '"forked_history_ref"' not in line):
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
                idx = pending.pop(p.get("call_id"), None)
                if idx is None:
                    continue
                out = str(p.get("output", ""))
                frames[idx]["blocked"] = "blocked by PreToolUse hook" in out[:400]
                body = out[:20000]
                frames[idx]["output_lines"] = body.count("\n")
                frames[idx]["output_head"] = body[:300]
                continue
            if p.get("type") != "function_call":
                continue
            ts = parse_ts(d.get("timestamp", ""))
            if ts is None or ts < since or ts > until:
                continue
            if forked and meta_ts is not None and \
                    abs((ts - meta_ts).total_seconds()) <= REPLAY_BURST_S:
                continue
            cid = p.get("call_id")
            if cid:
                if cid in seen_call_ids:
                    continue
                seen_call_ids.add(cid)
            if p.get("name", "") not in ("exec_command", "shell_command", "shell", "local_shell"):
                continue
            try:
                a = json.loads(p.get("arguments", "") or "{}")
            except Exception:
                continue
            cmd = a.get("cmd") or a.get("command") or ""
            if isinstance(cmd, list):
                cmd = " ".join(str(x) for x in cmd)
            if not cmd or not re.search(r"\b(grep|rg|egrep|fgrep)\b", cmd):
                continue
            fcwd = a.get("workdir") or a.get("cwd") or cwd
            bucket, reason, detail = classify_intent(cmd, fcwd)
            if bucket == "not_grep":
                continue
            frames.append({
                "session": sid, "ts": d.get("timestamp", ""), "cwd": fcwd,
                "cmd": cmd[:4000], "bucket": bucket, "reason": reason,
                "detail": detail, "blocked": False,
                "output_lines": None, "output_head": "",
            })
            if cid:
                pending[cid] = len(frames) - 1
    return frames


def build_corpus(days=2.0, until=None):
    now = until or datetime.now(timezone.utc)
    since = now - timedelta(days=days)
    seen_call_ids = set()
    all_frames = []
    for f in iter_session_files(since):
        all_frames.extend(extract_frames(f, since, now, seen_call_ids))
    all_frames.sort(key=lambda x: x["ts"])
    return all_frames, since, now


def main():
    import argparse
    ap = argparse.ArgumentParser(description="抽取 grep/rg 帧语料并固化为 JSONL")
    ap.add_argument("--days", type=float, default=2.0)
    ap.add_argument("--out", default="test/fixtures/grep-corpus-48h.jsonl")
    args = ap.parse_args()

    frames, since, now = build_corpus(args.days)
    out = os.path.abspath(args.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        f.write(json.dumps({
            "_meta": True,
            "window_since_utc": since.isoformat(),
            "window_until_utc": now.isoformat(),
            "generated_by": "scripts/metrics/grep_corpus.py",
            "frames": len(frames),
        }, ensure_ascii=False) + "\n")
        for fr in frames:
            f.write(json.dumps(fr, ensure_ascii=False) + "\n")

    from collections import Counter
    bc = Counter(x["bucket"] for x in frames)
    rc = Counter(x["reason"] for x in frames)
    tz8 = timezone(timedelta(hours=8))
    print(f"窗口（北京时间）：{since.astimezone(tz8):%Y-%m-%d %H:%M} → "
          f"{now.astimezone(tz8):%Y-%m-%d %H:%M}")
    print(f"语料：{out}  帧数={len(frames)}  会话数={len({x['session'] for x in frames})}")
    print(f"被 hook 拦截={sum(1 for x in frames if x['blocked'])}")
    print("\n分桶：")
    for k, v in bc.most_common():
        print(f"  {k:14s} {v:5d}  {v / max(1, len(frames)) * 100:5.1f}%")
    print("\n判据 top15：")
    for k, v in rc.most_common(15):
        print(f"  {k:26s} {v:5d}")


if __name__ == "__main__":
    main()
