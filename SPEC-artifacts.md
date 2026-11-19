# 编译产物格式

## 字节码（.fbc v6）

`.fbc` 是单个源码文件编译结果的磁盘表示，供 `featherc` 生成，并由 `feather run` 或精简的 `feathervm` 执行。它保存代码、初始化入口、命名函数索引、快捷行需求标记和 `export` 名称表，不保存 VM 全局变量、对象堆、调用栈或依赖图。当前格式实验性，同一版本的编码必须确定性。

当前加载器仅接受 v6。

`import("name")` 仅作为普通全局调用保存于函数代码中；本格式不记录或打包它指向的依赖。宿主提供的导入契约见 [SPEC-modules.md](SPEC-modules.md#导入)。

所有整数和 IEEE 754 binary64 数值均按小端编码。字符串是 `u32` 字节长度加原始 UTF-8 字节，不含终止符；代码是 `u32` 字节长度加原始指令字节。读取方必须验证 UTF-8、长度和完整消费，不接受尾随数据。

### 文件布局

| 字段 | 编码 | 含义 |
|---|---|---|
| magic | 8 bytes | `46 54 48 52 42 43 00 00`（`FTHRBC` 加两个零字节） |
| format version | u16 | 6 |
| instruction version | u16 | 新编译的程序为 3。加载器也接受指令版本 2，按文件声明的版本验证 opcode |
| payload size | u32 | 后续负载的精确字节数 |
| source size | u64 | 源文件字节数，计入 payload size；无源文件大小信息时为 UINT64_MAX |
| constant count | u32 | 常量条数，最多 65536 |
| constants | 逐条 | 见下文 |
| initializer | u32 | 必须指向无参数函数常量 |
| named function count | u32 | 名称映射条数，最多 65536 |
| named functions | 逐条 | UTF-8 标识符字符串、函数常量索引；按名称字节序升序编码 |
| uses quick operators | u8 | 0 或 1；需要宿主注册缺失的快捷函数，独立运行程序拒绝值为 1 的程序 |
| export count | u32 | 显式 `export` 名称数，最多 65536 |
| exports | 逐条 | UTF-8 标识符字符串，按名称字节序升序编码，不重复 |

常量以一个 `u8` tag 开始。tag 4 后跟 u32 长度和任意二进制字节，Const 将其物化为不可变 ByteBuffer；tag 3 后跟单个 u8 byte 载荷；tag 0 后跟 binary64 原始 64 位；tag 1 后跟字符串；tag 2 后跟 `parameter count: u32`、`local count: u32`、`default count: u32`、逐个默认值及代码字节串。默认值 tag 为：0 缺省、1 `null`、2 `false`、3 `true`、4 后跟 binary64、5 后跟 UTF-8 字符串、6 后跟单个 u8 byte。默认值数量必须与形参数相等；不编码 object 默认值。

编译器当前还会在内存函数原型中生成指令位置表；v6 不编码它。可选的独立 `.fbs` 保存这些位置，见[符号格式](#符号fbs-v3)。精简 `feathervm` 不加载符号文件，因而保持无位置回退；嵌入宿主可以显式附加匹配的 `.fbs`。

加载器在运行任何代码前核对 magic、两个版本、文件总长、各字段边界、常量和函数索引、重复函数名、默认值、UTF-8、标记合法性，并调用 `Module::Validate()` 检查每个函数的指令、跳转、局部槽位及栈效应。CLI 对整个输入文件和输出文件采用 64 MiB 上限，单个函数代码仍受内存模块的 1 MiB 上限。文件没有签名或完整性校验；加载校验保证格式与 VM 不变量，不限制合法脚本的运行时间或副作用。

`featherc input.fe -o output.fbc [--symbols output.fbs]` 只编译和写文件，不执行顶层代码。`feather run input.fbc` 与 `feathervm input.fbc` 都加载字节码、执行初始化函数，再调用存在时的无参数 `main`；`feathervm` 是不包含调试协议、Socket transport 或源码编译器的最小字节码 CLI，也不加载 `.fbs`。独立运行程序拒绝仍需要宿主提供快捷函数的模块。脚本可自行声明同名顶层函数。

`feather run input.fe` 在源码旁存在同名 `input.fbc` 且 `.fbc` 修改时间不早于 `.fe`、v6 中的源码字节数与当前文件大小一致时，优先加载并完整验证该字节码；缓存不存在、较旧、无法读取或验证失败时重新读取并编译源码。相同大小的不同内容可能命中缓存；需要确定性部署时应显式执行 `.fbc`。`feather debug` 总是编译 `.fe`，避免缓存字节码缺少内存调试表。

## 符号（.fbs v3）

`.fbs` 是可选的 `.fbc` 伴随文件，保存运行时源码位置、函数显示名以及局部槽位的名称和词法生命周期；普通 `.fbc` 不含这些调试信息，精简 `feathervm` 不寻找或加载 `.fbs`。编译器仅在 `--symbols` 被指定时写它，嵌入宿主可调用 `SerializeSymbols(program)` 和 `AttachSymbols(program, bytes)`。加载应在构造 VM 之前完成。源码 UTF-8 行列仍按 [SPEC-syntax.md](SPEC-syntax.md) 的字节规则计算；符号文件不保存源码内容或绝对路径。

快速开发期只接受当前 v3，不读取旧版本，也不提供跨版本兼容分支。

所有整数为小端。布局为下列字段顺序：

| 字段 | 编码 | 含义 |
|---|---|---|
| magic | 8 bytes | `FTHRSY` 后接两个零字节 |
| format version | u16 | 3 |
| instruction version | u16 | 2 或 3，必须匹配对应 `.fbc` 的指令版本 |
| payload size | u32 | 从头部结束到文件末尾的精确字节数，含校验和 |
| bytecode size | u32 | 对应 `.fbc` 的总字节数 |
| bytecode fingerprint | u64 | 标准 v6 程序编码的 FNV-1a 64 位指纹（计算时将源码大小字段统一为 UINT64_MAX） |
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

加载器先核对 CRC32、版本、长度和对应字节码的尺寸及指纹，再校验函数索引、位置表以及局部槽位和范围；任何失败都不修改模块，也不运行脚本。指纹与 CRC32 用于发现意外损坏和错误配对，均不是签名或安全认证。符号文件不参与 VM 运算，不改变字节码或 Error 的普通值语义。
