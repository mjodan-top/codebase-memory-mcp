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


def main():
    failed = 0
    for command, expect_deny in CASES:
        got = run_case(command)
        status = "ok" if got == expect_deny else "FAIL"
        if got != expect_deny:
            failed += 1
        print(f"[{status}] deny={got} expect={expect_deny} :: {command}")
    print(f"\n{len(CASES) - failed}/{len(CASES)} passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
