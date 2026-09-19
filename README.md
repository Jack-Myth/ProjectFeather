# ProjectFeather

当前版本：**0.3.1**。版本变化见 [CHANGELOG](CHANGELOG.md)。

ProjectFeather 是一个使用 C++20 编写、面向宿主嵌入的小型脚本语言。当前有栈式字节码 VM、单 VM 多文件模块、对象与 MetaObject、自动非移动标记清除 GC、源码编译器、状态快照、可选的嵌入式调试协议端、VS Code 语言与调试扩展，以及源码运行、独立编译和字节码执行命令行程序。公开接口和磁盘字节格式仍处实验阶段。变量用 `var` 声明，函数用 `def` 声明，顶层导出用 `export` 修饰声明。

当前快照使用带 CRC32 校验和的 v4 格式，可保存模块身份、私有全局、跨模块调用帧及按模块 GUID + 类型名注册的 NativeObject，并检测意外损坏；旧版快照不再加载。快照边界见 [快照契约](SPEC-snapshot.md)。

源码支持行首快捷调用，例如 `>你好，我是XXX` 等价于调用全局函数 `__QuickOperatorRightAngleBucket("你好，我是XXX");`。另外三个前缀 `#`、`$`、`&` 分别调用 `__QuickOperatorHash`、`__QuickOperatorDollar`、`__QuickOperatorAmpersand`，不访问 `Env`。详情见 [源码语法](SPEC-syntax.md)。

## 构建

开发构建。在 Windows 上请从 Visual Studio 的 **x64 Native Tools Command Prompt** 运行：

```text
meson setup build/debug
meson compile -C build/debug
meson test -C build/debug --print-errorlogs
```

