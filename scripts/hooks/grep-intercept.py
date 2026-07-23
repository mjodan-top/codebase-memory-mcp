#!/usr/bin/env python3
"""PreToolUse hook — 拦截跨文件 grep/rg/sed 代码扫射，重定向到 codebase-memory MCP 索引。

判定规则与 metrics 设计（mcp-adoption-metrics-design）一致：
- 拦：grep/rg 递归/多文件/glob 扫代码目录（找定义/调用方式的跨文件扫射）
- 豁免：单文件页内精定位；目标为非代码文本(.log/.toml/.json/.md 等)；纯管道过滤（grep 无文件参数）；
  项目外目标（解析后不在任何 git 仓库内，如 ~/.local/state/**——索引不覆盖，MCP 替代不了）；
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

# 记忆/会话档根（#48）：这些目录可能自带 .git（版本化只为审计），
# 「是 git 仓」不等于「是代码仓」——按第一性（索引不覆盖 + 非代码）显式豁免。
NONCODE_ROOTS = (
    "~/.agents/memory", "~/.coder/memory", "~/.coder/subagent-archive",
    "~/.claude/projects",
)

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


def deny(cmd, cwd=""):
    out = {
        "hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": "deny",
            "permissionDecisionReason": REDIRECT,
        }
    }
    log({"decision": "deny", "command": cmd, "cwd": cwd})
    print(json.dumps(out, ensure_ascii=False))
    sys.exit(0)


def resolve_target(tok, cwd):
    """展开 ~ / $VAR，相对路径按 cwd 拼接；无法确定则返回 None。"""
    t = os.path.expandvars(os.path.expanduser(tok))
    if "$" in t:
        return None  # 未定义变量展不开，无法判定
    if not os.path.isabs(t):
        if not cwd:
            return None
        t = os.path.join(cwd, t)
    return os.path.normpath(t)


def is_outside_git_repo(path):
    """路径存在且向上走到根都无 .git → 在任何 git 仓库之外（索引不覆盖）。

    不存在的路径返回 False（无法确认，走常规判定）。
    先 realpath 解 symlink：经仓外符号链接指进仓内的路径不得豁免。

    已知折衷：只向上爬 `.git`，不向下扫子目录——目标目录本身若是某仓的
    祖先目录（如 `grep -rn /tmp/` 而仓在 `/tmp/x/`），仍会豁免；索引按项目
    根注册，此形态极罕见且 metrics 端同口径，接受。
    """
    p = os.path.realpath(path)
    if not os.path.exists(p):
        return False
    if not os.path.isdir(p):
        p = os.path.dirname(p) or "/"
    while True:
        if os.path.exists(os.path.join(p, ".git")):
            return False
        parent = os.path.dirname(p)
        if parent == p:
            return True
        p = parent


def is_noncode_target(tok):
    t = tok.lower()
    for ext in NONCODE_EXT:
        if t.endswith(ext):
            return True
    # 日志/配置/记忆/会话档目录关键词（#48 补记忆仓与会话档形态）
    if any(k in t for k in ("/logs/", "daemon.err", "/log/", ".codex", ".coder",
                            "/memory/", ".agents/memory", "/sessions/",
                            "subagent-archive", ".claude/projects",
                            "history.jsonl")):
        return True
    # 文档目录（docs 树 = markdown 文档，非代码，#48）
    d = t.rstrip("/")
    if d == "docs" or d.endswith("/docs") or t.startswith("docs/") or "/docs/" in t:
        return True
    return False


def in_noncode_root(path):
    """解析后的绝对路径落在记忆/会话档根内（#48）。先 realpath 防 symlink 绕过。"""
    p = os.path.realpath(path)
    for root in NONCODE_ROOTS:
        r = os.path.realpath(os.path.expanduser(root))
        if p == r or p.startswith(r + os.sep):
            return True
    return False


