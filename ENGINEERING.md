# 工程结构与开工顺序

状态：M1 至 M5、单 VM 多文件模块、Native 动态库、嵌入式调试协议端、TCP 控制台调试器、传输无关 DAP adapter 和 VS Code 扩展首版可运行，源码与独立字节码命令行程序已接入；`stdio.felib`、`snapshot.felib`、`time.felib` 已作为独立 Native 模块运行，`render2d.felib` 已能绘制图片和文字。公开 API 和字节格式仍处实验阶段。运行时语义以 `SPEC-runtime-design.md` 为准，源码语法以 `SPEC-syntax.md` 为准，多模块以 `SPEC-multimodule.md` 为准，快照行为以 `SPEC-snapshot.md` 为准，磁盘编译产物以 `SPEC-bytecode-format.md` 为准，导入以 `SPEC-import.md`、`SPEC-native-modules.md` 为准，标准输入输出以 `SPEC-stdio.md` 为准，时间以 `SPEC-time.md` 为准，二维渲染以 `SPEC-render2d.md` 为准，调试嵌入边界以 `SPEC-debug-protocol.md`、`SPEC-debugger-cli.md` 和 `SPEC-dap-adapter.md` 为准。

## 目录

```text
ProjectFeather/
├─ meson.build                    # C++20；静态 runtime、命令行入口与测试目标
├─ meson_options.txt              # 可选 debugger 与 render2d 构建开关
├─ scripts/build-release.ps1      # 固定优化选项并验证 Release 构建
├─ CHANGELOG.md                   # 可交付版本节点与兼容性边界
├─ SPEC-runtime-design.md        # 唯一的运行时语义规范
├─ SPEC-syntax.md                # 独立的源码词法、文法与名称解析规范
├─ SPEC-snapshot.md              # M5 保存/恢复及格式契约
├─ SPEC-bytecode-format.md       # 实验版磁盘编译产物格式
├─ SPEC-symbol-format.md         # 独立可选的源码位置符号格式
├─ SPEC-multimodule.md           # 单 VM 多文件模块契约
├─ SPEC-import.md                # 宿主导入与跨 VM 代理边界
├─ SPEC-native-modules.md        # Native 搜索目录、入口与对象生命周期
├─ SPEC-stdio.md                 # 可选标准输入输出库契约
├─ SPEC-time.md                  # 单调时钟、等待、帧 Clock 与快照契约
├─ SPEC-render2d.md              # 可选图片/文字二维渲染与快照契约
├─ SPEC-debug-protocol.md        # 调试目标、宿主传输和 JSON 消息契约
├─ SPEC-debugger-cli.md          # 单 TCP 控制台调试宿主与 framing
├─ SPEC-dap-adapter.md           # Feather 协议到 DAP 的转换边界
├─ ROADMAP-multimodule.md        # 单 VM 多文件模块实施记录与验收
├─ ROADMAP-runtime-diagnostics.md # 后续运行时源码位置诊断目标
├─ Progress.md                   # 里程碑与待决项
├─ ENGINEERING.md                # 模块边界与构建结构
├─ include/Feather/             # 宿主可见的 C++ API；不暴露 VM 内部结构
├─ src/
│  ├─ Value/                     # Tagged Union、不可变 string、基础运算
│  ├─ Bytecode/                  # opcode、模块、构建器、校验器、字节码与符号格式
│  ├─ Vm/                        # 值栈、调用帧、解释循环、全局变量
│  ├─ Object/                    # ScriptObject、MetaObject、NativeObject
│  ├─ Compiler/                  # 词法、语法、AST、代码生成
│  ├─ Embed/                     # 宿主便捷适配接口
│  ├─ Cli/                       # 源码运行、独立编译和字节码执行入口
│  ├─ Snapshot/                  # 版本化序列化与恢复
│  ├─ Protocol/                  # debug/DAP 共用的内部 JSON 实现
│  └─ Debug/                     # 可选、可剥离的调试目标和 DAP adapter
├─ stdlib/                       # 独立于核心库的 Console/IO 实现
├─ modules/                      # Native 模块构建目标；输出到 build/<配置>/modules/
├─ editors/vscode-feather/       # 进程内 DAP 转换与单 TCP VS Code 调试扩展
└─ tests/
   ├─ M1.cpp                    # 手写模块与 VM
   ├─ M2.cpp                    # 对象协议
   ├─ M3.cpp                    # GC 根与宿主持有
   ├─ Embed.cpp                 # 宿主嵌入 API
   ├─ M4.cpp                    # 源码到字节码
   ├─ M5.cpp                    # 保存、恢复与损坏输入
   ├─ Quick.cpp                 # 行首快捷调用
   ├─ Artifact.cpp              # 磁盘编译产物与损坏输入
   ├─ Import.cpp                # 宿主导入及子 VM 代理样例
   ├─ StdIo.cpp                 # 控制台和二进制文件端到端测试
   ├─ Budget.cpp                # 指令预算及 native 重入测试
   ├─ Diagnostics.cpp           # Error 来源和独立符号文件
   ├─ Debug.cpp                 # 调试协议、跨线程暂停和检查
   ├─ Dap.cpp                   # DAP 映射、引用生命周期和事件
   ├─ Socket.cpp                # TCP framing 与全双工传输
   ├─ Multimodule.cpp           # 模块隔离、导出、导入和跨模块快照
   └─ fixtures/                 # 命令行集成样例
```

