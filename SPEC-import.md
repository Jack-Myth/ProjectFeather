# 宿主提供的 `import` 契约（第一版）

## 边界

`import` 是宿主在 VM 初始化之前注册的普通全局可调用值，不是关键字、指令或编译器指令。源码 `import("name")` 按普通名称读取与函数调用编译；`Compile(source)` 和 `.fbc` 不解析、记录或打包依赖。VM 只执行已提供的字节码，不负责文件系统、路径、网络、缓存、依赖图或其他 VM 的生命周期。脚本可以像普通全局名一样遮蔽或覆盖 `import`。`featherc` 可以编译这样的调用；当前 `feather` 和 `feathervm` 命令行宿主只解析 `std:console`、`std:io`，其他标识调用时明确报错。

第一版调用约定建议为 `import(specifier: string) -> Value`：恰好一个参数，且为 UTF-8 string。参数值是交给宿主的不透明标识；它可以是路径、包名或其他资源名，具体解析规则由宿主确定。参数数量或类型错误由注册的 native 函数返回 `Error`。宿主查找失败等可预期失败返回 `Error`；宿主/IO 致命故障沿已有 C++ 异常通道处理。`Error` 仍是普通值，不自动中止脚本；独立表达式语句可能丢弃它。

便捷注册适配器只允许返回基础值或宿主 `NativeObject`（包括新建的 `ErrorObject`），拒绝直接返回 ScriptObject 或 FunctionObject。宿主可创建另一台 VM 运行 Feather 模块，并用 `NativeObject` 代理其模块和函数：代理自行复制被允许的参数与结果，不能把子 VM 的脚本对象或函数 Value 交给调用方 VM。推荐的初版代理值边界是 `null`、bool、number、string；Error 应在接收方重新构造。对象代理协议、路径含义、IO、缓存、重复导入和循环依赖均由宿主定义。`NativeObject` 若保存当前 VM 的脚本对象，必须按既有 GC 可见成员或 `RootHandle` 契约维护引用。

宿主若启用快照，须为导入函数及返回的宿主对象提供适用的 `SnapshotHostCodec`。父 VM 的快照不自动保存子 VM 及代理的外部状态；宿主须另行实现一致的保存/恢复方案，或拒绝此类快照。

## 便捷注册接口

```cpp
using ImportCallback = std::function<Value(std::string_view Specifier)>;
void RegisterImport(Vm& Machine, ImportCallback Callback);
```

它只是 `NativeObject` + `Vm::RegisterNativeFunction("import", ...)` 的宿主侧适配器：校验一个 string 实参、转发回调并拒绝直接返回脚本对象/函数。它不读取文件、不编译依赖、不创建其他 VM，也不规定缓存策略。回调在同步 native 调用期间执行，遵守已有 GC、安全点和快照限制。若要让多个嵌入者使用不同解析方式，每台 VM 各自注册回调。回调可通过 C++ 捕获列表持有自己的模块管理器；须保证其生命周期覆盖回调可能被调用的时间。

宿主可以实现 Feather 模块导入，但 API 不规定统一的模块导出表或跨 VM 代理协议。`tests/Import.cpp` 展示了最小的子 VM、模块代理与函数代理；该例只复制 number 参数和结果。需要更丰富的值类型时，宿主必须明确定义复制或代理规则，并维护各 VM 独立的对象所有权。
