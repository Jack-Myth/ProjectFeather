# Feather Native 模块导入（首版）

## 目标与边界

脚本写 `var stdio = import("stdio");`，取得 `stdio.Console` 和 `stdio.IO`。`import` 仍是普通全局函数调用。编译器不记录依赖；VM 不解析标识、不访问路径或动态库。下文的搜索目录、Native 动态库加载和文件模块装载都属于独立解释器的宿主层。嵌入式宿主可以提供自己的解析器。

Native 模块不是 Feather 字节码实例：它返回一个只读的 `NativeObject` 命名空间，不占用 VM 的文件模块 ID，也不使用源码 `export`。同一 VM 的各文件模块只有在各自调用 `import("stdio")` 后，才把返回值放入本模块的私有全局表；导入值指向同一个 Native 实例。不同 VM 分别持有自己的 Native 模块实例和状态。

## 标识、文件名和搜索顺序

第一版裸模块名仅允许 ASCII 字母、数字、下划线，首字符不能是数字。例如 `stdio` 对应 Windows 的 `stdio.felib.dll`、Linux 的 `stdio.felib.so`；macOS 若支持则用 `stdio.felib.dylib`。这些文件不由 `.fbc` 引用，宿主根据模块名自行寻找。模块文件扩展名属于平台约定，脚本始终只写 `import("stdio")`。

独立解释器保存一份有序的模块目录列表。当前默认列表只有**可执行文件所在目录的 `modules/`**；目录以绝对路径记录。以后可以增加命令行或宿主 API 来修改列表，无需改 VM。裸名查找分两轮：

1. 按目录顺序查找 `<name>.felib.<platform-suffix>`；找到第一个即装载 Native 模块。
2. 如果全部目录都没有 Native 文件，再按同样的目录顺序查找 `<name>.fe`（`feather run`）或 `<name>.fbc`（`feathervm`），装载第一个 Feather 模块。

这样即使较靠后的目录有 Native 文件、较靠前的目录有同名脚本文件，也由 Native 文件获胜；同类型候选由目录顺序决定。找到候选后若装载或初始化失败，报告该候选的错误，不自动换下一个同名文件。缺失全部候选时返回可诊断的导入 Error。Native 动态库只接受裸名，不允许把 `./foo`、`../foo` 或绝对路径当作动态库路径。

现有 `import("./math")`、`import("../util.fe")` 继续走调用方文件目录，直接装载 Feather 源码/字节码，不参加模块目录搜索；这保证显式相对导入的含义稳定。第一版不处理包名、带斜杠的裸名或网络标识。宿主可以在未来扩展裸名语法，但必须保持 Native 与文件模块的选择顺序可预测。

## 装载、缓存与生命周期

宿主用规范化的绝对路径作为 Native 库身份，同一 VM 中同一文件仅打开一次、创建一份模块对象，重复 `import` 返回同一个对象。Native 标识也缓存解析结果，使反复查找不改变其身份；失败不缓存为成功实例。文件模块继续按其现有 ID 和初始化状态缓存。动态库句柄要活得比它创建的所有 `NativeObject`、可调用方法和文件句柄更久；CLI 在 VM 销毁之后才释放库句柄。宿主不可在仍可调用这些对象时卸载库。模块对象的析构函数不得调用已进入销毁流程的 VM。

Native 根对象建议实现 `GetObjectType() == ObjectType::Host`、只读 `GetMember`，把公开方法或子对象作为成员返回；方法对象实现 `IsCallable()` 和 `Call(arguments)`。不存在的成员、错误实参和可预期的 IO 失败返回 `ErrorObject`。Native 对象若持有当前 VM 的脚本 Value，使用 `SetGcVisibleMember` 或 `RootHandle` 保持 GC 可见；不能直接返回另一台 VM 的 ScriptObject/FunctionObject。宿主若启用快照，需为相关 Host 对象和 import 函数提供 codec，并另行保存外部资源状态。

## 动态库入口

文件名只解决定位，还需要统一的入口。每个库导出固定符号 `FeatherNativeModuleV1`，其开发期接口见 `include/Feather/NativeModule.hpp`：

