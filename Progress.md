# 脚本语言工程进度

更新日期：2026-09-17
设计依据：`SPEC-runtime-design.md`、`SPEC-syntax.md`、`SPEC-snapshot.md`、`SPEC-bytecode-format.md`、`SPEC-symbol-format.md`、`SPEC-multimodule.md`、`SPEC-import.md`、`SPEC-native-modules.md`、`SPEC-stdio.md`、`SPEC-debug-protocol.md`、`SPEC-debugger-cli.md`、`SPEC-dap-adapter.md`；实施记录见 `ROADMAP-multimodule.md` 和 `ROADMAP-runtime-diagnostics.md`，工程结构见 `ENGINEERING.md`。本文件只跟踪进度和待决事项；语义以设计规范为准。

## 当前状态

**0.3.1 版本节点已形成：M1 至 M5 首版、单 VM 多文件模块、源码与字节码统一运行入口、精简 `feathervm`、嵌入式调试 target、TCP 控制台调试器、DAP adapter，以及带基础语言支持的 VS Code 扩展均可运行。** Release 流程已固定三级优化、LTO、`NDEBUG` 和完整测试；版本内容见 `CHANGELOG.md`。ScriptObject 使用 VM 非移动堆；VM 可在指令安全点自动执行完整标记清除，也保留空闲时显式 GC、RootHandle、NativeObject 登记、内存与收集统计和分配故障通道。宿主可注册 native 函数、读写模块全局值。`Compile(source)` 生成模块、初始化函数、命名函数映射和导出表。实验版 v3 `.fbc` 支持独立编译和加载；可选 v3 `.fbs` 保存源码位置、可断点标记、函数名及带生命周期的局部变量名。v3 快照已能保存/恢复模块集合、跨模块函数和对象。默认静态 runtime 包含 `DebugTarget` 与 `DapAdapter`；`-Ddebugger=false` 可完整移除 JSON 调试实现和控制台调试器，核心仍只保留传输无关的安全点接口。公开 C++ API、调试协议和磁盘格式仍处实验阶段。

### 已确定的主要方向

- C++ 实现、面向对象内部接口、帕斯卡命名；单线程嵌入式 VM，正确性和小巧优先。
- Tagged Union；`null`、`bool`、`number`、`string`、`object` 五种顶层值。number 为 double；string 为不可变 UTF-8 值，长度按字节。
- ScriptObject 持有数据哈希表和单层 MetaObject。默认共享 RootMetaObject；第一版元函数为 `__index(self, key)`、`__call(self, ...args)`。NativeObject 不使用 MetaObject，走虚函数接口。
- ScriptObject 的 key 可为 string 或 number；NaN 读取和写入均返回 Error，写入不创建成员；`+0` 和 `-0` 是同一个 key。普通比较中 NaN 不等于自身。
- 位置参数、单返回值；缺失参数采用声明的默认值，否则补 null；多余实参求值后丢弃。赋值成功留下写入值，语言层失败留下 Error。
- 栈式 VM 已确定；第 3 节定义初版内存模块 ISA、调用帧与校验规则。`JUMP_IF` 为真时跳转并弹出条件值，NaN 条件为 true。语言层错误是普通 Error 值，不自动传播；VM/宿主致命故障不包装成 Error。
- GC 在执行中的安全指令边界按动态阈值自动触发，VM 空闲时宿主也可显式触发；根集由 VM 私有维护，宿主以 AddToRoot/RemoveFromRoot 持有 GC 对象。NativeObject 本体不由 GC 回收，但其 GC 可见成员表由 VM 遍历；未登记的 C++ 成员由 NativeObject 自行管理。VM 快照只保证 VM 内部一致，宿主状态由宿主接口负责。

`feather run <program.fe|program.fbc>`、`featherc <source.fe> -o <program.fbc>` 和精简的 `feathervm <program.fbc>` 均已加入。源码运行会优先使用不早于 `.fe` 的同名、有效 `.fbc`，源码调试仍总是重新编译；`feathervm` 不链接编译器和调试实现。运行入口先执行顶层语句，再调用可选的无参数 `main`；独立程序未注册快捷函数，会明确拒绝快捷行。嵌入式宿主可在初始化前用 `RegisterNativeFunction` 注入 `__QuickOperator…`，用 `SetGlobal` 注入一般宿主对象；编译器保留快捷行标记供加载后使用。

`RegisterImport` 只校验并转发普通全局调用；宿主自行解析来源、创建子 VM、代理导出值并管理缓存。CLI 宿主从可执行文件旁的 `modules/` 搜索裸名，Native 动态库优先于同名 Feather 文件；`stdio.felib` 是可运行的独立模块，返回含 Console/IO 的单一对象，不直接注入全局名。解释器与 Native 库分别静态链接同一套 runtime 源码，不要求旁置 Feather DLL。跨 VM ScriptObject/Function 不通过旧适配器返回。

