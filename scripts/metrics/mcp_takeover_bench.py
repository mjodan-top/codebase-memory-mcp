#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""MCP 接管率基准：把真实 grep 帧重放给本机 MCP，量「今天到底能接住多少」。

输入：test/fixtures/grep-corpus-48h.jsonl（grep_corpus.py 固化的语料）
只回放 bucket=code_symbol 的帧——这些是「跨文件找符号定义/调用」，
按 skill codebase-memory-first 口径本该由 MCP 接管。

调用方式**只用 `./build/c/codebase-memory-mcp cli <tool> '<json>'`**：
跑 bin/ 下的二进制会走 stdio 代理到常驻 daemon，测的是旧代码，不是当前构建。

接管成功（takeover=ok）三个条件同时满足：
  1. 从帧的 cwd 能解析到一个已索引 project（不是 stale）；
  2. search_code 返回 total_grep_matches > 0；
  3. 命中结果里含原 grep 的 pattern 主标识符。
任何一条不满足都进失败分桶，分桶名即根因，可逐条人工复核。

失败分桶：
  project_unindexed  cwd 对应的仓根本没建过索引
  project_stale      项目在册但 root_path 已删（查询静默返空）
  symbol_absent      项目健康、MCP 正常工作，但该符号确实不在图里
  regex_literal      pattern 含未转义 `|`，regex 默认 false 被当字面串 → 必然零命中
  call_error         MCP 调用本身报错
  other              上述都不是

跑法：
  python3 scripts/metrics/mcp_takeover_bench.py                 # 全量
  python3 scripts/metrics/mcp_takeover_bench.py --limit 40      # 抽样快跑
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from grep_corpus import (split_pipeline, parse_grep_segment, IDENT_RE,
                         strip_redirects)
import shlex

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BIN = os.path.join(REPO, "build", "c", "codebase-memory-mcp")
TIMEOUT_S = 45


