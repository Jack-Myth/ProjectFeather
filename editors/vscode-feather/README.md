# Feather Debug for VS Code

当前版本：**0.3.1**。

这是 Project Feather 的实验版 VS Code 语言与调试扩展。它为 `.fe` 文件提供语法高亮、括号与注释编辑配置，以及关键字、常用语句片段和当前文件声明的基础补全。扩展也在 VS Code extension host 内完成 DAP 与 Feather 调试协议的转换，并通过一条 `Content-Length` framing 的全双工 TCP 连接直接访问解释器；不需要额外的 DAP 中继进程。

## 本地安装

仓库中已生成可安装的 `feather-debug-0.3.1.vsix`，可在 VS Code 中执行 **Extensions: Install from VSIX...**，或运行：

```powershell
code --install-extension editors\vscode-feather\feather-debug-0.3.1.vsix
```

开发时也可以用 VS Code 打开本目录后按 F5 启动 Extension Development Host。

## Launch

先在仓库根目录构建默认的 debug-enabled 解释器，然后创建 `.vscode/launch.json`：

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "type": "feather",
      "request": "launch",
      "name": "Debug Feather program",
      "program": "${workspaceFolder}/hello.fe",
      "runtimeExecutable": "${workspaceFolder}/build/debug/feather.exe",
      "console": "integratedTerminal"
    }
  ]
}
```

这里的 `"version": "0.2.0"` 是 VS Code `launch.json` 的格式版本，不是扩展版本，因此保持不变。

扩展会选择空闲的本地端口，并启动：

```text
feather debug --listen <host:port> --wait-debugger <program>
```

断点与 Error 断点配置完成后才发送运行指令。默认的 `integratedTerminal` 会在 VS Code
集成终端中运行解释器，stdout、stderr 和 stdin 都连接到该终端，因此 `Console.InputLine()`
可以正常读取输入。若显式设置 `"console": "internalConsole"`，输出会进入 Debug Console，
但该模式不提供交互式 stdin。

## Attach

先自行启动等待调试器的目标：

```powershell
build\debug\feather.exe debug --listen 127.0.0.1:4711 --wait-debugger hello.fe
```

再使用：

```json
{
  "type": "feather",
  "request": "attach",
  "name": "Attach to Feather target",
  "program": "${workspaceFolder}/hello.fe",
  "host": "127.0.0.1",
  "port": 4711
}
```

`program` 用来把 VS Code 中的入口源码映射到 Feather 根模块（空 `moduleId`），建议总是填写。

## 当前范围

语言功能支持 Feather 关键字与字面量高亮、行注释、括号匹配和自动缩进，以及关键字、`def`/`if`/`while`/`var`/`import` 片段和当前文件中函数、参数、变量的补全。当前补全是单文件的轻量实现，不解析导入模块、对象成员或类型。

调试功能支持源码断点与条件断点、Error 结果断点、继续、暂停、单步进入/跨过/跳出、调用栈、locals/stack/globals、对象属性、Watch/悬停/Debug Console 表达式求值、scope 变量和已有对象属性修改，以及 launch 模式下通过集成终端进行交互式输入。暂不支持日志点、命中次数、多线程、热重载和远端路径映射。
