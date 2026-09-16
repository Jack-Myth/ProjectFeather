# 语言运行时设计规范

> 本文档目标：作为 AI 生成代码时的"唯一真相来源"。所有代码生成前应先参照本文档；
> 本文档未决定的事项，不应在代码生成过程中被"顺手"决定。
> 语法（词法/文法）不在本文档范围内，单独设计。

状态标记说明：`[ ]` 待定　`[x]` 已决定　`[~]` 倾向但未最终确认

---

## 0. 项目目标与非目标

### 0.1 设计目标

- [x] 核心目标：小巧、可嵌入、正确性优先于性能
- [ ] 代码规模预期：暂不设定具体行数目标
- [x] 主要使用场景：嵌入游戏的简单小脚本。由宿主维护一个 VM 实例，VM 内单线程
      执行（不做协程/多任务，见 0.2）
- [x] 实现语言为 C++，内部采用面向对象接口风格，标识符使用帕斯卡命名法；
      第一版只提供 C++ 嵌入接口，不提供 C ABI（见第 7 节）。

### 0.2 已确定不做的特性（非目标）

> 明确写"不做什么"，防止生成代码时被"顺手"加上。

- [x] 不做原生闭包 / upvalue 捕获（函数是一等值，闭包语义可由用户手动用 函数 + 容器 组合实现）
- [x] 不做异常处理机制（不做 try/catch，不做栈展开）。所有错误统一表示为
      Error 值类型，详见 5.2 节
- [x] 不做协程 / 多任务。VM 单线程执行，由宿主负责维护/驱动一个 VM 实例
- [x] 不做传统面向对象（class / 继承），不做任意层级的 metatable 查找链或
      运算符重载。ScriptObject 采用受限的单层 MetaObject 行为协议，
      NativeObject 仍通过虚函数接口实现行为，详见 1.4～1.5 节。
- [ ] 其他明确排除的特性：

---

## 1. 值表示（Value Representation）

### 1.1 物理布局

- [x] 采用 Tagged Union（而非 NaN-boxing），优先保证正确性与可读性
- [x] 当前 `Value` 有五种顶层 tag，内部为 tagged union：bool、double、
      引用计数不可变 UTF-8 string、非拥有的 ScriptObject 指针、拥有型的
      NativeObject `shared_ptr`。object 内部子标记区分 Script 与 Native，
      不增加脚本可见的顶层类型。NativeObject 的具体种类由其 ObjectType 标记。
      C++ 类的确切字段以当前头文件为准，公开嵌入 API 仍处实验阶段。

### 1.2 基础类型清单

- [x] `null`（不使用 "nil" 这个名称）
- [x] `bool`
- [x] `number`：不区分 int / float，底层统一使用 `double` 存储
      （精度权衡：放弃 2^53 以上的精确整数表示；若未来需要精确大整数，
      作为独立的 BigInt 类型单独引入，不改变 number 的既有语义）
- [x] `object`：所有复合类型（脚本自定义数据、数组、Map、函数、宿主对象、
      Error）统一归为 object，细分为 ScriptObject 与 NativeObject 两类，
      详见 1.4 节

除已确定的独立 string 类型外，table / array / function / error 不另设顶层
类型标签；它们归于 object 的具体子类（见 1.4）。

- [x] `string` 是独立的顶层值类型，不作为 NativeObject。脚本层采用值语义：
      不可变、按内容比较、无可观察的引用身份；赋值和传参得到同样内容的值。
      底层可以共享不可变存储，不要求每次赋值都复制全部字符。
- [x] 初版 string 使用引用计数的不可变 UTF-8 字节缓冲区；构造时校验编码，
      比较和哈希均按字节。`Value` 拷贝/销毁负责缓冲区引用；string 不参与 Object GC。
- [x] string 内容使用 UTF-8 编码；长度按字节计，不按 Unicode 字符数计。

### 1.3 值语义 vs 引用语义

- [x] `null`、`bool`、`number` 为值语义（栈上直接存储，赋值/传参即拷贝）
- [x] `string` 为脚本层值语义；其内容可由共享的不可变堆存储承载，
      赋值/传参不要求逐字节复制，但共享关系不可被脚本观察。
- [x] `object`（含其所有子类：ScriptObject、NativeObject 及其具体种类）为引用
      语义（堆分配，赋值/传参传递引用，多处持有共享同一实例）
- [x] 物理所有权按子类区分：ScriptObject 由所属 VM 的非移动堆独占持有，
      `Value` 对它保存非拥有引用；NativeObject 由 C++ `shared_ptr` 持有。
      宿主需要让 ScriptObject 跨越 GC 时点时必须使用第 2 节根句柄。

### 1.4 Object 体系：ScriptObject 与 NativeObject

- [x] 划分依据：由**脚本代码**定义的 object 实例 → `ScriptObject`；由 **VM 或
      宿主**定义的 object 实例 → `NativeObject`
