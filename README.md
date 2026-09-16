# ProjectFeather

ProjectFeather 是一个使用 C++20 编写、面向宿主嵌入的小型脚本语言。当前有栈式字节码 VM、单 VM 多文件模块、对象与 MetaObject、自动非移动标记清除 GC、源码编译器、状态快照，以及源码运行、独立编译和字节码执行命令行程序。公开接口和磁盘字节格式仍处实验阶段。变量用 `var` 声明，函数用 `def` 声明，顶层导出用 `export` 修饰声明。

当前快照使用带 CRC32 校验和的 v3 格式，可保存模块身份、私有全局与跨模块调用帧，并检测意外损坏；旧版快照不再加载。快照边界见 [快照契约](SPEC-snapshot.md)。

源码支持行首快捷调用，例如 `>你好，我是XXX` 等价于调用全局函数 `__QuickOperatorRightAngleBucket("你好，我是XXX");`。另外三个前缀 `#`、`$`、`&` 分别调用 `__QuickOperatorHash`、`__QuickOperatorDollar`、`__QuickOperatorAmpersand`，不访问 `Env`。详情见 [源码语法](SPEC-syntax.md)。

在 Windows 的 Visual Studio x64 Native Tools Command Prompt 中构建和运行测试：

```text
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
```

Meson 同时生成共享核心库和 `build/modules/stdio.felib.dll`（Linux 为 `.so`）。运行 `feather`、`feathervm` 时应保留核心库与 `modules/` 相对可执行文件的目录关系。

直接运行 UTF-8 源文件：

```text
build\feather.exe run hello.fe
build\featherc.exe hello.fe -o hello.fbc
build\feathervm.exe hello.fbc
build\featherc.exe hello.fe -o hello.fbc --symbols hello.fbs
build\feathervm.exe hello.fbc --symbols hello.fbs
```

`featherc` 只生成文件，不执行源码；`feather` 和 `feathervm` 先执行顶层语句，然后调用存在时的无参数 `def main()`。不要求定义 `main`，返回的非 Error 值不打印。用法错误的退出码为 2，编译、文件读取、VM 故障以及初始化或 `main` 返回 Error 的退出码为 1。独立运行程序没有注入快捷行所需的四个全局函数，因此它们会明确拒绝含快捷行的程序。一般表达式语句丢弃 Error 的语言语义仍然适用。`.fbc` 文件格式见 [字节码格式](SPEC-bytecode-format.md)。

独立程序的错误会标出输入文件；`feather run` 的初始化或 `main` 返回 Error 时还会显示可用的源码字节偏移与行列。当前 v3 `.fbc` 不保存位置表；需要同样的诊断时可另存独立 `.fbs`，并在 `feathervm` 运行时显式指定。普通运行只读 `.fbc`，不会自动加载符号。VM 故障也会在有位置时标出当前指令。`featherc` 写入失败会标出输出文件。符号格式见 [符号文件格式](SPEC-symbol-format.md)。`feather` 和 `feathervm` 每次最外层执行最多运行 10,000,000 条指令，并限制 100,000 个同时存活的 ScriptObject（含 RootMetaObject）；VM 在安全的指令边界自动回收不可达对象，并在达到硬上限前强制尝试一次完整收集。超出指令预算或收集后仍达到对象上限时以 VM 故障退出。嵌入式宿主可在 `Vm` 构造时自行设置这两项限额，默认不限制指令数；仍可在 VM 空闲时显式调用 `CollectGarbage()`。

`import("name")` 是宿主注册的普通全局函数。编译器和 VM 不加载依赖，也不跨 VM 传递脚本对象；宿主可用 `RegisterModuleImport` 把其他 Feather 文件装入同一 VM，也可用旧 `RegisterImport` 和 `NativeObject` 代理子 VM。接口与代理边界见 [import 契约](SPEC-import.md)。独立运行程序的宿主解析器支持 `import("stdio")`、模块目录中的裸名 Feather 文件，以及 `import("./math")`、`import("../shared/util.fe")` 这样的相对文件标识。裸名从可执行文件旁的 `modules/` 搜索，Native 动态库优先；相对标识由真正的调用方文件目录解析。`feather run` 读取相应 `.fe`，`feathervm` 读取同名 `.fbc`；先用 `featherc` 分别编译各文件，产物按相同目录关系放置。含 `.fe` 后缀的相对标识在字节码运行时映射到 `.fbc`。Native 库约定见 [Native 模块规范](SPEC-native-modules.md)。

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

独立运行程序可通过 `import("stdio")` 获取单一标准输入输出对象，函数内调用 `stdio.Console.PrintLine("hello")` 或 `stdio.IO.OpenFile(...)`。`IO` 还提供 `CloseFile`、`Seek`、`Tell`、`Read`、`Write`；`Read` 返回可逐字节访问的 `ByteBuffer`。嵌入式宿主可编译 `stdlib/StdIo.cpp` 并将 `StdIoLibrary::GetModule()` 接入自己的 `import`；接口和边界见 [标准输入输出库](SPEC-stdio.md)。

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
Feather::StdIoLibrary Library(std::cin, std::cout);
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

`MyDoubleFunction`、`MyHashFunction` 和 `MyHostObject` 是宿主实现的 `NativeObject` 子类。`RegisterNativeFunction` 把可调用对象绑定为全局函数；`SetGlobal` 可注入一般宿主对象。`Compiled.Initialize(Machine)` 会绑定顶层函数并执行顶层语句一次，顶层代码因此能立即调用已注册的函数。宿主跨 GC 持有脚本对象时需使用 `RootHandle`。快照由脚本调用宿主注册的 native 保存入口，在该入口中调用 `CaptureSnapshot`；恢复到新 VM 时调用 `ResumeSnapshot`。具体限制见 [源码语法](SPEC-syntax.md)、[运行时设计](SPEC-runtime-design.md) 和 [快照契约](SPEC-snapshot.md)。工程状态见 [Progress.md](Progress.md)；完整注入样例见 [快捷行测试](tests/Quick.cpp)。
