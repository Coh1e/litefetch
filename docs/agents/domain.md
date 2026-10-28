# Domain Docs

本项目采用 single-context 布局：
根目录 GLOSSARY.md 保存领域术语，docs/adr/ 保存架构决策。

## 阅读规则

探索领域概念前，阅读 GLOSSARY.md；
讨论架构或变更方案前，阅读 docs/adr/ 中相关决策。

文件不存在时继续当前已授权工作，无须为初始化创建空文件。
术语和决策明确后，由 domain-modeling 技能按需建立文档。

## 使用规则

任务标题、方案、假设和测试名称使用术语表中的领域词汇。
发现术语缺口时，交由 domain-modeling 整理。
方案与现有 ADR 冲突时，明确引用对应 ADR，
说明冲突和重新讨论的理由。