- [x] ScriptObject 自身的数据是一张哈希表；其可定制行为由关联的单层
      MetaObject 提供，不在数据表内查找元函数。
- [x] 每个 ScriptObject 创建时默认关联同一个 `RootMetaObject`。它是唯一
      没有关联 MetaObject 的 ScriptObject；Env 提供获取它的接口。修改
      RootMetaObject 的成员会影响所有仍关联它的对象。
- [x] 元函数查找只对关联的 MetaObject 自身做原始哈希表查找；即使该
      MetaObject 另有关联 MetaObject，也不继续查找。RootMetaObject 自身
      可直接读写其数据表，缺失成员返回 Error。
- [x] 第一版元函数仅包含 `__index(self, key)` 和 `__call(self, ...args)`；
      `__index` 仅在 ScriptObject 自身缺少成员时调用，`__call` 用于调用
      ScriptObject。元函数存在但不可调用时返回 Error。
- [x] NativeObject 不使用 MetaObject；其行为由 `call`、`get_member`、
      `set_member` 等虚函数接口实现。
- [x] NativeObject 涵盖：`Function`（脚本中定义的函数，其本质是创建了一个
      Function 类型的 NativeObject 实例）、数组、Map、宿主对象、`Error`
  - 数组、Map 虽是语言使用上的基础设施，但不要求由 VM 核心内建实现，可作为
        某个扩展模块 / 标准库、用 NativeObject 接口实现即可，不强制在运行时
        核心中硬编码
  - `Function` 必须是 NativeObject、不能是 ScriptObject：若 Function 本身是
        ScriptObject，则"调用 ScriptObject"需要委托给它内部的某个 Function
        字段，而该 Function 又是 ScriptObject 的话将导致无限递归定义，因此
        Function 只能落在"不可再分解"的 NativeObject 一侧
- [x] **每个 object 都有一个 Type 标记，标识其是 ScriptObject 还是
      NativeObject**（具体到 NativeObject 还需能区分其种类，如 Function /
      Array / Map / 宿主对象 / Error，用于虚函数分派与 `get_type()`）

### 1.5 三个基础原语操作（调用 / 取成员 / 写成员）

> 语言中一切"行为"（函数调用、数组索引、属性访问、类似继承的委托效果）都由
> 这三个原语组合表达。ScriptObject 的调用与缺失成员读取可经单层
> MetaObject 元函数实现；NativeObject 由虚函数接口实现。不做任意层级的
> 隐式查找链或运算符重载。

- [x] **调用**（对应概念上的 `Env.CallObject(obj, args)`）
  - 若 `obj` 是 NativeObject：直接调用其虚函数表中的 `call` 实现
  - 若 `obj` 是 ScriptObject：原始查找其 MetaObject 的 `__call`；若存在
        可调用值，则以 `obj` 为首参数、后接原始实参调用之；否则返回 Error。
  - 对不可调用的值（如 number、无有效 `__call` 的 ScriptObject）执行调用：
        视为未定义操作，返回 Error（呼应 5.2 节 total 化原则）
- [x] **取成员**（对应概念上的 `Env.FetchObjectMember(obj, key)`）
  - 若 `obj` 是 NativeObject：直接调用其虚函数表中的 `get_member` 实现
  - 若 `obj` 是 ScriptObject：先原始查找自身数据表；若缺失，则原始查找其
        MetaObject 中的 `__index`，存在可调用值时调用 `__index(obj, key)`，
        否则返回 Error。VM 不自动追踪更多层的 MetaObject。
- [x] `a.b` 与 `a[b]` 在 VM 原语层面统一为 `GetMember`/`SetMember`；
      点访问由编译器提供字符串 key，方括号访问先求值 key 表达式。
- [x] ScriptObject 允许 string 与 number 作为成员 key；点访问 `a.b` 只表示
      字符串 key `"b"`，方括号访问可使用 number key，以允许脚本层实现数组式容器。
- [x] NaN 不允许作为 ScriptObject 成员 key；读取 NaN key 始终返回 Error，
      不调用 `__index`。
      `+0` 与 `-0` 视为同一个 key，哈希值必须一致。
- [x] 写入 NaN key 时不创建或存储成员；写入表达式返回新的 Error，
      并按普通 Error 产生规则通知已启用的调试钩子。
- [x] 宿主可用 `CreateMetaObject()` 创建普通 ScriptObject，并用
      `SetMetaObject(target, meta)` 替换普通 ScriptObject 的关联对象；
      target 与 meta 均须为 ScriptObject，RootMetaObject 不可作为 target。
      脚本第一版不直接暴露替换接口。RootMetaObject 的快照身份规则留到第 6 节。
- [x] 原始查找区分“key 不存在”与“存在但值为 null/Error”。`__index`
      的返回值（包括 Error）就是读取结果，不再重试查找。缺失元函数和不可调用
      元函数分别生成新的 Error；NaN key 在查找元函数前生成 Error。
