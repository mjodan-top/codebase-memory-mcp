#!/usr/bin/env python3
"""PreToolUse hook — 拦截跨文件 grep/rg/sed 代码扫射，重定向到 codebase-memory MCP 索引。

判定规则与 metrics 设计（mcp-adoption-metrics-design）一致：
- 拦：grep/rg 递归/多文件/glob 扫代码目录（找定义/调用方式的跨文件扫射）
- 豁免：单文件页内精定位；目标为非代码文本(.log/.toml/.json/.md 等)；纯管道过滤（grep 无文件参数）；
  ls/find/cat 等非 grep 命令一律放行。
输出协议：stdout JSON（pre-tool-use.command.output schema），deny 走
hookSpecificOutput.permissionDecision="deny" + permissionDecisionReason。
"""
import json
import os
import re
import shlex
import sys

LOG = os.environ.get("CBM_HOOK_LOG", "")

NONCODE_EXT = {
    ".log", ".toml", ".json", ".jsonl", ".md", ".txt", ".yml", ".yaml",
    ".lock", ".cfg", ".ini", ".err", ".out", ".csv",
}

REDIRECT = (
    "grep/rg 跨文件扫代码被仓库纪律拦截：请改用 codebase-memory MCP 索引 — "
    "先调用 tool_search 拉取 codebase-memory 工具（如果工具面还没有），"
    "然后用 search_graph(找定义/符号)、search_code(全文检索)、get_code_snippet(按 qualified_name 取源码)、"
    "trace_path(查调用链)。拿到目标文件后允许对该单文件用 grep/sed 做页内精定位。"
)


def log(entry):
    if not LOG:
        return
    try:
        with open(LOG, "a") as f:
            f.write(json.dumps(entry, ensure_ascii=False) + "\n")
    except OSError:
        pass


def allow():
    print("{}")
    sys.exit(0)


def deny(cmd):
    out = {
        "hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": "deny",
            "permissionDecisionReason": REDIRECT,
        }
    }
    log({"decision": "deny", "command": cmd})
    print(json.dumps(out, ensure_ascii=False))
    sys.exit(0)


def is_noncode_target(tok):
    t = tok.lower()
    for ext in NONCODE_EXT:
        if t.endswith(ext):
            return True
    # 日志/配置目录关键词
    if any(k in t for k in ("/logs/", "daemon.err", "/log/", ".codex", ".coder")):
        return True
    return False


def split_pipeline(command):
    """两级切分：返回 (segment, is_pipeline_head) 列表。

    `&&` / `;` / `||` 切出的是相互独立的命令组（`cd x && grep …` 的 grep
    仍是新命令的首段），每组首段都要重新走拦截判定；仅组内 `|` 切出的
    非首段才算「管道中游过滤」可放行。
    """
    out = []
    for group in re.split(r"\|\||&&|;", command):
        stages = [s.strip() for s in group.split("|") if s.strip()]
        for j, seg in enumerate(stages):
            out.append((seg, j == 0))
    return out


GREP_RE = re.compile(r"^(grep|rg|egrep|fgrep)$")

# 带值标志：`-A 6` 的 `6` 是标志的值，不是文件/pattern（与 metrics 的
# GREP_NOFILE_OK 同一套口径）。`--include=*.mjs` 的 `=` 形式自带值，不吃下一个 token。
VALUED_FLAGS = {
    "-e", "--regexp", "-f", "--file", "-m", "--max-count", "-A", "-B", "-C",
    "-g", "--glob", "--iglob", "-t", "--type", "-T", "--type-not",
    "--include", "--exclude", "--exclude-dir", "--color", "-d",
    "--after-context", "--before-context", "--context",
}

# 重定向 token：`2>/dev/null`、`>out`、`2>&1`、`<in`；裸 `>` / `2>` / `<` 的
# 目标是下一个 token，需一并跳过。
REDIR_RE = re.compile(r"^\d*(?:>>|>|<)\S*$")


def strip_redirects(toks):
    out = []
    skip = False
    for t in toks:
        if skip:
            skip = False
            continue
        if REDIR_RE.match(t):
            if re.match(r"^\d*(?:>>|>|<)$", t):
                skip = True  # 裸重定向符，目标在下一个 token
            continue
        out.append(t)
    return out


def analyze_segment(seg, is_first):
    """返回 'deny' / 'allow'。"""
    try:
        toks = shlex.split(seg)
    except ValueError:
        toks = seg.split()
    toks = strip_redirects(toks)
    if not toks:
        return "allow"
    # 跳过 env 赋值与常见包装
    i = 0
    while i < len(toks) and ("=" in toks[i] and not toks[i].startswith("-")):
        i += 1
    if i < len(toks) and toks[i] in ("command", "sudo", "timeout", "xargs"):
        i += 1
        # timeout 的秒数参数
        if i < len(toks) and re.match(r"^\d", toks[i]):
            i += 1
    if i >= len(toks):
        return "allow"
    prog = toks[i].rsplit("/", 1)[-1]
    args = toks[i + 1:]

    if not GREP_RE.match(prog):
        return "allow"

    # 管道中游的 grep（非第一段）= 过滤已有输出，放行
    if not is_first:
        return "allow"

    flags, positional = [], []
    skip_next = False
    for a in args:
        if skip_next:
            skip_next = False
            continue
        if a.startswith("-") and a != "-":
            flags.append(a)
            if a in VALUED_FLAGS:
                skip_next = True  # 带值标志：下一个 token 是它的值
            continue
        positional.append(a)

    recursive = any(re.match(r"^-[a-zA-Z]*[rR]", f) or f == "--recursive" for f in flags) or any(
        f.startswith("--include") or f.startswith("--exclude") for f in flags
    )
    # rg 默认递归：rg pattern [path...]，无文件或给目录都算递归
    is_rg = prog == "rg"

    # 文件参数（pattern 之后）
    file_args = positional[1:] if positional else []

    if not file_args:
        # 无文件参数：grep 是读 stdin（管道过滤，放行）；rg 是递归扫 cwd（拦，除非上面已判非首段）
        if is_rg or recursive:
            return "deny"
        return "allow"

    # 全部目标是非代码文本 → 放行
    if all(is_noncode_target(f) for f in file_args):
        return "allow"

    # 单个具体文件（无通配、非目录样式）→ 页内精定位，放行
    if (
        len(file_args) == 1
        and not recursive
        and not any(ch in file_args[0] for ch in "*?[")
        and "." in file_args[0].rsplit("/", 1)[-1]
    ):
        return "allow"

    # 递归 / 多文件 / glob / 目录 → 跨文件扫射，拦
    return "deny"


def main():
    try:
        payload = json.load(sys.stdin)
    except Exception:
        allow()
    if not isinstance(payload, dict):
        # 合法 JSON 但非对象（如 [1,2,3]）→ fail-open 放行
        allow()
    tool = payload.get("tool_name", "")
    if tool != "Bash":
        allow()
    cmd = payload.get("tool_input", {})
    command = cmd.get("command", "") if isinstance(cmd, dict) else ""
    if not command:
        allow()

    for seg, is_head in split_pipeline(command):
        if analyze_segment(seg, is_first=is_head) == "deny":
            deny(command)
    log({"decision": "allow", "command": command})
    allow()


if __name__ == "__main__":
    main()
