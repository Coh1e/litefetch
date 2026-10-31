# Issue tracker: GitHub

本项目的票与规格记录在 Coh1e/litefetch 的 GitHub Issues。
使用 gh CLI。每条命令都显式指定 --repo Coh1e/litefetch。

## 常用操作

- 读取：gh issue view <number> --repo Coh1e/litefetch --json number,title,body,labels,comments
- 列表：gh issue list --repo Coh1e/litefetch --state open --json number,title,labels
- 创建：gh issue create --repo Coh1e/litefetch --title "..." --body-file <file>
- 更新正文：gh issue edit <number> --repo Coh1e/litefetch --body-file <file>
- 标签：gh issue edit <number> --repo Coh1e/litefetch --add-label "..."
- 评论：gh issue comment <number> --repo Coh1e/litefetch --body-file <file>
- 关闭：gh issue close <number> --repo Coh1e/litefetch

多行正文先写入项目内临时文件，再用 --body-file 提交。
评论和其他对外消息必须取得人类明确授权。

## 技能约定

技能要求“发布到任务跟踪器”时，创建 GitHub Issue。
技能要求“获取相关票”时，读取对应 Issue。

子票优先使用 GitHub 原生 sub-issues。
原生关系不可用时，在子票正文顶部写 Part of #<parent>，并在父票正文中维护子票清单。

阻塞关系优先使用 GitHub 原生 issue dependencies。
原生关系不可用时，在正文顶部写 Blocked by: #<number>。
只有全部阻塞票关闭后，才解除阻塞。

## Pull requests

PR 不作为需求入口。

## 执行边界

票的状态和标签不等于恢复开发或执行授权。
执行分工与授权以 AGENTS.md 和人类当前明确指令为准。
重新确定目标或核对迁移范围时，读取 docs/product-reset.md。
HANDOFF.md 和旧工作流只提供历史追溯，不恢复旧指令。