- [x] **写成员**（对应概念上的 `Env.StoreObjectMember(obj, key, value)`）
  - 若 `obj` 是 NativeObject：调用其虚函数表中的 `set_member` 实现
  - 若 `obj` 是 ScriptObject：朴素哈希表写入（不存在的 key 直接创建）
- [x] **NativeObject 虚函数表（vtable）现阶段确定的槽位**：`call`、
      `get_member`、`set_member` —— 先解决"完备性"（三个操作足以表达目前所有
      已讨论的语义），GC 相关钩子（标记子对象、释放资源）等后续按需在第 2 节
      GC 设计时补充，不在此阶段展开
- [x] **取成员对不存在的 key**：ScriptObject 先尝试 `__index`；无有效
      元函数时返回 Error（不是 null）。NativeObject 由其实现决定。
- [x] **ScriptObject 写入不存在的 key**：自动创建该成员；NativeObject 的
      写入行为由对应 `set_member` 实现决定，不要求自动创建。
- [x] NativeObject 的 `set_member` 返回一个 Value：成功必须返回传入的写入值，
      语言层失败返回新的 Error。未知 key 的 `get_member` 默认返回新的 Error。

---

## 2. 内存管理（GC）

- [x] 初版 ScriptObject 使用非移动标记清除 GC。
- [x] VM 独占 ScriptObject 堆，`CreateScriptObject` 和 `CreateMetaObject` 是
      创建入口；RootMetaObject 是该堆的永久根。对象存稳定地址，成员表与
      MetaObject 引用不持有堆所有权。跨 VM 的 ScriptObject 不能作为本 VM
      的实参、成员、MetaObject 或根使用。
- [x] VM 在没有 native 回调活跃、没有快照临时状态的指令边界按对象数量软阈值
      自动执行完整标记清除。收集后的下一阈值至少增长 1024，并至少为当前存活
      对象数的两倍；有限的 ScriptObject 硬上限同时限制该阈值。宿主仍可查询统计，
      并在 VM 空闲时显式调用 `CollectGarbage()`。
- [x] 达到 ScriptObject 硬上限前，VM 在可收集的指令安全点强制尝试完整收集；
      收集后仍无空间、或 native 回调及快照临时状态中无法安全收集时，以
      `std::bad_alloc` 走宿主致命故障通道，不生成语言层 Error。RootMetaObject
      占用一个对象名额；不提供 fatal 回调。
- [x] 宿主显式 GC 仅允许 VM 未执行指令、没有 native 回调活跃时调用；运行中
      显式请求仍是宿主 API 错误。自动 GC 只由 VM 自己在已知安全的指令边界触发。
- [x] 根集包括 RootMetaObject、全部模块的私有全局表、运行中帧的局部槽位和表达式栈、
      宿主根句柄、native 回调登记的临时值及仍存活 NativeObject 的 GC 可见成员。
- [x] 单条指令执行和 native 回调期间不触发 GC。回调中的分配达到软阈值时只
      记录待收集请求，返回值压回 VM 栈后在下一指令边界处理。native 回调若持有
      跨回调时点的 GC 对象，必须通过根句柄持有；回调期间显式 GC 仍被拒绝。
- [x] VM 维护 native 调用深度；进入 NativeObject `Call` 时加一，离开时
      （包括 C++ 异常离开）减一。回调的实参与外层 VM 帧在同步调用期间保持
      有效；此期间 GC 被拒绝。回调若在返回后仍需持有 ScriptObject，必须
      使用 RootHandle。未来快照保存入口使用相同深度计数。
- [x] GC 根集由 VM 创建和维护，不直接暴露给脚本或宿主修改；宿主通过
      `AddToRoot` 取得 RootHandle，并通过句柄析构或 `Reset()` 释放根。
      底层 `RemoveFromRoot` 仅供句柄内部调用。
- [x] NativeObject 本体由宿主或 VM 显式持有，不参与 GC 回收。
- [x] NativeObject 可把 ScriptObject 等 GC 管理的值存入其 GC 可见成员表；
      VM 在 GC 时遍历所有仍存活 NativeObject 的该成员表，并追踪其中的引用。
      其他未登记在该成员表中的原生状态/引用由 NativeObject 自行管理；初版
      不为任意 C++ 成员变量提供自动扫描。
- [x] 宿主通过 `RegisterNativeObject(shared_ptr<NativeObject>)` 登记需要被 GC
      扫描的原生对象；VM 保存弱引用，GC 时跳过并清理已销毁的登记项。
      NativeObject 的 GC 可见成员由专用 setter/移除接口维护，不扫描任意
      C++ 字段。NativeObject 本体由 C++ `shared_ptr` 持有，不由 ScriptObject GC 释放。
      RootMetaObject 随 VM 存活；string 生命周期见 1.2 节。公开宿主 API 返回
      的 GC 对象需由根句柄持有，VM 销毁后句柄失效，重复释放为 API 误用。
