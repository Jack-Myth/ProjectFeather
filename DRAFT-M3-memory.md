# M3 内存所有权与 GC 迁移初稿

状态：本文记录最初的显式 GC 迁移讨论，不再定义当前语义；后续加入的指令安全点自动收集、动态阈值和活跃帧扫描以 `SPEC-runtime-design.md` 为准。稳定嵌入 API 仍按进度文件跟踪。

## 为什么必须迁移

当前 `Value` 用 `shared_ptr<Object>` 持有所有 Object，`ScriptObject` 的成员表也持有 Value。`a.self = a` 会形成强引用环，即使 VM 和宿主都不再使用 `a`，引用计数也不会降为零。只在现有实现上扫描对象、但仍保留这些强引用，不能形成可靠的标记清除 GC。M3 应把 ScriptObject 的物理所有权移到 VM 的非移动堆，NativeObject 继续由明确的 C++ owner 持有。

## 建议的对象与值布局

1. VM 持有 `ScriptHeap`，其中每个 ScriptObject 用稳定地址的 `unique_ptr` 管理；仅 VM 可创建。RootMetaObject 是该堆的永久根。ScriptObject 的成员值和 MetaObject 引用都改为非拥有引用，堆对象只由 VM 销毁。
2. `Value` 的顶层 tag 仍是五类。object payload 细分为：ScriptObject 的非拥有堆引用，或 NativeObject 的明确拥有引用。这个内部子标记不暴露为第六种脚本类型。NativeObject 的物理 owner 类型应统一确定后再改头文件。
3. `Vm::CreateScriptObject()`、`Vm::CreateMetaObject()` 成为唯一路径；移除对宿主开放的 ScriptObject 公共构造函数。每个对象保存所属 VM 标识，跨 VM 使用在 API 边界拒绝，避免错用另一 VM 的根、代码或对象堆。
4. 当前 `Runtime.hpp` 中公开的 `shared_ptr<ScriptObject>` 和 M2 测试里的直接构造都要迁移；这是 API 形状变化，当前公开头仍属实验性接口。

## 根句柄与安全点

1. VM 内部根：RootMetaObject、全局表、运行/暂停帧的局部槽位与表达式栈、当前调用的参数和结果临时值。native 回调期间保持参数的临时根，回调返回后再释放。
2. 宿主通过 `AddToRoot(Value) -> RootHandle` 保持脚本堆对象。句柄内部登记到 VM 根表，复制句柄需要明确是共享一条根还是增加根计数；建议不可复制、可移动，析构自动调用 `RemoveFromRoot`。手动解除只允许一次。VM 销毁后句柄变为失效状态，读取报 API 错误。
3. 宿主收到裸 Value 后，如需跨越下一次 GC，必须先转成 RootHandle。API 文档应把裸 Value 的有效期写清；可以让未来稳定嵌入 API 直接返回持根句柄，降低误用风险。
4. 第一版 `CollectGarbage()` 仅允许 VM 空闲且无 native 回调时调用。调用中或回调中请求 GC 返回宿主 API 错误，不进入脚本 Error 体系。OOM 仍走 fatal 路径。

## 标记与清除

1. 从根集递归标记 ScriptObject，遍历其数据成员与 MetaObject 指针。遇到 NativeObject 时，遍历其已登记的 GC 可见成员表；不扫描任意 C++ 字段。
2. VM 持有仍存活 NativeObject 的弱登记表。NativeObject 登记和销毁时注销的协议必须避免 GC 遍历期间迭代器失效；初版 GC 只在空闲安全点运行。
3. 清除未标记 ScriptObject；地址不移动。GC 完成后，所有无 RootHandle 的宿主裸 ScriptObject Value 都可能失效，不能再访问。
4. GC 可见成员表由 VM 提供 setter 维护。宿主若把 ScriptObject 存到未登记的 C++ 字段，必须自行 RootHandle 保活，符合规范已有要求。

## M3 验收样例

- 脚本对象自环和双向环在失去根后能被回收；共享可达对象只标记一次。
- RootMetaObject 与从它可达的成员始终存活；删除/替换成员后，旧对象可被清除。
- 宿主 RootHandle 持有对象时不回收，释放后可回收；重复释放和 VM 销毁后使用得到明确 API 错误。
- NativeObject 的 GC 可见成员保活脚本对象，清空该成员后对象可回收。
- 在运行中、native 回调中请求 GC 被拒绝；回调参数和结果在允许的安全点前不会误收。
- 跨 VM 的 ScriptObject、Function 和 RootHandle 不会被当作本 VM 的对象或代码使用。

## 实施顺序

先在规范中冻结对象 payload、堆 owner、RootHandle 与错误通道；然后迁移 `Value`/ScriptObject 构造和 M2 测试；最后实现标记清除、NativeObject 登记及上述验收。迁移期间持续运行 M1/M2 测试，避免改变语言层语义。
