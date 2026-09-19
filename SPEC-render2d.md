# Feather Render2D 模块（首版）

## 边界

`render2d` 是独立的可选 Native 模块，全部实现收在 `modules/render2d/`，不属于 VM core，也不向 Feather 的安装头文件暴露渲染接口。脚本通过 `import("render2d")` 显式取得它。模块提供命令式的二维绘制能力，不保存场景树，也不理解背景、立绘、对话框或存档槽位。剧情框架在 Feather 中保存这些状态，并在每帧重新发出绘制命令。

标准实现以 SDL3 管理窗口和事件，以 bgfx 提交图形命令。SDL 指针、bgfx handle、View、pipeline、framebuffer 及具体 Vulkan、Direct3D、Metal、OpenGL 对象均不进入脚本接口。

模块是显式可选构建项：`-Drender2d=enabled` 才取得 wrap 中固定版本的 SDL3/bgfx 并生成 `render2d.felib`，默认构建不下载图形依赖。内置 quad shader 的源码保存在 `modules/render2d/*.sc`，各后端字节码随仓库保存；正常构建不要求额外编译 shader 工具，也不把这些内置 shader 暴露给脚本。

## 脚本接口

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

## 帧状态和错误

`BeginRender` 与 `EndRender` 必须严格配对。重复 Begin、帧外 Draw/End、使用关闭 context、将资源用于其他 context、资源加载失败及 backend 失败均返回语言 Error，不得静默忽略。关闭事件使 `IsOpen` 返回 false，但仍允许脚本显式 `Close`。

模块函数是无状态 `NativeSingletonType`；RenderContext、Texture 和 Font 是带类型的 NativeObject。资源不会直接暴露底层句柄。

## Snapshot

RenderContext 保存创建参数、稳定 context ID 和打开/关闭状态；恢复打开状态的对象时重新创建窗口和图形设备。Texture 与 Font 保存所属 context ID、规范化资源路径和加载参数，不保存 SDL/bgfx/GPU handle。恢复后在第一次 Draw 时重新读取资源。资源文件缺失或不兼容必须成为可观察错误。

快照只允许在完整帧之间创建。若可达的 RenderContext 正处于 Begin/End 之间，其 `Serialize` 必须拒绝保存。绘制命令、事件队列、当前 backbuffer 内容和未提交帧不进入快照。恢复后剧情循环重新绘制下一帧。

资源对象与 context 的恢复顺序没有约定，因此资源反序列化不得要求图形设备已经存在。关闭并重新创建 context 会取得新的 context ID；旧资源不能用于新 context。Snapshot 恢复则保留原 ID，使恢复出的资源重新绑定到恢复出的 context。
