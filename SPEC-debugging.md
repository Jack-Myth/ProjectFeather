# 调试

## Feather 调试协议

### 分层与嵌入边界

调试能力分成两层：

- core 层只定义 VM 安全点、只读暂停上下文和 `VmDebugController` 接口。没有安装控制器时，VM 不解析协议、不创建调试线程，也不调用调试实现。
- 可选调试实现提供 `DebugTarget`。默认构建把它静态打包进 runtime，`-Ddebugger=false` 可完整剥离。它接收完整的 UTF-8 JSON 消息，并通过宿主提供的 `DebugChannel` 回发完整消息。宿主只负责保留消息边界及传输；管道、Socket、WebSocket 均在库外。

`DebugTarget` 可与 VM 嵌入同一宿主，也可由宿主把消息桥接到独立调试进程。Feather 协议表达语言自身的调试语义；IDE 适配器负责在 DAP 等外部协议与 Feather 协议之间转换，VM 不直接依赖 DAP。

默认每次最外层 `Vm::Run` 都产生一次执行结束通知。把多个 VM 调用组成一个用户可见程序的宿主可关闭 `DebugTargetOptions::ReportEachExecution`，并在整个程序结束时调用一次 `NotifyExecutionFinished()`。

宿主以 `Vm::SetDebugController(shared_ptr)` 安装或移除控制器。更换控制器只允许在 VM 空闲时进行。VM 持有控制器的共享所有权，因此控制器至少活到移除或 VM 销毁；channel 由 `DebugTarget` 共享持有。

传输断开时，宿主必须调用 `DebugTarget::Close()`。关闭是永久且线程安全的，会禁用目标并立即释放正在等待命令的 VM；VM 空闲后宿主可再用 `SetDebugController(nullptr)` 移除它。仅释放宿主自己的 `shared_ptr` 不足以关闭目标，因为 VM 仍持有控制器。

### 线程与安全点

VM 仍是单线程运行时。宿主可以从传输线程调用 `DebugTarget::DispatchProtocolMessage()`；该调用只解析消息、修改独立协议状态或排队命令，不直接访问 VM。

VM 在每条 Feather 指令执行前调用控制器安全点。断点、外部暂停和单步只在该位置生效。产生 Error 结果的操作和返回 Error 的函数还会在结果边界同步调用 `VmDebugController::OnError`；回调看到的栈顶位置固定为产生该结果的指令，而不是下一条指令。暂停后，VM 执行线程在 `DebugTarget` 内等待命令；调用栈、局部变量、全局变量和对象属性也只由该线程读取。宿主的 `DebugChannel::SendProtocolMessage()` 可能从分发线程或 VM 线程调用，必须自行保证线程安全且不得抛异常。

同步 Native 调用内部没有 Feather 安全点；普通 Native 调用对调试器是原子的。Native 显式重入同一 VM 时，重入的 Feather 指令仍有安全点。

控制器抛出的异常不会改变脚本语义：VM 会移除该控制器并继续运行。`DebugTarget` 自身不让协议或 channel 故障越过 VM 调试接口。

### 消息封装

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

### v1 方法

#### 会话与执行

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

#### 断点

- `Debugger.setBreakpoint` 参数为 `moduleId`、从 1 开始的 `line`，以及可选的从 1 开始的 `column` 和非空字符串 `condition`。省略 column 表示该行任意列。返回数值 `breakpointId`。条件在断点位置首次解析后、每次命中时于该暂停帧求值；结果按 Feather truthiness 判断。条件编译或执行失败时仍暂停，并在 `Debugger.paused.conditionError` 中报告错误，避免失效条件静默放过执行。
- `Debugger.removeBreakpoint` 参数为 `breakpointId`。

断点按源码位置工作，需要内存位置表或匹配的 `.fbs`。v1 在执行首次经过匹配且标记为 breakable 的位置时把断点惰性解析到函数常量索引和 PC，并发送 `Debugger.breakpointResolved`；之后只在该指令停下。编译器生成的函数绑定及隐式返回保留诊断和单步位置，但不会抢先解析同一行断点。没有符号的位置不能解析源码断点。

