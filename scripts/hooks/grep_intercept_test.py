#!/usr/bin/env python3
"""grep-intercept.py 的分类断言单测（10 个样例覆盖拦/豁免边界）。

跑法: python3 scripts/hooks/grep_intercept_test.py
"""
import json
import os
import subprocess
import sys

HOOK = os.path.join(os.path.dirname(os.path.abspath(__file__)), "grep-intercept.py")


def _ensure_issue49_fixture():
    """#49 回放样本依赖的 /tmp 目录结构（易失）：缺则重建最小骨架。"""
    for d in ("/tmp/pr4868/run/services/project-service",):
        os.makedirs(d, exist_ok=True)


_ensure_issue49_fixture()

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
    # --- 项目外目标豁免（不在任何 git 仓库内 = 索引不覆盖，MCP 替代不了） ---
    # 绝对路径目录在仓库外：递归 grep 放行
    ("grep -rn socket ~/.local/state/ 2>/dev/null", False),
    # ~ 展开 + glob 前缀目录在仓库外：放行
    ("grep -l addr ~/.local/state/*.json", False),
    # /etc 在仓库外：多文件也放行
    ("grep -rn PermitRoot /etc/ssh/", False),
    # 仓库外目标 + 仓库内代码目录混合 → 从严照拦
    ("grep -rn foo ~/.local/state/ src/", True),
    # 不存在的仓库外路径：无法确认 → 照常规判定（递归扫 → 拦）
    ("grep -rn foo /nonexistent-dir-xyz/", True),
    # --- #47 远端执行豁免（quote-aware 切段 + REMOTE_WRAPPERS）：6 条审计样本回放 ---
    ('tmux send-keys -t esweb168:0 "cat ~/.config/systemd/user/container-solo-github.service; '
     "echo ---; ls ~/.runai/ 2>/dev/null; grep -ril 'CLIENT_SECRET\\\\|client_secret' ~/.runai/ "
     '~/data/ /data/solo 2>/dev/null | grep -v node_modules | head -20; echo ===D3===" Enter; '
     'timeout 60 bash -c "until tmux capture-pane -p -t esweb168:0 | grep -q \'===D3===\'; '
     'do sleep 2; done"; tmux capture-pane -p -t esweb168:0 | tail -45', False),
    ("tmux send-keys -t esweb168:0 'sed -E \"s/(SECRET=).{6}.*/\\\\1<redacted-old>/\" "
     "/data/solo-releases/shared/secrets/forge-oauth.env; echo ---WHOUSES---; for u in solo-broker "
     "solo-agent-bridge solo-matter solo-project solo-share-broker; do systemctl --user cat $u "
     "2>/dev/null | grep -l forge-oauth >/dev/null; done; grep -c forge-oauth "
     "~/.config/systemd/user/*.service 2>/dev/null | grep -v \":0\"; echo ===D9===' Enter; "
     "timeout 60 bash -c \"until tmux capture-pane -p -t esweb168:0 | grep -q '===D9==='; "
     "do sleep 2; done\"; tmux capture-pane -p -t esweb168:0 | sed -n "
     "'/---WHOUSES---/,$p; /forge-oauth.env; echo/,$p' | tail -25", False),
    ("ssh public 'D=/data/solo-releases/coder-gateway/current/coder-proxy; "
     'grep -rl "victor.annie" $D/config $D/secrets $D/src 2>/dev/null | head; '
     "ls $D/config 2>/dev/null | head -30; ls $D/secrets 2>/dev/null | head -20'", False),
    ("ssh public 'systemctl show -p User,Group,Environment runai-coder-gateway@18796 | head -5; "
     "D=/data/solo-releases/coder-gateway/current/coder-proxy; "
     'grep -n "\\.runai\\|dbPath\\|DB_PATH\\|coder-pool" $D/src/coder-pool/store-sqlite.ts | head -10; '
     'grep -rn "victor.annie\\|6164" /data/solo-state/gate-telemetry/events.jsonl 2>/dev/null '
     "| head -3 | cut -c1-300'", False),
    ("ssh public 'D=/data/solo-releases/coder-gateway/current/coder-proxy; "
     'grep -n "entryUsedPercent" $D/src/coder-pool/*.ts | head; echo ===; '
     "sed -n \"590,700p\" $D/src/coder-pool/lease.ts'", False),
    ("ssh public 'for d in 21 22; do echo \"== 07-$d distinct serving creds:\"; "
     "journalctl -u runai-coder-gateway@18796 -u runai-coder-gateway@18793 "
     '--since "2026-07-$d 00:00" --until "2026-07-$d 23:59" --no-pager 2>/dev/null | '
     'grep -o "serving_credential_fp\\":\\"[a-f0-9]*" | sort -u | wc -l; done; '
     "D=/data/solo-releases/coder-gateway/current/coder-proxy; "
     'grep -n "SOFT_QUOTA_PERCENT =\\|EXHAUSTED_QUOTA_PERCENT =" $D/src/coder-pool/lease.ts\'', False),
    # --- #47 反向用例：远端豁免绝不放松本地纪律 ---
    # 顶层 && 在引号外照切：第二段本地跨文件 grep 仍拦
    ("ssh host 'grep -r x /remote/dir' && grep -rn y src/", True),
    # 顶层 ; 在引号外照切：本地递归 grep 照拦
    ('tmux send-keys -t s "grep -r foo /r/" Enter; grep -rn bar services/', True),
    # 引号内 pattern 含 | / ; 的本地跨文件 grep：quote-aware 后仍整段判定 → 照拦
    ("grep -rn 'setStatus|close;done' src/", True),
    # 引号内含 ; 的单文件页内 grep：不再被误切，照常放行
    ("grep -n 'a;b|c' services/matter-service/store.mjs", False),
    # 未闭合引号 fail-open：整条按单段（段首 ssh → 放行），hook 不崩
    ("ssh public 'grep -rl x $D/src", False),
    # --- #48 非代码文本残漏（记忆/会话档/变量目标/fallback-cd/include-glob） ---
    # 记忆仓目录（可能自带 .git）：递归 grep 放行
    ('grep -ril "completion report" ~/.agents/memory/ 2>/dev/null | head', False),
    # 会话档 jsonl（变量目标，同命令内有字面赋值）：放行
    ('for h in ~/.codex ~/.coder; do hf="$h/history.jsonl"; '
     "[ -f \"$hf\" ] && { echo \"== $hf\"; grep -n 'gh auth' \"$hf\" | head -5; }; done", False),
    # 变量单文件 .jsonl（f=…; grep -c x "$f"）：放行
    ("f14=~/.coder.before/sessions/2026/04/14/rollout-a.jsonl; "
     "grep -o '\"cmd\":\"[^\"]*' \"$f14\" | tail -8", False),
    # fallback cd 链（cd X 2>/dev/null || cd Y 2>/dev/null）后 grep 记忆目录：放行
    ("cd ~/.agents/memory/sub-x 2>/dev/null || cd ~/.agents/memory 2>/dev/null; "
     "pwd; ls; grep -rn '14:42' . | head -20", False),
    # 变量赋值 + 变量目标 .md：放行
    ("M=/Users/zkf/.coder/memory/proj-x/MEMORY.md; "
     'grep -n "dispatcher-state-lock-done" "$M" && ls /tmp/', False),
    # --include=*.md 限定匹配面为文档：放行
    ('grep -rn "mcp-adoption" --include=*.md docs 2>/dev/null | head', False),
    # docs/ 文档目录：放行
    ('grep -rn "19/19" docs/design/issue4626/ 2>/dev/null', False),
    # --- #48 反向用例：豁免不放松代码纪律 ---
    # 变量展开后是代码文件多目标：照拦
    ('S=src; grep -rn setStatus "$S/" services/', True),
    # 变量展开后是代码目录递归：照拦
    ('D=services; grep -rn close "$D"', True),
    # --include=*.mjs 代码 glob：照拦
    ('grep -rn "budget" --include=*.mjs .', True),
    # 记忆根混排仓内代码目录：从严照拦
    ("grep -rn foo ~/.agents/memory/ src/", True),
    # fallback cd 链落点是代码仓时不背书：照拦（cd 进本仓后递归 grep）
    ("cd /nonexistent-x 2>/dev/null || cd " +
     os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))) +
     " 2>/dev/null; grep -rn setStatus src/", True),
    # --- #49 回归固化：历史误拦（新版 hook 已放行）characterization 回放 ---
    # 命令逐字节取自 blocked 审计全文（/tmp/cbm-denied-audit.json，16 条去重后
    # 10 条），expect=allow 以 merged main 回放结果为 approved 值。样本含本机
    # 绝对路径（~/.coder/memory、~/.local/state 等），属本机锚定 fixture；
    # /tmp/pr4868 依赖的目录结构由上方 _ensure_issue49_fixture() 兜底重建。
    ('ls /Users/zkf/.coder/memory/run-solo-company-db83f0d1/ 2>/dev/null | head -50; grep -ril "GH_CONFIG_DIR" /Users/zkf/.coder/memory/run-solo-company-db83f0d1/ 2>/dev/null | head', False),
    ('sed -n \'135,240p\' ~/work/codebase-memory-mcp/scripts/metrics/mcp-adoption.py; echo ===HINTS===; grep -n "NONCODE_PATH_HINTS\\s*=" -A 6 ~/work/codebase-memory-mcp/scripts/metrics/mcp-adoption.py', False),
    ('H=~/work/codebase-memory-mcp/scripts/hooks/grep-intercept.py\necho \'--- probe1: 单文件页内 grep + 前置 sed 链\'\necho \'{"tool_name":"Bash","tool_input":{"command":"sed -n \\"135,240p\\" scripts/metrics/mcp-adoption.py; echo ===; grep -n \\"NONCODE_PATH_HINTS\\" -A 6 scripts/metrics/mcp-adoption.py"}}\' | python3 $H\necho \'--- probe2: 单文件 grep 带 -A\'\necho \'{"tool_name":"Bash","tool_input":{"command":"grep -n \\"NONCODE_PATH_HINTS\\" -A 6 scripts/metrics/mcp-adoption.py"}}\' | python3 $H\necho \'--- probe3: 单文件 grep 带重定向\'\necho \'{"tool_name":"Bash","tool_input":{"command":"grep -i -h postreview $MEM/MEMORY.md 2>/dev/null"}}\' | python3 $H\necho \'--- metrics 侧同款\'\ncd ~/work/codebase-memory-mcp && python3 -c "\nimport sys; sys.path.insert(0,\'scripts/metrics\')\nimport importlib.util\nspec=importlib.util.spec_from_file_location(\'m\',\'scripts/metrics/mcp-adoption.py\'); m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m)\nfor cmd in [\'grep -i -h postreview \\$MEM/MEMORY.md 2>/dev/null\',\'grep -l -i postreview \\$MEM/*.md 2>/dev/null\',\'grep -n NONCODE_PATH_HINTS -A 6 scripts/metrics/mcp-adoption.py\']:\n    print(repr(cmd), \'->\', m.classify_exec(cmd))\n"', False),
    ('cd /tmp/pr4868 && awk \'/^  zh: {/,/^  },/\' head-misc.ts | grep -o \'"misc\\.tg\\.[^"]*"\' | sort > zh.keys; awk \'/^  en: {/,/^  },/\' head-misc.ts | grep -o \'"misc\\.tg\\.[^"]*"\' | sort > en.keys; diff zh.keys en.keys && echo KEY_NAME_PARITY_OK; grep -n "^export" head-server.mjs; grep -n "telegram" head-server.mjs | grep -in "publish\\b" ; sed -n \'/async function publish(/,/^}/p\' head-server.mjs | grep -n "telegram\\|inject" ; echo ---; grep -c "misc.tg" zh.keys en.keys', False),
    ('cd /tmp/pr4868/run/services/project-service && grep -n "telegram" README.md selftest.mjs 2>/dev/null | head; echo ---; cd /Users/zkf/work/solo/run-solo-company && git diff --stat 1667b573f..11ce511db | tail -5', False),
    ('ls ~/work/solo-wt/fix-4863-stall-guard/projects 2>&1; echo ---; grep -n -e "zero outcome" -e "auto-paused" -e noop_streak ~/work/solo/run-solo-company/projects/coder/codex-rs/core/src/goals.rs | head -20', False),
    ('cd /tmp/cbm-review-42 && grep -n "is_noncode_target" -A 18 scripts/hooks/grep-intercept.py | head -30', False),
    ("ls ~/.codex-zkf/scripts/ai-dev/ 2>/dev/null | head; echo ---; ls -d ~/work/RunAI/docs/specs/postreview.md 2>/dev/null; grep -rn 'formal-gh' ~/.codex-zkf/scripts/ai-dev/postreview-artifact-helper.py 2>/dev/null | head -3; python3 ~/.codex-zkf/scripts/ai-dev/postreview-artifact-helper.py 2>&1 | head -8", False),
    ("grep -o '^[A-Z_]*=' ~/.local/state/runai/public-ai-dev-review/public-ai-dev-review.env ~/.local/state/runai/public-ai-dev-review/public-ai-dev-review-bot-codex_gpt54_xhigh.env; echo ---; python3 ~/.codex-zkf/scripts/ai-dev/postreview-artifact-helper.py --help 2>&1 | head -30", False),
    ("ls -la ~/.local/state/runai/public-ai-dev-review/; echo ---SEP---; stat -f '%N %SB' ~/.local/state/runai/public-ai-dev-review/* ~/.local/github.env; echo ---SEP---; head -c 400 ~/.local/github.env 2>/dev/null | grep -v -i token; echo ---SEP---; grep -l -i 'esweb168' ~/.local/state/runai/public-ai-dev-review/*.env ~/.local/github.env 2>/dev/null", False),
]

