#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""grep_corpus.classify_intent 的 golden 单测（手工标注样本）。

样本全部取自真实会话语料形态（test/fixtures/grep-corpus-48h.jsonl），
标注依据 skill codebase-memory-first 的口径：
  code_symbol = 跨文件找「函数/类/符号的定义或调用关系」→ MCP 本该接管
  text_search = 日志/配置/文本检索、页内精定位、管道过滤、远端执行 → grep 正确
  unknown     = 目标或 pattern 解析不出，不进任何结论分母

跑法：python3 scripts/metrics/grep_corpus_test.py
"""
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from grep_corpus import (classify_intent, split_pipeline, parse_grep_segment,
                         pattern_looks_symbolic, target_kind)

# (命令, cwd, 期望桶, 标注理由)
CASES = [
    # ---------- code_symbol：跨文件找符号定义/调用 ----------
    ("grep -rn 'handleRequest' src/", "/r", "code_symbol", "递归扫代码目录找函数"),
    ("rg 'func NewPoolClient' gateway-go/", "/r", "code_symbol", "rg 默认递归+定义关键字"),
    ("grep -rn --include=*.go 'markDead' .", "/r", "code_symbol", "include 限定代码扩展名"),
    ("cd /repo && grep -rn 'parse_session' scripts/", "/r", "code_symbol", "链式 cd 后递归扫"),
    ("grep -n 'coder/pool/report' a.go b.go", "/r", "code_symbol", "多代码文件跨文件扫"),
    ("rg 'class SessionManager'", "/Users/zkf/work/repo", "code_symbol", "rg 无参递归扫代码 cwd"),
    ("grep -rn 'searchGraph\\|searchCode' src/mcp/", "/r", "code_symbol", "交替符号名扫目录"),
    ("grep -rnE 'def (index_repository|prune_projects)' .", "/r", "code_symbol", "def 前缀找定义"),
    ("cd /r && sed -n '1,50p' a.c && grep -rn 'xlock_acquire' src/", "/r", "code_symbol",
     "多段复合：后段递归扫射不得被前段页内读盖住"),
    ("grep -rn 'markDead' coder-proxy/*.mjs", "/r", "code_symbol", "代码 glob 多文件"),
    ("grep -w -rn 'CBM_NOT_FOUND' src include", "/r", "code_symbol", "多目录递归"),
    ("rg --glob='*.rs' 'impl Display'", "/r", "code_symbol", "rg glob 限定代码扩展名"),
    ("grep -rn 'useWebViewSessionHandoff' shells/", "/r", "code_symbol", "驼峰符号扫目录"),
    ("grep -rln 'get_code_snippet' .", "/r", "code_symbol", "递归列文件找符号"),
    ("grep -e 'trace_path' -rn src/", "/r", "code_symbol", "-e 形式 pattern"),

    # ---------- text_search：grep 本就正确 ----------
    ("grep -n 'ERROR' daemon.err", "/r", "text_search", "日志文件"),
    ("cat x.log | grep -n 'timeout'", "/r", "text_search", "管道中游过滤"),
    ("ssh esweb168 'grep -rn foo /etc'", "/r", "text_search", "远端执行，本地索引零覆盖"),
    ("grep -n 'ShortcutId::Quit' bottom_pane/footer.rs", "/r", "text_search", "单代码文件页内定位"),
    ("grep -n 'REF\\|SHA' .gitlab/ci/workflows/deploy.yml", "/r", "text_search", "yaml 配置"),
    ("grep -rn 'workflow' docs/", "/r", "text_search", "docs 树是文档非代码"),
    ("grep -n 'version' package.json", "/r", "text_search", "json 配置"),
    ("grep -c 'ota_traces' ota_taxo2.py", "/r", "text_search", "单文件计数，页内"),
    ("tmux capture-pane -p -t w | grep -q DONE", "/r", "text_search", "远端包装+管道"),
    ("grep -rn 'session' ~/.codex-zkf/sessions/", "/r", "text_search", "会话档目录"),
    ("grep -n 'Host ' ~/.ssh/config", "/r", "text_search", "配置文件"),
    ("grep --include=*.md -rn 'deploy' .", "/r", "text_search", "include 限定 markdown"),
    ("ps aux | grep codebase-memory", "/r", "text_search", "管道过滤进程列表"),
    ("grep -rn 'the quick brown fox jumps' notes/", "/r", "text_search", "自然语言散文"),
    ("grep -n 'MEMORY.md' AGENTS.md", "/r", "text_search", "markdown 文档"),
    ("grep -rn 'foo' /Users/zkf/.coder/memory/", "/r", "text_search", "记忆仓非代码"),
    ("grep -n 'x' /tmp/out.txt", "/r", "text_search", "txt 文本"),

    # ---------- not_grep：根本不是 grep 调用，整帧不进语料 ----------
    ("git log --grep='stall' --oneline", "/r", "not_grep", "git log 的 --grep 是 git 的标志"),
    ("ls -la | head -20", "/r", "not_grep", "无 grep 段"),

    # ---------- unknown：判不出，不进任何结论分母 ----------
    ("grep -rn 'handleX' $TARGET_DIR", "/r", "unknown", "目标是未展开变量"),
    ("F=$(mktemp) && grep -n 'doWork' \"$F\"", "/r", "unknown", "目标由命令替换产生，解析不出"),
]


def main():
    passed = failed = 0
    fails = []
    for cmd, cwd, want, why in CASES:
        got, reason, detail = classify_intent(cmd, cwd)
        if got == want:
            passed += 1
        else:
            failed += 1
            fails.append((cmd, want, got, reason, why))

    # 辅助纯函数断言
    assert pattern_looks_symbolic("handleRequest")
    assert pattern_looks_symbolic("parse_session")
    assert not pattern_looks_symbolic("ERROR")
    assert not pattern_looks_symbolic("the quick brown fox")
    assert target_kind("a.go") == "code"
    assert target_kind("x.log") == "noncode"
    assert target_kind("src/") == "dir"
    assert target_kind("$X") == "unknown"
    # 引号感知切分：引号内的 ; 不切
    segs = split_pipeline("ssh h 'a; grep -r x'")
    assert len(segs) == 1, f"引号内分隔符被误切: {segs}"
    # && 切出的后段仍是首段
    segs = split_pipeline("cd x && grep -rn y src/")
    assert segs[-1][1] is True, "&& 后段应判为命令组首段"
    # 管道非首段
    segs = split_pipeline("cat a | grep b")
    assert segs[-1][1] is False, "管道中游应判为非首段"
    assert parse_grep_segment("ls -la") is None
    assert parse_grep_segment("grep -rn x src/")["recursive"] is True

    print(f"golden cases: {passed} passed, {failed} failed  (总 {len(CASES)})")
    for cmd, want, got, reason, why in fails:
        print(f"  FAIL want={want:12s} got={got:12s} reason={reason:22s} | {why}\n"
              f"       cmd: {cmd[:110]}")
    print("辅助纯函数断言: 全部通过")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