- [x] `AddToRoot(Value)` 返回不可复制、可移动的 `RootHandle`；析构或一次
      `Reset()` 解除根。VM 销毁后读取句柄报 API 错误。裸 ScriptObject Value
      在下一次 GC 后可能失效；宿主不可在未持根时跨 GC 使用它。
- [x] 收集器从 RootMetaObject、函数对象、全部模块全局表、宿主根句柄、登记的
      NativeObject 成员以及自动收集时的活跃帧局部槽位和表达式栈开始标记，沿
      ScriptObject 成员和 MetaObject 引用遍历，清除未标记对象。宿主显式收集
      仍只发生在空闲安全点，因而没有活跃帧。
- [x] `GetGcStatistics()` 提供 ScriptObject 数、成员数、估算字节数、收集次数、
      累计分配/回收数和下一对象数阈值。估算值不包含所有 C++ 分配器开销、
      NativeObject 本体或引用计数字符串，不承诺精确总内存占用。

---

## 3. 字节码格式（Bytecode ISA）

- [x] 执行模型：栈式虚拟机。每次调用建立 VM 自行维护的调用帧，
      不依赖 C++ 原生调用栈来保存脚本帧。
- [x] 初版只支持内存中的字节码模块。每条指令为 1 字节 opcode，后接定宽
      小端操作数：常量索引、局部槽位、全局名称索引为 u32；`Call` 实参数量为
      u16；`JUMP`/`JUMP_IF` 相对偏移为 i32。无操作数指令只有 opcode。
- [x] 常量池只含 number、string 和函数原型。函数原型含代码、形参数、局部
      槽位总数和各形参的可选基础值默认项（null、bool、number 或 string）；
      默认项按值存储，不需要为了 null/bool 扩展 `Const` 常量类型。
      模块加载时为每个函数原型创建稳定的 Function
      NativeObject，`Const` 读取该原型时压入同一对象；只要 VM 或宿主仍引用
      该 Function，就不得销毁它所依赖的模块与代码。
- [x] 控制流指令命名：使用 `JUMP`（无条件跳转）与 `JUMP_IF`（条件跳转），
      不使用 `JUMP_IF_FALSE` 名称。
- [x] `JUMP_IF` 在条件值为 true 时跳转；条件值按语言真值规则解释。
- [x] `JUMP_IF` 弹出条件值，再按其真值决定是否跳转。
- [x] 跳转偏移从当前整条指令结束后的下一字节起算；目标必须是同一函数中
      一条指令的起始字节，不允许落在操作数内或代码末尾之后。

### 3.1 指令表

> 编译器和 VM 共享的唯一真相来源，需要双方严格对齐。
> 下表为初版内存模块指令集；操作数编码、跳转和校验规则见本节及 3.2 节。
> 栈效应中的方括号只列出受影响的栈顶部分，左侧较早入栈；未列出的栈前缀
> 保持不变。`[左, 右]` 中的 `右` 是栈顶。`Set*` 是表达式指令：成功留下
> 被写入的值；失败留下 Error。若源码把赋值作为独立语句，编译器需另发 `Pop`。

| Opcode | 操作数 | 栈效应（入栈/出栈） | 语义说明 |
|---|---|---|---|
| `Const` | 常量索引 | `[] → [值]` | 压入 number、string 或稳定的 Function 对象 |
| `Null` / `True` / `False` | 无 | `[] → [值]` | 压入对应基础值 |
| `Pop` | 无 | `[值] → []` | 丢弃栈顶；表达式语句的结果也由此丢弃 |
| `GetLocal` / `GetGlobal` | 槽位/名称索引 | `[] → [值]` | 读取局部或全局变量；未定义全局返回新 Error |
| `SetLocal` / `SetGlobal` | 槽位/名称索引 | `[值] → [结果]` | 写入变量，成功时结果是被写入的值 |
| `NewObject` | 无 | `[] → [对象]` | 新建 ScriptObject，默认关联 RootMetaObject |
| `GetMember` | 无 | `[对象, key] → [结果]` | 执行取成员原语；点访问由编译器压入字符串 key |
| `SetMember` | 无 | `[对象, key, 值] → [结果]` | 执行写成员原语；成功留下写入值，语言层失败留下 Error |
| `Add` / `Sub` / `Mul` / `Div` | 无 | `[左, 右] → [结果]` | 按 5.3 节运算规则；非法类型组合返回 Error |
| `Negate` | 无 | `[值] → [结果]` | 数值取负；非法类型返回 Error |
| `Equal` / `Less` | 无 | `[左, 右] → [结果]` | 按 5.3 节比较规则 |
| `JUMP` | 相对偏移 | `[] → []` | 无条件跳转 |
| `JUMP_IF` | 相对偏移 | `[条件] → []` | 弹出条件，按 5.3 节真值规则；为 true 则跳转，否则继续 |
| `Call` | 实参数量 | `[被调用值, 参数1, ..., 参数N] → [结果]` | 调用 ScriptObject/NativeObject；实参先求值，固定一个返回值 |
| `Return` | 无 | `[返回值] → []` | 结束当前帧，向调用者交付一个返回值；调用者栈上取得它 |