# 命令内静态赋值（#48）：`hf="$h/history.jsonl"; grep -n x "$hf"` 的 $hf
# 目标在同一条命令里有字面值，做纯文本代换后再判 noncode/ext——不执行任何
# 命令替换（ShellCheck 同款静态边界），解不开仍从严。
ASSIGN_RE = re.compile(
    r"(?:^|[;&|(]\s*|\b(?:do|then|else)\s+|\s)"
    r"([A-Za-z_][A-Za-z0-9_]*)="
    r"(\"[^\"]*\"|'[^']*'|\$\([^)]*\)|[^\s;|&]+)")

VAR_RE = re.compile(r"\$(?:\{([A-Za-z_][A-Za-z0-9_]*)\}|([A-Za-z_][A-Za-z0-9_]*))")


def collect_assignments(command):
    env = {}
    for m in ASSIGN_RE.finditer(command):
        val = m.group(2)
        if val[:1] in "\"'" and val[-1:] == val[:1]:
            val = val[1:-1]
        env[m.group(1)] = val
    return env


def expand_assigned(tok, env):
    """用命令内赋值做文本代换（最多 3 层，防自引用循环）。"""
    if not env or "$" not in tok:
        return tok
    for _ in range(3):
        new = VAR_RE.sub(
            lambda m: env.get(m.group(1) or m.group(2), m.group(0)), tok)
        if new == tok:
            break
        tok = new
    return tok


def split_pipeline(command):
    """quote-aware 两级切分：返回 (segment, is_pipeline_head) 列表。

    `&&` / `;` / `||` 切出的是相互独立的命令组（`cd x && grep …` 的 grep
    仍是新命令的首段），每组首段都要重新走拦截判定；仅组内 `|` 切出的
    非首段才算「管道中游过滤」可放行。

    引号/转义内的分隔符不切（#47 根因修复）：`ssh public 'a; grep -r x'`
    的 `;` 是 ssh 参数字符串的一部分，切开会把远端 grep 误判成本地段。
    口径同 stdlib shlex punctuation_chars 的引号保护语义（"different quote
    types protect each other as in the shell"），此处手写扫描以保留段原文。
    未闭合引号 → fail-open：整条按单段返回（hook 决不能崩成阻断）。
    """
    parts = []  # (text, sep_before)
    buf = []
    sep = ""
    q = None      # 当前引号态: None / "'" / '"'
    esc = False   # 上一字符是反斜杠（裸态或双引号内）
    i, n = 0, len(command)
    while i < n:
        ch = command[i]
        if esc:
            buf.append(ch)
            esc = False
            i += 1
            continue
        if q == "'":
            if ch == "'":
                q = None
            buf.append(ch)
            i += 1
            continue
        if q == '"':
            if ch == "\\":
                esc = True
            elif ch == '"':
                q = None
            buf.append(ch)
            i += 1
            continue
        if ch == "\\":
            esc = True
            buf.append(ch)
            i += 1
            continue
        if ch in ("'", '"'):
            q = ch
            buf.append(ch)
            i += 1
            continue
        if command[i:i + 2] in ("&&", "||"):
            parts.append(("".join(buf), sep))
            buf, sep = [], command[i:i + 2]
            i += 2
            continue
        if ch in ";\n":
            parts.append(("".join(buf), sep))
            buf, sep = [], ";"
            i += 1
            continue
        if ch == "|":
            parts.append(("".join(buf), sep))
            buf, sep = [], "|"
            i += 1
            continue
        buf.append(ch)
        i += 1
    parts.append(("".join(buf), sep))
    if q is not None:
        return [(command.strip(), True)]
    return [(t.strip(), s != "|") for t, s in parts if t.strip()]


GREP_RE = re.compile(r"^(grep|rg|egrep|fgrep)$")

# 远端执行包装：段首是这些程序时，其参数串是远端 shell 的输入（ShellCheck
# SC2029 同法理），远端文件本地索引零覆盖 → 整段放行（issue #47）。
# quote-aware 切分后段首本就是 ssh/tmux（自然走非 grep 放行），此集合作为
# 防守层显式声明语义，防未来切分逻辑变化回退。
REMOTE_WRAPPERS = {"ssh", "mosh", "et", "autossh", "tmux"}

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


