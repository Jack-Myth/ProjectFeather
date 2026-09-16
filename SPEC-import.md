# 宿主提供的 `import` 契约（第一版）

## 边界

`import` 是宿主在 VM 初始化之前注册的普通全局可调用值，不是关键字、指令或编译器指令。源码 `import("name")` 按普通名称读取与函数调用编译；`Compile(source)` 和 `.fbc` 不解析、记录或打包依赖。VM 只执行已提供的字节码，不负责文件系统、路径、网络、缓存、依赖图或其他 VM 的生命周期。脚本可以像普通全局名一样遮蔽或覆盖 `import`。`featherc` 可以编译这样的调用；独立运行程序的命令行宿主实现了下文的相对文件解析器。

第一版调用约定建议为 `import(specifier: string) -> Value`：恰好一个参数，且为 UTF-8 string。参数值是交给宿主的不透明标识；它可以是路径、包名或其他资源名，具体解析规则由宿主确定。参数数量或类型错误由注册的 native 函数返回 `Error`。宿主查找失败等可预期失败返回 `Error`；宿主/IO 致命故障沿已有 C++ 异常通道处理。`Error` 仍是普通值，不自动中止脚本；独立表达式语句可能丢弃它。

便捷注册适配器只允许返回基础值或宿主 `NativeObject`（包括新建的 `ErrorObject`），拒绝直接返回 ScriptObject 或 FunctionObject。宿主可创建另一台 VM 运行 Feather 模块，并用 `NativeObject` 代理其模块和函数：代理自行复制被允许的参数与结果，不能把子 VM 的脚本对象或函数 Value 交给调用方 VM。推荐的初版代理值边界是 `null`、bool、number、string；Error 应在接收方重新构造。对象代理协议、路径含义、IO、缓存、重复导入和循环依赖均由宿主定义。`NativeObject` 若保存当前 VM 的脚本对象，必须按既有 GC 可见成员或 `RootHandle` 契约维护引用。

宿主若启用快照，须为导入函数及返回的宿主对象提供适用的 `SnapshotHostCodec`。父 VM 的快照不自动保存子 VM 及代理的外部状态；宿主须另行实现一致的保存/恢复方案，或拒绝此类快照。

## 便捷注册接口

```cpp
using ImportCallback = std::function<Value(std::string_view Specifier)>;
void RegisterImport(Vm& Machine, ImportCallback Callback);
```

它只是 `NativeObject` + `Vm::RegisterNativeFunction("import", ...)` 的宿主侧适配器：校验一个 string 实参、转发回调并拒绝直接返回脚本对象/函数。它不读取文件、不编译依赖、不创建其他 VM，也不规定缓存策略。回调在同步 native 调用期间执行，遵守已有 GC、安全点和快照限制。若要让多个嵌入者使用不同解析方式，每台 VM 各自注册回调。回调可通过 C++ 捕获列表持有自己的模块管理器；须保证其生命周期覆盖回调可能被调用的时间。

旧适配器仍可用于子 VM 代理；`tests/Import.cpp` 展示最小的子 VM、模块代理与函数代理，只复制 number 参数和结果。需要更丰富的跨 VM 值类型时，宿主仍必须定义复制或代理规则，并维护各 VM 独立的对象所有权。

## 单 VM 多文件模块扩展

新增 `export var/def`、私有模块全局表和 ModuleNamespace 的规范见 `SPEC-multimodule.md`。`RegisterModuleImport(vm, moduleId, callback)` 在指定模块中注册同名的普通 `import` 函数；空 moduleId 指主模块。回调类型为 `Value(callerModuleId, specifier)`，由宿主根据调用方身份解析来源、装入及初始化 Feather 模块，再返回 VM 内部的模块对象或其他 Value。这个适配器允许返回同一 VM 的 ScriptObject/FunctionObject；`Vm::ValidateOwnedValue` 仍在结果进入栈时拒绝跨 VM 脚本值。旧 `RegisterImport` 保留原有“只返回基础值或宿主代理”的窄边界。

## 独立运行程序的文件解析器

`feather run entry.fe`、`feather run entry.fbc` 与 `feathervm entry.fbc` 都从入口文件所在目录解析相对标识。相对文件标识必须以 `./` 或 `../` 开始；没有扩展名时，源码运行补 `.fe`，字节码运行补 `.fbc`。源码写明 `.fe` 时，字节码运行把该后缀换成 `.fbc`，因而编译后无需修改脚本里的标识；其他扩展名拒绝。每次相对导入相对于真正调用 `import` 的文件解析，而不是总相对于入口文件；入口文件不能作为额外模块导入自身。

解析器把存在的文件规范化为绝对路径，以其 UTF-8 文本作为 VM 模块 ID，并在这次运行中缓存编译/读取的产物。重复导入同一路径只调用幂等的模块初始化，不重跑顶层语句；初始化循环由 VM 拒绝。源码运行遇到 `.fe` 时，若同目录同名 `.fbc` 的修改时间不早于源码，则优先读取并验证该缓存；缓存缺失、较旧、不可读或无效时编译 `.fe`。字节码运行只读取、反序列化 `.fbc`；`featherc` 仍逐文件编译，不打包依赖。文件读取、路径选择和标准库对象均在 CLI 宿主层，嵌入式宿主可选择完全不同的解析规则。CLI 不自动读取依赖模块的 `.fbs`。

裸名从可执行文件旁的 `modules/` 搜索：先找平台 Native 动态库，再找 `.fe`（源码运行）或 `.fbc`（字节码运行）文件模块。`import("stdio")` 返回含 `Console`、`IO` 成员的 Native 对象；库命名、入口与同名优先级见 `SPEC-native-modules.md`。当前 CLI 默认只有这一条搜索目录，尚无命令行修改入口。