优化的 Release 构建使用三级优化、LTO 与 `NDEBUG`，并在构建后自动运行测试：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-release.ps1
```

产物位于 `build/release/`。默认保留 `feather debug`、控制台调试器和 VS Code 调试所需实现；若只发布不含调试实现的解释器与运行器，可加 `-WithoutDebugger`，产物进入 `build/release-nodebug/`。等价的手工配置命令为：

```text
meson setup build/release --buildtype=release -Doptimization=3 -Db_lto=true -Db_ndebug=true -Ddebugger=true
meson compile -C build/release
meson test -C build/release --print-errorlogs
```

所有生成产物统一位于 `build/` 下，并按配置分为 `debug/`、`nodebug/`、`release/` 和 `release-nodebug/`。Meson 生成一个位置无关的静态 `feather-runtime`，不再依赖项目自己的 core/debug DLL；Native 模块位于对应配置的 `modules/`，Windows 文件名包括 `stdio.felib.dll`、`snapshot.felib.dll`、`time.felib.dll` 和 `render2d.felib.dll`。`feather` 完整链接 runtime，默认包含传输无关的 `DebugTarget` 和 Feather-to-DAP adapter；`feathervm` 只按需链接字节码执行所需对象，不包含源码编译器、调试协议或 Socket transport。若要构建连 `feather` 也不含 JSON 调试实现的版本，使用 `meson setup build/nodebug -Ddebugger=false`；VM 的低层安全点接口仍留在 core 中，debug/DAP 头文件对应实现则不可链接。

`render2d.felib` 提供 SDL3 + bgfx 二维渲染后端。它默认构建，需要 Git、CMake 和平台图形 SDK，Meson 会按 wrap 中固定的版本取得依赖；只构建轻量 runtime 时可显式传入 `-Drender2d=disabled`：

```text
meson setup build/debug -Drender2d=enabled
meson compile -C build/debug
```

脚本显式 `import("render2d")` 后可创建窗口，使用 `BeginRender`、`DrawTexture`、`DrawText`、`EndRender` 逐帧绘制。首版不公开 SDL/bgfx handle，也不提供自定义 Shader 或通用 Quad；资源快照只保存路径并在恢复后按需重载。完整接口与快照边界见 [Render2D 模块规范](SPEC-render2d.md)。

`time.felib` 提供单调时间、Unix 时间、同步 Sleep、deadline 等待和可快照的帧 Clock。Clock 恢复时重建单调时间基准，不把离线时间计入下一次 Tick；接口见 [Time 模块规范](SPEC-time.md)。

宿主实现 `DebugChannel`，把每条完整 JSON 消息送入 `DebugTarget::DispatchProtocolMessage()`，即可取得源码及条件断点、可选的 Error 结果断点、暂停、单步、调用栈、变量和对象属性、暂停帧表达式求值与变量修改。仓库同时提供最小的 `feather-debugger` 控制台前端和 [VS Code 调试扩展](editors/vscode-feather/README.md)。二者都与解释器建立唯一一条双向 TCP 连接，脚本 stdin/stdout 不承载调试数据；VS Code 扩展在 extension host 内完成 DAP 转换，不启动中继进程。嵌入式 IDE 宿主也可实现 `DapChannel`，把 DAP 与 target 两侧的完整消息交给 C++ `DapAdapter`。DAP 的 `setExceptionBreakpoints` 中 `error` filter 会控制 Error 停顿。协议见 [Feather 调试协议](SPEC-debug-protocol.md)、[控制台调试器](SPEC-debugger-cli.md) 和 [DAP adapter](SPEC-dap-adapter.md)。

直接运行 UTF-8 源文件：

```text
build\debug\feather.exe run hello.fe
build\debug\featherc.exe hello.fe -o hello.fbc
build\debug\feather.exe run hello.fbc
build\debug\feathervm.exe hello.fbc
build\debug\featherc.exe hello.fe -o hello.fbc --symbols hello.fbs
```

`feather run hello.fe` 会检查同目录的 `hello.fbc`；若字节码修改时间不早于源码且能通过完整验证，就直接执行该缓存，否则重新编译源码。源码模块导入采用相同规则。该时间戳只用于本地开发加速，确定性部署应显式运行 `.fbc`。`feather debug` 始终重新编译源码，以保留完整调试信息。

控制台调试分两个终端启动。解释器会在连接建立后继续等待 `run`，因此可先设置断点：

```text
build\debug\feather.exe debug --listen 127.0.0.1:4711 --wait-debugger hello.fe
build\debug\feather-debugger.exe --connect 127.0.0.1:4711
```

`feathervm` 是只接受单个 `.fbc` 参数的精简生产运行器，不提供调试选项。源码调试使用 `feather debug`；`--listen` 单独使用时只开启异步监听，程序立即执行，可供调试器附加到仍在运行的程序，只有再给出 `--wait-debugger` 才等待调试器连接和 `run` 命令。当前 TCP 没有鉴权和加密，只应监听本机回环地址。`feather-debugger` 中输入 `help` 可查看命令；`errors on` 开启 Error 结果断点，默认关闭。

VS Code 扩展 0.3.1 为 `.fe` 提供语法高亮、编辑配置、片段和当前文件基础补全，也包含调试支持。可直接安装 `editors/vscode-feather/feather-debug-0.3.1.vsix`，或把扩展目录作为开发扩展运行。`launch` 会自动选择本机端口、启动 `feather debug --wait-debugger`，并在断点配置完成后放行；`attach` 可连接已经监听的解释器。配置示例和当前限制见扩展自己的 [README](editors/vscode-feather/README.md)。

`featherc` 只生成文件，不执行源码；`feather` 和 `feathervm` 先执行顶层语句，然后调用存在时的无参数 `def main()`。不要求定义 `main`，返回的非 Error 值不打印。用法错误的退出码为 2，编译、文件读取、VM 故障以及初始化或 `main` 返回 Error 的退出码为 1。独立运行程序没有注入快捷行所需的四个全局函数，因此它们会明确拒绝含快捷行的程序。一般表达式语句丢弃 Error 的语言语义仍然适用。`.fbc` 文件格式见 [字节码格式](SPEC-bytecode-format.md)。

独立程序的错误会标出输入文件；直接编译源码时，初始化或 `main` 返回 Error 还会显示可用的源码字节偏移与行列。当前 v3 `.fbc` 不保存调试表；可选 v3 `.fbs` 保存函数显示名、参数名、局部变量名、词法生命周期和源码位置是否允许设置断点，供嵌入宿主使用。普通 CLI 字节码执行不加载符号。VM 故障也会在有位置时标出当前指令。`featherc` 写入失败会标出输出文件。符号格式见 [符号文件格式](SPEC-symbol-format.md)。`feather` 和 `feathervm` 每次最外层执行最多运行 10,000,000 条指令，并限制 100,000 个同时存活的 ScriptObject（含 RootMetaObject）；VM 在安全的指令边界自动回收不可达对象，并在达到硬上限前强制尝试一次完整收集。超出指令预算或收集后仍达到对象上限时以 VM 故障退出。嵌入式宿主可在 `Vm` 构造时自行设置这两项限额，默认不限制指令数；仍可在 VM 空闲时显式调用 `CollectGarbage()`。

`import("name")` 是宿主注册的普通全局函数。编译器和 VM 不加载依赖，也不跨 VM 传递脚本对象；宿主可用 `RegisterModuleImport` 把其他 Feather 文件装入同一 VM，也可用旧 `RegisterImport` 和 `NativeObject` 代理子 VM。接口与代理边界见 [import 契约](SPEC-import.md)。独立运行程序的宿主解析器支持 `import("stdio")`、模块目录中的裸名 Feather 文件，以及 `import("./math")`、`import("../shared/util.fe")` 这样的相对文件标识。裸名从可执行文件旁的 `modules/` 搜索，Native 动态库优先；相对标识由真正的调用方文件目录解析。源码模式读取 `.fe` 并优先使用较新的同名 `.fbc`，显式字节码模式只读取 `.fbc`；先用 `featherc` 分别编译各文件，产物按相同目录关系放置。含 `.fe` 后缀的相对标识在字节码运行时映射到 `.fbc`。Native 库约定见 [Native 模块规范](SPEC-native-modules.md)。

一台 VM 已可装入多个 Feather 文件模块，并为每个文件维护私有全局表。源码 `export var/def` 声明可见名字；模块对象实时读取这些导出。独立 CLI 从单个入口文件开始，按运行时遇到的相对 `import` 装入依赖；路径、读取和缓存由 CLI 宿主层处理。接口、初始化循环与快照边界见 [多模块规范](SPEC-multimodule.md)，验收见 [实施记录](ROADMAP-multimodule.md)。

嵌入式宿主可这样把两个已取得的源码文件装进一台 VM；`import` 回调里实际加载哪个文件由宿主决定：

```cpp
auto Main = Feather::Compile("var math = import(\"math\"); def main() { return math.sum(7); }");
auto Math = Feather::Compile("export var base = 6; export def sum(x) { return base + x; }");
Feather::Vm Machine(Main.Program);
Math.LoadInto(Machine, "math");
Feather::RegisterModuleImport(Machine, {}, [&](std::string_view, std::string_view Name) {
    if (Name != "math") return Feather::Value::FromObject(
        std::make_shared<Feather::ErrorObject>("unknown module"));
    Math.InitializeModule(Machine, "math");
    return Machine.GetModuleNamespace("math");
});
Main.Initialize(Machine);
auto Result = Machine.Run(Main.Functions.at("main")); // 13
```

独立运行程序可通过 `import("stdio")` 获取单一标准输入输出对象，函数内调用 `stdio.Console.PrintLine("hello")` 或 `stdio.IO.OpenFile(...)`。`IO` 还提供 `CloseFile`、`Seek`、`Tell`、`Read`、`Write`；`Read` 返回可逐字节访问的 `ByteBuffer`。嵌入式宿主可编译 `stdlib/StdIo.cpp`，用 `StdIoLibrary(Vm&, input, output)` 将可快照模块接入自己的 `import`；不传 VM 的重载只用于非快照场景。接口和边界见 [标准输入输出库](SPEC-stdio.md)。

保存点可由脚本直接控制：`var snapshot = import("snapshot");` 后调用 `snapshot.Checkpoint(path)`；正常保存返回 `false`，通过 `snapshot.Restore(path)` 回到该位置时返回 `true`。路径只是底层文件能力，slot、标题、缩略图和索引由脚本台本框架自行组织。CLI 会在存档中记录已导入模块并在新 VM 上重建；完整失败边界与 `FTHA` 容器见 [快照契约](SPEC-snapshot.md)。

嵌入时在初始化前注册解析器。宿主可以提供标准库，也可以继续解析自己的模块标识：

```cpp
#include <Feather/Compiler.hpp>
#include <Feather/StdIo.hpp>
#include <Feather/Import.hpp>
#include <iostream>

