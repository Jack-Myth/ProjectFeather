# Feather 符号文件格式（实验版 v3）

`.fbs` 是可选的 `.fbc` 伴随文件，保存运行时源码位置、函数显示名以及局部槽位的名称和词法生命周期；普通 `.fbc` 不含这些调试信息，精简 `feathervm` 不寻找或加载 `.fbs`。编译器仅在 `--symbols` 被指定时写它，嵌入宿主可调用 `SerializeSymbols(program)` 和 `AttachSymbols(program, bytes)`。加载应在构造 VM 之前完成。源码 UTF-8 行列仍按 `SPEC-syntax.md` 的字节规则计算；符号文件不保存源码内容或绝对路径。

快速开发期只接受当前 v3，不读取旧版本，也不提供跨版本兼容分支。

所有整数为小端。布局为下列字段顺序：

| 字段 | 编码 | 含义 |
|---|---|---|
| magic | 8 bytes | `FTHRSY` 后接两个零字节 |
| format version | u16 | 3 |
| instruction version | u16 | 1，与 `.fbc` 的指令版本一致 |
| payload size | u32 | 从头部结束到文件末尾的精确字节数，含校验和 |
| bytecode size | u32 | 对应 `.fbc` 的总字节数 |
| bytecode fingerprint | u64 | 完整、确定性 v3 `.fbc` 字节的 FNV-1a 64 位指纹 |
| function count | u32 | 模块中的函数常量数，最多 65536 |
| functions | 逐个 | 按常量索引升序编码，见下文 |
| CRC32 | u32 | 前面所有字节的 IEEE CRC32 |

每个函数条目依次包含：

| 字段 | 编码 | 含义 |
|---|---|---|
| constant index | u32 | 函数在模块常量表中的索引 |
| function name | `length: u32` + UTF-8 bytes | 显示名；手写匿名函数可为空 |
| location count | u32 | 后续位置条目数 |
| locations | 逐个 | 每项为四个 u32：`instruction PC`、`source byte offset`、`line`、`column`，再跟一个 u8 `breakable` |
| local count | u32 | 后续局部变量条目数，不得超过函数 LocalCount |
| locals | 逐个 | 每项为 `slot: u32`、`start PC: u32`、`end PC: u32`、`name` |

编译器产生的函数每条指令都有位置；手工构造且没有源码位置的函数可写零条。非空位置表须恰好覆盖该函数所有指令，并按指令起始 PC 顺序排列；行列从 1 开始，源码字节偏移从 0 开始。`breakable` 只能为 0 或 1；函数声明在初始化器中生成的绑定指令为 0，保留诊断和单步位置但不抢先解析同一行的源码断点。

局部变量范围为 `[start PC, end PC)`。起点必须是指令边界，终点必须是指令边界或函数代码末尾，且起点严格小于终点。一个槽位最多有一个条目；参数槽位的范围必须覆盖整个函数。名称是非空 UTF-8。调试器只显示当前 PC 位于范围内的条目；同名范围嵌套时只显示后声明的内层变量。没有局部符号的函数回退为 `$0`、`$1` 等槽位名。

加载器先核对 CRC32、版本、长度和对应字节码的尺寸及指纹，再校验函数索引、位置表以及局部槽位和范围；任何失败都不修改模块，也不运行脚本。指纹与 CRC32 用于发现意外损坏和错误配对，均不是签名或安全认证。符号文件不参与 VM 运算和快照，不改变字节码或 Error 的普通值语义。
