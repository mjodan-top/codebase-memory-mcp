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

指标 2 —— 每 action 耗时（2026-07-23 新增）：同一 call_id 的
function_call → function_call_output 墙钟差，mcp / legacy 各一桶；
hook-denied 帧不计（拦截返回不是真实检索耗时），负值/超 600s 的
异常差值丢弃（时钟回拨、跨压缩残帧）。这是 agent 视角的单动作等待，
与 daemon 侧 duration_ms（纯服务端）互为对照。

指标 3 —— hook 误判率（2026-07-23 新增，次要指标）：窗口内被 hook
拦截（denied）的命令逐条按**当前版本** hook 回放，allow ⇒ 该 deny 在
现行规则下属误拦（FP）。rate = FP / 回放成功数，目标线 < 5%
（FP_TARGET，#47/#48/#49 修完后持续达标）。语义：修复合入后，历史
误拦回放转 allow → rate 先升后随窗口滚动清零；稳态下新增 deny 都是
现行规则的产物，rate ≈ 0 即无已知误拦形态。
辅助诊断 classifier_disagree（不进 rate）：回放仍 deny 但 metrics 自身
classify_exec 不判 legacy 的条数——独立第二意见，用来在 hook 修复
**之前**暴露候选新 FP 形态（如 ssh 远端/非代码残漏）；含 tmpclone
（/tmp 克隆代码，hook 按设计拦、metrics 记 noncode）这类口径灰区，
所以只作分诊线索，不做达标判据。
"""
import argparse
import glob
import json
import os
import re
import shlex
import subprocess
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

# 远端执行包装（与 hook REMOTE_WRAPPERS 同口径，#47）：段首是这些程序时
# 参数串在远端 shell 执行，本地索引零覆盖 → exempt_remote，不压 S 分母。
REMOTE_WRAPPERS = {"ssh", "mosh", "et", "autossh", "tmux"}
LATENCY_MAX_S = 600  # 指标 2：单 action call→output 超过此值视为异常帧丢弃
FP_TARGET = 0.05     # 指标 3：hook 误判率目标线（#47/#48/#49 修完后 <5%）
REPLAY_TIMEOUT_S = 10   # 指标 3：单条回放超时（与 hook 自身 timeout 对齐）
REPLAY_MAX = 400        # 指标 3：单次统计回放上限（防日志爆量拖死报表）

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


def _outside_git_repo(tok):
    """~/$HOME 绝对路径存在且向上无 .git → 项目外（索引不覆盖），与 hook 同口径。

    相对路径/不存在的路径返回 False（历史命令无 cwd 可靠展开，保守不豁免）。
    """
    t = os.path.expandvars(os.path.expanduser(tok))
    if "$" in t or not os.path.isabs(t):
        return False
    m = re.search(r"[*?\[]", t)
    if m:
        head = t[: m.start()]
        t = head.rsplit("/", 1)[0] if "/" in head else ""
        if not t:
            return False
    t = os.path.realpath(t)  # 解 symlink，与 hook 同口径：链接指进仓内不豁免
    if not os.path.exists(t):
        return False
    p = t if os.path.isdir(t) else (os.path.dirname(t) or "/")
    while True:
        if os.path.exists(os.path.join(p, ".git")):
            return False
        parent = os.path.dirname(p)
        if parent == p:
            return True
        p = parent


def _path_kind(tok, cwd=""):
    """返回 'code' / 'noncode' / 'dir' / 'unknown'。"""
    t = tok.rstrip("/")
    low = tok.lower()
    if any(h.lower() in low for h in NONCODE_PATH_HINTS):
        return "noncode"
    if _outside_git_repo(tok):
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

# ---------------- 窄窗读影子观测（issue #54） ----------------
# 影子模式硬承诺：narrow_read 只观测、不判罚，绝不进 s/S 的任何分母。
# prior art：GCP Organization Policy dry-run（强制前至少观测两周）、Istio
# AuthorizationPolicy dry-run（策略求值并记录、流量照常通过）、Flagright
# Shadow Rules（并行跑看潜在影响，平均观测一周再转 live）。
NARROW_READ_SHADOW = True

# 窗口合并阈值：相邻窗口间隔 ≤ 此值视为「本该合并成一刀」。
# 注意：**「间隔<200」本身不是可疑信号** —— spike 全量实测（530 会话档）
# 显示总体 72% 的邻窗间隔天然 <200，用它标红会误伤七成正常行为。
# 它只作为合并运算的参数，可疑信号看 redundancy。
MERGE_GAP = 200

# 窄窗读命令形态。sed 已在 classify_search_cmd 里判 sed_page；nl / awk NR /
# head / tail 此前完全不在 prog 白名单内（spike 实测 nl 388 次属真空）。
_NR_SED_RANGE = re.compile(r"sed\s+(?:-[a-zA-Z]+\s+)*-n\s+['\"]?(\d+)\s*,\s*(\d+)p")
_NR_SED_ONE = re.compile(r"sed\s+(?:-[a-zA-Z]+\s+)*-n\s+['\"]?(\d+)p")
_NR_AWK_RANGE = re.compile(r"awk\s+['\"]NR\s*(>=|>)\s*(\d+)\s*&&\s*NR\s*(<=|<)\s*(\d+)")
_NR_NL = re.compile(r"(?:^|[\s;|&])nl\b")
_NR_HEADTAIL = re.compile(r"\b(head|tail)\s+-n\s*(\+?)(\d+)\s+(\S+)")
NARROW_READ_TOOLS = ("sed", "nl", "awk", "head", "tail")

# 降级可观测（AGENTS.md 硬纪律）：窄读解析的每条降级分支都要计数 + 留样本，
# 并在报表里显示。判据：若这条分支从明天起 100% 触发，能不能当场看出来。
NR_PARSE_DEGRADED = {"unclosed_quote": 0, "unknown_range": 0, "samples": []}


def _nr_code_files(seg):
    """段内看起来像代码文件路径的 token（按 CODE_EXT 判定）。"""
    out = []
    for t in seg.split():
        t = t.strip("'\"();|&<>")
        if not t or t.startswith("-"):
            continue
        if os.path.splitext(t)[1].lower() in CODE_EXT:
            out.append(t)
    return out


def _nr_split(cmd):
    """引号感知地按 ; && || | 切段。

    不能直接用 WORD_SPLIT_RE：`awk 'NR>=1 && NR<=9' f.c` 的 `&&` 在单引号
    内，裸切会把 awk 表达式劈成两半（与 hook 侧 #47 同一类根因）。
    未闭合引号 → 整条按单段返回（宁可少抽，不要崩），并计入
    NR_PARSE_DEGRADED 供报表显示——降级分支必须可观测（AGENTS.md 硬纪律：
    若这条降级从明天起 100% 触发，日志/计数必须能看出来）。
    """
    parts, buf, q, esc = [], [], None, False
    i, n = 0, len(cmd)
    while i < n:
        ch = cmd[i]
        if esc:
            buf.append(ch); esc = False; i += 1; continue
        if q:
            if q == '"' and ch == "\\":
                esc = True
            elif ch == q:
                q = None
            buf.append(ch); i += 1; continue
        if ch in ("'", '"'):
            q = ch; buf.append(ch); i += 1; continue
        if ch == "\\":
            esc = True; buf.append(ch); i += 1; continue
        if cmd[i:i + 2] in ("&&", "||"):
            parts.append("".join(buf)); buf = []; i += 2; continue
        if ch in ";\n|":
            parts.append("".join(buf)); buf = []; i += 1; continue
        buf.append(ch); i += 1
    parts.append("".join(buf))
    if q is not None:
        NR_PARSE_DEGRADED["unclosed_quote"] += 1
        NR_PARSE_DEGRADED["samples"].append(cmd[:160])
        return [cmd]
    return [p for p in parts if p.strip()]


def _nr_prog(seg):
    """段内**实际执行**的程序名（跳过 env 赋值与 timeout/sudo 等包装）。

    必须按 token 解析而非扫描原文：`echo head -n 40 f.ts` 里的 head 只是
    字面量，不是执行的程序（审查发现的误计根因）。
    """
    try:
        toks = shlex.split(seg)
    except ValueError:
        toks = seg.split()
    toks = _strip_redirects(toks)
    i = 0
    while i < len(toks) and ("=" in toks[i] and not toks[i].startswith("-")):
        i += 1
    while i < len(toks) and toks[i] in ("timeout", "command", "nice", "xargs", "sudo"):
        i += 1
        if i < len(toks) and toks[i - 1] == "timeout" and re.match(r"^\d", toks[i]):
            i += 1
    return os.path.basename(toks[i]) if i < len(toks) else ""


def _nr_key(tok, cwd=""):
    """聚合键：规范化的相对路径，**不是 basename**。

    basename 会把 `src/a.c` 与 `tests/a.c` 混成同一条记录，虚增冗余
    （审查发现）。解析不出相对位置时退回原 token，仍保留目录信息。
    """
    t = tok.strip("'\"")
    t = os.path.expanduser(os.path.expandvars(t)) if "~" in t or "$" in t else t
    if os.path.isabs(t) and cwd:
        try:
            return os.path.relpath(t, cwd)
        except ValueError:
            return t
    return os.path.normpath(t)


def extract_narrow_windows(cmd, cwd=""):
    """抽取窄窗读事件：[(key, lo, hi, tool)]。

    key = 规范化相对路径（非 basename）。lo/hi 为 None 表示区间未知
    （整文件 `nl -ba f.c`，或 `tail -n N` 这种需文件总行数才能定界的形态）
    —— 未知区间不参与 coverage/redundancy 计算。

    只对**实际执行**的 sed/nl/awk/head/tail 抽取；远端包装（ssh/tmux）
    整段跳过（远端文件不在本地索引覆盖内，与 legacy 口径一致）。
    """
    res = []
    for seg in _nr_split(cmd):
        s = (seg or "").strip()
        if not s:
            continue
        prog = _nr_prog(s)
        if prog in REMOTE_WRAPPERS:
            continue
        if prog not in NARROW_READ_TOOLS:
            continue
        files = [f for f in _nr_code_files(s)]
        if not files:
            continue
        k0 = _nr_key(files[0], cwd)
        if prog == "sed":
            m = _NR_SED_RANGE.search(s)
            if m:
                lo, hi = int(m.group(1)), int(m.group(2))
                if 1 <= lo <= hi:          # 反向/非法区间丢弃（审查发现负 coverage）
                    res.append((k0, lo, hi, "sed"))
                continue
            m = _NR_SED_ONE.search(s)
            if m:
                n = int(m.group(1))
                if n >= 1:
                    res.append((k0, n, n, "sed"))
            continue
        if prog == "awk":
            m = _NR_AWK_RANGE.search(s)
            if m:
                # 开/闭区间按运算符定界：NR>200 && NR<340 实读 201..339
                lo = int(m.group(2)) + (0 if m.group(1) == ">=" else 1)
                hi = int(m.group(4)) - (0 if m.group(3) == "<=" else 1)
                if 1 <= lo <= hi:
                    res.append((k0, lo, hi, "awk"))
            continue
        if prog in ("head", "tail"):
            m = _NR_HEADTAIL.search(s)
            if not m:
                continue
            tgt = _nr_key(m.group(4), cwd)
            n = int(m.group(3))
            plus = m.group(2) == "+"
            if prog == "head":
                if n >= 1:
                    res.append((tgt, 1, n, "head"))
            elif plus:
                # `tail -n +N` = 第 N 行至 EOF：起点已知、终点未知
                res.append((tgt, None, None, "tail"))
            else:
                # `tail -n N` = 末尾 N 行：需文件总行数才能定界 → 未知
                res.append((tgt, None, None, "tail"))
            continue
        if prog == "nl":
            res.append((k0, None, None, "nl"))
    return res


def merge_adjacent(windows, gap=MERGE_GAP):
    """把 [(lo,hi)] 按「间隔 ≤ gap」合并，返回 [[lo,hi], ...]。"""
    ranged = sorted((lo, hi) for lo, hi in windows if lo is not None and hi is not None)
    if not ranged:
        return []
    out = [list(ranged[0])]
    for lo, hi in ranged[1:]:
        if lo - out[-1][1] <= gap:
            out[-1][1] = max(out[-1][1], hi)
        else:
            out.append([lo, hi])
    return out


def redundancy(windows, gap=MERGE_GAP):
    """读冗余倍率 = 窗口数 / 合并后段数，即「本可几刀读完却用了几刀」。

    1.0 = 无冗余（每刀都读了独立区域，健康）。实测总体 81.4% 的
    会话×文件 ≤2.0；P90=4.0、P99≈19.4。坏样本可达 95.0（95 刀读完一个
    458 行文件、合并后是连续 1 段）。
    """
    ranged = [(lo, hi) for lo, hi in windows if lo is not None and hi is not None]
    if not ranged:
        return None
    merged = merge_adjacent(ranged, gap)
    return round(len(ranged) / len(merged), 2)


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
    if prog in REMOTE_WRAPPERS:
        # 远端执行（ssh/tmux 等）：目标不在本地索引覆盖内，不压 S 分母（#47）
        return ("exempt_remote", seg.strip()[:120])
    if prog not in ("grep", "egrep", "fgrep", "rg", "sed", "nl", "awk", "head", "tail"):
        return ("none", "")

    # nl / awk NR / head / tail：此前完全不在白名单内（#54 真空）。
    # 只作影子观测，永不判 legacy。
    if prog in ("nl", "awk", "head", "tail"):
        if piped_before:
            return ("exempt_filter", seg.strip()[:120])
        return ("narrow_read", seg.strip()[:120]) if extract_narrow_windows(seg) \
            else ("none", "")

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
        # 单文件 sed：既是既有 sed_page，也进 #54 的窄窗读影子桶。
        # 桶名换成 narrow_read（sed_page 作为别名保留在报表 page 合计里），
        # 语义不变：仍不进 s/S 分母。
        if extract_narrow_windows(seg):
            return ("narrow_read", seg.strip()[:120])
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

    远端执行豁免（#47，quote-aware 切段 + REMOTE_WRAPPERS）：

    >>> classify_exec("ssh public 'D=/data/x; grep -rl victor $D/src | head'")[0][0]
    'exempt_remote'
    >>> classify_exec('tmux send-keys -t w:0 "grep -ril SECRET ~/x/ | head" Enter')[0][0]
    'exempt_remote'
    >>> [k for k, _ in classify_exec("ssh h 'grep -r x /r/' && grep -rn setStatus src/")]
    ['exempt_remote', 'legacy']
    """
    out = []
    m = CLI_SEARCH_RE.search(cmd)
    if m:
        out.append(("mcp_cli", m.group(0)))
    elif CLI_ANY_RE.search(cmd):
        out.append(("mcp_cli_mgmt", CLI_ANY_RE.search(cmd).group(0)))
    # 按分隔符切段（quote-aware，与 hook split_pipeline 同口径，#47）：
    # 引号/转义内的 ; && || | 不切，`ssh public 'a; grep -r x'` 整体一段。
    segs = []
    buf, prev_sep = "", ""
    q, esc = None, False
    i, n = 0, len(cmd)
    while i < n:
        ch = cmd[i]
        if esc:
            buf += ch; esc = False; i += 1; continue
        if q == "'":
            if ch == "'":
                q = None
            buf += ch; i += 1; continue
        if q == '"':
            if ch == "\\":
                esc = True
            elif ch == '"':
                q = None
            buf += ch; i += 1; continue
        if ch == "\\":
            esc = True; buf += ch; i += 1; continue
        if ch in ("'", '"'):
            q = ch; buf += ch; i += 1; continue
        if cmd[i:i + 2] in ("&&", "||"):
            segs.append((buf, prev_sep)); buf, prev_sep = "", cmd[i:i + 2]; i += 2; continue
        if ch in ";\n":
            segs.append((buf, prev_sep)); buf, prev_sep = "", ch; i += 1; continue
        if ch == "|":
            segs.append((buf, prev_sep)); buf, prev_sep = "", "|"; i += 1; continue
        buf += ch; i += 1
    segs.append((buf, prev_sep))
    if q is not None:  # 未闭合引号：fail-open，整条按单段
        segs = [(cmd, "")]
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
    pending_lat = {}      # call_id -> (call_ts, kind)，等 output 算单 action 耗时
                          # 归属规则：一条多段 cmd 只按第一个搜索段的 kind 记整帧墙钟，
                          # 含帧内非搜索段（如 `cargo test && grep …` 的 build 时间）——
                          # legacy 桶解读时带此 caveat
    latencies = []        # (kind, seconds)  指标 2：call→output 墙钟差
    pending_cmd = {}      # call_id -> 完整原始 cmd（等 output 判 denied 后供指标 3 回放）
    denied_cmds = []      # (ts_iso, cmd, cwd)  指标 3：被拦命令 + 当时 cwd
    narrow_windows = defaultdict(list)   # #54: 相对路径 -> [(lo,hi)]（影子观测）
    narrow_tools = Counter()             # #54: tool -> 次数
    pending_narrow = {}  # #54: call_id -> 事件列表，等 output 确认未被拦截才提交
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
            denied = False
            idxs = pending_legacy.pop(cid, None)
            blocked = "blocked by PreToolUse hook" in str(p.get("output", ""))[:400]
            if idxs and blocked:
                counters["legacy_denied"] += len(idxs)
                counters["legacy"] -= len(idxs)
                denied_idx.update(idxs)
                denied = True
            # #54 窄读影子：只有未被拦截（= 真执行过）才提交窗口形态
            nr_pending = pending_narrow.pop(cid, None)
            if nr_pending:
                if blocked:
                    counters["narrow_read_denied"] += len(nr_pending)
                else:
                    for f, lo, hi, tool in nr_pending:
                        narrow_windows[f].append((lo, hi))
                        narrow_tools[tool] += 1
            # 指标 3：凡被 hook 拦的 exec 帧都入回放桶（不限 metrics 是否判 legacy——
            # hook 的拦截面比 legacy 分类宽，ssh/噪声形态也要算误判率分母）
            dcmd = pending_cmd.pop(cid, None)
            if blocked and dcmd is not None:
                denied_cmds.append((d.get("timestamp", ""), dcmd, cwd))
            lat = pending_lat.pop(cid, None)
            if lat is not None and not denied:
                t0, lkind = lat
                ots = parse_ts(d.get("timestamp", ""))
                if ots is not None:
                    dt = (ots - t0).total_seconds()
                    if 0 <= dt <= LATENCY_MAX_S:
                        latencies.append((lkind, dt))
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
                if cid:
                    pending_lat[cid] = (ts, "mcp")
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
        if cid:
            # 指标 3：所有 exec 帧都留底——hook 拦截面比 legacy 分类宽
            # （ssh/噪声形态 metrics 不判 legacy 但也会被拦），output 时弹出
            pending_cmd.setdefault(cid, cmd)
        # #54 影子观测：窗口形态与判定分离——无论 classify_exec 归到哪个桶，
        # 只要抽出窄窗读事件就记形态，供 redundancy 聚合。不影响任何计分。
        # 但**必须等 output 确认未被 hook 拦截**才提交：被拦的命令根本没执行，
        # 计入会污染事件总数/覆盖行数/冗余（与 legacy_denied 同一法理）。
        nr_ev = extract_narrow_windows(cmd, cwd)
        if nr_ev and cid:
            pending_narrow[cid] = nr_ev
        elif nr_ev:
            for f, lo, hi, tool in nr_ev:
                narrow_windows[f].append((lo, hi))
                narrow_tools[tool] += 1
        for kind, detail in classify_exec(cmd):
            if kind == "mcp_cli":
                counters["mcp_cli"] += 1
                calls.append((ts, "mcp", f"[cli直调] {detail}"))
                if cid:
                    pending_lat.setdefault(cid, (ts, "mcp"))
            elif kind == "mcp_cli_mgmt":
                counters["mcp_cli_mgmt"] += 1
            elif kind == "legacy":
                counters["legacy"] += 1
                calls.append((ts, "legacy", detail))
                if cid:
                    pending_legacy.setdefault(cid, []).append(len(calls) - 1)
                    pending_lat.setdefault(cid, (ts, "legacy"))
            else:
                counters[kind] += 1
    # #54：会话结束仍未见 output 的窄读（日志截断/会话中断）按「已执行」提交——
    # 与 legacy 同口径：只有明确看到 hook 拦截字样才算未执行。
    for ev in pending_narrow.values():
        for f, lo, hi, tool in ev:
            narrow_windows[f].append((lo, hi))
            narrow_tools[tool] += 1
    if denied_idx:
        calls = [c for i, c in enumerate(calls) if i not in denied_idx]
    return {"sid": sid, "cwd": cwd, "calls": sorted(calls), "counters": counters,
            "latencies": latencies, "denied_cmds": denied_cmds,
            "narrow_windows": dict(narrow_windows), "narrow_tools": narrow_tools}


