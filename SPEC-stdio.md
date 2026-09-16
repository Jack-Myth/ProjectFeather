# Feather 标准输入输出库（首版）

`stdio` 是独立于共享 `FeatherCore` 的可选 Native 模块。VM 不自行打开文件，也不创建标准输入输出对象。独立解释器从可执行文件旁的 `modules/` 装载 `stdio.felib.dll`（Linux 为 `.so`），脚本通过 `import("stdio")` 取得根对象。嵌入式宿主也可直接构造 `StdIoLibrary(input, output)`，调用 `GetModule()` 并把返回值接入自己的 `import` 回调。根对象、Console/IO、文件句柄都是 `NativeObject`。传入的输入输出流须比这些对象及使用它们的 VM 活得更久。

独立运行程序 `feather` 与 `feathervm` 给模块创建入口提供进程的 `std::cin` 和 `std::cout`。`featherc` 只编译源码，不装载本模块。`GetModule()` 返回一个只读根对象，其 `Console` 与 `IO` 成员分别是控制台和文件对象。本模块不会覆盖已有的 `import`，也不会注入 `Console` 或 `IO` 全局名。搜索、缓存和动态库生命周期见 `SPEC-native-modules.md`。

脚本应在顶层写 `var stdio = import("stdio");`。顶层 `var` 存入当前文件模块的私有全局表，函数内可按该模块的全局名访问 `stdio`；这里不依赖闭包。函数外导入必须在调用函数之前执行，独立运行程序的初始化流程满足这一顺序。

## Console

| 调用 | 返回及行为 |
|---|---|
| `stdio.Console.Print(value)` | 写入一个基础值并刷新输出流；成功返回 `null`。 |
| `stdio.Console.PrintLine(value)` | 同上，随后写入 `\n`；成功返回 `null`。 |
| `stdio.Console.InputLine()` | 读取一行，去掉行尾换行；EOF 返回 `null`，正常行返回 UTF-8 `string`。 |

输出支持 `null`、bool、number 和 string。对象、错误参数个数、输出失败或无效 UTF-8 输入返回 Error 值。number 使用经典 locale 的十进制表示；此显示格式只用于人类阅读，不承诺与 Feather 字面量或文件格式相同。`InputLine` 不打印提示；可先调用 `Print`。

## IO

所有文件以二进制模式打开；路径为 UTF-8 字符串，按进程当前工作目录解析相对路径。首版不设路径沙箱，宿主可选择不安装 `IO`。每个文件句柄属于创建它的 `StdIoLibrary` 实例，不能传给另一个库实例的 `IO` 方法。句柄析构时关闭底层文件；显式关闭后再次使用返回 Error。IO 故障、错误参数或模式不允许的操作均返回 Error 值。

| 调用 | 返回及行为 |
|---|---|
| `stdio.IO.OpenFile(path, mode)` | 返回文件句柄；模式为 `r`、`w`、`a`、`r+`、`w+`、`a+`。`w` 截断，`a` 追加，`+` 同时允许读写。 |
| `stdio.IO.CloseFile(handle)` | 关闭成功返回 `true`。 |
| `stdio.IO.Seek(handle, offset, origin)` | `origin` 为 `start`、`current`、`end`；返回移动后的字节位置。 |
| `stdio.IO.Tell(handle)` | 返回当前字节位置。 |
| `stdio.IO.Read(handle, count)` | 最多读取 `count` 字节，返回 `ByteBuffer`；EOF 返回空缓冲区。单次 `count` 不超过 16 MiB。 |
| `stdio.IO.Write(handle, data)` | `data` 是 string（UTF-8 字节）或 `ByteBuffer`；完整写入时返回写入字节数。 |

`offset`、`count` 和位置是整数，精确表示范围为双精度数值的 ±(2^53−1)；`count` 非负。读写共享一个文件位置；同一个 `+` 句柄在读写方向之间切换时，先调用 `Seek`。`Read` 不将任意二进制数据误作 UTF-8 字符串，也不会在 EOF 自动关闭句柄。

`ByteBuffer.Length` 是字节数；`ByteBuffer.Get(index)` 返回 0–255 的字节数值；`ByteBuffer.ToString()` 在全部字节合法 UTF-8 时返回 string，否则返回 Error。缓冲区不可从脚本修改。需要把任意文件内容交给宿主时，直接传递这个宿主对象；跨 VM 使用仍由宿主代理边界决定。

这些宿主对象目前没有快照 codec。包含它们的 VM 快照需由宿主提供对应 codec；文件句柄不能仅靠 VM 状态恢复底层文件位置或权限。本库不注册 codec，也不替宿主决定文件恢复策略。
