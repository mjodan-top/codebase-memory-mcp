#!/usr/bin/env python3
"""grep-intercept.py 的分类断言单测（10 个样例覆盖拦/豁免边界）。

跑法: python3 scripts/hooks/grep_intercept_test.py
"""
import json
import os
import subprocess
import sys

HOOK = os.path.join(os.path.dirname(os.path.abspath(__file__)), "grep-intercept.py")

# (command, expect_deny)
CASES = [
    # 跨文件扫射 → 拦
    ('grep -rn "setStatus" src/', True),
    ('rg "GOAL_TRANSITIONS" services/', True),
    ("rg setStatus", True),  # rg 无文件参数默认递归
    ('grep -rn "budget" --include=*.mjs .', True),
    ("grep -c foo *.mjs", True),  # glob 多文件
    # 页内精定位 → 放行
    ('grep -n "close" services/matter-service/store.mjs', False),
    # 非代码文本 → 放行
    ('grep "tools/call" ~/Library/Logs/codebase-memory-mcp/daemon.err', False),
    # 管道过滤 → 放行
    ("ls -la | grep foo", False),
    ("cat file.mjs | grep close", False),
    # 非 grep 命令 → 放行
    ('sed -n "500,560p" services/matter-service/store.mjs', False),
    # 链式命令：&& / ; / || 切出的是新命令组，组首段 grep 照拦
    ('cd src && grep -rn setStatus .', True),
    ("make build; rg foo", True),
    ('true || grep -rn "close" services/', True),
    # 链式组内首段拦截不受管道影响：首段仍 deny
    ("grep -rn foo src/ | head", True),
    # 链式组内的管道中游 grep 仍放行
    ("cd src && ls -la | grep foo", False),
    # --- FP 修复回归（带值标志 / 重定向 / $VAR / glob-noncode） ---
    # -A/-B/-C 的值不是第二个文件：单文件页内精定位照旧放行
    ('grep -n "NONCODE_PATH_HINTS" -A 6 scripts/metrics/mcp-adoption.py', False),
    # 链式里组内 grep 段同样不受 -A 值误判影响
    ("sed -n '135,240p' scripts/metrics/mcp-adoption.py; echo ===; "
     'grep -n "HINTS" -A 6 scripts/metrics/mcp-adoption.py', False),
    # 重定向 token 不是文件参数：单文件 + 2>/dev/null 放行
    ("grep -n close services/matter-service/store.mjs 2>/dev/null", False),
    # $VAR 前缀路径按 basename 扩展名判 noncode（.md）放行；-h/-i 无参标志不吃 pattern
    ("grep -i -h postreview $MEM/MEMORY.md 2>/dev/null", False),
    # glob 展开目标全是 .md noncode：按扩展名豁免；-l 无参标志不吃 pattern
    ("grep -l -i postreview $MEM/*.md 2>/dev/null", False),
    # 裸重定向符 `>` 的目标 token 也要跳过
    ("grep -n foo file.md > /tmp/out.txt", False),
    # 带值标志修复不放松拦截：-A 值后还有代码 glob 多文件照拦
    ('grep -n "x" -A 3 src/a.c src/b.c', True),
    # 重定向剥离后仍是递归扫代码 → 照拦
    ("grep -rn setStatus src/ 2>/dev/null", True),
]


def run_case(command):
    payload = {"tool_name": "Bash", "tool_input": {"command": command}}
    out = subprocess.run(
        [sys.executable, HOOK],
        input=json.dumps(payload),
        capture_output=True,
        text=True,
        timeout=10,
    )
    body = json.loads(out.stdout)
    decision = (
        body.get("hookSpecificOutput", {}).get("permissionDecision")
        if isinstance(body, dict)
        else None
    )
    return decision == "deny"


def run_raw(stdin_text):
    """原始 stdin 直调 hook，返回 (exit_code, stdout)。"""
    out = subprocess.run(
        [sys.executable, HOOK],
        input=stdin_text,
        capture_output=True,
        text=True,
        timeout=10,
    )
    return out.returncode, out.stdout.strip()


def main():
    failed = 0
    for command, expect_deny in CASES:
        got = run_case(command)
        status = "ok" if got == expect_deny else "FAIL"
        if got != expect_deny:
            failed += 1
        print(f"[{status}] deny={got} expect={expect_deny} :: {command}")

    # 健壮性：合法 JSON 但非 dict → fail-open（exit 0 + "{}"）
    rc, stdout = run_raw("[1,2,3]")
    ok = rc == 0 and stdout == "{}"
    if not ok:
        failed += 1
    print(f"[{'ok' if ok else 'FAIL'}] fail-open rc={rc} stdout={stdout!r} :: non-dict JSON [1,2,3]")

    total = len(CASES) + 1
    print(f"\n{total - failed}/{total} passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
