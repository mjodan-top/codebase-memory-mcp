#!/usr/bin/env python3
"""窄窗读影子观测的单测（issue #54）。

设计要求（评审 PR #55 时发现首版三条断言全是自证，故重写）：
**影子承诺必须跑真实聚合路径断言**，不能只检查分类标签或布尔常量——
后者在人为把 narrow_read 塞进 s 分母后仍会报绿（实测确认过的假绿）。

覆盖：
1. 窗口抽取：sed -n / nl / awk（开闭区间）/ head -n / tail -n / tail -n +N
2. 只对真实执行的程序抽取：`echo head -n 40 f.ts` 不算，ssh 远端不算
3. 聚合键是路径而非 basename：src/a.c 与 tests/a.c 不得混同
4. 非法区间丢弃：反向 `sed -n '100,10p'` 不得产生负 coverage
5. 冗余倍率精确值（不是"大于某阈值"这种放水断言）
6. **影子承诺**：构造含 mcp/legacy/narrow_read 的合成会话，跑真实报表聚合，
   断言加任意数量窄读前后 s_cmd_level / S_episode_level 逐位不变

跑法：python3 scripts/metrics/narrow_read_test.py
"""
import importlib.util
import io
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "mcp-adoption.py")
spec = importlib.util.spec_from_file_location("mcp_adoption", SCRIPT)
M = importlib.util.module_from_spec(spec)
spec.loader.exec_module(M)

FAIL = []


def check(name, got, want):
    if got != want:
        FAIL.append(f"{name}: got {got!r}, want {want!r}")


def check_true(name, cond, detail=""):
    if not cond:
        FAIL.append(f"{name}: expected true {detail}")


# ---------------- 1. 窗口抽取 ----------------
def t_extract():
    cases = [
        ("sed -n '1128,1160p' src/store.c", [("src/store.c", 1128, 1160, "sed")]),
        ("sed -n '5p' a/b/foo.rs", [("a/b/foo.rs", 5, 5, "sed")]),
        # awk 闭区间
        ("awk 'NR>=200 && NR<=340' scripts/x.py",
         [("scripts/x.py", 200, 340, "awk")]),
        # awk 开区间：NR>200 && NR<340 实读 201..339
        ("awk 'NR>200 && NR<340' scripts/x.py",
         [("scripts/x.py", 201, 339, "awk")]),
        # awk 混合：>= 与 <
        ("awk 'NR>=10 && NR<20' scripts/x.py",
         [("scripts/x.py", 10, 19, "awk")]),
        ("nl -ba src/daemon.c", [("src/daemon.c", None, None, "nl")]),
        ("head -n 40 lib/mod.ts", [("lib/mod.ts", 1, 40, "head")]),
        # tail 两种语义都不是 1..N，区间未知
        ("tail -n +40 lib/mod.ts", [("lib/mod.ts", None, None, "tail")]),
        ("tail -n 40 lib/mod.ts", [("lib/mod.ts", None, None, "tail")]),
        # 非代码目标不计
        ("sed -n '1,20p' /tmp/out.log", []),
        # 纯管道过滤不计
        ("cat x | sed -n '1,5p'", []),
        # 多段命令各自抽取，且**保留目录**（不混同）
        ("sed -n '1,10p' src/a.c && sed -n '90,120p' tests/a.c",
         [("src/a.c", 1, 10, "sed"), ("tests/a.c", 90, 120, "sed")]),
        # 反向/非法区间丢弃（防负 coverage）
        ("sed -n '100,10p' a.c", []),
        ("sed -n '0p' a.c", []),
    ]
    for cmd, want in cases:
        check(f"extract({cmd[:42]!r})", M.extract_narrow_windows(cmd), want)