### 3.2 指令执行约定

- [x] `JUMP_IF` 为 true 时跳转，且无论是否跳转都消耗条件值。`JUMP`
      不读写值栈。`if/else` 可由编译器排列代码并组合这两条指令实现。
- [x] `Call` 的实参数量是**源码已经求值并入栈**的数量。形参缺省和多余
      实参规则见 5.1 节；多余实参仍保留其求值副作用，然后被丢弃。
- [x] ScriptObject 的 `__call` 接收原对象 `self` 作为首参数，再接原始
      实参；NativeObject 直接走自身 `call` 接口。
- [x] 加载或执行前校验指令边界、操作数与常量类型、跳转目标、局部槽位、
      所有可达路径的栈下溢和合流处栈高度。栈下溢、非法槽位、坏跳转、PC
      越界、超出资源上限都属于无效字节码或 VM 故障，不生成语言层 Error。
      栈容量、调用深度与模块大小的具体上限由实现配置，不属于语言语义。
- [x] 宿主可在构造 VM 时设置每次最外层执行的指令预算；每取出一条指令消耗一单位。同一 VM 的 native 回调中再次调用 `Run` 共用当前预算，不能通过重入重置计数。预算耗尽抛 VM 故障异常，不生成普通 Error，也不回滚已发生的脚本副作用。下一次独立 `Run` 或快照恢复重新取得完整预算；预算是宿主资源策略，不进入快照。嵌入式 VM 默认不限指令数；独立运行程序限制每次最外层执行最多 10,000,000 条指令和 100,000 个同时存活的 ScriptObject（含 RootMetaObject）。
- [x] 局部槽位和表达式栈逻辑分开。每帧保存 Function、下一条指令 PC、
      调用者恢复位置、局部槽位及表达式栈起点。`Return` 前当前帧表达式栈
      必须恰有一个值；取走该值并销毁当前帧，然后压入调用者表达式栈，
      或作为最外层调用的宿主结果。任何路径不得直接落出函数代码末尾；
      编译器对源码中无显式返回的路径补发 `Null; Return`。

---

## 4. 编译流水线（Compiler Pipeline）

- [x] 保留 AST 中间表示：lexer → parser → AST → 字节码生成，最后校验模块。
- [x] 词法 token 与 EBNF 文法见独立的 `SPEC-syntax.md`。
- [x] AST 第一版包含字面量、名称、对象创建、成员访问、调用、一元/二元运算、
      赋值，以及声明、返回、条件、循环、块、表达式语句和顶层函数声明。

### 4.1 作用域与变量解析

- [x] 变量仅有局部和全局两种，不含 upvalue；形参和块内 `var` 分配独立局部槽位，
      编译期按词法作用域解析，内层可遮蔽。离开作用域后槽位不复用。
- [x] 顶层 `var` 和未解析为局部的名称使用当前函数所属模块的字符串键全局表；顶层声明函数
      在初始化函数中先绑定到该模块全局，再执行顶层语句。编译结果由模块、初始化函数、
      函数名到常量索引的映射及显式导出名组成，见 `Compiler.hpp` 与 `SPEC-multimodule.md`。

---

## 5. 运行时语义

### 5.1 函数调用约定

- [x] 仅支持位置参数，函数固定返回一个值。
- [x] 赋值是表达式，成功时留下被写入的值；语言层写入失败时留下 Error。
      表达式结果若被外层语句丢弃，Error 也作为普通值被丢弃，不自动传播。
- [x] 形参可在定义时声明默认值；实参不足时按形参声明补值，未声明默认值时
      补 null。多余实参在求值后丢弃，不传入函数体。
- [x] 第一版默认值只允许 null、bool、number、string 常量，在调用时填入
      缺少的形参；未声明默认值补 null。多余实参求值后丢弃。其余局部槽位初始为 null。
- [x] 不支持多返回值。
- [x] 调用帧结构及 Return 语义见 3.2 节。局部槽位和表达式栈逻辑分开；
      `GetLocal` 读取已分配槽位，`SetLocal` 写入并留下写入值。
      全局变量存入 VM 私有的字符串键哈希表；全局名称索引必须引用 string
      常量。`GetGlobal` 读取不存在的名称返回新 Error；`SetGlobal` 创建或覆盖。
- [x] `__index`、`__call` 元成员存在但不是可调用值时，当前操作返回 Error。
- [x] 普通 number 相等比较遵循浮点语义：NaN 不等于自身；`+0` 等于 `-0`。
- [x] 普通赋值操作成功时留下右侧值，失败时留下 Error；独立的赋值语句
      可以丢弃结果，但不会自动传播 Error。

### 5.2 错误处理