auto Compiled = Feather::Compile(
    "var stdio = import(\"stdio\"); "
    "def main() { stdio.Console.PrintLine(\"hello\"); }");
Feather::Vm Machine(Compiled.Program);
Feather::StdIoLibrary Library(Machine, std::cin, std::cout);
Feather::RegisterImport(Machine, [&Library](std::string_view Name) {
    if (Name == "stdio") return Library.GetModule();
    return Feather::Value::FromObject(
        std::make_shared<Feather::ErrorObject>("unknown module"));
});
Compiled.Initialize(Machine);
Machine.Run(Compiled.Functions.at("main"));
```

宿主可以从源码创建 VM 并调用脚本函数：

```cpp
#include <Feather/Compiler.hpp>

auto Compiled = Feather::Compile("def answer() { return hostDouble(21); }");
Feather::Vm Machine(Compiled.Program);
Machine.RegisterNativeFunction("hostDouble", std::make_shared<MyDoubleFunction>());
Machine.RegisterNativeFunction("__QuickOperatorHash", std::make_shared<MyHashFunction>());
Machine.SetGlobal("hostObject", Feather::Value::FromObject(std::make_shared<MyHostObject>()));
Compiled.Initialize(Machine);
auto Result = Machine.Run(Compiled.Functions.at("answer"));
```

`MyDoubleFunction`、`MyHashFunction` 和 `MyHostObject` 是宿主实现的 `NativeObject` 子类。`RegisterNativeFunction` 把可调用对象绑定为全局函数；`SetGlobal` 可注入一般宿主对象。`Compiled.Initialize(Machine)` 会绑定顶层函数并执行顶层语句一次，顶层代码因此能立即调用已注册的函数。宿主跨 GC 持有脚本对象时需使用 `RootHandle`。VM 的低层接口用 `CaptureSnapshot` 生成字节、用新 VM 的 `ResumeSnapshot` 恢复；独立解释器可 `import("snapshot")`，调用 `snapshot.Checkpoint(path)` 和 `snapshot.Restore(path)`。CLI 存档同时记录已导入模块并在恢复前重建它们，而 slot、metadata 与索引仍由脚本框架实现。具体限制见 [源码语法](SPEC-syntax.md)、[运行时设计](SPEC-runtime-design.md) 和 [快照契约](SPEC-snapshot.md)。工程状态见 [Progress.md](Progress.md)；完整注入样例见 [快捷行测试](tests/Quick.cpp)。

调试端的最小接入形态如下；`MyChannel` 只负责消息传输，不需要了解 VM 帧或对象布局：

```cpp
#include <Feather/Debug.hpp>

auto Channel = std::make_shared<MyChannel>();
auto Target = std::make_shared<Feather::DebugTarget>(Channel);
Machine.SetDebugController(Target);

// 收到一条完整的协议消息时：
Target->DispatchProtocolMessage(
    R"({"id":1,"method":"Debugger.enable"})");
```

`DebugChannel::SendProtocolMessage` 可能由传输分发线程或 VM 线程调用，宿主实现必须线程安全且不得抛异常。传输线程不会直接读取 VM；暂停后的检查命令由 VM 线程处理。
传输断开时应调用 `Target->Close()` 解除可能存在的暂停；VM 空闲后再调用 `Machine.SetDebugController({})`。

DAP adapter 的最小装配同样只需要一个宿主 channel：

```cpp
#include <Feather/Dap.hpp>

auto Adapter = std::make_shared<Feather::DapAdapter>(
    DapChannel, Feather::DapAdapterOptions{.PrimarySourcePath = "main.fe"});
// IDE 收到完整 DAP JSON：Adapter->DispatchDapMessage(message);
// target 收到完整 Feather JSON：Adapter->DispatchTargetMessage(message);
```
