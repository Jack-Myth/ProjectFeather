# 工程结构与开工顺序

状态：M1 至 M5、单 VM 多文件模块、Native 动态库、嵌入式调试协议端、TCP 控制台调试器和传输无关 DAP adapter 首版可运行，源码与独立字节码命令行程序已接入；`stdio.felib` 是首个独立 Native 模块。公开 API 和字节格式仍处实验阶段。运行时语义以 `SPEC-runtime-design.md` 为准，源码语法以 `SPEC-syntax.md` 为准，多模块以 `SPEC-multimodule.md` 为准，快照行为以 `SPEC-snapshot.md` 为准，磁盘编译产物以 `SPEC-bytecode-format.md` 为准，导入以 `SPEC-import.md`、`SPEC-native-modules.md` 为准，标准输入输出以 `SPEC-stdio.md` 为准，调试嵌入边界以 `SPEC-debug-protocol.md`、`SPEC-debugger-cli.md` 和 `SPEC-dap-adapter.md` 为准。

## 目录

```text
ProjectFeather/
├─ meson.build                    # C++20；静态 runtime、命令行入口与测试目标
├─ meson_options.txt              # 可选 debugger 打包开关
├─ SPEC-runtime-design.md        # 唯一的运行时语义规范
├─ SPEC-syntax.md                # 独立的源码词法、文法与名称解析规范
├─ SPEC-snapshot.md              # M5 保存/恢复及格式契约
├─ SPEC-bytecode-format.md       # 实验版磁盘编译产物格式
├─ SPEC-symbol-format.md         # 独立可选的源码位置符号格式
├─ SPEC-multimodule.md           # 单 VM 多文件模块契约
├─ SPEC-import.md                # 宿主导入与跨 VM 代理边界
├─ SPEC-native-modules.md        # Native 搜索目录、入口与对象生命周期
├─ SPEC-stdio.md                 # 可选标准输入输出库契约
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
├─ modules/                      # Native 模块构建目标；输出到 build/modules/
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

目录在首次添加相应代码时创建；不放空占位文件。使用 Meson 构建位置无关的静态 runtime、四个自包含命令行程序（含 `feather-debugger`）、`stdio.felib` 动态库和测试可执行程序。Native C++ 接口要求同工具链/运行时，不承诺跨工具链稳定 ABI；磁盘字节码格式为实验版。构建目录放在工作区内的 `build/`，不纳入源码。

在 Windows 上，从 Visual Studio 的 **x64 Native Tools Command Prompt** 进入本目录，运行 `meson setup build`，随后运行 `meson compile -C build` 和 `meson test -C build --print-errorlogs`。

## 依赖方向

`Value` 不依赖 VM；`Bytecode` 可依赖 Value 类型定义；`Vm` 依赖 Value 与 Bytecode。`Object`、`Memory` 与 `Embed` 通过窄接口接入 VM，避免 Value 反向依赖解释器。`Compiler` 只生成 Bytecode 模块，不直接调用解释循环。`Snapshot` 读取 VM 的显式帧和对象图，不依赖 C++ 调用栈。`include/Feather/Runtime.hpp` 暂时公开手写字节码所需构建器与原型，是实验性接口；稳定嵌入接口完成时再收窄公开头文件。ScriptObject 现由 VM 非移动堆持有，宿主跨 GC 使用它须持 RootHandle。

`stdio.felib` 静态链接 runtime 的公开嵌入实现，核心不反向依赖标准输入输出，也不再要求旁置 `feather-core.dll`。`feather` 和 `feathervm` 运行时从旁边的 `modules/` 装载它；`featherc` 不装载。`StdIoLibrary::GetModule()` 也允许嵌入式宿主自行装配；库不注入标准输入输出全局名。Native 库入口见 `include/Feather/NativeModule.hpp`，动态库加载与缓存位于 `src/Cli/NativeModules.cpp`。

默认静态 runtime 在 core 之后加入 `DebugTarget`、共用 JSON 和 `DapAdapter`；`-Ddebugger=false` 会从目标源码中完全移除后三者以及 `feather-debugger`。core 只公开 `VmDebugController` 和回调期内有效的 `VmDebugContext`，不反向调用调试实现。宿主分别实现 `DebugChannel` 与 `DapChannel` 传输完整消息。可复用库仍不依赖 Socket；TCP、字节流 framing 和控制台交互位于 `src/Cli` 的薄宿主中。CLI 的 `--debug-listen` 采用异步附加生命周期，`--wait-debugger` 才启用连接与启动门控。

## 里程碑

1. **M1 手写字节码 VM**：Value、模块构建器/校验器、帧、全局表、基础指令；用手写模块验证循环、嵌套调用、默认参数、缺省返回和非法字节码。
2. **M2 对象协议**：ScriptObject、RootMetaObject、Function/NativeObject、成员指令；验证对象身份、键哈希、`__index`/`__call`。
3. **M3 所有权与嵌入**：GC、RootHandle、NativeObject 登记、宿主调用与回调；验证循环引用、临时根和 OOM 故障路径。
4. **M4 编译器**：先完成独立语法规范，再实现 lexer、parser、AST 与字节码生成；与 M1/M2 手写模块的行为对齐。
5. **M5 快照**：先冻结保存/恢复语义，再实现对象图和帧序列化。

每个里程碑须有能运行的验收样例。M1 至 M5 均已具备对应测试；快照当前以保存/恢复往返、v3 CRC32 意外损坏检测、基本资源边界和宿主 codec 失败回滚验收，不再扩展细碎的畸形输入组合。

单 VM 多文件模块已作为独立阶段接入 VM 执行、GC 和 v3 快照；模块身份、显式导出与初始化状态见 `SPEC-multimodule.md`。独立 CLI 仍只执行单个入口文件，文件解析由宿主层决定。
