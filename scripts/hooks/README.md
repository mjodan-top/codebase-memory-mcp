# grep-intercept PreToolUse hook

拦截 agent 会话里的跨文件 `grep`/`rg` 代码扫射，deny 并重定向到 codebase-memory MCP 索引（`search_graph` / `search_code` / `get_code_snippet` / `trace_path`）。

## 效果（2026-07-22 实测，spike S3）

同款 3 个代码定位问题、同一 worktree、同一模型：

| 配置 | MCP 主导 | 备注 |
| --- | --- | --- |
| 仅 AGENTS.md 纪律文本（spike S1） | 0/3 | 全程 grep/sed，索引零调用 |
| 本 hook（spike S3） | **3/3** | Q1/Q3 首跳 grep 被拦后即转 MCP 链；Q2 直接首跳 MCP |

live 会话拦截 3 次、放行 31 次（页内精定位、非代码文本、管道过滤零误伤）。

## 拦截/豁免规则（与 metrics 设计 1.2 分类一致）

- **拦**：grep/rg 递归（`-r`）/ 多文件 / glob / rg 无文件参数（默认递归扫 cwd）——即跨文件"找定义/找调用方"扫射。
- **放行**：
  - 单个具体代码文件上的 grep（页内精定位，纪律第 6 条唯一合法退化）；
  - 目标全为非代码文本（`.log/.toml/.json/.md/...`、`/logs/`、`daemon.err` 等）；
  - 管道中游的 grep（过滤已有输出）与 stdin grep；
  - 非 grep/rg 命令一律不碰。

## 安装

任一 config 层（用户级 `$CODER_HOME/hooks.json` 或项目级 `.codex/hooks.json`）：

```json
{
  "hooks": {
    "PreToolUse": [
      {
        "matcher": "^Bash$",
        "hooks": [
          {
            "type": "command",
            "command": "python3 /path/to/scripts/hooks/grep-intercept.py",
            "timeout": 10,
            "statusMessage": "checking grep discipline"
          }
        ]
      }
    ]
  }
}
```

首次运行 TUI 会弹 hook 信任审核（审查后信任）；非交互 `coder exec` 可用 `--dangerously-bypass-hook-trust`。

### 已实证的坑

- **env 必须用 `CODER_HOME`**：`CODEX_HOME`（legacy 别名）下 hooks.json 不会进 hooks discovery（sessions 落盘同样不认）；`find_codex_home()` 优先 `CODER_HOME`。
- shell 类工具的 hook `tool_name` 是 `Bash`（`exec_command`/`shell` 统一映射），matcher 写 `^Bash$` 即可。
- 调试：设 `CBM_HOOK_LOG=/path/to/log` 落每笔 allow/deny 决策。

## 单测

```
python3 scripts/hooks/grep_intercept_test.py
```
