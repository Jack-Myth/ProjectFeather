# Feather 源码语法（第一版）

本规范定义 M4 源码到现有内存字节码的映射。运行时值、错误、真值、调用和对象语义以 `SPEC-runtime-design.md` 为准。源码为 UTF-8；关键字、运算符及标识符限 ASCII，字符串内容允许 UTF-8。语法区分大小写。

## 词法

- 空格、制表符和换行分隔 token；词法器扫描时跳过 `//` 至行尾的注释，不存在先对整个源码删注释的独立预处理步骤。
- 标识符：`[A-Za-z_][A-Za-z0-9_]*`；保留字为 `def var export return if else while break continue null true false object`。
- `import` 是普通标识符和宿主注册的全局函数名，不属于保留字；编译器不解析依赖。契约见 `SPEC-import.md`。
- 数字：十进制 `DIGIT+ ('.' DIGIT+)? ([eE] [+-]? DIGIT+)?`，无符号；负号为一元运算。词法阶段拒绝超出有限 double 范围的字面量。
- 双引号字符串；支持 `\\`、`\"`、`\n`、`\r`、`\t`，其他反斜线转义报错。原始换行不得出现在字符串中；解码后的字节必须是合法 UTF-8。
- 普通语句以及 `break`/`continue` 以分号结束；块语句、函数声明、`if`/`while` 和快捷行后不加分号。

### 行首快捷调用

一行的第一个非空白字符若是 `#`、`$`、`&`、`>`，词法器把它识别为快捷调用。可在顶层或块中使用；缩进不属于正文，符号后直到未转义的 `//` 或行结束符之间的文本是唯一字符串实参。去掉正文末尾的空格和制表符，保留符号后的前导空格及正文内部空格；空正文允许。正文不解析引号、普通字符串转义或插值。只有 `\//` 有特殊含义：它产生字面量 `//`，不开始注释；其他反斜线原样保留。例如 `>https:\//example.com // 备注` 传入 `"https://example.com"`。

| 前缀 | 展开后的调用 |
|---|---|
| `#` | `__QuickOperatorHash(text);` |
| `$` | `__QuickOperatorDollar(text);` |
| `&` | `__QuickOperatorAmpersand(text);` |
| `>` | `__QuickOperatorRightAngleBucket(text);` |

快捷行是调用语句，复用普通 `GetGlobal`、`Call`、`Pop` 指令；四个目标都是普通全局可调用值，编译器不创建或访问 `Env`。因此返回值（包括 Error 值）按普通表达式语句规则丢弃。编译结果用 `UsesQuickOperators` 标记该程序需要宿主提供对应的 `__QuickOperator…` 函数，磁盘格式保留此标记。前缀只在物理行首的第一个非空白字符位置有效；同一行已有其他 token 后再写前缀是语法错误。

## 文法

以下 EBNF 中 `{ X }` 表示重复，`[ X ]` 表示可选；`IDENT`、`NUMBER`、`STRING` 是词法 token。

```ebnf
program       = { [ "export" ] function | [ "export" ] top_var | statement } EOF ;
top_var       = "var" IDENT [ "=" expression ] ";" ;
function      = "def" IDENT "(" [ parameter { "," parameter } ] ")" block ;
parameter     = IDENT [ "=" literal ] ;
block         = "{" { statement } "}" ;
statement     = "var" IDENT [ "=" expression ] ";"
              | "return" [ expression ] ";"
              | "if" "(" expression ")" block [ "else" block ]
              | "while" "(" expression ")" block
              | "break" ";" | "continue" ";"
              | block | expression ";" | QUICK_LINE ;
expression    = assignment ;
assignment    = equality [ "=" assignment ] ;
equality      = comparison { ( "==" | "!=" ) comparison } ;
comparison    = term { "<" term } ;
term          = factor { ( "+" | "-" ) factor } ;
factor        = unary { ( "*" | "/" ) unary } ;
unary         = "-" unary | postfix ;
postfix       = primary { "(" [ expression { "," expression } ] ")"
              | "." IDENT | "[" expression "]" } ;
primary       = literal | IDENT | "object" "(" ")" | "(" expression ")" ;
literal       = "null" | "true" | "false" | NUMBER | STRING ;
```

`QUICK_LINE` 是上文定义的整行词法 token，不参与普通表达式文法。

## 名称、作用域和执行

- 顶层允许函数声明及语句。`export` 只可修饰顶层 `var` 或 `def`，记录该模块公开的名字；在块内或其他语句前使用报编译错误。函数声明只允许在顶层；同名函数、同一作用域重复 `var`、重复形参均报编译错误。函数无闭包。
- 形参和 `var` 占用函数局部槽位；块按词法作用域查找，允许内层遮蔽。离开块后名字不可见。其他标识符读写当前函数所属模块的字符串键全局表。顶层 `var` 声明该模块的全局名；顶层不支持局部变量。
- 赋值右结合，左边只允许标识符、点成员或方括号成员。赋值是表达式。`var` 无初值、`return;`、函数末尾和初始化函数末尾均产生 `null`。
- 函数默认参数只能是字面量，且带默认值的形参后面不允许无默认值的形参。调用参数从左到右求值；其余补值、截断规则沿用运行时。
- 编译结果包含一个模块、初始化函数索引、声明函数名到常量索引的映射及导出名表。宿主可用此模块构造主 VM，或调用 `CompiledProgram::LoadInto(vm, id)` 装入已有 VM，再以 `InitializeModule(vm, id)` 初始化；后者只执行顶层代码一次。主模块的旧 `Initialize(vm)` 接口仍可重复执行顶层代码。初始化先将所有声明函数写入所属模块的全局表，再按源码顺序执行顶层语句。因此函数可相互递归，顶层语句可调用任意声明函数。宿主随后可按映射索引调用函数。装入多个模块后的运行契约见 `SPEC-multimodule.md`。
- `if` 和 `while` 条件使用 VM 的真值规则；`else` 只接块。`object()` 编译为 `NewObject`。没有隐式分号、闭包、方法 `this` 绑定、逻辑短路运算或浮点字面量 `NaN`/`Infinity`。
- `break` 和 `continue` 只允许出现在 `while` 循环体内，并作用于词法上最近的外层循环。`break` 跳到该循环之后，`continue` 跳回该循环的条件求值处；两者均以 `JUMP` 降级，不增加 ISA opcode。即使循环位于顶层，这些语句也不能跨越函数边界。
- `!=` 与 `==` 具有相同优先级并左结合，其结果严格等于对应 `==` 结果的布尔取反。编译器可将其降级为 `Equal; False; Equal`，不新增字节码指令。

## 诊断与边界

词法或语法错误、非法赋值目标、重复声明、过多形参/实参/常量/局部槽位、超过 256 层嵌套及字节码校验失败，均使编译失败，不产生可执行的部分模块。诊断至少包含源码字节偏移及行列；行列按 UTF-8 字节计数。编译器生成的每个函数最终由 `Module::Validate()` 校验。

编译成功时，内存函数原型可附带每条指令起始 PC 对应的源码字节偏移与行列，供 VM 查询 Error 来源。表达式操作一般对应运算符或调用符号；跨函数、跨模块返回保持原 Error 的来源。当前 v3 `.fbc` 不保存位置表，可由独立 `.fbs` 提供。
