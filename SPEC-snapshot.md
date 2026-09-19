# Feather 快照契约（实验版 v4）

本文件规定 M5 的保存和恢复行为。值、调用、对象和 GC 语义仍以 `SPEC-runtime-design.md` 为准。快照不是 C++ 对象内存转储，而是版本化字节序列；不能把指针或 `shared_ptr` 的地址写入其中。

## 保存点与返回值

宿主把一个 native 函数（例如 `checkpoint`）注册到需要保存的模块全局表。脚本正常调用它；这个 native 函数调用 `Vm::CaptureSnapshot()`，把得到的字节交给宿主持久化，然后返回 `false`。恢复后的 VM 从同一次脚本调用的返回处继续，但该调用的结果改为 `true`。因此脚本可以写：

```text
if (checkpoint()) {
    // 只在恢复后进入
}
```

保存入口是普通 native 调用，不新增字节码指令，也不把 native 帧放进快照。`CaptureSnapshot` 只允许在同一个 VM 恰有一个活动 `Run`、native 调用深度恰好为 1、当前 native 调用尚未返回时执行。此时 `Call` 的目标和参数已经从脚本表达式栈移除，PC 已指向下一条指令，返回值尚未压栈；快照标记顶层帧有一个待注入的调用结果。恢复时先压入 `true`，随后继续解释。正常保存路径由 native 入口返回 `false`；如宿主保存字节失败，不得假装保存成功。

`CaptureSnapshot` 在深度不符、没有活动脚本帧或 VM 状态不适合保存时抛 C++ `std::logic_error`，不产生半份快照。对脚本暴露的 native 包装器应把这种预期的拒绝转成 Error 值；宿主编码器抛出的异常仍走 C++ 故障通道。宿主直接调用恢复 API 时的错误也走 C++ 异常通道。

## 持久化边界与 Snapshot 模块

`Vm::CaptureSnapshot` 只生成快照字节，不接收文件路径，也不读写文件；这使嵌入宿主仍可把快照放入内存、数据库或其他介质。为了让独立解释器中的内容不需要重新构建宿主，标准发行可以另带一个可导入的 `snapshot` Native 模块。该模块负责脚本无法自行完成的 VM continuation 捕获、恢复控制转移及安全文件提交，而不实现存档槽位、标题、章节、缩略图、索引或用户档案等上层策略。

标准发行现提供 `import("snapshot")`，其脚本接口为 `snapshot.Checkpoint(path)` 与 `snapshot.Restore(path)`：

- `path` 是最终快照文件的 UTF-8 路径。首版可与 `stdio.IO` 保持一致：相对路径按进程当前工作目录解析，不设路径沙箱；不希望授予文件能力的宿主可以不安装本模块，或提供受限实现。
- slot 到路径的映射、目录布局、metadata 和存档索引均由 Feather 台本框架实现。Snapshot 模块不理解这些概念，也不把绝对路径写入 `FTHS` 数据。
- 正常调用只有在快照文件成功提交后才返回 `false`；从该快照恢复后同一调用返回 `true`。路径非法、编码失败或持久化失败必须作为可观察失败返回 Error 或走已约定的 C++ 故障通道，不得返回 `false` 假装保存成功。

脚本若要完整实现槽位管理，还需要列目录、判断存在、创建目录、删除和重命名等通用文件系统能力；当前 `stdio.IO` 只提供文件句柄读写。缺少的操作应补在通用 IO/文件系统 Native 模块中，而不是由 Snapshot 模块添加 `ListSlots`、`DeleteSlot` 等存档专用接口。

恢复后脚本回调不直接传给 Native 保存入口：台本框架可用 Feather 包装函数把回调保存在脚本帧局部槽位中，并根据 `Checkpoint` 的恢复返回值调用它，例如伪代码 `save(path, onRestore) { if (snapshot.Checkpoint(path)) { return onRestore(); } }`。包装器必须显式区分 Error 与 bool，因为 Error 在条件中为真。

模块写文件时应在目标文件的同一目录生成临时文件，完整写入并按所选耐久性策略刷新后，再原子替换目标；失败时保留上一份有效快照。这里的“提交成功”不等同于 `CaptureSnapshot` 已经返回字节。压缩、加密等字节转换若以后提供，也应是显式的低层能力或独立模块，不把 slot 策略重新塞回 Snapshot 模块。

`snapshot.Restore(path)` 成功时不从普通 native 调用返回。模块先要求 CLI 读取并校验外层存档容器；失败时在旧 VM 中返回 Error。容器有效后发出专用的非语言控制转移，CLI 退出当前执行会话，销毁旧 VM，按清单重新装入 Native 模块和 Feather 文件模块，再对新 VM 调用 `ResumeSnapshot`。因此恢复不会在旧 native 调用栈上原地进行。切换开始后若模块缺失、代码身份不符、Native 资源无法重建或恢复后的脚本失败，当前实现把它报告为 CLI 失败；旧脚本调用栈已经退出，不能再向它返回 Error。

