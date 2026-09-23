# MCP 接管率度量（issue #73）

回答一个问题：**本该由 codebase MCP 接管的代码检索，今天真的接住了多少？**

## 为什么需要它

工具描述里写 `use INSTEAD OF grep` 是无效的——48 小时真实流量实测，MCP 只占代码检索的 1.04%。
要改进就得先能度量，而度量必须基于**真实流量回放**，不能靠拍脑袋。

## 三个组件

| 文件 | 职责 |
|---|---|
| `grep_corpus.py` | 从会话 jsonl 抽取 grep/rg 帧，按意图分类，固化成语料 |
| `grep_corpus_test.py` | 分类器的 golden test（36 例手工标注） |
| `mcp_takeover_bench.py` | 把语料里的 `code_symbol` 帧真的重放给 MCP，出接管率 |
| `mcp-gap-24h.py` | 近 N 小时两个缺口指标：A=该用没用 MCP 的比例，B=用了 MCP 但没拿到期望结果的比例；`--json-out` 追加 JSONL 供迭代对比 |

## 跑法

```bash
# 1. 分类器自测（改了分类规则必跑）
python3 scripts/metrics/grep_corpus_test.py

# 2. 重新抽取语料（默认 48h 窗口）
python3 scripts/metrics/grep_corpus.py

# 3. 回放基准
python3 scripts/metrics/mcp_takeover_bench.py --out reports/takeover.jsonl
python3 scripts/metrics/mcp_takeover_bench.py --limit 40      # 抽样快跑
```

## 关键约束（踩过的坑）

**必须用 `./build/c/codebase-memory-mcp cli <tool>`，不能用 `bin/` 下的二进制。**
`bin/` 走 stdio 会代理到常驻 daemon，测到的是旧代码——改了代码却测不出差异，是最容易上当的一个坑。

**会话 jsonl 要去 fork 重放帧。** clone/fork 子会话把父历史整体重放写入自己的 jsonl，裸统计会翻 3 倍。
抽取器已处理：跳过 `session_meta` ±10s 的重放帧 + `call_id` 全局去重 + 按 basename 跨目录去重。

**语料已脱敏。** `grep-corpus-48h.jsonl` 来自真实会话命令，入库前扫过并替换了凭证字面量
（`glpat-*` / `gh?_*` / `AKIA*` / `Bearer *`）。**新抽取的语料入库前必须重跑这个扫描。**

## 判据

「接管成功」需同时满足：

1. 从帧的 cwd 能解析到一个已索引 project（且非 stale）
2. `search_code` 返回 `total_grep_matches > 0`
3. 命中结果里含原 grep 的 pattern 主标识符

失败分桶名即根因，可逐条人工复核：
`project_unindexed` / `project_stale` / `symbol_absent` / `regex_literal` / `pattern_shape` / `call_error`

## 已记录的基准

| 时点 | 接管率 | 说明 |
|---|---:|---|
| PR #74 之前 | 24.4%（53/217） | BRE 多选 `\|` 零命中是最大阻碍 |
| PR #74 之后 | **53.5%（116/217）** | 归一化 BRE 多选，+29.1pp |

剩余未接管的最大一块是 `pattern_shape`（48 帧）：主标识符过于宽泛，MCP 返回几百条等于没答。
这属于结果排序/收窄问题，与 pattern 改写不同源。

### A/B 缺口（`mcp-gap-24h.py`）
- A 分母 = code_symbol grep（放行 + 被 hook 拦）+ MCP 调用；分子 = 放行的 code_symbol grep + 被拦后下一步没转 MCP 的。
- B 分母 = MCP 调用；分子 = empty / error / project_not_found / root_missing / timeout / flood / snippet_ambiguous / no_output，以及命中后 15 分钟内又用同一关键词做跨文件 grep 的。
- 2026-09-23 基线（24h）：A 63.6%（150/236），B 45.1%（32/71）。按「≤3 个具体文件 = 页内精定位」修正口径后 A 为 55.0%（105/191）。B 的 32 次里：10 次 error 全部是 daemon 重启后 shim 报 Transport closed，4 次 not_found + 2 次 timeout 发生在全量重建窗口（#81）。

## 已知局限

- 意图分类器的 36 条标注样本由实现者自己标注，未做独立盲标一致性检验
- 度量口径只覆盖 `grep`/`rg`，**不含窄读绕过**（`sed -n` / `awk NR>=` / `cat`）——实测这是真实的绕过形态，但目前没有度量
