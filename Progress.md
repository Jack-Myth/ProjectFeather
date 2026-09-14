# 脚本语言工程进度

更新日期：2026-09-15  
设计依据：`SPEC-runtime-design.md`；工程结构见 `ENGINEERING.md`。本文件只跟踪进度和待决事项；语义以设计规范为准。未决事项不得由实现时顺手决定。

## 当前状态

**M1/M2/M3 及 M4 首版可运行，嵌入 API 已形成宿主闭环。** ScriptObject 使用 VM 非移动堆，显式 GC、RootHandle、NativeObject 登记、近似内存统计和分配故障通道可运行。宿主可注册 native 函数、读写全局值。`SPEC-syntax.md` 已定义第一版源码语法；`Compile(source)` 生成模块、初始化函数和命名函数映射。Meson 下 M1/M2/M3/嵌入/M4 五组测试通过。磁盘字节码和快照尚未实现。工程结构见 `ENGINEERING.md`。

### 已确定的主要方向

- C++ 实现、面向对象内部接口、帕斯卡命名；单线程嵌入式 VM，正确性和小巧优先。
- Tagged Union；`null`、`bool`、`number`、`string`、`object` 五种顶层值。number 为 double；string 为不可变 UTF-8 值，长度按字节。
- ScriptObject 持有数据哈希表和单层 MetaObject。默认共享 RootMetaObject；第一版元函数为 `__index(self, key)`、`__call(self, ...args)`。NativeObject 不使用 MetaObject，走虚函数接口。
- ScriptObject 的 key 可为 string 或 number；NaN 读取和写入均返回 Error，写入不创建成员；`+0` 和 `-0` 是同一个 key。普通比较中 NaN 不等于自身。
- 位置参数、单返回值；缺失参数采用声明的默认值，否则补 null；多余实参求值后丢弃。赋值成功留下写入值，语言层失败留下 Error。
- 栈式 VM 已确定；第 3 节定义初版内存模块 ISA、调用帧与校验规则。`JUMP_IF` 为真时跳转并弹出条件值，NaN 条件为 true。语言层错误是普通 Error 值，不自动传播；VM/宿主致命故障不包装成 Error。
- GC 由宿主决定触发；根集由 VM 私有维护，宿主以 AddToRoot/RemoveFromRoot 持有 GC 对象。NativeObject 本体不由 GC 回收，但其 GC 可见成员表由 VM 遍历；未登记的 C++ 成员由 NativeObject 自行管理。VM 快照只保证 VM 内部一致，宿主状态由宿主接口负责。

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

- 快照保存/恢复的返回值、代码身份、RootMetaObject 身份和宿主恢复失败语义。
- NativeObject 的 GC 追踪与释放钩子；脚本主动构造 Error、错误字段和调试钩子形式。
- 字节码/快照格式版本校验、恶意或损坏输入的验证策略。

## 下一个具体动作

M4 已加入源码位置诊断、UTF-8 检查、嵌套深度界限及 `CompiledProgram::Initialize(vm)` 宿主入口。下一步先冻结 M5 保存/恢复语义，再写快照实现。当前 `Compile` 与内存模块接口仍处实验阶段。
