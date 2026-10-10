# Feather 运行期 Checkpoint 契约（首版）

本文规定同一台 VM、同一次最外层 `Run` 内的轻量控制流回退。Checkpoint 不产生字节，
不访问磁盘，也不等同于 `SPEC-snapshot.md` 定义的完整状态快照。需要跨 VM、跨进程或
持久化恢复时必须使用 Snapshot。

## 脚本接口

标准发行提供独立的 `checkpoint.felib`：

```feather
var checkpoint = import("checkpoint");
var id = checkpoint.Capture();
checkpoint.Restore(id);
checkpoint.Unload(id);
```

- `Capture()` 创建 checkpoint，并返回大于零、可由 `number` 精确表示的 ID。
- `Restore(id)` 成功时不从当前调用点继续，而从对应 `Capture()` 之后继续；该次
  `Capture()` 表达式仍得到同一个 ID。失败返回 Error。
- `Unload(id)` 删除 checkpoint，成功返回 `true`；无效、已卸载或过期 ID 返回 Error。
- checkpoint 可重复恢复，直到显式卸载、创建它的帧失效，或最外层 `Run` 结束。
  ID 在同一 VM 内不复用。

参数数量、类型、非整数、非有限数、零以及超出 `2^53 - 1` 的 ID 都是可观察的 Error。
当前上限为同时存在 256 个 checkpoint，单个 checkpoint 的全部已保存操作数栈合计不
超过 65,536 个值；调用帧上限仍沿用 VM 的 1,024 帧限制。

## 方案 B：控制流与栈语义

Checkpoint 保存创建时的脚本调用帧身份、各帧下一条指令 PC，以及各帧的表达式操作数
栈。它不复制局部槽位。恢复时：

1. 当前调用栈必须仍以前述同一批帧实例为前缀；递归调用产生的新帧即使函数相同也不是
   同一帧实例。
2. 创建点之上的后续子调用帧被弹出。
3. 保留下来的帧恢复 PC 和表达式栈，但局部槽位维持恢复发生前的当前值。
4. 在创建点顶层帧的表达式栈压入 ID，模拟 `Capture()` 再次返回。

因此 checkpoint 可以从子函数跳回尚未返回的调用者，但创建它的任一帧一旦返回，就不再
可恢复。脚本必须用保留的局部变量改变分支条件，否则反复恢复到同一点可能形成无限循环。

## 不恢复的状态

Checkpoint 不恢复或复制以下内容：

- 局部变量、模块全局变量；
- ScriptObject 堆、NativeObject 内部或外部资源状态；
- 已经过的 native 副作用、文件位置、时钟、渲染状态；
- VM 指令预算、调试器状态、宿主状态；
- Snapshot 文件、模块清单或任何磁盘格式。

保存在 checkpoint 操作数栈中的 Value 在 checkpoint 有效期间属于 GC 根，因而仅由这些
栈值引用的 ScriptObject 不会被提前回收。Checkpoint 自身不进入 `CaptureSnapshot()` 的
对象图；从 Snapshot 恢复得到的新 VM 没有旧 checkpoint。

## 运行时边界

三个操作都必须由脚本直接调用对应 Native 方法。捕获和恢复要求当前 VM 恰有一个活动
`Run`、native 深度恰为 1，并位于尚未取得结果的 `Call` 指令中；native 回调脚本后再次
调用 checkpoint 会被拒绝。成功恢复由解释循环在 Native 调用返回边界应用，不增加 ISA
opcode，也不让 Native C++ 栈参与续执行。

最外层 `Run` 无论正常返回还是异常退出，都会清空其 checkpoint。后续 `Run` 即使在同一
VM 上执行，也不能恢复旧 ID。嵌入接口对应 `Vm::CaptureCheckpoint()`、
`Vm::RequestCheckpointRestore(id)` 和 `Vm::UnloadCheckpoint(id)`；公开接口与模块 ABI
当前仍属实验性。
