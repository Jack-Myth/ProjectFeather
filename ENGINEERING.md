# 工程结构与开工顺序

状态：M1/M2/M3 和 M4 第一版可运行；运行时语义以 `SPEC-runtime-design.md` 为准，源码语法以 `SPEC-syntax.md` 为准。

## 目录

```text
ProjectFeather/
├─ meson.build                    # C++20；核心静态库与测试目标
├─ SPEC-runtime-design.md        # 唯一的运行时语义规范
├─ SPEC-syntax.md                # 独立的源码词法、文法与名称解析规范
├─ Progress.md                   # 里程碑与待决项
├─ ENGINEERING.md                # 模块边界与构建结构
├─ include/Feather/             # 宿主可见的 C++ API；不暴露 VM 内部结构
├─ src/
│  ├─ Value/                     # Tagged Union、不可变 string、基础运算
│  ├─ Bytecode/                  # opcode、模块、构建器、校验器
│  ├─ Vm/                        # 值栈、调用帧、解释循环、全局变量
│  ├─ Object/                    # ScriptObject、MetaObject、NativeObject
│  ├─ Compiler/                  # 词法、语法、AST、代码生成
│  └─ Snapshot/                  # 后续：序列化与恢复
└─ tests/
   ├─ M1.cpp                    # 手写模块与 VM
   ├─ M2.cpp                    # 对象协议
   ├─ M3.cpp                    # GC 根与宿主持有
   ├─ Embed.cpp                 # 宿主嵌入 API
   └─ M4.cpp                    # 源码到字节码
```

目录在首次添加相应代码时创建；不放空占位文件。使用 Meson 构建核心静态库和测试可执行程序。不承诺稳定 ABI，不定义磁盘字节码格式。构建目录放在工作区内的 `build/`，不纳入源码。

在 Windows 上，从 Visual Studio 的 **x64 Native Tools Command Prompt** 进入本目录，运行 `meson setup build`，随后运行 `meson compile -C build` 和 `meson test -C build --print-errorlogs`。

## 依赖方向

`Value` 不依赖 VM；`Bytecode` 可依赖 Value 类型定义；`Vm` 依赖 Value 与 Bytecode。`Object`、`Memory` 与 `Embed` 通过窄接口接入 VM，避免 Value 反向依赖解释器。`Compiler` 只生成 Bytecode 模块，不直接调用解释循环。`Snapshot` 读取 VM 的显式帧和对象图，不依赖 C++ 调用栈。`include/Feather/Runtime.hpp` 暂时公开手写字节码所需构建器与原型，是实验性接口；稳定嵌入接口完成时再收窄公开头文件。ScriptObject 现由 VM 非移动堆持有，宿主跨 GC 使用它须持 RootHandle。

## 里程碑

1. **M1 手写字节码 VM**：Value、模块构建器/校验器、帧、全局表、基础指令；用手写模块验证循环、嵌套调用、默认参数、缺省返回和非法字节码。
2. **M2 对象协议**：ScriptObject、RootMetaObject、Function/NativeObject、成员指令；验证对象身份、键哈希、`__index`/`__call`。
3. **M3 所有权与嵌入**：GC、RootHandle、NativeObject 登记、宿主调用与回调；验证循环引用、临时根和 OOM 故障路径。
4. **M4 编译器**：先完成独立语法规范，再实现 lexer、parser、AST 与字节码生成；与 M1/M2 手写模块的行为对齐。
5. **M5 快照**：先冻结保存/恢复语义，再实现对象图和帧序列化。

每个里程碑须有能运行的验收样例。M1 至 M4 已具备对应测试；M5 之前先冻结保存/恢复语义。
