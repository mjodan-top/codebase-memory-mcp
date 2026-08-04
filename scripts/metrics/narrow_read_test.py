#!/usr/bin/env python3
"""窄窗读影子观测的单测（issue #54）。

覆盖三件事：
1. 窗口抽取：sed -n 'a,bp' / nl / awk 'NR>=a&&NR<=b' / head -n / tail -n +N
2. 冗余倍率：redundancy = 窗口数 / 合并后段数（间隔 ≤ MERGE_GAP 视为本该合并）
3. 影子语义：narrow_read 计数绝不进 S 分母（s 值不因窄读变化）

跑法：python3 scripts/metrics/narrow_read_test.py
"""
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    "mcp_adoption", os.path.join(HERE, "mcp-adoption.py"))
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
        # (cmd, 期望 [(file, lo, hi, tool)])
        ("sed -n '1128,1160p' src/store.c",
         [("store.c", 1128, 1160, "sed")]),
        ("sed -n '5p' a/b/foo.rs",
         [("foo.rs", 5, 5, "sed")]),
        ("awk 'NR>=200 && NR<=340' scripts/x.py",
         [("x.py", 200, 340, "awk")]),
        ("nl -ba src/daemon.c",
         [("daemon.c", None, None, "nl")]),
        ("head -n 40 lib/mod.ts",
         [("mod.ts", 1, 40, "head")]),
        # 非代码目标不计
        ("sed -n '1,20p' /tmp/out.log", []),
        # 纯管道过滤不计
        ("cat x | sed -n '1,5p'", []),
        # 多段命令各自抽取
        ("sed -n '1,10p' a.c && sed -n '90,120p' a.c",
         [("a.c", 1, 10, "sed"), ("a.c", 90, 120, "sed")]),
    ]
    for cmd, want in cases:
        got = M.extract_narrow_windows(cmd)
        check(f"extract({cmd[:38]!r})", got, want)


# ---------------- 2. 冗余倍率 ----------------
def t_redundancy():
    # 相邻窗口紧贴 → 应合并成 1 段，3 刀读 1 段 = 冗余 3.0
    wins = [(1, 50), (55, 100), (105, 150)]
    check("merge_adjacent 紧贴", M.merge_adjacent(wins), [[1, 150]])
    check("redundancy 紧贴", M.redundancy(wins), 3.0)

    # 相隔很远 → 不合并，3 刀读 3 段 = 冗余 1.0（健康）
    wins = [(1, 50), (5000, 5050), (9000, 9050)]
    check("merge_adjacent 远隔", len(M.merge_adjacent(wins)), 3)
    check("redundancy 远隔", M.redundancy(wins), 1.0)

    # 单刀 = 无冗余
    check("redundancy 单刀", M.redundancy([(10, 20)]), 1.0)
    # 空
    check("redundancy 空", M.redundancy([]), None)

    # 边界：间隔正好等于 MERGE_GAP 应合并；超过 1 行则不合并
    g = M.MERGE_GAP
    check("redundancy gap==MERGE_GAP", M.redundancy([(1, 10), (10 + g, 10 + g + 5)]), 2.0)
    check("redundancy gap>MERGE_GAP", M.redundancy([(1, 10), (11 + g, 20 + g)]), 1.0)

    # 回归实测坏样本形态：76 窗合并成 3 段 ≈ 25.33
    wins = []
    for base in (0, 4000, 9000):        # 三个远隔簇
        for k in range(25 if base == 0 else 25 if base == 4000 else 26):
            lo = base + k * 10 + 1      # 簇内紧贴
            wins.append((lo, lo + 5))
    r = M.redundancy(wins)
    check_true("redundancy 坏样本形态 >19.4(P99)", r is not None and r > 19.4, f"got {r}")


# ---------------- 3. 影子语义：不进 S 分母 ----------------
def t_shadow_not_scored():
    """narrow_read 必须归到 sed_page/narrow_read 类，绝不是 legacy。"""
    for cmd in ("sed -n '100,140p' src/store.c",
                "nl -ba src/store.c",
                "awk 'NR>=10 && NR<=99' src/store.c"):
        kinds = [k for k, _ in M.classify_exec(cmd)]
        check_true(f"not legacy: {cmd[:30]!r}", "legacy" not in kinds, f"got {kinds}")
        check_true(f"has narrow_read: {cmd[:30]!r}", "narrow_read" in kinds,
                   f"got {kinds}")

    # 跨文件 sed 仍应判 legacy（既有行为不得回退）
    kinds = [k for k, _ in M.classify_exec("sed -n '1,50p' a.c b.c")]
    check_true("多文件 sed 仍 legacy", "legacy" in kinds, f"got {kinds}")

    # 跨文件 grep 仍 legacy（既有行为不得回退）
    kinds = [k for k, _ in M.classify_exec("grep -rn foo src/")]
    check_true("grep -rn 仍 legacy", "legacy" in kinds, f"got {kinds}")


def t_shadow_flag_present():
    """报表侧必须显式标注 shadow / 不判罚，防未来误接进判定。"""
    check_true("SHADOW_METRICS 声明存在",
               getattr(M, "NARROW_READ_SHADOW", None) is True,
               "模块应声明 NARROW_READ_SHADOW = True")


if __name__ == "__main__":
    for fn in (t_extract, t_redundancy, t_shadow_not_scored, t_shadow_flag_present):
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
    print("PASS narrow_read (窗口抽取 / 冗余倍率 / 影子不计分)")
