# Feather 单 VM 多文件模块（实验版）

一台 `Vm` 可装入多个宿主提供的 Feather 字节码模块。一个模块对应一个源码文件或一份 `.fbc`；文件读取、标识解析、依赖缓存和调用 `LoadInto` 的时机由宿主决定。`import(specifier)` 仍是普通全局函数调用，编译器和 VM 不解析路径、访问文件系统或生成依赖图。

## 模块身份与装载

主模块由 `Vm(primaryProgram)` 建立，调用 `Run(index)`、`GetGlobal` 和 `SetGlobal` 的既有入口，诊断中的模块 ID 为空。新增模块由宿主选择非空 UTF-8 逻辑 ID，调用 `CompiledProgram::LoadInto(vm, id)`。一个 ID 在一台 VM 中只有一个实例；完整 v3 `.fbc` 字节相同的编译模块再次装载复用该实例，即使宿主重新反序列化得到新的 C++ 对象也一样。手工调用 `Vm::LoadModule` 而未提供字节身份时，按源程序对象身份复用。尝试用不同内容占用同一 ID 则拒绝。同一源码在不同 ID 下可建立两个独立实例。

每个实例保存自己的只读代码副本、函数对象、全局表、导出名集合及初始化状态。`Const`、`GetGlobal` 和 `SetGlobal` 从当前函数所属实例读取；模块间脚本函数调用沿用同一 VM 的值栈、GC 与指令预算。`RunModule(id, index, args)` 是宿主调用新模块函数的入口。宿主可以用 `SetModuleGlobal` 注入该模块需要的 native 值；主模块与其他模块的全局名不会自动互通。

## `export` 与导入

源码 `export var name ...;` 和 `export def name(...) { ... }` 只允许在顶层使用。编译产物的导出名称表只列这些名字，按字节序保存于 v3 `.fbc`；无 `export` 的模块有空导出表。宿主可通过 `GetModuleNamespace(id)` 取得稳定的模块对象。读取 `namespace.name` 时，它只允许导出名，并从该实例的当前全局表取值；因此模块内或宿主改变已导出的 `var` 后，再次读取可见新值。模块对象只读，写入成员返回 Error；读取未导出名或未初始化模块也返回 Error。

`RegisterModuleImport(vm, moduleId, callback)` 给指定模块注册普通全局 `import`；空 `moduleId` 指主模块。回调接收实际调用方模块 ID 和不透明 specifier，由宿主决定装载、初始化及返回哪个 Value。它允许返回**同一 VM**的 ScriptObject/FunctionObject；现有跨 VM 直接传递限制继续生效。宿主通常先调用被导入模块的 `InitializeModule(vm, id)`，确认完成，再返回其模块对象。旧 `RegisterImport` 是更窄的跨 VM 代理适配器，仍不允许直接返回脚本对象或函数。

## 初始化与循环

新增模块有 Loaded、Initializing、Initialized、Failed 四种状态。`InitializeModule(vm, id)` 在 Loaded 状态先绑定所有顶层函数，再按源码顺序执行顶层语句；成功后成为 Initialized。重复调用 Initialized 模块不重跑顶层代码。初始化函数返回 Error 时成为 Failed，后续调用返回同一 Error；致命 C++ 异常后回到 Loaded，可由宿主决定是否重试。

如果初始化中的模块再次进入 `InitializeModule`，VM 抛 `circular module initialization` C++ 故障，拒绝该初始化循环。宿主不应在这种故障后把半初始化模块对象作为成功导入返回。第一版不承诺部分初始化导出可见；未来如需支持循环导入，需另定时序语义。

## GC、快照和诊断

GC 将所有模块实例的全局表、初始化 Error 与模块对象视为根；同一 VM 的 ScriptObject 和函数可跨模块引用，RootHandle 规则不变。v3 快照记录主模块及所有已装入模块的逻辑 ID、完整代码身份与导出表、每模块全局表和初始化状态、可达对象图、函数所属模块以及活动帧的模块/函数索引。恢复前宿主用相同代码和 ID 装入全部模块，但不执行初始化；恢复验证代码集合后提交状态并继续执行。处于 Initializing 状态的模块不允许保存。

运行时源码位置仍由源码内存表或显式 `.fbs` 提供；`GetErrorLocation` 和 `GetFaultLocation` 返回的 `SourceLocation::ModuleId` 指向真正产生错误的模块，避免把被导入模块的错误归到调用方。符号文件不编码绝对路径；宿主可把逻辑 ID 映射到显示名。