def cli(tool: str, payload: dict, timeout=TIMEOUT_S):
    """调一次 MCP cli 子命令 → (ok, obj_or_errstr)。"""
    try:
        p = subprocess.run([BIN, "cli", tool, json.dumps(payload)],
                           capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return False, "timeout"
    except OSError as e:
        return False, f"oserror:{e}"
    # stdout 里混有 ts=… 日志行与 deprecation warning，取最后一个 JSON 对象
    for line in reversed(p.stdout.splitlines()):
        line = line.strip()
        if line.startswith("{"):
            try:
                return True, json.loads(line)
            except Exception:
                continue
    return False, (p.stderr or p.stdout or "no-json")[:200]


def load_projects():
    ok, res = cli("list_projects", {"include_stale": True})
    if not ok:
        print(f"!! list_projects 失败: {res}", file=sys.stderr)
        return [], []
    live, stale = [], []
    for pr in res.get("projects", []):
        root = pr.get("root_path") or ""
        g = pr.get("git") or {}
        entry = {"name": pr.get("name"), "root": root,
                 "canonical": (g.get("canonical_root") or root),
                 "nodes": pr.get("nodes") or 0}
        if g.get("root_exists") is False or (root and not os.path.isdir(root)):
            stale.append(entry)
        else:
            live.append(entry)
    # 长路径优先，保证 /a/b/c 匹配到最深的已索引项目
    live.sort(key=lambda x: -len(x["canonical"] or ""))
    stale.sort(key=lambda x: -len(x["canonical"] or ""))
    return live, stale


def resolve_project(cwd, live, stale):
    """cwd → (project_name | None, status)；status ∈ ok/stale/unindexed。"""
    if not cwd:
        return None, "unindexed"
    c = os.path.realpath(os.path.expanduser(cwd))
    for pr in live:
        root = pr["canonical"] or pr["root"]
        if not root:
            continue
        r = os.path.realpath(root)
        if c == r or c.startswith(r + os.sep):
            return pr["name"], "ok"
    for pr in stale:
        root = pr["canonical"] or pr["root"]
        if not root:
            continue
        if c == root or c.startswith(root.rstrip("/") + os.sep):
            return pr["name"], "stale"
    return None, "unindexed"


def extract_query(cmd, cwd=""):
    """取出命令里**真正被判为 code_symbol 的那一段** grep 的 pattern。

    坑（实测）：一条命令常是 `ls … ; grep -rn Foo src/` 多段复合，取「第一个
    grep 段」会拿到与分类结论无关的段（如管道过滤），回放的就不是同一件事。
    必须复用 grep_corpus 的同一判据逐段定位，取第一个 code_symbol 段。
    """
    from grep_corpus import _classify_grep
    fallback = None
    for seg, is_head in split_pipeline(cmd):
        if re.match(r"^cd(\s|$)", seg):
            continue
        g = parse_grep_segment(seg)
        if g is None or g.get("remote") or not is_head or not g["pattern"]:
            continue
        if _classify_grep(g, cwd)[0] == "code_symbol":
            return g
        if fallback is None:
            fallback = g
    return fallback


# 正则元字符转义序列：`\b`、`\s`、`\w` 等。必须先剥掉，否则 `\bUPSTREAM\b`
# 会被 IDENT_RE 抽成 `bUPSTREAM`（实测踩到），拿去查必然零命中。
ESCAPE_RE = re.compile(r"\\[bBsSwWdDnrtAZ]")


def main_ident(pattern):
    """pattern 里最有辨识度的标识符（最长的一个）。"""
    p = ESCAPE_RE.sub(" ", pattern.strip("'\""))
    ids = IDENT_RE.findall(p)
    return max(ids, key=len) if ids else ""


def frame_cwd(fr):
    """帧的有效 cwd：命令里若有 `cd X` 前缀，以它为准。"""
    cwd = fr.get("cwd") or ""
    for seg, _ in split_pipeline(fr["cmd"]):
        if re.match(r"^cd(\s|$)", seg):
            try:
                toks = strip_redirects(shlex.split(seg))
            except ValueError:
                continue
            args = [t for t in toks[1:] if t != "--"]
            if len(args) == 1 and "$" not in args[0]:
                p = os.path.expanduser(args[0])
                cwd = p if os.path.isabs(p) else os.path.normpath(os.path.join(cwd, p))
            break
    return cwd


def replay_frame(fr, live, stale):
    """回放一帧 → dict(takeover, bucket, detail)。"""
    cwd0 = frame_cwd(fr)
    g = extract_query(fr["cmd"], cwd0)
    if not g:
        return {"takeover": "fail", "bucket": "other", "detail": "no-pattern"}
    pattern = g["pattern"].strip("'\"")
    ident = main_ident(pattern)
    if not ident:
        return {"takeover": "fail", "bucket": "other", "detail": "no-ident"}

    cwd = cwd0
    proj, status = resolve_project(cwd, live, stale)
    if status == "unindexed":
        return {"takeover": "fail", "bucket": "project_unindexed",
                "detail": f"cwd={cwd[:70]}"}
    if status == "stale":
        return {"takeover": "fail", "bucket": "project_stale",
                "detail": f"{proj} root_path 已删"}

    has_alt = "|" in pattern
    # 按调用方**当时的真实写法**回放：不传 regex（即默认 false）。
    ok, res = cli("search_code", {"project": proj, "pattern": pattern, "limit": 10})
    if not ok:
        return {"takeover": "fail", "bucket": "call_error",
                "detail": str(res)[:100], "project": proj}
    total = res.get("total_grep_matches", 0) or 0
    blob = json.dumps(res, ensure_ascii=False)
    if total > 0 and ident in blob:
        return {"takeover": "ok", "bucket": "ok",
                "detail": f"{proj} total={total}", "project": proj}

    # 零命中：分辨是 regex 字面量问题，还是符号真不在图里
    if has_alt:
        ok2, res2 = cli("search_code", {"project": proj, "pattern": pattern,
                                        "limit": 10, "regex": True})
        if ok2 and (res2.get("total_grep_matches", 0) or 0) > 0:
            return {"takeover": "fail", "bucket": "regex_literal",
                    "detail": f"{proj} regex=true 后 total={res2.get('total_grep_matches')}",
                    "project": proj}
    # 用单个主标识符再试：能命中说明符号在图里，零命中纯属 pattern 形态
    # （正则元字符 \b、多选、转义等）没被 MCP 按调用方意图解释。
    ok3, res3 = cli("search_code", {"project": proj, "pattern": ident, "limit": 5})
    if ok3 and (res3.get("total_grep_matches", 0) or 0) > 0:
        return {"takeover": "fail", "bucket": "pattern_shape",
                "detail": f"{proj} 主标识符 '{ident}' 单查 total={res3.get('total_grep_matches')}",
                "project": proj}
    return {"takeover": "fail", "bucket": "symbol_absent",
            "detail": f"{proj} '{ident}' 不在图里", "project": proj}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", default="test/fixtures/grep-corpus-48h.jsonl")
    ap.add_argument("--limit", type=int, default=0, help="只回放前 N 帧（0=全量）")
    ap.add_argument("--out", default="", help="逐帧结果落盘 JSONL")
    args = ap.parse_args()

    if not os.path.exists(BIN):
        print(f"!! 缺二进制 {BIN}\n   先跑 make -f Makefile.cbm build/c/codebase-memory-mcp -j8")
        return 2

    path = args.corpus if os.path.isabs(args.corpus) else os.path.join(REPO, args.corpus)
    rows = []
    for line in open(path, encoding="utf-8"):
        if '"_meta"' in line[:12]:
            continue
        rows.append(json.loads(line))
    targets = [r for r in rows if r["bucket"] == "code_symbol"]
    if args.limit:
        targets = targets[:args.limit]

    live, stale = load_projects()
    print(f"已索引项目 live={len(live)} stale={len(stale)}")
    print(f"语料 {len(rows)} 帧，其中 code_symbol {len(targets)} 帧待回放\n")

    results = []
    for i, fr in enumerate(targets, 1):
        r = replay_frame(fr, live, stale)
        r["cmd"] = fr["cmd"][:200]
        r["session"] = fr["session"]
        r["ts"] = fr["ts"]
        results.append(r)
        if i % 20 == 0:
            print(f"  … {i}/{len(targets)}", flush=True)

    bc = Counter(r["bucket"] for r in results)
    n = len(results)
    ok_n = bc.get("ok", 0)
    print("\n" + "=" * 62)
    print(f"# MCP 接管率基准（code_symbol 帧 n={n}）")
    print("=" * 62)
    print(f"\n接管成功：{ok_n} / {n} = {ok_n / max(1, n) * 100:.1f}%\n")
    print(f"| 分桶 | 帧数 | 占比 |")
    print(f"|---|---:|---:|")
    for k, v in bc.most_common():
        label = {"ok": "✅ 接管成功", "project_unindexed": "❌ 项目未索引",
                 "project_stale": "❌ 项目 stale（静默返空）",
                 "symbol_absent": "❌ 符号不在图里",
                 "regex_literal": "❌ regex 默认 false，多选被当字面量",
                 "pattern_shape": "❌ pattern 形态未被解释（元字符等）",
                 "call_error": "❌ 调用报错", "other": "❌ 其他"}.get(k, k)
        print(f"| {label} | {v} | {v / max(1, n) * 100:.1f}% |")

    print("\n## 各失败桶样例（最多 3 条）")
    for k, _ in bc.most_common():
        if k == "ok":
            continue
        print(f"\n### {k}")
        for r in [x for x in results if x["bucket"] == k][:3]:
            print(f"  - {r['detail']}")
            print(f"    cmd: {r['cmd'][:120]}")

    if args.out:
        out = args.out if os.path.isabs(args.out) else os.path.join(REPO, args.out)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, "w", encoding="utf-8") as f:
            for r in results:
                f.write(json.dumps(r, ensure_ascii=False) + "\n")
        print(f"\n逐帧结果: {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