> 核心决定：不存在异常/try-catch/栈展开机制。所有语言层面的"错误"统一表示为
> 一个一等值类型 `Error`，通过正常的返回值路径流转，不产生任何隐式控制流跳转。
> 该决定使错误处理与第 6 节的状态快照完全解耦——调用栈中不存在任何"悬而未决的
> 异常处理器绑定关系"，因此调用栈始终是规整、可直接序列化的普通数据结构。

- [x] **Total 化原则**：所有运算符、属性访问、函数调用等操作对任意类型组合都
      有定义。任何"未定义/不合法"的操作，统一返回一个新的 `Error` 值，而不是
      中断执行。
- [x] **Error 不自动传播**：后续操作按普通对象语义执行；已定义的操作（如
      `error.message`、条件判断、相等比较）正常返回结果，未定义操作生成新的
      Error。例如 `a.b.c` 中若 `a.b` 返回 Error，而 Error 不认识 key `c`，
      第二次成员读取生成新 Error，先前消息不会自动保留。
- [x] **Error 只是普通值**：不会自动中止当前函数，也不会自动跨调用向上传播。
      它可以被赋值、存入容器或被后续返回值覆盖；宿主只能检查实际收到的返回值。
      因此该机制只提供简单的语言层错误表示，不保证发现执行过程中发生过的每个错误。
- [x] **读取类操作是唯一的"正常实现"，不是例外**：`get_type(v)` 与
      `v.message`（及 Error 预置的其他字段）就是普通的类型判断 / 属性访问，
      只是 Error 类型恰好实现（预置）了 `message` 等字段。因此
      `error.message` 能正常取到消息，`get_type(error)` 能正常返回 Error 这个
      类型标记——这些操作本身对 Error 类型有明确定义的行为，不属于"未定义操作"，
      自然不会再产生新的 Error。
- [x] **Error 在 Object 体系中的位置**：Error 是 NativeObject 的一种（见 1.4
      节）。`error.message` 是对 Error 这个 NativeObject 调用其 `get_member`
      虚函数实现，该实现认识 `message` 这个 key 并返回消息内容——这是
      `get_member` 的正常、明确定义的行为，不属于"未定义操作"。若对 Error
      取一个它不认识的成员（如 `error.foo`），则该 `get_member` 实现对未知 key
      的默认行为同样是返回新的 Error（呼应 1.5 节"取成员对未定义 key 返回
      Error"的通用约定），无需为此单独设计规则
- [ ] Error 携带的字段（待定，至少包含）：
  - `message`（字符串）
  - 是否需要错误分类/kind 标记：
  - 是否记录产生位置（调用栈信息）：倾向不记录在值本身中（见下方调试钩子设计），
        保持 Error 作为轻量普通 Value，不因调试需求增加正常路径的开销
- [ ] 是否允许脚本主动构造 Error（如 `Error("自定义消息")`）：
- [x] **调试钩子（与正常执行路径解耦）**：
  - 安装 `VmDebugController` 后，VM 在操作结果为 Error 或函数返回 Error 的结果边界
        调用 `OnError`。这是结果事件而不是对象创建事件：同一个 Error 经过多个函数
        返回边界可以报告多次，成员操作取得已有 Error 也会报告。
  - 来源分为 `Operation` 与 `FunctionReturn`。前者覆盖非法运算/调用、未定义全局、
        取成员和写成员等操作；后者覆盖 Native 与 Feather 函数返回 Error。
  - 普通局部读取及读取已存在的 Error 全局不报告。Error 的普通值语义、控制流和
        `GetErrorLocation` 首次来源记录均不受调试事件影响。
  - `DebugTarget` 默认忽略该回调；前端通过 `Debugger.setPauseOnErrors` 选择是否暂停，
        暂停事件仍带来源与 Error 摘要。未安装控制器时不发生协议事件或暂停。
- [ ] 错误信息如何传递给宿主：宿主每次调用脚本函数后，通过 `get_type` 检查
      返回值是否为 Error 类型，由宿主决定后续处理（详见第 7 节）
- [x] Error 仅表示语言层操作错误。OOM、VM 内部故障及宿主故障不进入语言的
      Error 体系；由宿主自行处理，或走 VM 的致命故障路径。

源码诊断另见 `ROADMAP-runtime-diagnostics.md`：编译器生成可选的内存指令位置表，也可写入独立 `.fbs`；VM 为首次观察到的 Error 记录每 VM 独立来源，宿主通过 `Vm::GetErrorLocation(value)` 查询。执行预算、分配和宿主回调等未捕获故障继续抛原有 C++ 异常，宿主可在捕获后通过 `Vm::GetFaultLocation()` 查询位置。最外层新执行会清除上次故障位置；继承 `std::exception` 的同一异常穿过宿主重入时保留内层位置，宿主改抛的新异常标在当前调用点。这些位置不进入 Error 值本身，也不改变 Error 的普通值语义。普通来源查询记录对象的首次位置；Error 调试事件观察每个结果边界，两者相互独立。

