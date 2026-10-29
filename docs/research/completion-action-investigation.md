# 完成动作（#25）为何在窗口层没验成

日期：2026-10-29。目标：让窗口在最后一批任务结束后自行退出（`onAllComplete = 2`），用 harness 断言 `WM_QUIT`。

## 已确认的事实

1. `MainWindow::refreshFromWorker()` 里确实有这段逻辑：
   `snapshot.allCompleteSequence != lastCompletionSequence_` → `completionAction_ == 2` → `PostMessageW(WM_CLOSE)`；
2. `TaskWorker::noteBatchProgress()` 只在 `batchActive_` 为真且 `jobs_/pendingStarts_` 都为空时递增序列；
3. 在 harness 里从窗口启动新任务**是成功的**（`statusOfTaskNamed` 从"等待中"变为"下载失败"），
   说明 `WM_COMMAND ID_TASK_START` 与选择行都没问题；
4. 但这次传输**以"下载失败"结束**，不是成功；窗口在 30 秒内没有退出。

## 目前的判断（尚未证实）

第 3 条说明"任务没启动"不是原因；第 4 条提示失败路径可能没有触发 `noteBatchProgress()`，
或者批次根本没被视为开始过。也可能是 harness 的 fixture 只服务有限次数，导致这次下载本身失败。

另外 `WM_CLOSE` 会走 `beginClose()`：先 `beginShutdown()`，等 worker 停稳再 `DestroyWindow`，
所以即使完成动作触发，退出也不是瞬间的——测试等待时间需要覆盖这段收尾。

## 下一步（按顺序，一轮内可完成）

1. 在用例里把任务的错误文本打出来，确认失败原因（区分 fixture 复用问题与真实失败）；
2. 若确认是 fixture 限制，换成本地文件服务或在启动前重建 fixture；
3. 用 `allCompleteSequence` 的测试访问器直接断言"批次结束后序列 +1"，再断言 `WM_QUIT`；
   把"通知链"和"退出链"分开验，失败点就能立刻定位。