CLI 写入的是外层 `FTHA` v1 存档：固定头和版本之后依次记录有序 Native 裸模块名 + descriptor GUID 列表、按逻辑 ID 排序的 Feather 文件模块列表、一个完整 `FTHS` v4 VM 快照以及覆盖整个前部内容的 CRC32。Native 裸名用于重新搜索模块，GUID 用于确认找到的是原模块，对象类型身份还会继续校验实际可达 NativeObject；Feather 文件模块使用相对入口文件目录的 `file:<relative-path>` ID 定位。清单只用于恢复前重建环境，VM 仍以 `FTHS` 内的模块 ID 和代码身份逐字节验证内容，不能靠修改清单绕过匹配。只要入口目录及其相对依赖布局一起移动，存档无需保存原机器的绝对工程路径；跨卷依赖当前不支持。存档上限当前为 64 MiB。写入采用目标同目录临时文件，刷新并原子替换目标；失败不会把半份内容当成成功存档。

## 恢复入口与原子性

`Vm::ResumeSnapshot(bytes)` 只在新构造且尚未运行、没有 RootHandle 的 VM 上调用。宿主先用与保存时完全相同的主模块构造 VM，再用相同的逻辑 ID、代码和导出表装入所有额外 Feather 模块，并重新导入快照可能引用的 Native 模块，使它们的 `NativeObjectType` 完成注册；此阶段不初始化 Feather 文件模块。恢复入口检查版本、模块集合、Native 类型及全部引用，构建对象图与跨模块脚本帧，最后继续解释并返回最外层脚本函数的最终 `Value`。恢复不重新执行 Feather 模块初始化函数，不重新调用已经完成的 native 函数。恢复后调用 `checkpoint()` 的脚本表达式得到 `true`。

恢复分为验证/构建和提交两阶段。输入损坏、版本不支持、模块不匹配、Native 类型未注册、Native 对象解码失败或资源限制超出时，VM 自身保持调用前状态，不运行脚本。`NativeObjectType::Deserialize` 的外部副作用不在 VM 的回滚保证内；类型实现必须自行清理失败时创建的宿主资源。恢复成功后，宿主获得新 VM 的所有权；旧 VM 可以独立存在。

## 状态范围

字节序列包含主模块和全部已装入模块的逻辑 ID、规范化代码及导出名集合，RootMetaObject，每模块的私有全局表与初始化状态，从它们和活动脚本帧可达的 ScriptObject/NativeObject 图，以及所有活动脚本帧。每帧记录模块序号、函数常量索引、下一条指令 PC、局部槽位数组和完整表达式栈；顶层帧另有待注入的调用结果标记。函数对象按模块序号与常量索引恢复，不另存 C++ 对象地址。模块对象是 VM 内部对象，按模块序号恢复，不交给 Native 类型。ErrorObject 按消息恢复，不保存 VM 为其记录的源码来源。字符串保存 UTF-8 字节，number 保存 IEEE 754 位模式。

ScriptObject 和需保留身份的 NativeObject 按遍历时分配的对象 ID 编码；重复引用写同一 ID。读取时先分配所有对象骨架，再填成员、MetaObject 和 native GC 可见成员，因而共享引用与环均保留。RootMetaObject 具有固定保留 ID，恢复后每个默认对象仍指向恢复后的同一个 RootMetaObject。只有可达图被保存；保存前不强制触发 GC，也不改变当前 VM 的对象身份或根表。

宿主 `RootHandle` 的 token、VM 的弱 native 登记表、当前 C++ 调用栈和外部宿主状态不写入快照。只被宿主根持有且不在上述 VM 可达图中的对象由宿主自行保存。恢复后没有旧 RootHandle；宿主如需跨 GC 保留恢复得到的脚本对象，应在新 VM 上重新创建句柄。恢复出的 Host NativeObject 由 VM 重新登记，以保证其 GC 可见成员被遍历。

指令预算属于宿主执行策略，不编码在快照中。恢复继续执行时使用新 VM 配置的完整预算；快照不能保证跨保存/恢复的累计指令数量。

## NativeObjectType

FunctionObject、ErrorObject 和 ModuleNamespace 由 VM 自身编码。其他需要进入快照的 NativeObject（包括宿主提供的 `checkpoint` 函数）必须绑定到一个由 VM 拥有的 `NativeObjectType`。类型在构造时取得 VM，并通过 `Vm::CreateNativeObjectType` 注册；其稳定身份是 `NativeModuleGuid` 的 16 字节值与模块内唯一的 UTF-8 类型名组成的二元组。模块 GUID 全零、空类型名和同一 VM 内重复的二元组均被拒绝。首版不提供别名、重命名表或自动迁移。

