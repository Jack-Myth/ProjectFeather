# Feather 调试协议（实验版 v1）

## 1. 分层与嵌入边界

调试能力分成两层：

- core 层只定义 VM 安全点、只读暂停上下文和 `VmDebugController` 接口。没有安装控制器时，VM 不解析协议、不创建调试线程，也不调用调试实现。
- 可选调试实现提供 `DebugTarget`。默认构建把它静态打包进 runtime，`-Ddebugger=false` 可完整剥离。它接收完整的 UTF-8 JSON 消息，并通过宿主提供的 `DebugChannel` 回发完整消息。宿主只负责保留消息边界及传输；管道、Socket、WebSocket 均在库外。

`DebugTarget` 可与 VM 嵌入同一宿主，也可由宿主把消息桥接到独立调试进程。Feather 协议表达语言自身的调试语义；IDE 适配器负责在 DAP 等外部协议与 Feather 协议之间转换，VM 不直接依赖 DAP。

默认每次最外层 `Vm::Run` 都产生一次执行结束通知。把多个 VM 调用组成一个用户可见程序的宿主可关闭 `DebugTargetOptions::ReportEachExecution`，并在整个程序结束时调用一次 `NotifyExecutionFinished()`。

宿主以 `Vm::SetDebugController(shared_ptr)` 安装或移除控制器。更换控制器只允许在 VM 空闲时进行。VM 持有控制器的共享所有权，因此控制器至少活到移除或 VM 销毁；channel 由 `DebugTarget` 共享持有。

传输断开时，宿主必须调用 `DebugTarget::Close()`。关闭是永久且线程安全的，会禁用目标并立即释放正在等待命令的 VM；VM 空闲后宿主可再用 `SetDebugController(nullptr)` 移除它。仅释放宿主自己的 `shared_ptr` 不足以关闭目标，因为 VM 仍持有控制器。

## 2. 线程与安全点

VM 仍是单线程运行时。宿主可以从传输线程调用 `DebugTarget::DispatchProtocolMessage()`；该调用只解析消息、修改独立协议状态或排队命令，不直接访问 VM。

VM 在每条 Feather 指令执行前调用控制器安全点。断点、外部暂停和单步只在该位置生效。产生 Error 结果的操作和返回 Error 的函数还会在结果边界同步调用 `VmDebugController::OnError`；回调看到的栈顶位置固定为产生该结果的指令，而不是下一条指令。暂停后，VM 执行线程在 `DebugTarget` 内等待命令；调用栈、局部变量、全局变量和对象属性也只由该线程读取。宿主的 `DebugChannel::SendProtocolMessage()` 可能从分发线程或 VM 线程调用，必须自行保证线程安全且不得抛异常。

同步 Native 调用内部没有 Feather 安全点；普通 Native 调用对调试器是原子的。Native 显式重入同一 VM 时，重入的 Feather 指令仍有安全点。

控制器抛出的异常不会改变脚本语义：VM 会移除该控制器并继续运行。`DebugTarget` 自身不让协议或 channel 故障越过 VM 调试接口。

## 3. 消息封装

一次 `DispatchProtocolMessage` 接收恰好一个 JSON 对象；一次 channel 回调发送恰好一个 JSON 对象。协议不规定字节流分帧。

`DebugTarget` 默认拒绝超过 1 MiB 的单条输入消息，宿主可在构造时设置更小或更大的正数限制；JSON 嵌套深度最多 64 层。传输层仍应在完整消息到达前实施自己的连接级缓存上限。

请求：

```json
{"id":1,"method":"Debugger.enable","params":{}}
```

成功响应：

```json
{"id":1,"result":{}}
```

失败响应：

```json
{"id":1,"error":{"code":"InvalidRequest","message":"..."}}
```

通知没有 `id`，使用 `method` 和 `params`。请求 `id` 是非负整数。未知字段应忽略；缺少或类型错误的必需字段应返回错误。无法取得请求 ID 的畸形消息产生 `Debugger.protocolError` 通知。

## 4. v1 方法

### 4.1 会话与执行

- `Debugger.enable`：启用目标。重复调用成功且无副作用。
- `Runtime.runIfWaitingForDebugger`：解除宿主可选的启动等待。只有以
  `DebugTargetOptions::WaitForDebugger` 创建的宿主需要在执行 VM 前调用
  `WaitForExecutionPermission()`；普通嵌入不受影响。方法可重复调用。
- `Debugger.disable`：禁用目标；若已暂停则恢复执行。
- `Debugger.pause`：请求在下一个安全点暂停。请求只确认已接受，不表示已经暂停。
- `Debugger.resume`：从暂停点继续。
- `Debugger.stepInto`：执行到下一个不同指令位置，允许进入被调函数。
- `Debugger.stepOver`：执行到当前或更浅调用深度的下一个不同指令位置。
- `Debugger.stepOut`：执行到比当前更浅的调用深度。
- `Debugger.setPauseOnErrors`：参数 `enabled` 为 bool；控制是否在 VM 报告 Error 结果边界时暂停，默认关闭。`Debugger.disable` 同时恢复为关闭。

`resume` 和单步方法仅在暂停时有效。继续执行会使本次暂停的所有 frame ID 和 object ID 立即失效。