def t_extract_real_prog_only():
    """只对真实执行的程序抽取——命令文本里出现工具字样不算。"""
    for cmd in ("echo head -n 40 lib/mod.ts",
                "printf 'tail -n +20 src/a.c'",
                "echo \"sed -n '1,10p' src/a.c\""):
        check(f"not counted: {cmd[:34]!r}", M.extract_narrow_windows(cmd), [])
    # 远端执行整段跳过（远端文件不在本地索引覆盖内）
    check("ssh 远端不计",
          M.extract_narrow_windows("ssh host \"sed -n '1,10p' src/a.c\""), [])
    # 包装层必须逐层剥到真实程序（复审实测原版这些全部漏计）
    for cmd in ("env X=1 sed -n '1,10p' src/a.c",
                "env -i X=1 sed -n '1,10p' src/a.c",
                "sudo -u nobody sed -n '1,10p' src/a.c",
                "nice -n 5 sed -n '1,10p' src/a.c",
                "command -- sed -n '1,10p' src/a.c",
                "timeout 30 sed -n '1,10p' src/a.c",
                "timeout 1.5s sed -n '1,10p' src/a.c"):
        check(f"包装剥离 {cmd[:26]!r}", M.extract_narrow_windows(cmd),
              [("src/a.c", 1, 10, "sed")])


def t_aggregation_key_is_path():
    """同名不同目录必须是两条记录（basename 聚合会虚增冗余）。"""
    got = M.extract_narrow_windows(
        "sed -n '1,10p' src/a.c; sed -n '20,30p' tests/a.c")
    keys = sorted({k for k, *_ in got})
    check("聚合键保留目录", keys, ["src/a.c", "tests/a.c"])


# ---------------- 2. 冗余倍率 ----------------
def t_redundancy():
    wins = [(1, 50), (55, 100), (105, 150)]
    check("merge 紧贴", M.merge_adjacent(wins), [[1, 150]])
    check("redundancy 紧贴", M.redundancy(wins), 3.0)

    wins = [(1, 50), (5000, 5050), (9000, 9050)]
    check("merge 远隔段数", len(M.merge_adjacent(wins)), 3)
    check("redundancy 远隔", M.redundancy(wins), 1.0)

    check("redundancy 单刀", M.redundancy([(10, 20)]), 1.0)
    check("redundancy 空", M.redundancy([]), None)
    # 未知区间不参与计算
    check("redundancy 全未知", M.redundancy([(None, None), (None, None)]), None)

    g = M.MERGE_GAP
    check("gap==MERGE_GAP 合并", M.redundancy([(1, 10), (10 + g, 10 + g + 5)]), 2.0)
    check("gap>MERGE_GAP 不合并", M.redundancy([(1, 10), (11 + g, 20 + g)]), 1.0)

    # 精确断言坏样本形态：76 窗 → 3 段 → 25.33（不是"大于阈值"放水断言）
    wins = []
    for base, n in ((0, 25), (4000, 25), (9000, 26)):
        for k in range(n):
            lo = base + k * 10 + 1
            wins.append((lo, lo + 5))
    check("坏样本 窗口数", len(wins), 76)
    check("坏样本 合并段数", len(M.merge_adjacent(wins)), 3)
    check("坏样本 冗余倍率", M.redundancy(wins), 25.33)

    # 负 coverage 不可能出现：merge 后每段 hi>=lo
    for lo, hi in M.merge_adjacent(wins):
        check_true("merge 段非负", hi >= lo, f"({lo},{hi})")


# ---------------- 3. 影子承诺：跑真实聚合 ----------------
def _mk_session(root, entries, name="rollout-2026-08-04T00-00-00-test.jsonl"):
    """写一个最小 rollout jsonl。

    必须落到 `<root>/YYYY/MM/DD/` —— iter_session_files 按
    `root/*/*/*/*.jsonl` 四层 glob 收集，平铺目录不会被发现。
    """
    day = os.path.join(root, "2026", "08", "04")
    os.makedirs(day, exist_ok=True)
    path = os.path.join(day, name)
    with open(path, "w") as f:
        f.write(json.dumps({"type": "session_meta",
                            "timestamp": "2026-08-04T00:00:00.000Z",
                            "payload": {"timestamp": "2026-08-04T00:00:00.000Z",
                                        "cwd": "/repo"}}) + "\n")
        # 时间戳设计有两条硬要求，缺一就会让 S 的断言退化：
        # ① 严格递增——相同时间戳时 sorted(calls) 会按字符串序把 legacy 排到
        #    mcp 前，基准直接变 E_mcp=0 / S=0.0，那 S 的相等断言就是
        #    「0.0 == 0.0」（复审实测：sabotage 后 s 转红但 S 仍绿）。
        # ② 每条调用间隔 > EPISODE_GAP_S（120s）——否则全部被归并成一个事件，
        #    首动作是 MCP 就永远 E_mcp=1，往里塞多少 legacy 都抓不到。
        step = M.EPISODE_GAP_S + 60
        for i, (name, cmd) in enumerate(entries):
            sec = i * step
            f.write(json.dumps({
                "timestamp": (f"2026-08-04T{sec // 3600:02d}:"
                              f"{sec % 3600 // 60:02d}:{sec % 60:02d}.000Z"),
                "type": "response_item",
                "payload": {"type": "function_call", "name": name,
                            "call_id": f"c{i}",
                            "arguments": json.dumps({"cmd": cmd})}}) + "\n")


