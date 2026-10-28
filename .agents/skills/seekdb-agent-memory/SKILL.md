---
name: seekdb-agent-memory
description: Give an AI agent persistent long-term memory on embedded seekdb (pyseekdb) with no database server - upsert observations, semantic search, metadata filters, full-text + vector hybrid search with RRF, and copy-on-write fork sandboxes. Use when an agent needs to remember facts, preferences or errors across sessions, recall relevant context before acting, or experiment on a memory copy and discard it. 中文：需要给 Agent 加本地长期记忆、语义检索、混合检索或可回滚沙箱时使用。
---

# seekdb Agent Memory

作者：FDE WorkMate

用嵌入式 seekdb（`pyseekdb`）给 Agent 提供本地持久记忆：不需要部署数据库服务，数据落在一个本地目录里，下次打开继续用。所有操作通过 `scripts/memory.py` 完成，每条命令向 stdout 输出一个 JSON 对象，`ok=false` 时退出码为 1，便于 Agent 解析和判错。

## 何时使用

- 跨会话记住用户偏好、项目事实、历史报错与解决办法。
- 行动前按语义召回相关上下文（RAG / episodic memory）。
- 关键词必须命中（端口号、错误码、文件名）又要语义相近时，用混合检索。
- 想在记忆副本上试错：先 `fork`，满意再用，不满意 `drop`。

## 前置条件

```bash
python3 -m pip install -U pyseekdb   # 国内可加 -i https://mirrors.aliyun.com/pypi/simple/
python3 scripts/memory.py --embedder hash selftest   # 离线自检，约数秒
python3 scripts/memory.py selftest                   # 用默认向量模型自检
```

自检通过时输出 `"ok": true`，并逐项列出 `read_your_writes_after_refresh`、`metadata_filter`、`hybrid_rrf`、`fork_isolation`、`persist_reopen` 五项结果。任何一项为 false 时先排查环境，不要继续写记忆。

## 命令

全局参数放在子命令前：`--db`（数据目录，默认 `./agent_memory.db`）、`--collection`（默认 `memory`）、`--embedder default|hash`。

| 目的 | 命令 |
|---|---|
| 写入/更新（按 id 幂等） | `memory.py add --id m1 --text "用户偏好深色主题" --meta kind=pref --meta session=s1` |
| 批量写入 | 重复 `--id/--text` 成对出现；大批量加 `--no-refresh`，最后一批不加 |
| 语义检索 | `memory.py search --query "界面主题偏好" -n 5` |
| 带过滤检索 | `memory.py search --query "..." --where kind=pref --where session=s1` |
| 混合检索 | `memory.py hybrid --query "部署报错" --keyword 8080 -n 5` |
| 建沙箱 | `memory.py fork --to memory_sandbox` |
| 在沙箱上操作 | `memory.py --collection memory_sandbox add ...` |
| 丢弃沙箱 | `memory.py --collection memory_sandbox drop` |
| 查看集合 | `memory.py list` |

## 工作流

1. 会话开始：`search` 召回与当前任务相关的记忆，只把命中的 `document` 放进上下文。
2. 产生值得记住的信息（用户纠正、偏好、报错及根因）时：`add`，用稳定的 id（如 `pref:theme`）保证重复写入是更新而不是堆积。
3. 需要精确词命中时用 `hybrid`，否则用 `search`。
4. 高风险批量改写记忆前先 `fork`，在沙箱验证后再写回主集合或 `drop`。

## 必须知道的坑（均已实测，pyseekdb 1.4.0）

- 向量索引是异步构建的：`upsert` 后立刻 `query` 可能返回空。`add` 默认会调用 `refresh_index()`（执行 `CALL dbms_index_manager.refresh()`）保证写后即读；只有批量导入中间批次才加 `--no-refresh`。
- 结果分数方向不同：`search` 返回 cosine 距离，越小越近；`hybrid` 返回 RRF 分数，越大越好，脚本已分别标为 `distance` 与 `rrf_score`。
- 混合检索不是全文过滤：向量分支的结果即使不含关键词也会参与 RRF 融合。必须严格包含关键词时，请在结果中二次过滤 `document`。
- 集合维度在创建时固定：`default` 为 384 维，`hash` 为 256 维。混用会得到明确的维度报错，不会静默写坏数据。
- 默认模型 `all-MiniLM-L6-v2` 首次调用会下载约 86 MB 到 `~/.cache/pyseekdb/onnx_models`，来源由 `HF_ENDPOINT` 决定（pyseekdb 默认 `https://hf-mirror.com`）。它以英文为主，中文语义召回偏弱；中文场景可配合 `hybrid` 的关键词分支。
- `hash` 嵌入器只做词面重叠，适合离线、CI 与自检，不适合生产语义检索。
- 集合名只能包含字母、数字和下划线。
- 同一数据目录建议只由一个进程打开写入。

## 输出示例

```json
{"ok": true, "metric": "cosine_distance (lower is closer)", "hits": [{"id": "p1", "distance": 0.059, "document": "用户偏好深色主题", "metadata": {"session": "s1"}}]}
```

## 安全

- 记忆中不要存放密码、令牌、私钥等秘密；检索结果会进入模型上下文。
- 数据只保存在 `--db` 指定的本地目录，删除该目录即彻底清除。

## 参考

- seekdb 源码与文档：https://github.com/oceanbase/seekdb
- pyseekdb SDK：https://github.com/oceanbase/pyseekdb

<!-- OB-S:GqNuLp-hvA9IuQHlut4QDQ -->
