# ProjectFeather

ProjectFeather 是用 C++20 实现的轻量嵌入式脚本语言。它提供源码编译器、栈式字节码 VM、单 VM 多文件模块、自动 GC、Native 模块和可选调试器。当前版本为 **0.3.1**，发布记录见 [CHANGELOG](CHANGELOG.md)。公开 C++ 接口与编译产物格式仍处于实验阶段。

## 构建

在 Windows 上，从 Visual Studio 的 **x64 Native Tools Command Prompt** 运行以下命令。构建使用 Meson 和 Ninja，默认启用的 SDL3/bgfx 渲染模块还需要 Git、CMake 和平台图形 SDK。

```text
meson setup build/debug
meson compile -C build/debug
meson test -C build/debug --print-errorlogs
```

配置时可用 `-Drender2d=disabled`、`-Dnetwork=false` 或 `-Ddebugger=false` 裁剪对应功能。

Windows Release 脚本会启用三级优化、LTO 与 `NDEBUG`，并在构建后运行测试。默认使用构建工具的并行度，传入 `-Jobs 1` 可限制构建和测试的并行度。可加 `-WithoutDebugger`，此时产物位于 `build/release-nodebug/`。

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-release.ps1 -Jobs 1
powershell -ExecutionPolicy Bypass -File scripts\collect-dist.ps1
```

收集脚本默认将 `build/release` 的运行程序与 Native 模块复制到 `dist/`，同时写入校验清单。可用 `-BuildDirectory` 和 `-OutputDirectory` 指定其他目录。重新收集时，脚本校验新包后直接替换上次生成的输出。应用脚本、图片、字体及目标机器所需的 MSVC/UCRT 运行库需自行准备。

## 性能基准

在 Release 构建目录运行以下命令。基准单独构建，普通功能测试不计时。

```text
meson test -C build/release --benchmark --verbose runtime
```

每组预热一次，再采样七次。输出为 CSV，单位毫秒，包含中位数和单次最大值。比较两个版本时，在同一机器上使用相同的构建参数和基准源码。普通算术与跨模块算术各执行同一个 50 万次循环，后者从入口脚本调用已加载模块。成员用例执行 20 万次属性读写。GC 用例先创建 32,768 个已过期的 Native 弱引用，再单独计时一次回收。各项均校验执行结果，编译、加载、预热和 GC 样本准备耗时排除在计时之外。

## 运行脚本

把以下内容保存为 UTF-8 的 `hello.fe`。

```feather
var stdio = import("stdio");

def main() {
    stdio.Console.PrintLine("Hello, Feather!");
}
```

```text
build\debug\feather.exe run hello.fe
build\debug\featherc.exe hello.fe -o hello.fbc --symbols hello.fbs
build\debug\feathervm.exe hello.fbc
```

`feather run` 支持源码与字节码，`featherc` 逐文件编译，`feathervm` 是只执行字节码的精简运行器。运行器先执行顶层初始化，再调用可选的无参数 `main()`。返回值不会自动打印。用法错误退出码为 2，读取、编译或运行失败退出码为 1。

源码模式会复用不早于源码、记录的源码字节数匹配且验证通过的同名 `.fbc`。发布时可显式运行 `.fbc`，并按原目录关系携带各依赖模块的编译产物。`featherc` 输出带源码大小的 `.fbc` v6，运行器仅接受 v6；符号格式为 `.fbs` v3。

## 调试

在两个终端分别启动解释器和控制台调试器。连接后可先设断点，再在控制台输入 `run`。

```text
build\debug\feather.exe debug --listen 127.0.0.1:4711 --wait-debugger hello.fe
build\debug\feather-debugger.exe --connect 127.0.0.1:4711
```

也可用 [VS Code 扩展](editors/vscode-feather/README.md) 启动或附加调试。调试连接没有鉴权或加密，开发时使用本机回环地址。

## 嵌入 C++ 应用

公开接口在 `include/Feather/`，Meson 构建静态 `feather-runtime`。宿主用 `Compile(source)` 取得程序，以 `Vm(compiled.Program)` 构造 VM，在初始化前注册宿主函数或导入解析器，然后调用 `compiled.Initialize(vm)` 和 `vm.Run(...)`。宿主跨 GC 持有脚本对象时用 `RootHandle`。端到端示例见 [Embed.cpp](tests/Embed.cpp) 和 [Import.cpp](tests/Import.cpp)。

## 参考

| 文档 | 内容 |
|---|---|
| [源码语法](SPEC-syntax.md) | 文法、快捷行、数组、`@{ ... }` 编译块 |
| [模块与标准库](SPEC-modules.md) | 文件与 Native 导入、VM 模块路径、stdio、time、text、network、render2d |
| [运行时](SPEC-runtime-design.md) | 值、对象、GC、指令、错误与嵌入 API |
| [编译产物](SPEC-artifacts.md) | `.fbc`、`.fbs` 格式和校验 |
| [调试](SPEC-debugging.md) | Feather 调试协议、控制台、TCP 和 DAP |
