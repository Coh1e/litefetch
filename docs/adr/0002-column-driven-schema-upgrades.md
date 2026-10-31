# 旧库升级按列补齐，不按版本分支

Status: accepted (2026-10-31)

`TaskRepository` 打开任何 schema 1–8 的库时，保留全部行，用 `PRAGMA table_info(native_tasks)`
找出缺失的列并逐列 `ALTER TABLE ADD COLUMN`，最后把 `schema_version` 写成 9；不再为每个旧版本
维护一段固定 SQL。旧版本号只表示"可能缺哪些列"，列集合才是事实。

## Considered Options

- **只保证最新 schema**：旧库直接拒绝并提示备份。实现最简单，但用户会打不开自己的任务库，
  与 #29 "不得静默销毁无法导入的数据" 冲突。
- **逐版本重放固定 SQL**：每个版本一段语句。实际效果是 v2→v9 直接跳到 9，却漏掉
  `sha256`/`verified`/`priority`；不同分支还互相重复添加 `bt_uploaded`，v6 一升就撞
  `duplicate column name`。历史版本的具体列集合已经无法从代码可靠还原。
- **本次选择**：按缺失列补齐。v2、v6、v8 三个形状各有一条回归用例。

## Consequences

- 新增列时只改 `migrateSchema` 的补齐列表，不再复制版本分支。
- 破坏性变更（删列、改类型、改约束）仍需新的迁移步骤和 ADR；本决定只覆盖"加列"。
- 旧库不会被自动丢弃；升级失败仍抛 `StorageError`，不会带着半套 schema 继续运行。