对象应由类型的 `CreateObject` 路径创建和登记，销毁也回到该类型的 `Destroy` 虚函数。VM 以共享所有权持有类型，`CreateObject` 的删除器也保留类型，因此类型至少活到最后一个所属对象销毁；对象的其他行为仍不得在 VM 销毁后访问原 VM。为了保留旧的非快照宿主用法，未绑定类型的 NativeObject 仍可调用，但一旦出现在可达快照图中，保存会明确失败。

保存时 VM 写入模块 GUID、类型名、该类型声明的非零 schema 版本及 `Serialize` 返回的不透明字节。恢复前宿主重新导入 Native 模块，使相同身份的类型注册到**新 VM**；VM 精确查找该类型并调用 `Deserialize(storedVersion, payload)`。类型实现自行决定支持哪些旧 schema 版本，首版框架不提供迁移注册表。VM 随后恢复 GC 可见成员，再调用 `FinalizeRestore`，使对象可以在完整对象图建立后重建派生缓存或外部绑定。`Deserialize` 不应修改新 VM 的全局表、脚本堆或 RootHandle；未知类型、无效返回对象、重复复用同一对象身份或异常都会使整个恢复失败。文件句柄、渲染资源等外部状态能否重建及怎样失败，由对应类型定义。

`NativeObjectType::Serialize` 的默认实现验证对象属于本类型并返回空 payload，只有确实存在实例状态的类型才需要覆盖它。对于模块重新导入时会重建的命名空间、静态函数等单一规范对象，可直接使用 `NativeSingletonType`：模块初始化时用它创建并绑定对象，恢复时由它返回新 VM 中的规范实例，既不用实现 `Serialize`，也不用实现 `Deserialize`。一般的非单例类型仍须明确实现 `Deserialize`；框架不会猜测构造函数。

## 格式和兼容性

当前 v4 格式含固定 magic、主/次版本、各段长度和计数，整数使用定宽小端编码；末尾附加 u32 CRC32，覆盖此前的全部字节。读取时先核对版本和 CRC32，再解析对象图。CRC32 用于发现意外损坏，不是防伪认证；读取器仍核对段边界、计数、Value tag、对象引用、模块/函数索引和 PC，以避免越界与资源失控。模块集合标识逐字节保存并比较逻辑 ID、全部常量、默认值、函数代码及导出名，避免仅凭路径、指针或短哈希误认代码。快速开发期不读取旧 v1/v2/v3 快照；快照仍与精确模块内容及 Native 类型实现绑定。

文件头为 `FTHS`、`major: u16 = 4`、`minor: u16 = 0`，随后依次是四个 `u32 length + bytes` 段：模块集合、对象图、模块全局表、活动帧，末尾为 CRC32。模块集合首项是主模块（空 ID），其余按逻辑 ID 字节序排列；每项写 ID、代码身份字节及升序导出名。经 `CompiledProgram::LoadInto` 装入的模块使用完整 v3 `.fbc` 字节作为代码身份；手工装入而未提供身份字节的模块使用规范化代码字节。对象记录 tag 0 是 ScriptObject、1 是 FunctionObject（模块序号和常量索引）、2 是 ErrorObject、3 是带 16 字节模块 GUID、类型名、schema 版本和 payload 的 NativeObject、4 是 VM ModuleNamespace（模块序号）。全局段先写模块数，逐模块写名称和值；额外模块还写初始化状态及失败时的 Error。帧段逐帧写模块序号、函数常量索引、PC、局部与表达式栈。

每次恢复都对全部模块调用 `Module::Validate()`；输入字节有独立上限，默认 64 MiB，宿主可在 API 中下调。编码器在写入前检查大小上限；读取时先用对象数据段的最小记录长度核对对象数量，再分配对象记录表；解析途中即检查 ScriptObject 数量限额，并限制帧数、栈/局部长度及字符串长度。常规验收以保存/恢复往返、校验和损坏检测、基本安全边界与 Native 类型失败回滚为主，不持续扩展细碎的畸形输入组合。版本或模块不符抛可辨认的 C++ 异常，不作为脚本 Error 值继续执行。

## M5 验收

1. `checkpoint()` 正常执行返回 `false`；恢复后同一表达式得到 `true`，并从正确 PC 继续，最终返回值与预期一致。
2. 嵌套脚本调用、循环中保存、局部与表达式栈中保存对象；恢复后继续执行，无重复执行此前指令。
3. 共享对象、环、MetaObject 与 RootMetaObject 身份保持；不可达对象不进入快照。
4. NativeObject 通过注册类型重建，重复引用保持同一实例；缺失类型或序列化/反序列化失败不改变目标 VM。
5. native 深度大于 1、错误版本、不同模块、截断与伪造索引均被拒绝；恢复失败不运行脚本。
