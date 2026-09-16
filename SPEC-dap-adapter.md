# Feather DAP adapter（实验版 v1）

## 1. 边界

`DapAdapter` 是 DAP 与 `SPEC-debug-protocol.md` 所定义 Feather 调试协议之间的状态转换器。它不拥有 VM、`DebugTarget`、子进程、Socket 或字节流 framing。宿主实现 `DapChannel`，分别把完整的 UTF-8 JSON 对象送往 IDE 和 Feather target；DAP 的 `Content-Length` framing 仍属于宿主传输层。

adapter 与 `DebugTarget` 可以装在同一进程，也可以位于 IDE 插件或独立调试进程。`DapAdapterOptions::PrimarySourcePath` 对应 Feather 的空 `moduleId`；其他 DAP `source.path` 原样映射成模块 ID。路径规范化和远端路径映射由宿主负责。

`DispatchDapMessage` 与 `DispatchTargetMessage` 可由不同线程调用。adapter 不在持有内部锁时调用 channel，因此同进程桥接可以同步回送响应；channel 回调必须线程安全且不得抛异常。每条输入默认限制为 1 MiB，JSON 最大嵌套深度为 64。

## 2. v1 支持范围

- 会话：`initialize`、`launch`、`attach`、`configurationDone`、`disconnect`。
- 执行：`threads`（单一 Feather VM 线程）、`continue`、`next`、`stepIn`、`stepOut`、`pause`。
- 断点：`setBreakpoints`。同一 source 再次设置时先替换旧断点；最初返回 `verified:false`，target 解析位置后发送 DAP `breakpoint/changed`。
- Error 断点：`initialize` 声明一个默认关闭的 `error` exception filter；`setExceptionBreakpoints` 是否包含该 filter 会转换为 `Debugger.setPauseOnErrors`。target 的 Error 暂停映射为 DAP `stopped` 的 `reason:"exception"`，Error 摘要放入 `text`。
- 检查：`stackTrace`、`scopes`、`variables`。三个 scope 为 Locals、Stack、Globals；对象属性继续通过新的 `variablesReference` 展开。
- 事件：`initialized`、`stopped`、`continued`、`breakpoint`、`terminated` 和协议故障的 `output/stderr`。

frame ID 直接使用 Feather 当前暂停的 frame ID。`variablesReference` 由 adapter 分配，只在当前暂停内有效；收到 resumed、下一次 paused 或 executionFinished 后全部失效。所有不支持的 DAP 请求以失败 response 返回。

## 3. 暂不支持

v1 不实现条件断点、日志点、命中次数、`exceptionInfo`、表达式求值、变量修改、内存访问、反向执行、多线程、多 target、restart、terminate、source 内容请求及路径映射。具体 IDE/进程如何完成 launch 或 attach 也不由 adapter 决定；这两个请求只启用既有 Feather target。