### 4.2 断点

- `Debugger.setBreakpoint` 参数为 `moduleId`、从 1 开始的 `line`，以及可选的从 1 开始的 `column` 和非空字符串 `condition`。省略 column 表示该行任意列。返回数值 `breakpointId`。条件在断点位置首次解析后、每次命中时于该暂停帧求值；结果按 Feather truthiness 判断。条件编译或执行失败时仍暂停，并在 `Debugger.paused.conditionError` 中报告错误，避免失效条件静默放过执行。
- `Debugger.removeBreakpoint` 参数为 `breakpointId`。

断点按源码位置工作，需要内存位置表或匹配的 `.fbs`。v1 在执行首次经过匹配且标记为 breakable 的位置时把断点惰性解析到函数常量索引和 PC，并发送 `Debugger.breakpointResolved`；之后只在该指令停下。编译器生成的函数绑定及隐式返回保留诊断和单步位置，但不会抢先解析同一行断点。没有符号的位置不能解析源码断点。

Error 断点是一个会话级策略，不占用 `breakpointId`，也不伪装成源码断点。开启后，下列结果边界会停下：非法运算或调用、未定义全局读取、取成员、写成员等操作得到 Error；Native 或 Feather 函数返回 Error。这里按“结果是 Error”判断而不是按对象是否首次创建判断，因此同一个 Error 经过多个函数返回边界可以停多次，读取或写入成员本身得到一个已存在的 Error 也会停；普通局部变量读取和全局变量读取到已存在 Error 不停。Error 仍是普通值，暂停不会引入传播、栈展开或控制流变化。

### 4.3 检查

- `Debugger.getStackTrace` 返回从栈顶开始的 frame。每个 frame 包含本次暂停内有效的 `frameId`、`functionId`、`functionName`、`pc` 和可用源码位置。缺少 v3 `.fbs` 调试名时，显示名回退为 `<function #N>`。
- `Debugger.getVariables` 参数为 `frameId` 和 `scope`；scope 可为 `locals`、`stack` 或 `globals`。
- `Debugger.getProperties` 参数为本次暂停返回的 `objectId`。ScriptObject 返回原始成员和 `[[MetaObject]]`；Error 返回 `message`；普通 NativeObject 只返回显式登记的 GC 可见成员。
- `Debugger.evaluate` 参数为 `frameId` 和非空字符串 `expression`，返回普通值摘要。表达式采用 Feather 表达式语法，可读取该帧当前词法范围内的具名局部变量和所属模块 globals；缺少局部符号时只能通过 globals 求值。求值发生在暂停 VM 线程上，使用同一 VM 的对象和函数身份。
- `Debugger.setVariable` 使用非空字符串 `expression` 计算新值。scope 变量要求 `frameId`、`scope`（`locals`、`stack` 或 `globals`）和 `name`；对象属性要求 `objectId` 和 `name`。局部变量只允许写当前词法范围内的具名变量或无符号帧的 `$N`，stack 使用 `$N`，global 可创建或覆盖当前帧所属模块的名字；对象属性必须是当前可枚举且名字无歧义的可写属性。返回写入后的值摘要。

调试表达式是表达式而非语句，不能声明函数或变量。函数调用、Native 调用和成员赋值仍可能产生脚本可见副作用；对求值临时函数自身参数或临时 global 的直接赋值不会回写暂停帧，修改帧变量应使用 `Debugger.setVariable`。求值期间不触发安全点、Error 断点或 executionFinished 通知，因此不会递归进入调试器；它仍共享当前 VM 的指令预算和对象上限。

值摘要包含 `type` 和可读 `description`；对象另含 `objectId`。有限 number 还包含 JSON 数值 `value`，bool 和 string 包含其原值。对象 ID 不是地址，不能跨暂停复用。

## 5. v1 通知

- `Debugger.paused`：含 `stopId`、`reason`（`pause`、`breakpoint`、`step`、`error`）和栈顶位置。Error 停顿另含 `origin`（`operation` 或 `functionReturn`）以及普通值摘要格式的 `error`；其中的 `objectId` 可在本次暂停中传给 `Debugger.getProperties`。条件断点求值失败时另含字符串 `conditionError`。
- `Debugger.resumed`：含刚结束的 `stopId`。
- `Debugger.breakpointResolved`：含断点 ID 及解析后的位置。
- `Debugger.executionFinished`：一次最外层 `Vm::Run`/`RunModule` 正常返回或抛出故障后发送，参数 `faulted` 表示是否抛出。
- `Debugger.protocolError`：无法关联到合法请求 ID 的消息错误。

每次暂停分配递增 `stopId`。检查请求的响应只在 VM 仍停于同一 `stopId` 时有效。

## 6. 暂不属于 v1

DAP 转换由独立 `DapAdapter` 实现，见 `SPEC-dap-adapter.md`。网络监听、进程管理、鉴权、日志点、命中次数、数据断点、热重载及 Native C++ 单步仍不属于 v1 target。v3 `.fbs` 提供可断点标记、函数和当前词法作用域内的局部变量名；没有局部符号的手写函数仍使用 `$0`、`$1` 等槽位名，但这类局部槽只能修改，不能在表达式中以 `$N` 引用。