Error 断点是一个会话级策略，不占用 `breakpointId`，也不伪装成源码断点。开启后，下列结果边界会停下：非法运算或调用、未定义全局读取、取成员、写成员等操作得到 Error；Native 或 Feather 函数返回 Error。这里按“结果是 Error”判断而不是按对象是否首次创建判断，因此同一个 Error 经过多个函数返回边界可以停多次，读取或写入成员本身得到一个已存在的 Error 也会停；普通局部变量读取和全局变量读取到已存在 Error 不停。Error 仍是普通值，暂停不会引入传播、栈展开或控制流变化。

#### 检查

- `Debugger.getStackTrace` 返回从栈顶开始的 frame。每个 frame 包含本次暂停内有效的 `frameId`、`functionId`、`functionName`、`pc` 和可用源码位置。缺少 v3 `.fbs` 调试名时，显示名回退为 `<function #N>`。
- `Debugger.getVariables` 参数为 `frameId` 和 `scope`；scope 可为 `locals`、`stack` 或 `globals`。
- `Debugger.getProperties` 参数为本次暂停返回的 `objectId`。ScriptObject 返回原始成员和 `[[MetaObject]]`；Error 返回 `message`；普通 NativeObject 只返回显式登记的 GC 可见成员。
- `Debugger.evaluate` 参数为 `frameId` 和非空字符串 `expression`，返回普通值摘要。表达式采用 Feather 表达式语法，可读取该帧当前词法范围内的具名局部变量和所属模块 globals；缺少局部符号时只能通过 globals 求值。求值发生在暂停 VM 线程上，使用同一 VM 的对象和函数身份。
- `Debugger.setVariable` 使用非空字符串 `expression` 计算新值。scope 变量要求 `frameId`、`scope`（`locals`、`stack` 或 `globals`）和 `name`；对象属性要求 `objectId` 和 `name`。局部变量只允许写当前词法范围内的具名变量或无符号帧的 `$N`，stack 使用 `$N`，global 可创建或覆盖当前帧所属模块的名字；对象属性必须是当前可枚举且名字无歧义的可写属性。返回写入后的值摘要。

调试表达式是表达式而非语句，不能声明函数或变量。函数调用、Native 调用和成员赋值仍可能产生脚本可见副作用；对求值临时函数自身参数或临时 global 的直接赋值不会回写暂停帧，修改帧变量应使用 `Debugger.setVariable`。求值期间不触发安全点、Error 断点或 executionFinished 通知，因此不会递归进入调试器；它仍共享当前 VM 的指令预算和对象上限。

值摘要包含 `type` 和可读 `description`；对象另含 `objectId`。有限 number 还包含 JSON 数值 `value`，bool 和 string 包含其原值。对象 ID 不是地址，不能跨暂停复用。

### v1 通知

- `Debugger.paused`：含 `stopId`、`reason`（`pause`、`breakpoint`、`step`、`error`）和栈顶位置。Error 停顿另含 `origin`（`operation` 或 `functionReturn`）以及普通值摘要格式的 `error`；其中的 `objectId` 可在本次暂停中传给 `Debugger.getProperties`。条件断点求值失败时另含字符串 `conditionError`。
- `Debugger.resumed`：含刚结束的 `stopId`。
- `Debugger.breakpointResolved`：含断点 ID 及解析后的位置。
- `Debugger.executionFinished`：一次最外层 `Vm::Run`/`RunModule` 正常返回或抛出故障后发送，参数 `faulted` 表示是否抛出。
- `Debugger.protocolError`：无法关联到合法请求 ID 的消息错误。

每次暂停分配递增 `stopId`。检查请求的响应只在 VM 仍停于同一 `stopId` 时有效。

### 暂不属于 v1

