# Feather 时间模块（首版）

`time` 是独立 Native 模块。VM 核心不读取系统时钟，也不阻塞线程；脚本通过
`var time = import("time");` 显式取得模块。模块使用固定 GUID，Native 类型名在该
GUID 下唯一。

所有时长和时间点均使用 `number` 表示秒，可包含小数。要求时长和 deadline 为有限
非负数；参数数量或类型错误返回 Error。

| 调用 | 返回及行为 |
|---|---|
| `time.Monotonic()` | 返回单调时钟时间点。只允许在当前运行会话中求差或作为 `SleepUntil` 的 deadline。 |
| `time.UnixTime()` | 返回 Unix epoch 起经过的秒数。系统校时可能使其跳变，不应用于帧 delta。 |
| `time.Sleep(seconds)` | 同步阻塞当前 VM 线程；成功返回 `null`。`0` 是有效时长。 |
| `time.SleepUntil(deadline)` | 阻塞到单调时钟到达 deadline；deadline 已过则立即返回 `null`。 |
| `time.CreateClock()` | 创建一个 `Clock` NativeObject，并以当前单调时间为基准。 |
| `time.Tick(clock)` | 返回距该 Clock 上次创建、重置或 Tick 经过的秒数，并更新基准。 |
| `time.Reset(clock)` | 把 Clock 基准重置到当前单调时间，返回 `null`。 |

`Sleep` 和 `SleepUntil` 不提供异步调度，也不处理窗口事件。需要在等待期间响应窗口
事件时应使用 `render2d.WaitEvent`；帧率、固定时间步、最大 delta、FPS 统计和任务调度
均由脚本框架实现。

## 快照

操作系统单调时钟的绝对值不写入快照，也不保证跨进程或重启可比较。脚本若直接把
`Monotonic()` 的结果保存在变量里，应在 `snapshot.Checkpoint(path)` 的恢复分支重新
建立 deadline。

`Clock` 使用类型身份 `TimeModuleGuid + "Clock"`，payload 为空。恢复前重新导入
`time` 后，反序列化创建新 Clock；`FinalizeRestore` 把其基准绑定到新运行会话的当前
单调时间。因此离线时间不会进入下一次 `Tick`，但恢复完成后脚本实际执行和等待的
时间会正常计入。`UnixTime()` 始终反映现实时间，包括离线期间。
