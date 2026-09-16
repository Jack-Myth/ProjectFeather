# Feather 字节码文件格式（实验版 v3）

`.fbc` 是单个源码文件编译结果的磁盘表示，供 `featherc` 与 `feathervm` 分进程使用。它保存代码、初始化入口、命名函数索引、快捷行需求标记和 `export` 名称表，不保存 VM 全局变量、对象堆、调用栈或依赖图。运行状态仍由 `SPEC-snapshot.md` 的快照表示。当前格式实验性，同一版本的编码必须确定性。

快速开发期不保证旧格式兼容；当前加载器拒绝 v2，测试以 v3 产物的编译、确定性往返和执行验收。

`import("name")` 仅作为普通全局调用保存于函数代码中；本格式不记录或打包它指向的依赖。宿主提供的导入契约见 `SPEC-import.md`。

所有整数和 IEEE 754 binary64 数值均按小端编码。字符串是 `u32` 字节长度加原始 UTF-8 字节，不含终止符；代码是 `u32` 字节长度加原始指令字节。读取方必须验证 UTF-8、长度和完整消费，不接受尾随数据。

## 文件布局

| 字段 | 编码 | 含义 |
|---|---|---|
| magic | 8 bytes | `46 54 48 52 42 43 00 00`（`FTHRBC` 加两个零字节） |
| format version | u16 | 当前为 3；旧版产物拒绝 |
| instruction version | u16 | 当前为 1；指令语义和编码改变时提升 |
| payload size | u32 | 后续负载的精确字节数 |
| constant count | u32 | 常量条数，最多 65536 |
| constants | 逐条 | 见下文 |
| initializer | u32 | 必须指向无参数函数常量 |
| named function count | u32 | 名称映射条数，最多 65536 |
| named functions | 逐条 | UTF-8 标识符字符串、函数常量索引；按名称字节序升序编码 |
| uses quick operators | u8 | 0 或 1；独立运行程序未注入 `__QuickOperator…` 函数时拒绝值为 1 的程序 |
| export count | u32 | 显式 `export` 名称数，最多 65536 |
| exports | 逐条 | UTF-8 标识符字符串，按名称字节序升序编码，不重复 |

常量以一个 `u8` tag 开始。tag 0 后跟 binary64 原始 64 位；tag 1 后跟字符串；tag 2 后跟 `parameter count: u32`、`local count: u32`、`default count: u32`、逐个默认值及代码字节串。默认值 tag 为：0 缺省、1 `null`、2 `false`、3 `true`、4 后跟 binary64、5 后跟 UTF-8 字符串。默认值数量必须与形参数相等；不编码 object 默认值。

编译器当前还会在内存函数原型中生成指令位置表；v3 `.fbc` 不编码它。可选的独立 `.fbs` 保存这些位置，格式见 `SPEC-symbol-format.md`。`feathervm` 只有显式指定匹配的符号文件时才可显示运行时 Error 的源码行列；单独加载 `.fbc` 保持无位置回退。

加载器在运行任何代码前核对 magic、两个版本、文件总长、各字段边界、常量和函数索引、重复函数名、默认值、UTF-8、标记合法性，并调用 `Module::Validate()` 检查每个函数的指令、跳转、局部槽位及栈效应。CLI 对整个输入文件和输出文件采用 64 MiB 上限，单个函数代码仍受内存模块的 1 MiB 上限。文件没有签名或完整性校验；加载校验保证格式与 VM 不变量，不限制合法脚本的运行时间或副作用。

`featherc input.fe -o output.fbc [--symbols output.fbs]` 只编译和写文件，不执行顶层代码。`feathervm output.fbc [--symbols output.fbs]` 加载后执行初始化函数，再调用存在时的无参数 `main`。快捷行可编译进文件，独立运行程序会因缺少宿主快捷函数而在运行前拒绝；嵌入式宿主仍可通过 `DeserializeProgram` 加载后自行注册四个 `__QuickOperator…` 全局函数。`feather run input.fe` 直接执行源码，遵循相同入口约定。