def _glob_prefix_dir(tok):
    """glob token 取无通配前缀目录（`~/x/*.py` → `~/x`）。"""
    m = re.search(r"[*?\[]", tok)
    head = tok[: m.start()] if m else tok
    return head.rsplit("/", 1)[0] if "/" in head else ""


def targets_all_unindexed(file_args, cwd):
    """全部目标都能解析且都在索引覆盖之外 → True（放行）。

    「之外」= 不在任何 git 仓库内（PR #44），或落在记忆/会话档根
    NONCODE_ROOTS 内（#48：记忆仓可能自带 .git，「是 git 仓」≠「是代码仓」）。
    """
    if not file_args:
        return False
    for f in file_args:
        tok = _glob_prefix_dir(f) if any(ch in f for ch in "*?[") else f
        if not tok:
            return False
        p = resolve_target(tok, cwd)
        if p is None:
            return False
        if not is_outside_git_repo(p) and not in_noncode_root(p):
            return False
    return True


def analyze_segment(seg, is_first, cwd="", env=None):
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

    if prog in REMOTE_WRAPPERS:
        return "allow"  # 远端执行：目标不在本地索引覆盖内（#47）

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

    # 文件参数（pattern 之后）；命令内静态赋值先做文本代换（#48：
    # `hf="$h/history.jsonl"; grep -n x "$hf"` 的 $hf 有同命令字面值）
    file_args = positional[1:] if positional else []
    if env:
        file_args = [expand_assigned(f, env) for f in file_args]

    # --include=GLOB 把匹配面完全限定（GNU grep 语义）：全部 include glob
    # 都是非代码扩展名 → 内容面为非代码文本，放行（#48）
    include_globs = [f.split("=", 1)[1] for f in flags
                     if f.startswith("--include=") and "=" in f]
    if include_globs and all(
            os.path.splitext(g)[1].lower() in NONCODE_EXT for g in include_globs):
        return "allow"

    if not file_args:
        # 无文件参数：grep 是读 stdin（管道过滤，放行）；rg 是递归扫 cwd（拦，除非上面已判非首段）
        if is_rg or recursive:
            # cwd 本身在索引覆盖之外（仓外 / 记忆根）→ 放行
            if cwd and os.path.isdir(cwd) and (
                    is_outside_git_repo(cwd) or in_noncode_root(cwd)):
                return "allow"
            return "deny"
        return "allow"

    # 全部目标是非代码文本 → 放行
    if all(is_noncode_target(f) for f in file_args):
        return "allow"

    # 全部目标解析后都在索引覆盖之外（仓外 ~/.local/state/**、记忆/会话档根）→ 放行
    if targets_all_unindexed(file_args, cwd):
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
    cwd = payload.get("cwd", "") or (cmd.get("cwd", "") if isinstance(cmd, dict) else "")

    env = collect_assignments(command)
    for seg, is_head in split_pipeline(command):
        # 追踪链内 `cd <path>`：后续段的相对路径/rg-无参按新 cwd 判定。
        # #48：先剥重定向再解析（`cd X 2>/dev/null || cd …` fallback 链），
        # 支持 `cd -- <path>` 与带引号路径；参数含 $UNDEF / $(…) 命令替换
        # 等解析不了 → cwd 置空从严：陈旧仓外 cwd 不得给后续段背书。
        if re.match(r"^cd(\s|$)", seg):
            try:
                ctoks = strip_redirects(shlex.split(seg))
            except ValueError:
                ctoks = []
            cargs = [t for t in ctoks[1:] if t != "--"]
            if len(cargs) == 1:
                cwd = resolve_target(expand_assigned(cargs[0], env), cwd) or ""
            else:
                cwd = ""
            continue
        if analyze_segment(seg, is_first=is_head, cwd=cwd, env=env) == "deny":
            deny(command, cwd)
    log({"decision": "allow", "command": command})
    allow()


if __name__ == "__main__":
    main()