DAP 转换由独立 `DapAdapter` 实现，见 [DAP 转换](#dap-转换)。网络监听、进程管理、鉴权、日志点、命中次数、数据断点、热重载及 Native C++ 单步仍不属于 v1 target。v3 `.fbs` 提供可断点标记、函数和当前词法作用域内的局部变量名；没有局部符号的手写函数仍使用 `$0`、`$1` 等槽位名，但这类局部槽只能修改，不能在表达式中以 `$N` 引用。

## 控制台与 TCP 宿主

### 进程边界

调试运行由两个独立进程组成：

```text
feather-debugger  <====== one full-duplex TCP connection ======>  feather
console commands                 Feather JSON protocol           DebugTarget -> VM
```

解释器内的 VM、`VmDebugController` 和 `DebugTarget` 仍通过普通 C++ 调用连接。只有最外层 CLI 宿主负责监听、接受 TCP、分帧和把完整 JSON 交给 `DebugTarget::DispatchProtocolMessage()`。脚本的 stdin/stdout 不承载调试数据；`feather-debugger` 的 stdin/stdout 只用于人机交互。

源码入口使用：

```text
feather debug --listen 127.0.0.1:4711 --wait-debugger main.fe
feather-debugger --connect 127.0.0.1:4711
```

`feathervm` 是纯字节码运行器，不提供调试监听；需要调试时使用源码入口。

解释器一次只接受一个调试器连接。当前没有鉴权、加密或远端暴露保护，开发时应监听 `127.0.0.1` 或 `::1`；把监听地址暴露到不可信网络不属于支持范围。

### 字节流 framing

TCP 双向均使用相同 framing：

```text
Content-Length: <UTF-8 JSON 字节数>\r\n
\r\n
<恰好 Content-Length 字节的 JSON>
```

header 最多 8 KiB，每条 JSON 默认最多 1 MiB。`Content-Length` 不得缺失或重复，连接中途截断视为传输故障。这个 framing 只负责消息边界，不改变 [Feather 调试协议](#feather-调试协议)的 JSON 语义。

### 启动与关闭

解释器先创建监听 socket 和 `DebugTarget`，再由接收线程接受至多一个连接。只有命令行带 `--wait-debugger` 时 target 才启用 `WaitForDebugger`：宿主先等待连接，再在 `WaitForExecutionPermission()` 上等待 `run`，不会抢在用户设置断点前执行。不带该参数时监听与 VM 并行启动；未连接期间的输出事件不缓存，程序结束时关闭监听和已有连接。

`feather-debugger` 连接后首先发送 `Debugger.enable`。等待模式下用户可设置断点，再用 `run` 发送 `Runtime.runIfWaitingForDebugger`；解释器随后执行顶层初始化和可选的 `main()`。非等待模式的 `run` 无需使用。CLI 把两次可能的 VM 外层调用合并成一个用户可见程序，只发送一次 `Debugger.executionFinished`。

等待模式下，程序结束后解释器发送一次结束通知，并保留连接直到控制台调试器退出；非等待模式发送通知后立即关闭监听和连接。任意一侧断开都会关闭 socket；解释器调用 `DebugTarget::Close()`，从而释放启动等待或已经暂停的 VM。控制台调试器退出时尝试发送 `Debugger.disable`，随后关闭连接。

### 控制台命令

- `run`：解除启动等待。
- `break <line> [module]`、`delete <breakpoint-id>`：管理断点；省略 module 表示入口模块。
- `breakif <line> <expression>`：在入口模块设置条件断点。
- `errors on`、`errors off`：开启或关闭 Error 结果断点；默认关闭。
- `continue`、`pause`、`step`、`next`、`out`：执行控制。
- `stack`：调用栈。
- `locals <frame-id>`、`values <frame-id>`、`globals <frame-id>`：三个变量 scope。
- `properties <object-id>`：展开对象。
- `eval <frame-id> <expression>`：在暂停帧求值。
- `set <frame-id> <locals|stack|globals> <name> <expression>`：修改 scope 变量。
- `setprop <frame-id> <object-id> <name> <expression>`：修改可枚举对象属性。
- `raw <json>`：直接发送一条 Feather 调试协议消息，供实验协议时使用。
- `help`、`quit`：帮助和退出。

frame ID 和 object ID 只在当前暂停中有效。没有 `.fbs` 的字节码仍能执行和使用指令级控制，但源码位置、具名函数及局部变量可能不可用。

### 与 DAP 的关系

`feather-debugger` 直接消费 Feather 调试协议，不使用 `DapAdapter`，也不涉及 IDE。VS Code 扩展在 extension host 内完成同等 DAP 转换，并复用同一条到解释器的 TCP 连接；独立 DAP 中继进程不是本版本架构的一部分。

## DAP 转换

### 边界

`DapAdapter` 是 DAP 与 [Feather 调试协议](#feather-调试协议)之间的状态转换器。它不拥有 VM、`DebugTarget`、子进程、Socket 或字节流 framing。宿主实现 `DapChannel`，分别把完整的 UTF-8 JSON 对象送往 IDE 和 Feather target；DAP 的 `Content-Length` framing 仍属于宿主传输层。

adapter 与 `DebugTarget` 可以装在同一进程，也可以位于 IDE 插件或独立调试进程。`DapAdapterOptions::PrimarySourcePath` 对应 Feather 的空 `moduleId`；CLI 的 `./`、`../` 模块 ID 相对主源码目录映射为 DAP `source.path`，绝对路径直接定位源文件，其他宿主自定义 ID 原样转发。更复杂的路径规范化和远端路径映射仍由宿主负责。

仓库的 `editors/vscode-feather` 是 IDE 侧具体接入：它使用 JavaScript 在 VS Code extension host 内实现本规范的同等状态转换，并直接持有到 Feather target 的单条 TCP 连接，不启动独立 DAP 中继。C++ `DapAdapter` 仍供 C++ 宿主嵌入；两者以本规范和协议测试对齐，不要求跨语言链接同一实现。

`DispatchDapMessage` 与 `DispatchTargetMessage` 可由不同线程调用。adapter 不在持有内部锁时调用 channel，因此同进程桥接可以同步回送响应；channel 回调必须线程安全且不得抛异常。每条输入默认限制为 1 MiB，JSON 最大嵌套深度为 64。

### v1 支持范围

- 会话：`initialize`、`launch`、`attach`、`configurationDone`、`disconnect`。
- 执行：`threads`（单一 Feather VM 线程）、`continue`、`next`、`stepIn`、`stepOut`、`pause`。
- 断点：`setBreakpoints`，包括 `condition`。同一 source 再次设置时先替换旧断点；最初返回 `verified:false`，target 解析位置后发送 DAP `breakpoint/changed`。条件失败导致的停顿以普通 breakpoint stop 呈现，并把错误放入 `description`。
- Error 断点：`initialize` 声明一个默认关闭的 `error` exception filter；`setExceptionBreakpoints` 是否包含该 filter 会转换为 `Debugger.setPauseOnErrors`。target 的 Error 暂停映射为 DAP `stopped` 的 `reason:"exception"`，Error 摘要放入 `text`。
- 检查：`stackTrace`、`scopes`、`variables`、`evaluate`、`setVariable`。三个 scope 为 Locals、Stack、Globals；对象属性继续通过新的 `variablesReference` 展开。`evaluate` 支持 watch、hover 和 Debug Console context，`setVariable` 对 scope 和可枚举对象属性使用相同的 Feather 表达式 RHS。
- 事件：`initialized`、`stopped`、`continued`、`breakpoint`、`terminated` 和协议故障的 `output/stderr`。

frame ID 直接使用 Feather 当前暂停的 frame ID。`variablesReference` 由 adapter 分配，只在当前暂停内有效；收到 resumed、下一次 paused 或 executionFinished 后全部失效。所有不支持的 DAP 请求以失败 response 返回。

### 暂不支持

v1 不实现日志点、命中次数、`exceptionInfo`、内存访问、反向执行、多线程、多 target、restart、terminate、source 内容请求及路径映射。具体 IDE/进程如何完成 launch 或 attach 也不由 adapter 决定；这两个请求只启用既有 Feather target。