通用嵌入式调试见 `SPEC-debug-protocol.md`。核心在指令前安全点及 Error 结果边界调用可选 `VmDebugController`，并提供回调期间有效的只读帧、全局和对象属性视图；JSON 命令、暂停等待和宿主 channel 位于可剥离的 `feather-debug`。暂停时执行帧仍在 `ExecutionState` 中，继续作为 GC 根；协议线程不得直接访问该上下文。

### 5.3 条件真值与数值边界

> 条件不要求 bool；编译器无须做静态类型推断。`JUMP_IF` 在运行时按以下
> 规则将条件值解释为 true/false。此处定义的真值不改变该值本身的类型。

| 值类型 | 条件真值 |
|---|---|
| `null` | false |
| `bool` | 其自身的 true/false |
| `number` | `+0`、`-0` 为 false；其他有限非零数及正负无穷大为 true |
| `string` | UTF-8 字节长度为 0 时 false，否则 true |
| `object` | true；包括 Error、ScriptObject、NativeObject |

- [x] NaN 的条件真值为 true；number 中只有 `+0` 和 `-0` 为 false。
- [x] 不存在脚本可观察的“无效 Object 引用”值；缺少对象使用 `null`。
      因此 object 的条件判断不检查底层指针是否仍有效。
- [x] Error 是普通 object，故在条件中按 object 规则为 true；`if (error)`
      不表示错误检测。需要显式检查其类型或字段。
- [x] 普通相等比较中 `NaN == NaN` 为 false，`+0 == -0` 为 true；
      ScriptObject 的 key 规则另见 1.5 节（NaN 禁止作 key）。
- [x] `Equal`：同类型的 null、bool、number 按值比较，string 按 UTF-8
      字节内容比较，object 按对象身份比较；不同顶层类型返回 false。
      NaN 不等于任何数，`+0` 等于 `-0`。
- [x] `Less` 仅对 number/number 有定义；NaN 参与时返回 false，其余
      类型组合生成新 Error。初版不支持字符串排序。
- [x] `Add` 对 number/number 执行 IEEE 754 双精度加法，对 string/string
      执行字节拼接；不做隐式转换。`Sub`、`Mul`、`Div`、`Negate` 只接受 number，
      结果采用 IEEE 754 双精度；除以零可得到无穷大或 NaN，不生成 Error。
      其他非法操作数组合生成新 Error。

---

## 6. 状态快照（保存 / 恢复）

> 核心功能：宿主注册 native 保存入口，脚本调用后可保存当前执行状态；恢复时
> 相当于从该调用处返回。M5 的返回值、对象图及格式契约见 `SPEC-snapshot.md`。

### 6.1 边界约束（已讨论确定的方向）

- [x] 捕获点的活动调用链除保存入口自身外必须是纯脚本帧，**不允许**跨越其他
      正在执行的宿主（native）函数调用；恢复只能在新构造且空闲的 VM 上开始。
- [x] 保存入口本身按一次 native 调用计数：进入保存入口后，native 调用深度
      必须恰好为 1；若深度大于 1（如 native 回调脚本后再次调用保存入口），
      不允许保存。快照不包含保存入口自身的 native 帧，而记录脚本侧的续执行状态。
- [x] 调用栈是运行时自行维护的数据结构（而非借助 C 原生调用栈），因此可以完整
      序列化与重建
- [x] 保存点由宿主注册的 native 函数触发，不新增专门字节码指令；宿主在该
      函数内部调用 `Vm::CaptureSnapshot(codec)`，持久化字节后让函数返回 false。
- [x] 约束检测：维护 native 调用深度，保存入口检查其值是否恰好为 1。
- [x] 深度不符的捕获 API 抛 `std::logic_error`，面向脚本的 native 包装器把
      预期拒绝映射为 Error；正常保存调用返回 false，恢复后该调用返回 true。

### 6.2 序列化方案

- [x] VM 快照仅保证 VM 内部状态一致；宿主状态的一致性依赖宿主正确实现
      相应的保存/恢复接口，VM 不作无条件保证。

- [x] 可达 ScriptObject/NativeObject 用 ID 保留共享与环，读取时两遍重建；
      RootMetaObject 保留固定身份。详细范围见 `SPEC-snapshot.md`。
- [x] 每帧保存函数常量索引、下一条指令 PC、局部槽位和表达式栈；保存点顶层帧
      记录待注入的 native 调用结果。恢复时先压入 true，再继续执行。
- [x] 保存前不强制 GC，只遍历快照约定的 VM 可达根；宿主 RootHandle 不序列化。

### 6.3 宿主对象引用（host handle）

> 脚本状态中若引用了宿主对象（文件句柄、宿主指针等），无法直接序列化，
> 需要宿主提供序列化/反序列化钩子。

- [x] FunctionObject/ErrorObject 由 VM 自身编码；其他 NativeObject 由宿主 codec
      编为稳定类型 ID 与不透明字节，并使用新 VM 引用重建。NativeObject 的 VM
      可见成员表由 VM 单独序列化。具体接口形状见 `SPEC-snapshot.md`。