```cpp
struct NativeModuleContext {
    Vm& Machine;
    std::istream* Input = nullptr;
    std::ostream* Output = nullptr;
};
struct NativeModuleDescriptor {
    std::uint32_t InterfaceVersion; // 首版为 1
    const char* Name;               // 例如 "stdio"
    Value (*Create)(const NativeModuleContext&);
};
extern "C" const NativeModuleDescriptor* FeatherNativeModuleV1();
```

宿主打开文件后只查找该符号，核对版本、声明名与请求的裸名相同，再调用 `Create`。`stdio` 从上下文取得输入/输出流；缺失服务时返回明确错误。创建失败或版本不符时关闭尚未交付对象的库句柄并报告错误；脚本看不到半创建模块。额外的宿主服务以后扩展上下文版本，模块不得自行假定所有宿主都允许文件 IO。

首版开发期可要求 Native 库与解释器使用相同编译器、标准库、运行时和 Feather 构建版本，采用版本化的 **C 导出符号 + C++ 模块接口**。`extern "C"` 只固定符号名，并不让 `Value`、`std::shared_ptr` 或 `NativeObject` 自动成为稳定 C ABI。为了让对象类型与分配/释放边界一致，动态库和解释器还必须链接同一份共享 `FeatherCore`，而不是各自静态链接一份 Core。Meson 工程因此需要共享 Core、符号导出和 Native 库目标；这些是构建/宿主接口变化，不是新的 VM 指令。若以后要支持任意编译器制作的第三方库，应另设计全不透明句柄的 C ABI；不把开发期 C++ 接口冒充跨工具链规范。

## `stdio` 例子

`stdlib/StdIo.cpp` 与 `stdlib/StdIoModule.cpp` 组成 `stdio.felib`，返回一个只读根对象：`Console` 成员包含 `Print`、`PrintLine`、`InputLine`；`IO` 成员包含 `OpenFile`、`CloseFile`、`Seek`、`Tell`、`Read`、`Write`。方法行为及 `ByteBuffer`、文件句柄仍见 `SPEC-stdio.md`。脚本只导入一次：

```feather
var stdio = import("stdio");

def main() {
    stdio.Console.PrintLine("hello");
    var file = stdio.IO.OpenFile("result.txt", "w");
    stdio.IO.Write(file, "hello\n");
    stdio.IO.CloseFile(file);
}
```

构建产物放在解释器旁的 `modules/stdio.felib.dll` 或 `modules/stdio.felib.so`。宿主把进程输入/输出流等服务交给模块创建入口，模块本身不私自决定宿主的 IO 策略。`stdio` 是首个可运行的 Native 模块例子；两个旧 `std:` 标识已从 CLI 删除。目录列表当前只有默认路径，修改路径列表的接口留待后续。

写一个 Native 模块时，作者实现返回 Host 根对象的创建函数，导出与文件名对应的 descriptor，再让 Meson 把它建成动态库。`stdlib/StdIoModule.cpp` 的入口如下：

```cpp
#include <Feather/NativeModule.hpp>
#include <Feather/StdIo.hpp>

Feather::Value Create(const Feather::NativeModuleContext& context) {
    if (!context.Input || !context.Output)
        return Feather::Value::FromObject(
            std::make_shared<Feather::ErrorObject>("stdio requires streams"));
    Feather::StdIoLibrary library(*context.Input, *context.Output);
    return library.GetModule();
}

extern "C" FEATHER_NATIVE_EXPORT
const Feather::NativeModuleDescriptor* FeatherNativeModuleV1() {
    static const Feather::NativeModuleDescriptor descriptor{
        Feather::NativeModuleInterfaceVersion, "stdio", &Create};
    return &descriptor;
}
```

对应的 `modules/meson.build` 将 `stdlib/StdIo.cpp` 和入口文件编译为 `shared_module('stdio.felib', name_prefix: '')`，依赖同一份共享 Core，输出到 `modules/`。新的模块把 `stdio` 换为自己的裸名，返回自身的 `NativeObject` 根对象即可。模块里的对象/方法可以参考 `stdlib/StdIo.cpp`；资源句柄活多久、怎样关闭、哪些失败返回 Error，都由该模块定义。