## 阶段 0 已讨论的事项与实施前检查

1. **执行与调用**：栈式 VM、独立的局部槽位和表达式栈、全局字符串表、默认基础值参数、显式 `Return` 及编译器补发 `Null; Return` 已写入规范；M1 已实现对应的字节码执行路径。
2. **字节码契约**：内存模块、定宽小端操作数、相对下一指令的跳转、指令边界与栈高度校验已认可。M1 不承诺磁盘格式。
3. **真值与运算**：NaN 条件为 true；数值遵循 IEEE 754；相等、大小比较及字符串相加见规范 5.3 节。
4. **成员操作**：MetaObject 创建/替换、原始查找的缺失判定及 NativeObject 写入结果见规范 1.5 节；具体 C++ 签名在 M2 开始前写入规范。
5. **内存与 API 边界**：string 使用引用计数不可变 UTF-8 存储；ScriptObject 使用 VM 非移动堆。宿主 RootHandle、安全点、同步 native 回调生存期、NativeObject GC 可见成员和 `std::bad_alloc` 故障通道已写入规范。第一版只提供 C++ API。

阶段 0 验收：规范无相互矛盾的已决定条目；每条指令可写出输入栈、输出栈和错误路径；有覆盖边界条件的手写字节码样例。

## 实施路线

| 阶段 | 范围 | 完成判据 |
|---|---|---|
| 1. VM 骨架 | Value、常量池、值栈、调用帧、局部/全局变量、数值运算、跳转、函数调用；用手写字节码驱动 | 条件、循环、嵌套调用、默认/多余参数、Error 值的测试通过 |
| 2. 对象协议 | ScriptObject 哈希表、RootMetaObject、`__index`/`__call`、NativeObject 虚函数；成员访问 | 缺失成员、元函数非法值、RootMetaObject 共享修改、NaN/±0 key 测试通过 |
| 3. 内存与嵌入 | GC、宿主根句柄、native 回调、资源释放与调试钩子 | 循环引用可回收；宿主持有值和回调临时值不误收；OOM 路径可验证 |
| 4. 编译器 | 独立语法规范、词法/语法分析、AST（若采用）、字节码生成与校验 | 源码生成的字节码与手写样例行为一致 |
| 5. 快照 | VM 栈帧与可达对象图序列化、宿主 handle 钩子、恢复 | 恢复后从保存点继续；共享引用/环/宿主对象及版本错误测试通过 |

## 后置但不可遗忘

- **单 VM 多文件模块**：第一版已完成；模块实例的私有全局表、函数/帧身份、显式导出、重复/循环初始化、GC 和 v3 快照见 `SPEC-multimodule.md`。CLI 宿主层已能按调用方目录解析相对 `.fe`/`.fbc` 导入并缓存文件模块。
- **运行时源码位置诊断**：内存指令位置表、普通 Error 查询、独立 `.fbs` 与 VM 故障位置已完成；需要时的快照 Error 来源再按版本演进，v3 `.fbc` 可单独加载并使用无位置回退。
- **嵌入式调试**：v1 Feather 调试协议、宿主 channel、跨线程命令队列和 VM 线程内暂停检查已完成。v3 `.fbs` 已提供可断点标记、函数名、参数名、局部变量名和词法范围；合成的函数绑定和隐式返回不会抢占源码断点。Error 结果边界可按前端请求暂停，并区分操作结果与函数返回；暂停线程还支持 Feather 表达式求值、条件断点以及 locals/stack/globals 和已有对象属性修改，求值期间屏蔽递归调试回调。DAP adapter 映射标准 exception breakpoint filter、evaluate 与 setVariable。`feather-debugger` 和 VS Code 扩展均用一条双向 TCP 连接消费同一 target；VS Code 侧在 extension host 内完成 DAP 转换并支持 launch/attach。`--debug-listen` 默认异步附加，显式 `--wait-debugger` 才允许先设断点再运行，脚本 stdio 与调试传输隔离。
- 快照保存/恢复的返回值、代码身份、RootMetaObject 身份和宿主恢复失败语义。
- NativeObject 的 GC 追踪与释放钩子；脚本主动构造 Error 及更多错误字段。
- 字节码/快照格式版本校验、恶意或损坏输入的验证策略。

## 下一个具体动作

0.3.1 已完成构建、运行、调试与编辑器入口的第一轮收束。下一节点优先把当前单文件正则补全升级为可解析导入和导出表的语言服务，再考虑交互式终端、远端路径映射、日志点/命中次数，以及可配置模块目录。`Compile`、内存模块、调试协议、字节码、符号及快照字节格式仍处实验阶段；在承诺兼容性前应继续保持显式版本校验。