目录在首次添加相应代码时创建；不放空占位文件。使用 Meson 构建位置无关的静态 runtime、四个命令行程序（含 `feather-debugger`）、Native 动态库和测试可执行程序。`feather` 完整链接编译器与可选调试实现；`feathervm` 通过普通静态链接只拉入字节码执行路径，不链接源码编译器、调试协议或 Socket transport。Native C++ 接口要求同工具链/运行时，不承诺跨工具链稳定 ABI；磁盘字节码格式为实验版。所有生成目录收在 `build/` 下，当前配置名为 `debug`、`nodebug`、`release` 和 `release-nodebug`，均不纳入源码。

在 Windows 上，从 Visual Studio 的 **x64 Native Tools Command Prompt** 进入本目录，运行 `meson setup build/debug`，随后运行 `meson compile -C build/debug` 和 `meson test -C build/debug --print-errorlogs`。

Release 流程统一由 `scripts/build-release.ps1` 驱动，显式设置 `buildtype=release`、`optimization=3`、`b_lto=true` 和 `b_ndebug=true`，并把完整测试作为成功条件。默认仍构建调试协议与前端，以保持 0.3.1 的完整功能面；传入 `-WithoutDebugger` 才设置 `-Ddebugger=false`，用于只需要运行能力的分发。不要把 Release 与“无调试功能”混为一谈：前者是编译优化配置，后者是独立的功能裁剪开关。

## 依赖方向

`Value` 不依赖 VM；`Bytecode` 可依赖 Value 类型定义；`Vm` 依赖 Value 与 Bytecode。`Object`、`Memory` 与 `Embed` 通过窄接口接入 VM，避免 Value 反向依赖解释器。`Compiler` 只生成 Bytecode 模块，不直接调用解释循环。`Snapshot` 读取 VM 的显式帧和对象图，不依赖 C++ 调用栈。`include/Feather/Runtime.hpp` 暂时公开手写字节码所需构建器与原型，是实验性接口；稳定嵌入接口完成时再收窄公开头文件。ScriptObject 现由 VM 非移动堆持有，宿主跨 GC 使用它须持 RootHandle。

`stdio.felib` 与 `snapshot.felib` 静态链接 runtime 的公开嵌入实现，核心不反向依赖控制台、文件或 CLI 会话。`feather` 和 `feathervm` 运行时从旁边的 `modules/` 装载它们；`featherc` 不装载。`StdIoLibrary::GetModule()` 允许嵌入式宿主自行装配；`SnapshotLibrary` 则要求宿主实现显式的 `SnapshotHost` 能力。库均不注入全局名。Native 库入口见 `include/Feather/NativeModule.hpp`，动态库加载与缓存位于 `src/Cli/NativeModules.cpp`，CLI 的存档容器和会话切换位于 `src/Cli/SnapshotHost.cpp` 与 `Runner.cpp`。

