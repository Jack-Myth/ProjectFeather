# ProjectFeather

ProjectFeather 是一个使用 C++20 编写、面向宿主嵌入的小型脚本语言。当前有栈式字节码 VM、对象与 MetaObject、显式 GC、源码编译器、第一版状态快照，以及源码运行、独立编译和字节码执行命令行程序。公开接口和磁盘字节格式仍处实验阶段。变量用 `var` 声明，函数用 `def` 声明。

源码支持行首快捷调用，例如 `>你好，我是XXX` 等价于调用全局函数 `__QuickOperatorRightAngleBucket("你好，我是XXX");`。另外三个前缀 `#`、`$`、`&` 分别调用 `__QuickOperatorHash`、`__QuickOperatorDollar`、`__QuickOperatorAmpersand`，不访问 `Env`。详情见 [源码语法](SPEC-syntax.md)。

在 Windows 的 Visual Studio x64 Native Tools Command Prompt 中构建和运行测试：

```text
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
```

直接运行 UTF-8 源文件：

```text
build\feather.exe run hello.fe
build\featherc.exe hello.fe -o hello.fbc
build\feathervm.exe hello.fbc
```

`featherc` 只生成文件，不执行源码；`feather` 和 `feathervm` 先执行顶层语句，然后调用存在时的无参数 `def main()`。不要求定义 `main`，返回的非 Error 值不打印。用法错误的退出码为 2，编译、文件读取、VM 故障以及初始化或 `main` 返回 Error 的退出码为 1。独立运行程序没有注入快捷行所需的四个全局函数，因此它们会明确拒绝含快捷行的程序。一般表达式语句丢弃 Error 的语言语义仍然适用。`.fbc` 文件格式见 [字节码格式](SPEC-bytecode-format.md)。

独立程序的错误会标出输入文件；`featherc` 写入失败会标出输出文件。`feather` 和 `feathervm` 每次最外层执行最多运行 10,000,000 条指令，并限制 100,000 个 ScriptObject（含 RootMetaObject）；超出指令预算时以 VM 故障退出。嵌入式宿主可在 `Vm` 构造时自行设置这两项限额，默认不限制指令数。

`import("name")` 是宿主注册的普通全局函数。编译器和 VM 不加载依赖，也不跨 VM 传递脚本对象；宿主可自行创建子 VM，用 `NativeObject` 代理暴露模块能力。嵌入时在 `Initialize` 前调用 `RegisterImport(Machine, Callback)`；接口和代理边界见 [import 契约](SPEC-import.md)。独立运行程序只内置 `std:console` 和 `std:io` 两个库标识，其他导入仍需宿主解析器。

后续计划让一台 VM 装入多个文件，并为每个文件维护私有全局表；这项尚未实现的目标见 [单 VM 多模块路线图](ROADMAP-multimodule.md)。

独立运行程序可通过 `import("std:console")` 获取控制台对象，通过 `import("std:io")` 获取文件对象。例如在顶层写 `var console = import("std:console");`，函数内即可调用 `console.PrintLine("hello");`。文件对象提供 `OpenFile`、`CloseFile`、`Seek`、`Tell`、`Read`、`Write`；`Read` 返回可逐字节访问的 `ByteBuffer`。嵌入式宿主可链接独立的 `FeatherStdIo` 库并将解析器接入自己的 `import`；接口和边界见 [标准输入输出库](SPEC-stdio.md)。

嵌入时在初始化前注册解析器。宿主可以只提供其中一个库，也可以继续解析自己的模块标识：

```cpp
#include <Feather/Compiler.hpp>
#include <Feather/StdIo.hpp>
#include <Feather/Import.hpp>
#include <iostream>

auto Compiled = Feather::Compile(
    "var console = import(\"std:console\"); "
    "def main() { console.PrintLine(\"hello\"); }");
Feather::Vm Machine(Compiled.Program);
Feather::StdIoLibrary Library(std::cin, std::cout);
Feather::RegisterImport(Machine, [&Library](std::string_view Name) {
    if (Name == "std:console" || Name == "std:io")
        return Library.Resolve(Name);
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

`MyDoubleFunction`、`MyHashFunction` 和 `MyHostObject` 是宿主实现的 `NativeObject` 子类。`RegisterNativeFunction` 把可调用对象绑定为全局函数；`SetGlobal` 可注入一般宿主对象。`Compiled.Initialize(Machine)` 会绑定顶层函数并执行顶层语句一次，顶层代码因此能立即调用已注册的函数。宿主跨 GC 持有脚本对象时需使用 `RootHandle`。快照由脚本调用宿主注册的 native 保存入口，在该入口中调用 `CaptureSnapshot`；恢复到新 VM 时调用 `ResumeSnapshot`。具体限制见 [源码语法](SPEC-syntax.md)、[运行时设计](SPEC-runtime-design.md) 和 [快照契约](SPEC-snapshot.md)。工程状态见 [Progress.md](Progress.md)；完整注入样例见 [快捷行测试](tests/Quick.cpp)。
