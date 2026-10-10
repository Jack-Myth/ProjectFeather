# import 宿主接口

`import` 是宿主注册的普通函数，VM 不自行读取文件或加载动态库。唯一适配器为：

```cpp
using ModuleImportCallback = std::function<Value(
    std::string_view CallerModuleId, std::string_view Specifier)>;
void RegisterModuleImport(Vm& Machine, std::string_view ModuleId,
                          ModuleImportCallback Callback);
```

空 ModuleId 指根模块。回调收到实际调用方模块 ID 和不透明的 specifier，负责解析、缓存、初始化及返回模块值。参数必须是单个字符串，否则返回 Error；空回调被拒绝。允许返回基础值、宿主对象及同一 VM 的脚本对象/函数；跨 VM 的脚本值仍由所有权校验拒绝。

需要隔离 VM 时，宿主可在此回调中返回 NativeObject 代理，自行定义跨 VM 参数与结果的复制规则。`tests/Import.cpp` 保留代理及跨 VM 拒绝测试。每个 VM 分别注册自己的回调；捕获对象必须覆盖回调的使用期。

适配器注册无 payload 的 NativeSingletonType。恢复快照前须在目标 VM 为相同模块 ID 重新注册回调；导入结果中的其他 Native 对象也须注册自己的类型。快照不会自动递归保存代理背后的另一个 VM 或外部状态。

同一 VM 的 export、模块私有全局和 ModuleNamespace 见 `SPEC-multimodule.md`。

## 独立运行程序的文件解析器

`feather run entry.fe`、`feather run entry.fbc` 与 `feathervm entry.fbc` 都从入口文件所在目录解析相对标识。相对文件标识必须以 `./` 或 `../` 开始；没有扩展名时，源码运行补 `.fe`，字节码运行补 `.fbc`。源码写明 `.fe` 时，字节码运行把该后缀换成 `.fbc`，因而编译后无需修改脚本里的标识；其他扩展名拒绝。每次相对导入相对于真正调用 `import` 的文件解析，而不是总相对于入口文件；入口文件不能作为额外模块导入自身。

解析器先把存在的文件规范化为绝对路径用于本次进程内定位，再以入口文件所在目录为内容根，生成 `file:<relative-path>` 形式、统一使用 `/` 分隔符的 UTF-8 VM 模块 ID。相对路径可以含 `..`，但跨卷或其他无法相对表示的位置会明确拒绝，不回退为绝对 ID。重复导入同一路径只调用幂等的模块初始化，不重跑顶层语句；初始化循环由 VM 拒绝。源码运行遇到 `.fe` 时，若同目录同名 `.fbc` 的修改时间不早于源码，则优先读取并验证该缓存；缓存缺失、较旧、不可读或无效时编译 `.fe`。字节码运行只读取、反序列化 `.fbc`；`featherc` 仍逐文件编译，不打包依赖。文件读取、路径选择和标准库对象均在 CLI 宿主层，嵌入式宿主可选择完全不同的解析规则。CLI 不自动读取依赖模块的 `.fbs`。

裸名从可执行文件旁的 `modules/` 搜索：先找平台 Native 动态库，再找 `.fe`（源码运行）或 `.fbc`（字节码运行）文件模块。`import("stdio")` 返回含 `Console`、`IO` 成员的 Native 对象；库命名、入口与同名优先级见 `SPEC-native-modules.md`。当前 CLI 默认只有这一条搜索目录，尚无命令行修改入口。
