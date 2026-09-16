# Feather 控制台调试器与 TCP 宿主（实验版 v1）

## 1. 进程边界

调试运行由两个独立进程组成：

```text
feather-debugger  <====== one full-duplex TCP connection ======>  feather / feathervm
console commands                 Feather JSON protocol           DebugTarget -> VM
```

解释器内的 VM、`VmDebugController` 和 `DebugTarget` 仍通过普通 C++ 调用连接。只有最外层 CLI 宿主负责监听、接受 TCP、分帧和把完整 JSON 交给 `DebugTarget::DispatchProtocolMessage()`。脚本的 stdin/stdout 不承载调试数据；`feather-debugger` 的 stdin/stdout 只用于人机交互。

源码入口使用：

```text
feather debug --listen 127.0.0.1:4711 --wait-debugger main.fe
feather-debugger --connect 127.0.0.1:4711
```

字节码入口使用：

```text
feathervm main.fbc --symbols main.fbs --debug-listen 127.0.0.1:4711 --wait-debugger
feather-debugger --connect 127.0.0.1:4711
```

解释器一次只接受一个调试器连接。当前没有鉴权、加密或远端暴露保护，开发时应监听 `127.0.0.1` 或 `::1`；把监听地址暴露到不可信网络不属于支持范围。

## 2. 字节流 framing

TCP 双向均使用相同 framing：

```text
Content-Length: <UTF-8 JSON 字节数>\r\n
\r\n
<恰好 Content-Length 字节的 JSON>
```

header 最多 8 KiB，每条 JSON 默认最多 1 MiB。`Content-Length` 不得缺失或重复，连接中途截断视为传输故障。这个 framing 只负责消息边界，不改变 `SPEC-debug-protocol.md` 的 JSON 语义。

## 3. 启动与关闭

解释器先创建监听 socket 和 `DebugTarget`，再由接收线程接受至多一个连接。只有命令行带 `--wait-debugger` 时 target 才启用 `WaitForDebugger`：宿主先等待连接，再在 `WaitForExecutionPermission()` 上等待 `run`，不会抢在用户设置断点前执行。不带该参数时监听与 VM 并行启动；未连接期间的输出事件不缓存，程序结束时关闭监听和已有连接。

`feather-debugger` 连接后首先发送 `Debugger.enable`。等待模式下用户可设置断点，再用 `run` 发送 `Runtime.runIfWaitingForDebugger`；解释器随后执行顶层初始化和可选的 `main()`。非等待模式的 `run` 无需使用。CLI 把两次可能的 VM 外层调用合并成一个用户可见程序，只发送一次 `Debugger.executionFinished`。

等待模式下，程序结束后解释器发送一次结束通知，并保留连接直到控制台调试器退出；非等待模式发送通知后立即关闭监听和连接。任意一侧断开都会关闭 socket；解释器调用 `DebugTarget::Close()`，从而释放启动等待或已经暂停的 VM。控制台调试器退出时尝试发送 `Debugger.disable`，随后关闭连接。

## 4. 控制台命令

- `run`：解除启动等待。
- `break <line> [module]`、`delete <breakpoint-id>`：管理断点；省略 module 表示入口模块。
- `errors on`、`errors off`：开启或关闭 Error 结果断点；默认关闭。
- `continue`、`pause`、`step`、`next`、`out`：执行控制。
- `stack`：调用栈。
- `locals <frame-id>`、`values <frame-id>`、`globals <frame-id>`：三个变量 scope。
- `properties <object-id>`：展开对象。
- `raw <json>`：直接发送一条 Feather 调试协议消息，供实验协议时使用。
- `help`、`quit`：帮助和退出。

frame ID 和 object ID 只在当前暂停中有效。没有 `.fbs` 的字节码仍能执行和使用指令级控制，但源码位置、具名函数及局部变量可能不可用。

## 5. 与 DAP 的关系

`feather-debugger` 直接消费 Feather 调试协议，不使用 `DapAdapter`，也不涉及 IDE。未来 IDE 插件可以在插件进程内链接传输无关的 `DapAdapter`，并复用同一条到解释器的 TCP 连接；独立 DAP 中继进程不是本版本架构的一部分。