- [x] 缺少 codec、未知类型 ID 或 codec 失败均拒绝快照/恢复，不丢弃对象。

### 6.4 版本兼容性

- [x] 格式含 magic、主/次版本和显式长度；恢复前验证版本、边界与对象引用。
- [x] 用规范化模块内容逐字节比较绑定代码；与宿主 codec 类型实现绑定，首版
      不保证跨版本兼容。

---

## 7. 嵌入式 API（Embedding API）

- [x] 内部实现与主要嵌入接口使用 C++、面向对象风格和帕斯卡命名。
- [x] 第一版只提供 C++ 嵌入 API，不提供 C ABI。宿主跨 GC 时点持有
      ScriptObject 使用 `AddToRoot(Value) -> RootHandle`；句柄不可复制、可移动，
      析构或 `Reset()` 自动解除根。重复 Reset 为 API 误用；VM 销毁后句柄失效。
- [x] 宿主可继承 `NativeObject`，实现 `IsCallable()` 和 `Call(args)`；通过
      `RegisterNativeFunction(name, shared_ptr<NativeObject>)` 登记到 VM 并绑定
      全局名称。其他持有 GC 可见成员的原生对象通过 `RegisterNativeObject`
      登记，再使用专用 GC 可见成员 setter/移除接口。
- [x] `Vm(shared_ptr<Module>, MaxScriptObjects, MaxInstructionsPerInvocation)` 构造时验证并冻结模块；
      析构销毁 VM 堆及全局状态。宿主用 `Run(functionConstantIndex, args)`
      调用主模块入口。`GetGlobal(name)`/`SetGlobal(name, value)` 访问主模块全局表；
      未定义全局读取返回新 Error。跨 VM ScriptObject 或 Function 使用报 API 错误。
- [x] 宿主对返回 Value 先看顶层类型，再看 `GetObjectType()` 或 `IsError()`；
      Error 的 `message` 可通过其 `get_member` 或 C++ ErrorObject 接口读取。
      语言 Error 是正常返回值；无效模块/API 误用与 `std::bad_alloc` 走 C++
      异常通道。`Compile(source)` 返回 `CompiledProgram`；宿主用其模块构造 VM，
      注册 native 后调用 `Initialize(vm)` 一次，再按 `Functions` 映射调用脚本函数。该接口仍处
      实验阶段。`SerializeProgram` / `DeserializeProgram` 处理实验版磁盘模块，
      格式及严格加载边界见 `SPEC-bytecode-format.md`。
- [x] `import` 是宿主通过 `RegisterImport(vm, callback)` 注册的普通 native 全局函数，
      编译器与 VM 不解析依赖或跨 VM 传递脚本对象。宿主 Feather 模块可通过
      NativeObject 代理暴露；参数、返回值和快照责任见 `SPEC-import.md`。
- [x] 单 VM 多文件模块通过 `LoadModule` / `RunModule` / `InitializeModule` 维护私有全局和代码身份；
      `RegisterModuleImport` 是另一种宿主适配器，可返回同一 VM 的脚本值。VM 不读取文件，
      逻辑模块 ID、`export`、循环初始化、GC 和快照边界见 `SPEC-multimodule.md`。
- [x] 与第 6 节快照功能相关的 C++ API 首版：`CaptureSnapshot(codec)`、
      `ResumeSnapshot(bytes, codec)` 与 `SnapshotHostCodec` 接口，语义见
      `SPEC-snapshot.md`；公开签名和磁盘格式尚未承诺稳定。
- [ ] 调试钩子注册方式（对应 5.2 节"调试钩子"，宿主如何注册 Error 产生时的
      回调，回调参数需包含来源标记：VM 内部产生 / 脚本主动构造）：

---

## 8. 待决问题清单（讨论中，尚未定论）

> 汇总当前对话中提到但还未最终拍板的问题，方便后续逐条清空。

- [x] **最小可运行 VM**：指令编码、调用帧、局部/全局变量、返回与校验
      规则见第 3、5 节。M1 仅加载内存模块；后续磁盘格式见
      `SPEC-bytecode-format.md`。
- [x] **值与操作语义**：string 存储与生命周期、NaN、除零、比较及 object
      相等性见第 1、5 节。
- [ ] **嵌入 API 的后续契约**：GC 堆、根句柄、统计、安全边界、第一版
      分配失败通道、宿主调用入口及 native 注册方式见第 2、7 节；调试钩子
      签名仍待完善；磁盘模块加载入口为实验版。
- [x] **快照最小语义设计**：保存/恢复返回值、续执行位置、深度错误、代码身份、
      宿主 codec 与失败语义已写入 `SPEC-snapshot.md`；实现仍属 M5 待办。
- [x] **实现顺序**：先做内存模块驱动的最小 VM，再接对象协议、GC 与嵌入、
      编译器、快照；目录边界和各阶段验收见 `ENGINEERING.md`。源码语法
      现见独立的 `SPEC-syntax.md`。