def _run_report(session_dir):
    """跑真实 CLI 报表（--json），返回 dict。"""
    out = subprocess.run(
        [sys.executable, SCRIPT, "--hours", "999999", "--json", "--no-replay",
         "--sessions-root", session_dir, "--daemon-log", os.devnull],
        capture_output=True, text=True)
    if out.returncode != 0:
        raise RuntimeError(f"report rc={out.returncode}: {out.stderr[-400:]}")
    return json.loads(out.stdout)


def t_shadow_promise_real_aggregation():
    """核心硬承诺：加任意数量窄读，s/S 逐位不变。

    这是唯一能挡住「未来有人把 narrow_read 接进分母」的断言：首版只检查
    分类标签，人为破坏后仍报绿（已实测）。
    """
    base_calls = [
        ("exec_command", "codebase-memory-mcp cli search_graph '{\"q\":\"x\"}'"),
        ("exec_command", "grep -rn needle src/"),          # legacy
        ("exec_command", "grep -rn other lib/"),           # legacy
    ]
    narrow_calls = [
        ("exec_command", "sed -n '1,40p' src/a.c"),
        ("exec_command", "sed -n '45,90p' src/a.c"),
        ("exec_command", "nl -ba src/b.c"),
        ("exec_command", "awk 'NR>=10 && NR<=99' src/c.c"),
        ("exec_command", "head -n 30 src/d.c"),
    ] * 4          # 20 条窄读

    with tempfile.TemporaryDirectory() as d1, tempfile.TemporaryDirectory() as d2:
        _mk_session(d1, base_calls)
        _mk_session(d2, base_calls + narrow_calls)
        try:
            r_without = _run_report(d1)
            r_with = _run_report(d2)
        except Exception as e:
            FAIL.append(f"shadow_promise: 无法跑真实聚合 → {type(e).__name__}: {e}")
            return

        # 先证基准非退化：S 必须真由 MCP 主导且非 0，否则下面的相等断言
        # 只是「0.0 == 0.0」，塞任何窄读进 S 分母都抓不到（复审发现）。
        base_S = r_without["S_episode_level"]
        check_true("基准 E_mcp>0（防退化断言）", base_S.get("E_mcp", 0) > 0,
                   f"got {base_S}")
        check_true("基准 S 非 0（防退化断言）", (base_S.get("value") or 0) > 0,
                   f"got {base_S}")
        base_s = r_without["s_cmd_level"]
        check_true("基准 s 非 0 且非 1（防退化断言）",
                   0 < (base_s.get("value") or 0) < 1, f"got {base_s}")

        for key in ("s_cmd_level", "S_episode_level"):
            check(f"影子承诺 {key} 不变", r_with[key], r_without[key])

        nr = r_with.get("narrow_read_shadow", {})
        check_true("窄读确实被统计到", nr.get("events_total", 0) >= 20,
                   f"got {nr.get('events_total')}")
        check_true("无窄读时统计为空",
                   r_without.get("narrow_read_shadow", {}).get("events_total", 0) == 0,
                   f"got {r_without.get('narrow_read_shadow', {}).get('events_total')}")


