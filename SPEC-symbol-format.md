# Feather 符号文件格式（实验版 v1）

`.fbs` 是可选的 `.fbc` 伴随文件，只保存运行时源码位置；普通 `.fbc` 不含调试信息，`feathervm` 不会自动寻找或加载 `.fbs`。编译器仅在 `--symbols` 被指定时写它，宿主可调用 `SerializeSymbols(program)` 和 `AttachSymbols(program, bytes)`。加载应在构造 VM 之前完成。源码 UTF-8 行列仍按 `SPEC-syntax.md` 的字节规则计算；符号文件不保存源码内容或绝对路径。

所有整数为小端。布局为下列字段顺序：

| 字段 | 编码 | 含义 |
|---|---|---|
| magic | 8 bytes | `FTHRSY` 后接两个零字节 |
| format version | u16 | 1 |
| instruction version | u16 | 1，与 `.fbc` 的指令版本一致 |
| payload size | u32 | 从头部结束到文件末尾的精确字节数，含校验和 |
| bytecode size | u32 | 对应 `.fbc` 的总字节数 |
| bytecode fingerprint | u64 | 完整、确定性 v3 `.fbc` 字节的 FNV-1a 64 位指纹 |
| function count | u32 | 模块中的函数常量数，最多 65536 |
| functions | 逐个 | 按常量索引升序编码，见下文 |
| CRC32 | u32 | 前面所有字节的 IEEE CRC32 |

每个函数条目由 `constant index: u32`、`location count: u32` 和逐条位置构成。位置条目是 `instruction PC: u32`、`source byte offset: u32`、`line: u32`、`column: u32`。编译器产生的函数每条指令都有位置；手工构造且没有源码位置的函数可写零条。非空位置表须恰好覆盖该函数所有指令，并按指令起始 PC 顺序排列；行列从 1 开始。源字节偏移从 0 开始。

加载器先核对 CRC32、版本、长度和对应字节码的尺寸及指纹，再校验函数索引与位置表的指令边界；任何失败都不修改模块，也不运行脚本。指纹与 CRC32 用于发现意外损坏和错误配对，均不是签名或安全认证。符号文件不参与 VM 运算和快照，不改变字节码或 Error 的普通值语义。快速开发期不保证符号或字节码旧版本兼容。
