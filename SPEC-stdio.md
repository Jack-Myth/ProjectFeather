# Feather 标准输入输出库（首版）

`FeatherStdIo` 是独立于 `FeatherCore` 的可选 C++ 库。VM 不自行打开文件，也不创建标准输入输出对象。宿主构造 `StdIoLibrary(input, output)`，并在初始化之前把 `Resolve` 接入 `RegisterImport` 的宿主解析器。库对象和返回的文件句柄是 `NativeObject`。传入的输入输出流须比库对象及使用它的 VM 活得更久。

独立运行程序 `feather` 与 `feathervm` 默认使用进程的 `std::cin` 和 `std::cout`。`featherc` 只编译源码，不链接本库。`StdIoLibrary::Resolve("std:console")` 和 `Resolve("std:io")` 分别返回对应对象；未知标识返回 `null` 供宿主继续尝试自己的解析器。本库不会覆盖已有的 `import`，也不会注入 `Console` 或 `IO` 全局名。独立运行程序只解析这两个标识，其他导入仍需宿主解析器。

脚本应使用 `var console = import("std:console");` 与 `var io = import("std:io");`。顶层 `var` 存入 VM 全局表，因此函数内可按全局名访问 `console`、`io`；这里不依赖闭包。函数外导入必须在调用函数之前执行，独立运行程序的初始化流程满足这一顺序。

## Console

| 调用 | 返回及行为 |
|---|---|
| `console.Print(value)` | 写入一个基础值并刷新输出流；成功返回 `null`。 |
| `console.PrintLine(value)` | 同上，随后写入 `\n`；成功返回 `null`。 |
| `console.InputLine()` | 读取一行，去掉行尾换行；EOF 返回 `null`，正常行返回 UTF-8 `string`。 |

输出支持 `null`、bool、number 和 string。对象、错误参数个数、输出失败或无效 UTF-8 输入返回 Error 值。number 使用经典 locale 的十进制表示；此显示格式只用于人类阅读，不承诺与 Feather 字面量或文件格式相同。`InputLine` 不打印提示；可先调用 `Print`。

## IO

所有文件以二进制模式打开；路径为 UTF-8 字符串，按进程当前工作目录解析相对路径。首版不设路径沙箱，宿主可选择不安装 `IO`。每个文件句柄属于创建它的 `StdIoLibrary` 实例，不能传给另一个库实例的 `IO` 方法。句柄析构时关闭底层文件；显式关闭后再次使用返回 Error。IO 故障、错误参数或模式不允许的操作均返回 Error 值。

| 调用 | 返回及行为 |
|---|---|
| `io.OpenFile(path, mode)` | 返回文件句柄；模式为 `r`、`w`、`a`、`r+`、`w+`、`a+`。`w` 截断，`a` 追加，`+` 同时允许读写。 |
| `io.CloseFile(handle)` | 关闭成功返回 `true`。 |
| `io.Seek(handle, offset, origin)` | `origin` 为 `start`、`current`、`end`；返回移动后的字节位置。 |
| `io.Tell(handle)` | 返回当前字节位置。 |
| `io.Read(handle, count)` | 最多读取 `count` 字节，返回 `ByteBuffer`；EOF 返回空缓冲区。单次 `count` 不超过 16 MiB。 |
| `io.Write(handle, data)` | `data` 是 string（UTF-8 字节）或 `ByteBuffer`；完整写入时返回写入字节数。 |

`offset`、`count` 和位置是整数，精确表示范围为双精度数值的 ±(2^53−1)；`count` 非负。读写共享一个文件位置；同一个 `+` 句柄在读写方向之间切换时，先调用 `Seek`。`Read` 不将任意二进制数据误作 UTF-8 字符串，也不会在 EOF 自动关闭句柄。

`ByteBuffer.Length` 是字节数；`ByteBuffer.Get(index)` 返回 0–255 的字节数值；`ByteBuffer.ToString()` 在全部字节合法 UTF-8 时返回 string，否则返回 Error。缓冲区不可从脚本修改。需要把任意文件内容交给宿主时，直接传递这个宿主对象；跨 VM 使用仍由宿主代理边界决定。

这些宿主对象目前没有快照 codec。包含它们的 VM 快照需由宿主提供对应 codec；文件句柄不能仅靠 VM 状态恢复底层文件位置或权限。本库不注册 codec，也不替宿主决定文件恢复策略。