def t_blocked_not_counted():
    """被 hook 拦下的命令根本没执行，不得计入窄读（与 legacy_denied 同法理）。"""
    with tempfile.TemporaryDirectory() as d:
        day = os.path.join(d, "2026", "08", "04")
        os.makedirs(day, exist_ok=True)
        with open(os.path.join(day, "rollout-2026-08-04T00-00-00-blk.jsonl"), "w") as f:
            f.write(json.dumps({"type": "session_meta",
                                "timestamp": "2026-08-04T00:00:00.000Z",
                                "payload": {"timestamp": "2026-08-04T00:00:00.000Z",
                                            "cwd": "/repo"}}) + "\n")
            f.write(json.dumps({
                "timestamp": "2026-08-04T00:00:01.000Z", "type": "response_item",
                "payload": {"type": "function_call", "name": "exec_command",
                            "call_id": "c0", "arguments": json.dumps(
                                {"cmd": "sed -n '1,10p' src/a.c && grep -rn foo src/"})}
            }) + "\n")
            f.write(json.dumps({
                "timestamp": "2026-08-04T00:00:02.000Z", "type": "response_item",
                "payload": {"type": "function_call_output", "call_id": "c0",
                            "output": "Command blocked by PreToolUse hook: 纪律拦截"}
            }) + "\n")
        try:
            r = _run_report(d)
        except Exception as e:
            FAIL.append(f"blocked_not_counted: 无法跑聚合 → {e}")
            return
        check("被拦命令不计窄读", r["narrow_read_shadow"]["events_total"], 0)
        check_true("被拦窄读单独计数",
                   r["narrow_read_shadow"].get("events_denied", 0) >= 1,
                   f"got {r['narrow_read_shadow'].get('events_denied')}")


def t_no_regression():
    """既有分类行为不得回退。"""
    kinds = [k for k, _ in M.classify_exec("grep -rn foo src/")]
    check_true("grep -rn 仍 legacy", "legacy" in kinds, f"got {kinds}")
    kinds = [k for k, _ in M.classify_exec("sed -n '1,50p' a.c b.c")]
    check_true("多文件 sed 仍 legacy", "legacy" in kinds, f"got {kinds}")
    for cmd in ("sed -n '100,140p' src/store.c", "nl -ba src/store.c",
                "awk 'NR>=10 && NR<=99' src/store.c"):
        kinds = [k for k, _ in M.classify_exec(cmd)]
        check_true(f"窄读非 legacy: {cmd[:28]!r}", "legacy" not in kinds, f"got {kinds}")


def t_degradation_observable():
    """降级分支必须可观测（AGENTS.md 硬纪律）。"""
    check_true("NARROW_READ_SHADOW 声明", getattr(M, "NARROW_READ_SHADOW", None) is True)
    check_true("降级计数器存在", isinstance(getattr(M, "NR_PARSE_DEGRADED", None), dict))
    before = M.NR_PARSE_DEGRADED["unclosed_quote"]
    M.extract_narrow_windows("sed -n '1,10p src/a.c")     # 未闭合引号
    after = M.NR_PARSE_DEGRADED["unclosed_quote"]
    check_true("未闭合引号降级被计数", after == before + 1, f"{before}→{after}")
    check_true("降级留了样本", len(M.NR_PARSE_DEGRADED["samples"]) > 0)


if __name__ == "__main__":
    for fn in (t_extract, t_extract_real_prog_only, t_aggregation_key_is_path,
               t_redundancy, t_no_regression, t_degradation_observable,
               t_blocked_not_counted, t_shadow_promise_real_aggregation):
        try:
            fn()
        except AttributeError as e:
            FAIL.append(f"{fn.__name__}: 缺少实现 → {e}")
        except Exception as e:
            FAIL.append(f"{fn.__name__}: 异常 {type(e).__name__}: {e}")
    if FAIL:
        print(f"FAIL ({len(FAIL)})")
        for f in FAIL:
            print("  -", f)
        sys.exit(1)
    print("PASS narrow_read（抽取/真实程序/路径键/冗余精确值/无回退/降级可观测/"
          "影子承诺跑真实聚合）")
