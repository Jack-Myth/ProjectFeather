# 模块与标准库

[导入与模块搜索路径](#导入) · [delegate](#delegate) · [文件模块](#文件模块) · [Native 动态库](#native-动态库) · [stdio](#stdio) · [time](#time) · [text](#text) · [network](#network) · [audio](#audio) · [render2d](#render2d)

## 导入

`import` 是宿主注册的普通函数，VM 不自行读取文件或加载动态库。唯一适配器为：

```cpp
using ModuleImportCallback = std::function<Value(
    std::string_view CallerModuleId, std::string_view Specifier)>;
void RegisterModuleImport(Vm& Machine, std::string_view ModuleId,
                          ModuleImportCallback Callback);
```

空 ModuleId 指根模块。回调收到实际调用方模块 ID 和不透明的 specifier，负责解析、缓存、初始化及返回模块值。参数必须是单个字符串，否则返回 Error；空回调被拒绝。允许返回基础值、宿主对象及同一 VM 的脚本对象/函数；跨 VM 的脚本值仍由所有权校验拒绝。

需要隔离 VM 时，宿主可在此回调中返回 NativeObject 代理，自行定义跨 VM 参数与结果的复制规则。`tests/Import.cpp` 保留代理及跨 VM 拒绝测试。每个 VM 分别注册自己的回调；捕获对象必须覆盖回调的使用期。

适配器使用 NativeSingletonType 注册模块导入函数，其生命周期由 VM 管理。

### 独立运行程序的文件解析器

`feather run entry.fe`、`feather run entry.fbc` 与 `feathervm entry.fbc` 都从入口文件所在目录解析相对标识。相对文件标识必须以 `./` 或 `../` 开始；没有扩展名时，源码运行补 `.fe`，字节码运行补 `.fbc`。源码写明 `.fe` 时，字节码运行把该后缀换成 `.fbc`，因而编译后无需修改脚本里的标识；其他扩展名拒绝。每次相对导入相对于真正调用 `import` 的文件解析，而不是总相对于入口文件；入口文件不能作为额外模块导入自身。

解析器先把存在的文件规范化为绝对路径用于本次进程内定位，再以入口文件所在目录为内容根，生成 `file:<relative-path>` 形式、统一使用 `/` 分隔符的 UTF-8 VM 模块 ID。相对路径可以含 `..`，但跨卷或其他无法相对表示的位置会明确拒绝，不回退为绝对 ID。重复导入同一路径只调用幂等的模块初始化，不重跑顶层语句；初始化循环由 VM 拒绝。源码运行遇到 `.fe` 时，若同目录同名 `.fbc` 的修改时间不早于源码，则优先读取并验证该缓存；缓存缺失、较旧、不可读或无效时编译 `.fe`。字节码运行只读取、反序列化 `.fbc`；`featherc` 仍逐文件编译，不打包依赖。文件读取、路径选择和标准库对象均在 CLI 宿主层，嵌入式宿主可选择完全不同的解析规则。CLI 不自动读取依赖模块的 `.fbs`。

`RegisterModuleImport` 将裸名 `vm` 解析为当前 VM 的内建模块，提供 `eval_bytecode` 和模块搜索路径接口。其他裸名从可执行文件旁的 `modules/` 搜索：先找平台 Native 动态库，再找 `.fe`（源码运行）或 `.fbc`（字节码运行）文件模块。`import("stdio")` 返回含 `Console`、`IO` 成员的 Native 对象；库命名与入口见下文。当前 CLI 默认只有这一条搜索目录，可通过下述接口替换。

### 模块搜索路径

路径列表使用 `array()` 创建的数组，每项为非空路径字符串。

```feather
var vm = import("vm");
var paths = vm.get_module_paths();
paths.add("./game_modules");
vm.set_module_paths(paths);
```

`vm.get_module_paths()` 返回当前 VM 路径列表的副本。`vm.set_module_paths(paths)` 验证完整列表后替换搜索路径，成功返回 `true`，无效参数返回 Error。`vm.set_module_paths(array())` 可清空搜索路径。相对目录从进程当前工作目录解释。

CLI 按列表顺序搜索裸名 Native 模块，再按同样顺序搜索 Feather 文件模块。相对文件导入继续按调用方文件所在目录解析。已导入的 Native 模块仍按裸名缓存。嵌入宿主可通过 `Vm::GetModulePaths()` 获取同一列表并用于自己的导入回调。

### 当前函数的实参

`vm.get_func_argc()` 返回当前 Feather 调用帧的实参数量。用 `vm.get_func_argv(index)` 按零基索引读取调用时的值。多传的实参和显式传入的 `null` 都计入。默认值只填入命名形参。重新给形参赋值时，原实参仍可通过 `get_func_argv` 读取。

索引必须是范围内的整数，接受 number 和 byte。参数错误、索引越界或宿主在没有活动脚本帧时调用，均返回 Error。嵌套函数读取自己的实参。通过 `__call` 等元方法调用时，VM 注入的接收对象也是实参。执行 `@{ ... }` 块时，它有独立的调用帧，唯一实参是 `context`，省略时为 `null`。

### 用数组传递实参

`vm.invoke_function(fn, args)` 读取 `array()` 的元素，按索引顺序作为实参调用 `fn`，并返回调用结果。空数组对应零个实参。它接受 Feather 函数、Native 函数及具有 `__call` 的对象。调用沿用普通的默认形参、实参读取和 Error 规则。目标不可调用或第二个参数不是数组时，返回 Error。

```feather
var vm = import("vm");

def forward(fn) {
    var args = array();
    var index = 1;
    while (index < vm.get_func_argc()) {
        args.Add(vm.get_func_argv(index));
        index = index + 1;
    }
    return vm.invoke_function(fn, args);
}
```

`vm.set_metaobject(object, meta)` 为普通脚本对象设置元对象并返回该对象。元对象可定义 `__call`，使对象可调用。

## delegate

`delegate.Create(fn, self, context = null)` 返回可调用对象。每次调用先传入绑定的 `self`，随后传入非空的 `context`（若有），最后按顺序追加调用时提供的实参，并返回 `fn` 的结果。

```feather
var delegate = import("delegate");
def report(self, context, sound) {
    return self.Name + context;
}
var owner = object();
owner.Name = "player";
var listener = delegate.Create(report, owner, ": finished");
```

## 文件模块

一台 `Vm` 可装入多个宿主提供的 Feather 字节码模块。一个模块对应一个源码文件或一份 `.fbc`；文件读取、标识解析、依赖缓存和调用 `LoadInto` 的时机由宿主决定。`import(specifier)` 仍是普通全局函数调用，编译器和 VM 不解析路径、访问文件系统或生成依赖图。

### 模块身份与装载

主模块由 `Vm(primaryProgram)` 建立，调用 `Run(index)`、`GetGlobal` 和 `SetGlobal` 的既有入口，诊断中的模块 ID 为空。新增模块由宿主选择非空 UTF-8 逻辑 ID，调用 `CompiledProgram::LoadInto(vm, id)`。一个 ID 在一台 VM 中只有一个实例；完整 v5 `.fbc` 字节相同的编译模块再次装载复用该实例，即使宿主重新反序列化得到新的 C++ 对象也一样。手工调用 `Vm::LoadModule` 而未提供字节身份时，按源程序对象身份复用。尝试用不同内容占用同一 ID 则拒绝。同一源码在不同 ID 下可建立两个独立实例。

每个实例保存自己的只读代码副本、函数对象、全局表、导出名集合及初始化状态。`Const`、`GetGlobal` 和 `SetGlobal` 从当前函数所属实例读取；模块间脚本函数调用沿用同一 VM 的值栈、GC 与指令预算。`RunModule(id, index, args)` 是宿主调用新模块函数的入口。宿主可以用 `SetModuleGlobal` 注入该模块需要的 native 值；主模块与其他模块的全局名不会自动互通。

### `export` 与导入

源码 `export var name ...;` 和 `export def name(...) { ... }` 只允许在顶层使用。编译产物的导出名称表只列这些名字，按字节序保存于 v5 `.fbc`；无 `export` 的模块有空导出表。宿主可通过 `GetModuleNamespace(id)` 取得稳定的模块对象。读取 `namespace.name` 时，它只允许导出名，并从该实例的当前全局表取值；因此模块内或宿主改变已导出的 `var` 后，再次读取可见新值。模块对象只读，写入成员返回 Error；读取未导出名或未初始化模块也返回 Error。

宿主应在装载后调用 `InitializeModule(vm, id)`，成功后再把模块对象交给导入方。

### 初始化与循环

新增模块有 Loaded、Initializing、Initialized、Failed 四种状态。`InitializeModule(vm, id)` 在 Loaded 状态先绑定所有顶层函数，再按源码顺序执行顶层语句；成功后成为 Initialized。重复调用 Initialized 模块不重跑顶层代码。初始化函数返回 Error 时成为 Failed，后续调用返回同一 Error；致命 C++ 异常后回到 Loaded，可由宿主决定是否重试。

如果初始化中的模块再次进入 `InitializeModule`，VM 抛 `circular module initialization` C++ 故障，拒绝该初始化循环。宿主不应在这种故障后把半初始化模块对象作为成功导入返回。部分初始化的导出不可读。

## Native 动态库

裸名仅允许 ASCII 字母、数字与下划线，首字符不能是数字。Native 文件使用 `<name>.felib.dll`（Windows）、`<name>.felib.so`（Linux）或 `<name>.felib.dylib`（macOS）。裸名先遍历全部模块目录查找 Native 候选，再按相同顺序查找脚本候选。找到首个候选后，装载失败会返回该候选的错误。

### 缓存与生命周期

宿主用规范化的绝对路径作为 Native 库身份，同一 VM 中同一文件仅打开一次、创建一份模块对象，重复 `import` 返回同一个对象。Native 标识也缓存解析结果，使反复查找不改变其身份；失败不缓存为成功实例。文件模块继续按其现有 ID 和初始化状态缓存。动态库句柄要活得比它创建的所有 `NativeObject`、可调用方法和文件句柄更久；CLI 在 VM 销毁之后才释放库句柄。宿主不可在仍可调用这些对象时卸载库。模块对象的析构函数不得调用已进入销毁流程的 VM。

Native 根对象建议实现 `GetObjectType() == ObjectType::Host`、只读 `GetMember`，把公开方法或子对象作为成员返回；方法对象实现 `IsCallable()` 和 `Call(arguments)`。不存在的成员、错误实参和可预期的 IO 失败返回 `ErrorObject`。Native 对象若持有当前 VM 的脚本 Value，使用 `SetGcVisibleMember` 或 `RootHandle` 保持 GC 可见；不能直接返回另一台 VM 的 ScriptObject/FunctionObject。对象类型与生命周期通过 NativeObjectType 管理。

### 动态库入口

文件名只解决定位，还需要统一的入口。每个库导出固定符号 `FeatherNativeModule`，其开发期接口见 `include/Feather/NativeModule.hpp`：

```cpp
struct NativeModuleContext {
    Vm& Machine;
    std::istream* Input = nullptr;
    std::ostream* Output = nullptr;
};
struct NativeModuleDescriptor {
    NativeModuleGuid Id;            // 模块稳定 GUID，不得全零
    const char* Name;               // 例如 "stdio"
    Value (*Create)(const NativeModuleContext&);
};
extern "C" const NativeModuleDescriptor* FeatherNativeModule();
```

宿主打开文件后只查找该符号，核对非零 GUID、声明名与请求的裸名相同，再调用 `Create`；同一宿主已装入的另一个模块若使用相同 GUID，也会被拒绝。GUID 是模块作者维护的稳定身份，同一模块的各个 `NativeObjectType` 都使用它，类型身份由 GUID 与类型名共同组成；模块改名或文件换路径不应顺便更换 GUID。`stdio` 从上下文取得输入/输出流；缺失服务时返回明确错误。创建失败或描述结构不符时关闭尚未交付对象的库句柄并报告错误；脚本看不到半创建模块。额外的宿主服务以后扩展上下文版本，模块不得自行假定所有宿主都允许文件 IO。

Native 模块和宿主通过 Meson 的静态 runtime 构建，需使用同一工具链、C++ 运行时和 Feather 头文件。`extern "C"` 只固定导出符号名。入口样例见 [StdIoModule.cpp](stdlib/StdIoModule.cpp)，模块构建定义见 [modules/meson.build](modules/meson.build)。

Windows 构建与发行包把 SDL3 动态库放在 `modules/`，供同目录的 `audio.felib.dll` 和 `render2d.felib.dll` 共用。

### 与 Native 交换数组

脚本 `array()` 与宿主 `Vm::CreateArray()` 创建同一种由 VM 管理的数组。Native 代码可以用 `Value::FromScript(Array)` 返回数组，也可以检查传入值的 `IsScriptObject()` 和 `AsScriptObject()->IsArray()` 后直接操作。双方共享同一个数组。

| C++ 接口 | 用途 |
|---|---|
| `AppendArray(value)` | 追加一个值。 |
| `RemoveArrayAt(index)` | 按零基索引删除，越界返回 `false`。 |
| `GetArrayLength()` | 读取当前元素数量。 |
| `GetArrayElements()` | 获取只读元素视图。数组修改后应重新获取视图。 |

宿主跨 GC 持有数组时使用 `RootHandle`。数组和其中的脚本对象遵守原有的 VM 所有权约束。

## stdio

`stdio` 是独立的 Native 模块。VM 不自行打开文件，也不创建标准输入输出对象。独立解释器从可执行文件旁的 `modules/` 装载 `stdio.felib.dll`（Linux 为 `.so`），脚本通过 `import("stdio")` 取得根对象。嵌入式宿主可构造 `StdIoLibrary(vm, input, output)`，调用 `GetModule()` 并把返回值接入自己的 `import` 回调；根对象、Console/IO、文件句柄都是 `NativeObject`。传入的输入输出流须比这些对象及使用它们的 VM 活得更久。

独立运行程序 `feather` 与 `feathervm` 给模块创建入口提供进程的 `std::cin` 和 `std::cout`。`featherc` 只编译源码，不装载本模块。`GetModule()` 返回一个只读根对象，其 `Console` 与 `IO` 成员分别是控制台和文件对象。本模块不会覆盖已有的 `import`，也不会注入 `Console` 或 `IO` 全局名。

脚本应在顶层写 `var stdio = import("stdio");`。顶层 `var` 存入当前文件模块的私有全局表，函数内可按该模块的全局名访问 `stdio`；这里不依赖闭包。函数外导入必须在调用函数之前执行，独立运行程序的初始化流程满足这一顺序。

### Console

| 调用 | 返回及行为 |
|---|---|
| `stdio.Console.Print(value)` | 写入一个基础值并刷新输出流；成功返回 `null`。 |
| `stdio.Console.PrintLine(value)` | 同上，随后写入 `\n`；成功返回 `null`。 |
| `stdio.Console.InputLine()` | 读取一行，去掉行尾换行；EOF 返回 `null`，正常行返回 UTF-8 `string`。 |

输出支持 `null`、bool、number、byte 和 string。对象、错误参数个数、输出失败或无效 UTF-8 输入返回 Error 值。number 使用经典 locale 的十进制表示；此显示格式只用于人类阅读，不承诺与 Feather 字面量或文件格式相同。`InputLine` 不打印提示；可先调用 `Print`。

### IO

所有文件以二进制模式打开；路径为 UTF-8 字符串，按进程当前工作目录解析相对路径。首版不设路径沙箱，宿主可选择不安装 `IO`。每个文件句柄属于创建它的 `StdIoLibrary` 实例，不能传给另一个库实例的 `IO` 方法。句柄析构时关闭底层文件；显式关闭后再次使用返回 Error。IO 故障、错误参数或模式不允许的操作均返回 Error 值。

| 调用 | 返回及行为 |
|---|---|
| `stdio.IO.OpenFile(path, mode)` | 返回文件句柄；模式为 `r`、`w`、`a`、`r+`、`w+`、`a+`。`w` 截断，`a` 追加，`+` 同时允许读写。 |
| `stdio.IO.CloseFile(handle)` | 关闭成功返回 `true`。 |
| `stdio.IO.Seek(handle, offset, origin)` | `origin` 为 `start`、`current`、`end`；返回移动后的字节位置。 |
| `stdio.IO.Tell(handle)` | 返回当前字节位置。 |
| `stdio.IO.Read(handle, count)` | 最多读取 `count` 字节，返回 `ByteBuffer`；EOF 返回空缓冲区。单次 `count` 不超过 16 MiB。 |
| `stdio.IO.Write(handle, data)` | `data` 是 byte（一个原始字节）、string（UTF-8 字节）或 `ByteBuffer`；完整写入时返回写入字节数。 |

`offset`、`count` 和位置是整数，精确表示范围为双精度数值的 ±(2^53−1)；`count` 非负。读写共享一个文件位置；同一个 `+` 句柄在读写方向之间切换时，先调用 `Seek`。`Read` 不将任意二进制数据误作 UTF-8 字符串，也不会在 EOF 自动关闭句柄。

`ByteBuffer.Length` 是字节数；`ByteBuffer.Get(index)` 返回 0–255 的 byte 类型值；`ByteBuffer.ToString()` 在全部字节合法 UTF-8 时返回 string，否则返回 Error。缓冲区不可从脚本修改。需要把任意文件内容交给宿主时，直接传递这个宿主对象；跨 VM 使用仍由宿主代理边界决定。

用 `StdIoLibrary(Vm&, input, output)` 构造时，模块、Console/IO 命名空间、方法及资源对象均绑定到当前 VM；类型注册管理对象身份与生命周期。ByteBuffer 是不可变二进制数据，文件句柄应显式关闭。

byte 控制台输出为十进制 0～255，不作为字符打印。IO 的整数参数接受 byte 或符合原范围要求的 number；`IO.Write(handle, byte(255))` 恰写一个 0xFF 字节。ByteBuffer 仍不可变，`Get` 同时支持 byte/number 索引。

## time

`time` 是独立 Native 模块。VM 正常执行不主动等待；显式 idle 支持协作式等待；脚本通过
`var time = import("time");` 显式取得模块。模块使用固定 GUID，Native 类型名在该
GUID 下唯一。

所有时长和时间点均使用 `number` 表示秒，可包含小数。要求时长和 deadline 为有限
非负数；参数数量或类型错误返回 Error。

| 调用 | 返回及行为 |
|---|---|
| `time.Monotonic()` | 返回单调时钟时间点。只允许在当前运行会话中求差或作为 `SleepUntil` 的 deadline。 |
| `time.UnixTime()` | 返回 Unix epoch 起经过的秒数。系统校时可能使其跳变，不应用于帧 delta。 |
| `time.Sleep(seconds)` | 在当前 VM 线程进入协作式 idle；成功返回 `null`。`0` 是有效时长。 |
| `time.SleepUntil(deadline)` | 阻塞到单调时钟到达 deadline；deadline 已过则给一次零时间 idle 机会。 |
| `time.CreateClock()` | 创建一个 `Clock` NativeObject，并以当前单调时间为基准。 |
| `time.Tick(clock)` | 返回距该 Clock 上次创建、重置或 Tick 经过的秒数，并更新基准。 |
| `time.Reset(clock)` | 把 Clock 基准重置到当前单调时间，返回 `null`。 |

`Sleep` 和 `SleepUntil` 会派发已注册的 VM idle 服务（如 network 接收回调），不创建线程，也不自动处理窗口事件。需要在等待期间响应窗口
事件时应使用 `render2d.WaitEvent`；帧率、固定时间步、最大 delta、FPS 统计和任务调度
均由脚本框架实现。

## text

`import("text")` 提供按 Unicode 码点处理 UTF-8 字符串的接口。

- `Length(value)` 返回码点数。
- `Slice(value, start, count)` 按零基码点索引截取。起点和长度须为非负整数，越界返回 Error。
- `Before(value, delimiter)` 和 `After(value, delimiter)` 取第一次匹配的非空分隔符之前或之后的文本。无匹配时分别返回原字符串和空字符串。

## network

`import("network")` 获取可选 Native 模块。Windows 使用 Winsock 2.2，Linux/macOS 使用 POSIX socket，共享脚本 API。所有 socket 始终非阻塞，不启动后台线程。模块仅提供 TCP/UDP；不提供消息分帧、HTTP、TLS 或可靠 UDP。

### 接口

成功的配置操作返回 null，错误返回普通 Error。所有整数参数都接受符合范围的 number/byte。

| 调用 | 行为 |
|---|---|
| CreateTcp(family = "ipv4") / CreateUdp(family = "ipv4") | 创建 socket；family 为 ipv4 或 ipv6，IPv6 为 v6-only。 |
| Bind(socket, address, port) | 绑定数值 IP；port 为 0～65535，0 由系统分配。 |
| Listen(socket, backlog = 16) | 已绑定 TCP 开始监听，backlog 为 1～1024。 |
| Accept(listener) | 返回新的已连接 TCP socket；无连接返回 null。 |
| Connect(socket, address, port) | 启动 TCP 连接；true 完成、false 仍在连接，失败返回 Error 并关闭 socket。 |
| FinishConnect(socket) | 非阻塞查询连接结果；true 完成、false 仍待定、Error 失败并关闭。 |
| Poll(socket, read, write, timeoutMs = 0) | 至少选择一个方向，等待 0～60000 整数毫秒；返回 Readable/Writable/TimedOut。就绪也可能意味着 EOF/错误，需执行操作确认。不会派发接收回调。 |
| Send(socket, data, offset = 0, count = 剩余长度) | TCP 发送，返回实际发送字节数；null 表示 would-block。必须按返回数量推进偏移，不能假定一次发完。空数据返回 0。 |
| Receive(socket, maxBytes = 65536) | TCP 读取最多指定正数字节，返回 ByteBuffer；null 为 would-block，false 为 EOF。TCP 是字节流，返回值不代表完整应用消息。 |
| SendTo(socket, data, address, port) | UDP 发送一个数据报；返回字节数或 null（would-block），空数据报成功返回 0。 |
| ReceiveFrom(socket, maxBytes = 65507) | 返回 {Data: ByteBuffer, Address: 数值IP, Port: 来源端口}；null 表示 would-block。maxBytes 可以为 0，以接收空数据报。 |
| OnReceive(socket, callback) | 注册接收回调，callback(event)；null 注销。TCP 必须已连接且未读到 EOF，UDP 必须先 Bind。 |
| ShutdownWrite(socket) | TCP 半关闭发送方向，保留接收，重复调用安全。 |
| Close(socket) | 关闭并注销接收回调，重复调用安全。 |
| LocalAddress(socket) / PeerAddress(socket) | 返回 {Address, Port}；PeerAddress 仅限已连接 TCP。 |
| Resolve(host, family = "ipv4") | 显式同步系统地址解析，返回该地址族的首个数值 IP。可能阻塞，不在游戏关键循环内调用。 |
| Buffer(...parts) | 拼接 byte、string 或 ByteBuffer，得到不可变 ByteBuffer；无参数得到空缓冲区。 |

Send/SendTo 接受 byte、string（原始 UTF-8 字节）或现有 ByteBuffer；缓冲区的 Length/Get/ToString 与 stdio 相同，可在两模块间直接传递。普通收发和 Bind/Connect 只接受数值 IP，不隐式解析域名。IPv6 暂不支持带 scope ID 的地址。

UDP 最大载荷 65507 字节。超过接收上限或发生系统级截断时，整个数据报被消费并丢弃，返回明确 Error，绝不把前缀伪装成完整数据。下一次接收处理下一数据报。UDP 没有 EOF，空数据报仍返回包含空 Data 的对象。TCP 的硬收发故障关闭该 socket；UDP 操作错误通常保留 socket 以便继续使用。参数错误不自动关闭 socket。

### Sleep 驱动的回调

注册后由 VM idle 服务在 `time.Sleep` / `SleepUntil` 期间同线程派发，无需手动 Update。回调事件：

- `Kind == "data"`：包含 Socket 和 Data；UDP 还包含 Address、Port。TCP 一次回调只有当前读到的字节片段。
- `Kind == "eof"`：包含 Socket，TCP 对端结束发送；自动注销接收，仍可发送或 Close。
- `Kind == "error"`：包含 Socket 和 Error，自动注销接收；TCP 硬故障同时关闭。
- 主动 Close 直接注销，不额外排队触发 close 回调。

OnReceive 启用期间，Receive/ReceiveFrom 返回 Error，避免竞争读取。可以在回调中关闭、注销或替换回调；当前调用安全完成，新回调从下一事件起生效。回调返回 Error 时原值由 Sleep 返回并中止本轮，未被替换的失败回调自动注销。VM/C++ 故障以 IdleCallbackFault 向上传播；指令预算仍由外层调用共享。

每次网络 idle 服务最多派发一个事件，在 socket 间轮转。VM 每次 idle 最多调用 256 次服务并遵守单调截止时间。Sleep(0) 只有一个服务调用机会，不保证调用所有模块，也不保证数据已经到达。无事件时 VM 真实等待最多 5 ms 后重新检查，不忙轮询；回调耗时计入 sleep，已运行的回调不能被时间预算抢占。嵌套 sleep 只等待、不递归派发。回调中的长计算或阻塞 Native 调用仍可能造成超时。

回调保存在 socket 的 GC 可见成员中，派发时还临时固定回调和事件。模块只弱引用已订阅 socket；脚本或宿主必须保留 socket 的引用，丢弃最后引用将关闭它。回调注册不使 socket 永久存活。Feather 无词法闭包，可用全局函数或带 __call 的对象保存状态。

### 生命周期、权限和边界

默认每个 network 实例最多 256 个打开的 socket，单次缓冲区/传输最多 1 MiB；宿主 NetworkLimits 可配置，最大为 65536 个 socket / 16 MiB，UDP 仍受协议载荷上限约束。这些是 Native 资源的显式上限，不能冒充 VM 的全局内存统计。网络记录对象、脚本回调继续受现有对象与指令限制。

Close 与对象销毁均释放句柄；Winsock 初始化由实例共享状态持有，最后相关资源释放后配对清理。POSIX 使用 MSG_NOSIGNAL（Linux）或 SO_NOSIGPIPE（macOS），不会修改进程全局 SIGPIPE 策略。socket 设为不可继承/close-on-exec。

项目的 Native 模块不是权限沙箱。向脚本暴露 network 即授予网络能力，沿用宿主决定允许导入哪些模块的机制；CLI 的模块搜索路径中存在 network 时允许显式 import。构建可用 `-Dnetwork=false` 不生成/安装它；此选项不会删除安装目录中以前留下的库，部署时也应排除旧库。受限宿主应拒绝该导入，不应把路径搜索当作访问控制。模块不会自行联网或解析域名；所有操作由脚本显式发起。

Socket 和 idle 注册属于进程内资源，由模块负责关闭和释放；网络数据使用共享的 ByteBuffer 实现。

### 示例和验证

`tests/fixtures/network-tcp.fe`、`network-udp.fe` 是同进程回环 echo 示例，分别输出 255，以及 0、255：
`build/feather.exe run tests/fixtures/network-udp.fe`。

正式测试在 tests/Network.cpp 和 tests/Idle.cpp。全部网络测试仅绑定 127.0.0.1/::1，不访问第三方服务。Windows 已实际构建运行；Linux/macOS 分支采用对应系统接口，但本环境没有进行实际运行或交叉编译。

平台边界参考：[Winsock recvfrom](https://learn.microsoft.com/en-us/windows/win32/api/winsock/nf-winsock-recvfrom)、[POSIX recvmsg](https://man7.org/linux/man-pages/man3/recvmsg.3p.html)、[Apple socket SIGPIPE](https://developer.apple.com/library/archive/documentation/NetworkingInternetWeb/Conceptual/NetworkingOverview/CommonPitfalls/CommonPitfalls.html)。

## audio

`import("audio")` 使用 SDL3 在默认设备上播放 WAV 和 Ogg Opus。构建默认包含该模块，可通过 `-Daudio=false` 禁用。路径为 UTF-8。打开时解码整个文件，输入和解码后的音频分别限制为 128 MiB。

| 接口 | 行为 |
|---|---|
| `audio.Open(path)` | 打开声音，返回声音对象。它的只读属性 `Duration` 以秒计。 |
| `audio.Play(sound)` | 播放或从暂停处恢复。自然播完后再次播放会从头开始。 |
| `audio.Pause(sound)` | 暂停并保留当前播放位置。 |
| `audio.Seek(sound, seconds)` | 跳到指定秒数，保留正在播放或暂停状态。 |
| `audio.Stop(sound)` | 停止并将位置重置为开头。 |
| `audio.Close(sound)` | 释放音频数据、设备和回调。 |

把可调用值赋给声音对象的 `OnPlay`、`OnPaused`、`OnStoped` 或 `OnFinished`。每个回调接收声音对象作为一个实参。赋值 `null` 可清除回调。兼容拼写为 `onPlay` 和 `OnStopped`。显式播放、暂停和停止在状态改变时同步通知。自然结束通过 VM 空闲服务通知，脚本可用 `time.Sleep` 派发。保留声音对象即可保持播放和回调有效。

```feather
var audio = import("audio");
var delegate = import("delegate");
var time = import("time");
var stdio = import("stdio");

def finished(self, sound) {
    stdio.Console.PrintLine(self);
}

def main() {
    var sound = audio.Open("music.opus");
    if (get_type(sound) == "Error") { return sound; }
    sound.OnFinished = delegate.Create(finished, "播放结束");
    var result = audio.Play(sound);
    if (get_type(result) == "Error") { return result; }
    time.Sleep(sound.Duration + 0.2);
    return audio.Close(sound);
}
```

## render2d

### 边界

`render2d` 是独立的可选 Native 模块，全部实现收在 `modules/render2d/`，不属于 VM core，也不向 Feather 的安装头文件暴露渲染接口。脚本通过 `import("render2d")` 显式取得它。模块提供命令式的二维绘制能力，不保存场景树，也不理解背景、立绘、对话框或存档槽位。剧情框架在 Feather 中保存这些状态，并在每帧重新发出绘制命令。

标准实现以 SDL3 管理窗口和事件，以 bgfx 提交图形命令。SDL 指针、bgfx handle、View、pipeline、framebuffer 及具体 Vulkan、Direct3D、Metal、OpenGL 对象均不进入脚本接口。

模块是显式可选构建项：`-Drender2d=enabled` 才取得 wrap 中固定版本的 SDL3/bgfx 并生成 `render2d.felib`，默认构建不下载图形依赖。内置 quad shader 的源码保存在 `modules/render2d/*.sc`，各后端字节码随仓库保存；正常构建不要求额外编译 shader 工具，也不把这些内置 shader 暴露给脚本。

### 脚本接口

模块导出以下只读函数：

- `CreateRenderContext(config)`：创建并打开唯一的活动窗口。`config` 是 `object()`，支持 `Title`、`WindowWidth`、`WindowHeight`、`RenderWidth`、`RenderHeight`、`VSync`、`Resizable`。未提供的字段使用 1280x720、标题 `Feather`、VSync 与可调整大小窗口。尺寸必须是 1..16384 的整数。同一 VM 同时只能有一个打开的 context。
- `Close(context)`：确定性关闭窗口和图形设备；重复关闭成功。NativeObject 析构只作为兜底。
- `LoadTexture(context, path)`、`LoadFont(context, path)`：创建属于该 context 的资源。路径在创建时做词法规范化并保存为 UTF-8 路径标识；相对路径仍相对进程工作目录，绝对路径仍保持绝对。首版文件资源在首次使用时加载。
- `BeginRender(context, r, g, b, a)`：开始一帧并清除画面，颜色分量为 0..1。
- `DrawTexture(context, texture, x, y, width, height, r, g, b, a)`。
- `DrawText(context, font, text, x, y, size, r, g, b, a)`。首版不承诺复杂文字塑形；具体字形缓存属于 backend。
- `EndRender(context)`：提交并显示当前帧。
- `PollEvent(context)`、`WaitEvent(context, timeoutMilliseconds)`：分别非阻塞和限时等待一个窗口事件；无事件返回 `null`。事件是普通 Feather 对象，至少包含字符串 `Type`，坐标事件还包含映射到逻辑渲染分辨率的 `X`、`Y`，键盘事件可包含 `Key`。
- `IsOpen(context)`：窗口未关闭时返回 `true`。

所有绘制坐标均使用 `RenderWidth` x `RenderHeight` 的逻辑像素空间，窗口大小只决定显示缩放。绘制覆盖顺序就是调用顺序。首版不提供自定义 QuadShader、DrawQuad、旋转、裁剪、render target、混合状态或任意底层 uniform；需要时在保持后端无关的前提下另行扩展。

### 帧状态和错误

`BeginRender` 与 `EndRender` 必须严格配对。重复 Begin、帧外 Draw/End、使用关闭 context、将资源用于其他 context、资源加载失败及 backend 失败均返回语言 Error，不得静默忽略。关闭事件使 `IsOpen` 返回 false，但仍允许脚本显式 `Close`。

模块函数是无状态 `NativeSingletonType`；RenderContext、Texture 和 Font 是带类型的 NativeObject。资源不会直接暴露底层句柄。

### 窗口尺寸与布局

`GetWindowMetrics(context)` 返回 `WindowWidth/WindowHeight`（窗口客户区逻辑单位）、`PixelWidth/PixelHeight`（drawable 实际像素）、`PixelDensity`（SDL 像素密度）、`DisplayScale`（SDL 建议内容缩放系数）和 `Minimized`。这两个缩放系数不是物理 DPI 数值，不应重复乘到绘制坐标。初始布局可直接查询，无需等待事件。

`PollEvent` / `WaitEvent` 增加 `WindowResized`、`PixelSizeChanged`、`DisplayScaleChanged`、`Minimized`、`Restored`。事件携带 `Metrics`，为处理事件时查询的当前尺寸（连续 resize 可能已合并到更新的状态）。这些事件由窗口轮询推进，Sleep 不代替轮询窗口事件。

`SetViewport(context, x, y, width, height)` 在 BeginRender 之前设置窗口逻辑单位的目标区域，将固定 RenderWidth/RenderHeight 画布映射到该区域；纹理与文字共用投影，区域外绘制被裁剪，整窗使用 BeginRender 的清屏色。`SetViewport(context, null)` 恢复默认整窗拉伸，不改变现有程序的默认效果。viewport 是临时显示状态，窗口尺寸变化后由脚本重新计算；不序列化。

`WindowToCanvas(context, windowX, windowY)` 返回画布坐标 `X/Y` 和 `InsideViewport`。鼠标事件同样携带画布 `X/Y`、原始逻辑窗口 `WindowX/WindowY` 和 `InsideViewport`；留边外坐标不钳制，由标志判定是否响应。事件使用当时设置的 viewport；如同轮事件中修改布局，可用 WindowX/WindowY 重新换算。最小化、零 drawable 或零 viewport 不提交绘制，逆变换安全返回 (0,0,false)，Begin/End 仍须配对。

等比缩放留边由 Feather 层决定，例如每帧绘制前：

```feather
var m = render.GetWindowMetrics(context);
var scale = m.WindowWidth / 1600;
if (m.WindowHeight / 900 < scale) { scale = m.WindowHeight / 900; }
render.SetViewport(context,
    (m.WindowWidth - 1600 * scale) / 2,
    (m.WindowHeight - 900 * scale) / 2,
    1600 * scale, 900 * scale);
```

上述示例对应 RenderWidth=1600、RenderHeight=900；尺寸/DPI 变化不改变棋盘的逻辑坐标。文字沿用现有按虚拟字号缓存的字形，极高倍率下的清晰度增强不属于本次接口补齐。

关闭 context 会在 bgfx shutdown 之前释放该设备登记的图片纹理和字体字形纹理。脚本仍可持有资源包装对象；之后析构不再次释放 GPU 句柄，也不会误释放新 context 复用的句柄。正常先释放资源和重复 Close 同样安全。真实设备测试为 graphics suite，需要可用桌面/GPU；无图形环境可用 `meson test --no-suite graphics`。

#### DrawLine

`DrawLine(context, x1, y1, x2, y2, width, r, g, b, a)` 在 BeginRender/EndRender
之间画直线，成功返回 true。坐标、线宽为画布单位，RGBA 沿用现有绘制参数规则。
实现为两个三角形组成的平头矩形，与纹理/文字共享投影、viewport 裁剪与 alpha 混合，
不依赖 GPU 宽线支持。线宽必须为正且有限；非有限坐标、无法表示的几何边界返回 Error。
相同起终点成功但不画任何像素。独立线段不提供额外端点或折线连接样式。

### 控件渲染接口（FeatherWidgets 首版）

- `MeasureText(context,font,text,size)` 可在帧外调用，返回 `Width/Height/Baseline/LineHeight`。宽度是最大行 advance（含同绘制实现的 kerning），高度为行数乘 size，换行步长和 LineHeight 均为 size；空字符串 Width=0，仍有一个行盒。不承诺可见 glyph 包围盒、自动换行、shaping 或字体 fallback。font 仍按 context 校验和延迟加载。
- `FillRect(context,x,y,width,height,r,g,b,a)` 在帧内使用内置白纹理绘制实心矩形。尺寸非负，所有参数有限，RGBA 为 0..1；成功 true，错误返回 Error。
- `SetClipRect(context,x,y,width,height)` 在帧内设置画布单位的矩形裁剪，与当前 viewport 的 framebuffer scissor 相交。允许负位置，尺寸非负；零面积不提交绘制。`SetClipRect(context,null)` 恢复当前 viewport 的裁剪。BeginRender 每帧重置；该接口不会改变投影/输入坐标，不是 clip stack。嵌套父子交集由脚本库算好后传入，不序列化 clip。
- `CaptureMouse(context,bool)` 请求/释放系统鼠标捕获，成功 true，SDL 失败返回 Error；不是独立窗口、相对鼠标模式或控件焦点管理。关闭窗口会释放平台资源。控件焦点、pressed、capture target 仍由脚本维护。
- PointerDown/PointerUp/PointerMove 增加 `Button`，按钮事件取 SDL button 编号（1 左、2 中、3 右），Move 为 0。原字段不变。
- 新增 `FocusLost/FocusGained/PointerLeave/PointerEnter` 窗口事件，仅有 Type；无文本/IME/键盘修饰键扩展。

这些接口只增加 render2d 包装层能力，不修改原有函数签名、VM/语言语义或磁盘格式。后端 C++ 协议的新增虚函数有默认不支持错误，原有测试后端可不实现；提供这些新功能的后端需实现相应方法。