# (command, cwd, expect_deny) — 需要 cwd 的用例
CWD_CASES = [
    # cwd 在仓库外：rg 无文件参数（默认递归扫 cwd）放行
    ("rg -n pattern9x", os.path.expanduser("~/.local/state"), False),
    # cwd 在仓库内：rg 无文件参数照拦
    ("rg -n pattern9x", os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), True),
    # 链式 cd 到仓库外目录再递归 grep：跟踪 cd 后放行
    ("cd ~/.local/state && grep -rn socket .", "", False),
    # --- F1 回归：cd 形态跟踪不了时必须丢弃陈旧仓外 cwd（置空从严） ---
    # 带引号路径的 cd（regex 匹配不上）→ cwd 置空，后续递归 grep 照拦
    ('cd "/tmp/some dir" && grep -rn foo .', os.path.expanduser("~/.local/state"), True),
    # `cd --` 形态 → cwd 置空，照拦
    ("cd -- /tmp/anywhere && grep -rn foo .", os.path.expanduser("~/.local/state"), True),
    # `cd $UNDEF`（解析不了）→ cwd 置空，照拦
    ("cd $CBM_UNDEF_VAR_9X && grep -rn foo .", os.path.expanduser("~/.local/state"), True),
    # `..` normpath 回到仓库内 → 照拦（回归固化）
    ("grep -rn foo ~/.local/../work/codebase-memory-mcp/src", "", True),
    # 相对路径 + 仓库内 cwd：照拦
    ("grep -rn setStatus src/", os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), True),
]


def run_case(command, cwd=None):
    payload = {"tool_name": "Bash", "tool_input": {"command": command}}
    if cwd is not None:
        payload["cwd"] = cwd
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

    for command, cwd, expect_deny in CWD_CASES:
        got = run_case(command, cwd=cwd)
        status = "ok" if got == expect_deny else "FAIL"
        if got != expect_deny:
            failed += 1
        print(f"[{status}] deny={got} expect={expect_deny} cwd={cwd or '-'} :: {command}")

    # 健壮性：合法 JSON 但非 dict → fail-open（exit 0 + "{}"）
    rc, stdout = run_raw("[1,2,3]")
    ok = rc == 0 and stdout == "{}"
    if not ok:
        failed += 1
    print(f"[{'ok' if ok else 'FAIL'}] fail-open rc={rc} stdout={stdout!r} :: non-dict JSON [1,2,3]")

    total = len(CASES) + len(CWD_CASES) + 1
    print(f"\n{total - failed}/{total} passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