`render2d.felib` 是完整收在 `modules/render2d/` 下的独立 NativeModule，只通过显式导入取得，不向 core 或已安装的 Feather C++ 头文件加入渲染 API。目录内的 `Render2D.cpp` 实现脚本对象和快照协议，`Render2DBgfx.cpp` 实现 SDL3/bgfx backend，`Render2DModule.cpp` 只提供 Native 入口；测试通过目录内的窄接口注入 mock。它随标准配置构建，并通过 `subprojects/*.wrap` 获取固定版本依赖；轻量构建可显式使用 `-Drender2d=disabled`。RenderContext/Texture/Font 的可快照身份属于模块对象层，SDL 窗口、GPU 资源和帧内容始终属于 backend，不写入快照。

`time.felib` 完整位于 `modules/time/`，提供系统单调时钟、Unix 时间、同步等待和可快照 Clock。Clock 的序列化 payload 为空，恢复完成时重新建立单调时间基准，不持久化无意义的操作系统时间点。

默认静态 runtime 在 core 之后加入 `DebugTarget`、共用 JSON 和 `DapAdapter`；`feather` 可链接这些实现，`feathervm` 始终不链接。`-Ddebugger=false` 会从 runtime 源码中完全移除后三者以及 `feather-debugger`。core 只公开 `VmDebugController` 和回调期内有效的 `VmDebugContext`，不反向调用调试实现。宿主分别实现 `DebugChannel` 与 `DapChannel` 传输完整消息。可复用库仍不依赖 Socket；TCP、字节流 framing 和控制台交互位于 `src/Cli` 的薄宿主中。`feather` 的 `--debug-listen` 采用异步附加生命周期，`--wait-debugger` 才启用连接与启动门控。

`editors/vscode-feather` 使用 VS Code 的 inline debug adapter API，在 extension host 内将 DAP 映射到 Feather 协议，并直接持有到解释器的唯一 TCP 连接。它不链接 C++ runtime，也不启动独立 DAP 中继；`launch` 只额外创建用户要调试的 `feather` 进程，`attach` 则连接已有 target。扩展还以 TextMate grammar、语言配置和轻量 completion provider 提供基础语言支持；当前只分析单个文档，不承担完整语义分析。JavaScript 映射应与 `SPEC-dap-adapter.md` 及 C++ `DapAdapter` 的可观察行为保持一致。

## 里程碑

1. **M1 手写字节码 VM**：Value、模块构建器/校验器、帧、全局表、基础指令；用手写模块验证循环、嵌套调用、默认参数、缺省返回和非法字节码。
2. **M2 对象协议**：ScriptObject、RootMetaObject、Function/NativeObject、成员指令；验证对象身份、键哈希、`__index`/`__call`。
3. **M3 所有权与嵌入**：GC、RootHandle、NativeObject 登记、宿主调用与回调；验证循环引用、临时根和 OOM 故障路径。
4. **M4 编译器**：先完成独立语法规范，再实现 lexer、parser、AST 与字节码生成；与 M1/M2 手写模块的行为对齐。
5. **M5 快照**：先冻结保存/恢复语义，再实现对象图和帧序列化。

每个里程碑须有能运行的验收样例。M1 至 M5 均已具备对应测试；快照当前以保存/恢复往返、v4 CRC32 意外损坏检测、基本资源边界和 Native 类型失败回滚验收，不再扩展细碎的畸形输入组合。

单 VM 多文件模块已作为独立阶段接入 VM 执行、GC 和 v4 快照；模块身份、显式导出与初始化状态见 `SPEC-multimodule.md`。独立 CLI 仍只执行单个入口文件，文件解析由宿主层决定。
