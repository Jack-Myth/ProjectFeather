# network 基础网络模块

`import("network")` 获取可选 Native 模块。Windows 使用 Winsock 2.2，Linux/macOS 使用 POSIX socket，共享脚本 API。所有 socket 始终非阻塞，不启动后台线程。模块仅提供 TCP/UDP；不提供消息分帧、HTTP、TLS 或可靠 UDP。

## 接口

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

## Sleep 驱动的回调

注册后由 VM idle 服务在 `time.Sleep` / `SleepUntil` 期间同线程派发，无需手动 Update。回调事件：

- `Kind == "data"`：包含 Socket 和 Data；UDP 还包含 Address、Port。TCP 一次回调只有当前读到的字节片段。
- `Kind == "eof"`：包含 Socket，TCP 对端结束发送；自动注销接收，仍可发送或 Close。
- `Kind == "error"`：包含 Socket 和 Error，自动注销接收；TCP 硬故障同时关闭。
- 主动 Close 直接注销，不额外排队触发 close 回调。

OnReceive 启用期间，Receive/ReceiveFrom 返回 Error，避免竞争读取。可以在回调中关闭、注销或替换回调；当前调用安全完成，新回调从下一事件起生效。回调返回 Error 时原值由 Sleep 返回并中止本轮，未被替换的失败回调自动注销。VM/C++ 故障以 IdleCallbackFault 向上传播；指令预算仍由外层调用共享。

每次网络 idle 服务最多派发一个事件，在 socket 间轮转。VM 每次 idle 最多调用 256 次服务并遵守单调截止时间。Sleep(0) 只有一个服务调用机会，不保证调用所有模块，也不保证数据已经到达。无事件时 VM 真实等待最多 5 ms 后重新检查，不忙轮询；回调耗时计入 sleep，已运行的回调不能被时间预算抢占。嵌套 sleep 只等待、不递归派发。回调中的长计算或阻塞 Native 调用仍可能造成超时。

回调保存在 socket 的 GC 可见成员中，派发时还临时固定回调和事件。模块只弱引用已订阅 socket；脚本或宿主必须保留 socket 的引用，丢弃最后引用将关闭它。回调注册不使 socket 永久存活。Feather 无词法闭包，可用全局函数或带 __call 的对象保存状态。

## 生命周期、权限和边界

默认每个 network 实例最多 256 个打开的 socket，单次缓冲区/传输最多 1 MiB；宿主 NetworkLimits 可配置，最大为 65536 个 socket / 16 MiB，UDP 仍受协议载荷上限约束。这些是 Native 资源的显式上限，不能冒充 VM 的全局内存统计。网络记录对象、脚本回调继续受现有对象与指令限制。

Close 与对象销毁均释放句柄；Winsock 初始化由实例共享状态持有，最后相关资源释放后配对清理。POSIX 使用 MSG_NOSIGNAL（Linux）或 SO_NOSIGPIPE（macOS），不会修改进程全局 SIGPIPE 策略。socket 设为不可继承/close-on-exec。

项目的 Native 模块不是权限沙箱。向脚本暴露 network 即授予网络能力，沿用宿主决定允许导入哪些模块的机制；CLI 的模块搜索路径中存在 network 时允许显式 import。构建可用 `-Dnetwork=false` 不生成/安装它；此选项不会删除安装目录中以前留下的库，部署时也应排除旧库。受限宿主应拒绝该导入，不应把路径搜索当作访问控制。模块不会自行联网或解析域名；所有操作由脚本显式发起。

活动或已关闭但仍可达的 Socket 都不可快照；保存前 Close 并清除所有 socket 引用（包括事件对象），否则明确拒绝。网络数据 ByteBuffer 可快照，共享实现保留既有 stdio 类型身份和载荷格式。idle 注册是进程内资源，模块在新 VM 重新注册，不写进快照。

## 示例和验证

`tests/fixtures/network-tcp.fe`、`network-udp.fe` 是同进程回环 echo 示例，分别输出 255，以及 0、255：
`build/feather.exe run tests/fixtures/network-udp.fe`。

正式测试在 tests/Network.cpp 和 tests/Idle.cpp。全部网络测试仅绑定 127.0.0.1/::1，不访问第三方服务。Windows 已实际构建运行；Linux/macOS 分支采用对应系统接口，但本环境没有进行实际运行或交叉编译。

平台边界参考：[Winsock recvfrom](https://learn.microsoft.com/en-us/windows/win32/api/winsock/nf-winsock-recvfrom)、[POSIX recvmsg](https://man7.org/linux/man-pages/man3/recvmsg.3p.html)、[Apple socket SIGPIPE](https://developer.apple.com/library/archive/documentation/NetworkingInternetWeb/Conceptual/NetworkingOverview/CommonPitfalls/CommonPitfalls.html)。