# ---------------- 指标 3：hook 误判率（denied 回放） ----------------
HOOK_PATH = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                         "hooks", "grep-intercept.py")


def replay_denied(denied, hook_path=None, timeout=REPLAY_TIMEOUT_S, limit=REPLAY_MAX):
    """把窗口内被拦命令逐条喂当前版本 hook + classify_exec 双判据，统计误拦率。

    FP（进 rate）= 回放 allow。classifier_disagree（不进 rate，诊断用）=
    回放仍 deny 但 metrics classify_exec 不判 legacy 的条数（候选新 FP 形态）。
    返回 dict：n_denied / replayed / fp / tp / classifier_disagree /
    errors（回放失败不计分母）/ rate（fp/replayed）/ samples。
    """
    res = {"n_denied": len(denied), "replayed": 0, "fp": 0, "tp": 0,
           "classifier_disagree": 0,
           "errors": 0, "rate": None, "target": FP_TARGET, "samples": []}
    hook = hook_path or HOOK_PATH
    if not denied:
        return res
    hook_ok = os.path.exists(hook)
    for ts, cmd, cwd in denied[:limit]:
        if hook_ok:
            payload = {"tool_name": "Bash", "tool_input": {"command": cmd}}
            if cwd:
                payload["cwd"] = cwd
            try:
                r = subprocess.run([sys.executable, hook], input=json.dumps(payload),
                                   capture_output=True, text=True, timeout=timeout)
                deny = '"deny"' in (r.stdout or "") or "'deny'" in (r.stdout or "")
            except Exception:
                res["errors"] += 1
                continue
        else:
            res["errors"] += 1
            continue
        res["replayed"] += 1
        if not deny:
            res["fp"] += 1
            if len(res["samples"]) < 8:
                res["samples"].append({"ts": ts, "cwd": cwd, "cmd": cmd[:160]})
        else:
            res["tp"] += 1
            if not any(k == "legacy" for k, _ in classify_exec(cmd)):
                res["classifier_disagree"] += 1
    if res["replayed"]:
        res["rate"] = res["fp"] / res["replayed"]
    return res


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
    ap.add_argument("--no-replay", action="store_true",
                    help="跳过指标 3 denied 回放（回放要逐条起 hook 子进程，量大时可关）")
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

    # 指标 2：每 action 耗时（call→output，denied 已在 parse_session 内剔除）
    lat_mcp = [dt for s in sessions for k, dt in s.get("latencies", []) if k == "mcp"]
    # ---- #54 窄窗读影子观测（只统计，绝不入 s/S 分母）----
    nr_rows = []          # 每 (会话,文件) 一条
    nr_tools = Counter()
    for s in sessions:
        nr_tools += s.get("narrow_tools", Counter())
        for f, wins in s.get("narrow_windows", {}).items():
            red = redundancy(wins)
            merged = merge_adjacent(wins)
            nr_rows.append({
                "sid": s["sid"][:44], "file": f,
                "windows": len([w for w in wins if w[0] is not None]),
                "merged": len(merged),
                "redundancy": red,
                "coverage_lines": sum(hi - lo + 1 for lo, hi in merged),
                "mcp": s["counters"]["mcp_frame"] + s["counters"]["mcp_cli"],
            })
    nr_scored = [r for r in nr_rows if r["redundancy"] is not None]
    reds = [r["redundancy"] for r in nr_scored]
    wcounts = [r["windows"] for r in nr_scored]
    narrow_shadow = {
        "MODE": "shadow (observe only, never scored)",
        "events_total": sum(nr_tools.values()),
        "events_by_tool": dict(nr_tools),
        # 被 hook 拦下、未真执行的窄读单列（不计入 events_total）
        "events_denied": sum(s["counters"]["narrow_read_denied"] for s in sessions),
        "parse_degraded": {"unclosed_quote": NR_PARSE_DEGRADED["unclosed_quote"],
                           "samples": NR_PARSE_DEGRADED["samples"][:3]},
        "session_file_pairs": len(nr_rows),
        "sessions_with_narrow_read": sum(
            1 for s in sessions if s.get("narrow_windows")),
        "window_count": {
            "P50": pctl(wcounts, .5), "P90": pctl(wcounts, .9),
            "P99": pctl(wcounts, .99),
            "dist": {("≥5" if min(w, 5) == 5 else str(w)): c
                     for w, c in sorted(Counter(min(w, 5) for w in wcounts).items())},
        },
        "redundancy": {
            "P50": pctl(reds, .5), "P90": pctl(reds, .9), "P99": pctl(reds, .99),
            "share_healthy_le_2": (round(sum(1 for r in reds if r <= 2) / len(reds), 4)
                                   if reds else None),
            "dist": {"1.0": sum(1 for r in reds if r == 1.0),
                     "1.0-2": sum(1 for r in reds if 1.0 < r <= 2),
                     "2-5": sum(1 for r in reds if 2 < r <= 5),
                     ">5": sum(1 for r in reds if r > 5)},
        },
        "top_thinned_reads": sorted(nr_scored, key=lambda r: -r["redundancy"])[:10],
    }

    lat_leg = [dt for s in sessions for k, dt in s.get("latencies", []) if k == "legacy"]

    def lat_stats(xs):
        if not xs:
            return {"n": 0, "mean_s": None, "p50_s": None, "p90_s": None, "max_s": None}
        return {"n": len(xs), "mean_s": round(sum(xs) / len(xs), 3),
                "p50_s": round(pctl(xs, .5), 3), "p90_s": round(pctl(xs, .9), 3),
                "max_s": round(max(xs), 3)}

    # 指标 3：hook 误判率（denied 回放，--no-replay 可跳过）
    all_denied = [dc for s in sessions for dc in s.get("denied_cmds", [])]
    if args.no_replay:
        fp_replay = {"n_denied": len(all_denied), "replayed": 0, "fp": 0, "tp": 0,
                     "classifier_disagree": 0,
                     "errors": 0, "rate": None, "target": FP_TARGET,
                     "samples": [], "skipped": True}
    else:
        fp_replay = replay_denied(all_denied)

    report = {
        "window": {"since": since.isoformat(), "until": until.isoformat(),
                   "hours": args.hours},
        "s_cmd_level": {"value": s_val, "N_mcp": N_mcp,
                        "N_mcp_daemon_search": len(d_search), "N_mcp_cli": n_cli,
                        "N_legacy": N_legacy, "N_legacy_denied": N_denied},
        "S_episode_level": {"value": S_val, "E_mcp": E_mcp, "E_legacy": E_leg,
                            "episodes": len(all_eps)},
        "action_latency": {"mcp": lat_stats(lat_mcp), "legacy": lat_stats(lat_leg),
                           "note": f"call→output 墙钟（agent 视角）；denied 剔除；"
                                   f">{LATENCY_MAX_S}s 异常帧丢弃"},
        "hook_fp_rate": dict(
            fp_replay,
            note=f"窗口内 denied 按当前 hook 回放，allow=误拦(FP)；"
                 f"目标 <{FP_TARGET*100:.0f}%（#47/#48/#49 修复后）"),
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
        "narrow_read_shadow": narrow_shadow,
        "exempt_totals": {
            "page": sum(s["counters"]["exempt_page"] + s["counters"]["sed_page"]
                        + s["counters"]["narrow_read"] for s in sessions),
            "noncode": sum(s["counters"]["exempt_noncode"] for s in sessions),
            "filter": sum(s["counters"]["exempt_filter"] for s in sessions),
            "remote": sum(s["counters"]["exempt_remote"] for s in sessions)},
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
        nr = w["narrow_read_shadow"]
        if nr["events_total"]:
            print(f"\n## 窄窗读观测（影子模式 · 不判罚 · 不入 s/S 分母）")
            print(f"   事件 {nr['events_total']} 次"
                  f"（{', '.join(f'{k} {v}' for k, v in sorted(nr['events_by_tool'].items(), key=lambda kv: -kv[1]))}）"
                  f"，覆盖 {nr['sessions_with_narrow_read']} 会话 / "
                  f"{nr['session_file_pairs']} 会话×文件")
            rd = nr["redundancy"]
            if rd["share_healthy_le_2"] is not None:
                print(f"   读冗余倍率（窗口数/合并后段数，1.0=无冗余）："
                      f"P50={rd['P50']} P90={rd['P90']} P99={rd['P99']}"
                      f"，健康(≤2) 占 {rd['share_healthy_le_2']*100:.1f}%")
                print(f"     分布 " + "  ".join(f"{k}:{v}" for k, v in rd["dist"].items()))
            wc = nr["window_count"]
            print(f"   同文件窗口数：P50={wc['P50']} P90={wc['P90']} P99={wc['P99']}"
                  f"   分布 " + "  ".join(f"{k}窗:{v}" for k, v in wc["dist"].items()))
            if nr["top_thinned_reads"]:
                print("   Top 摊薄读（高冗余；mcp 列 >0 说明索引用得好、读侧照样摊薄）：")
                for r in nr["top_thinned_reads"][:5]:
                    print(f"     {r['redundancy']:>6}x  {r['windows']:>3}窗→{r['merged']}段 "
                          f"覆盖{r['coverage_lines']:>5}行  mcp={r['mcp']:<3} "
                          f"{r['file'][:28]:<28} {r['sid'][:34]}")
            print("   ⚠ 影子模式：本段只观测，阈值未定。prior art（GCP dry-run）建议"
                  "观测 ≥2 周再谈判定；告警须走独立渠道。")
        al = w["action_latency"]

        def lfmt(b):
            if not b["n"]:
                return "n=0"
            return (f"n={b['n']} mean={b['mean_s']:.2f}s "
                    f"p50={b['p50_s']:.2f}s p90={b['p90_s']:.2f}s max={b['max_s']:.1f}s")
        print(f"## 指标2 每 action 耗时（call→output）  "
              f"mcp: {lfmt(al['mcp'])}  |  legacy: {lfmt(al['legacy'])}")
        fpq = w["hook_fp_rate"]
        if fpq.get("skipped"):
            print(f"## 指标3 hook 误判率 = 跳过（--no-replay），denied {fpq['n_denied']} 条")
        else:
            mark = ""
            if fpq["rate"] is not None:
                mark = "  ✅达标" if fpq["rate"] < fpq["target"] else "  ⚠️超标"
            print(f"## 指标3 hook 误判率 = {fmt(fpq['rate'])}（目标 <{fpq['target']*100:.0f}%）"
                  f"{mark}  denied={fpq['n_denied']} 回放={fpq['replayed']} "
                  f"FP={fpq['fp']} 仍拦={fpq['tp']} 回放失败={fpq['errors']}；"
                  f"分类器异议 {fpq['classifier_disagree']}（诊断线索，不进 rate）")
            for smp in fpq["samples"]:
                print(f"      FP样本 {smp['ts'][:19]} :: {smp['cmd'][:110]}")
        sl = w["slot"]
        print(f"## slot 加载率 = {fmt(sl['rate'])}  ({sl['loaded']}/{sl['active_sessions']} 个有检索活动的会话)")
        d = w["daemon"]
        print(f"\n## daemon 侧：{d['rows']} 行，检索 tools/call {d['search_calls']}"
              f"（错误 {d['errors']}），管理面 {d['mgmt']}；"
              f"p50={d['p50_ms']}ms p95={d['p95_ms']}ms；按工具 {d['by_tool']}")
        r = w["reconcile"]
        print(f"## 对账：jsonl MCP 帧 {r['jsonl_mcp_frames']} vs daemon 检索 {r['daemon_search_calls']}（{r['note']}）")
        print(f"## 豁免剔除：页内精定位 {w['exempt_totals']['page']}，"
              f"非代码文本 {w['exempt_totals']['noncode']}，管道过滤 {w['exempt_totals']['filter']}，"
              f"远端执行 {w['exempt_totals']['remote']}")
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
