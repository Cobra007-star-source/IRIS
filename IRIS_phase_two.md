第一阶段（/plaintext 和 /json 赛道）我们用纯 CPU 算力和零分配的底盘，证明了网关具有霸榜的物理素质。但在真实的生产环境中，网关不仅要能吐 JSON，还必须具备访问数据库 (Database) 和处理安全加密 (TLS) 的能力。

这就是 TechEmpower 余下的四大高难度赛道（Single query, Multiple queries, Fortunes, Data updates），也是我们接下来的战场。

以下是 IRIS 3.0 第二阶段的详细技术栈规划：

一、 引入 libpq：打通 PostgreSQL 异步大动脉
在数据库打榜中，所有的高级 ORM（如 Hibernate）或 C++ 封装库（如 pqxx）都会带来致命的性能损耗（对象的创建、虚函数调用、字符串的拷贝）。为了贯彻我们的“零分配”铁律，我们必须绕过所有封装，直接对接 PostgreSQL 官方底层的 C 语言库：libpq 的异步 API。

1. 架构改造：无阻塞查询 (Non-blocking I/O)

放弃 PQexec： 这是同步阻塞函数。如果网关在一个连接上调用它，整个核心（Worker Thread）就会停转，千万级 QPS 瞬间归零。

启用 PQsendQuery 和 PQgetResult： 结合我们的 epoll 事件循环。当我们向 DB 发送查询后，立刻把该请求的上下文（Context）挂起（Suspend），交出 CPU 控制权去处理其他的 HTTP 请求。

监听 DB Socket： 通过 PQsocket 获取底层数据库连接的文件描述符 (FD)，将其注册到我们的 epoll 中。当数据库有数据回传可读时（EPOLLIN），再唤醒并恢复上下文。

2. Pipelining 与连接池 (Connection Pool)

每个核心（Core）必须维护一个线程私有（Thread-local）的 DB 连接池，坚决避免跨线程的锁争抢。

利用 PostgreSQL 的流水线模式（Pipeline Mode，PG 14+ 引入），允许我们在不等待上一个查询返回的情况下，连续发送多个 SQL 语句。这对于 Multiple queries 和 Data updates 赛道是霸榜的关键。

二、 引入 kTLS：跨越密码学的物理壁垒
如果 IRIS 3.0 开启了 HTTPS 流量，按照传统的做法（使用 OpenSSL/BoringSSL 的 SSL_write），我们好不容易实现的 writev (iovec 零拷贝) 会瞬间破功。因为用户态 SSL 库强制要求大块连续内存分配用于加密。

为了保持第一阶段的战斗力，我们必须引入 Linux Kernel TLS (kTLS)。

1. 混合握手机制 (The Hybrid Handshake)

非对称握手（用户态）： 依然使用 BoringSSL 来处理最开始复杂的 TLS 握手协商（证书校验、密钥交换）。

密钥移交（Kernel 下沉）： 握手一旦成功协商出对称密钥（如 AES-GCM），我们立刻通过 setsockopt(fd, SOL_TCP, TCP_ULP, "tls") 将这些密钥注入到 Linux 内核的 TCP 栈中。

2. 零拷贝加密回归 (Zero-Copy Encryption)

一旦注入完成，网关的 HTTP 响应逻辑依然可以像 /plaintext 赛道那样，直接吐出 iovec 分散数组，调用 sendmsg 发给内核。

内核网络栈（或者支持硬件内联加密的网卡 SmartNIC）会在最后发送阶段，流水线式地完成数据加密。

结果： 即便开启了极度消耗 CPU 的 TLS，我们在应用层的代码依然是零内存分配和零内存拷贝的。

三、 JIT 引擎的二次扩张：幽灵写入与 Fortunes 赛道
Fortunes 赛道要求从数据库读出数据，然后进行 HTML 模板渲染并做动态的转义（Escaping，如把 < 替换为 &lt;），最后按字母排序输出。这是典型的 CPU 与缓存破坏局。

JIT 幽灵压铸 (Ghost Cast)：

我们不再使用传统的模板引擎（如 Jinja 或 Mustache）去逐行解析模板。

利用我们已经跑通的 AsmJit 宽位块写（Block Write）技术，我们在网关启动时，将 HTML 模板编译成机器码。

当 libpq 返回二进制数据时，JIT 生成的汇编函数会将这些数据“缝合”进 HTML 模板中，并在寄存器层面直接完成 HTML 转义，一气呵成地将最终字节流压铸到 iovec 缓冲区中。

第二阶段行军图总结
里程碑 1 (DB 异步化)： 封装 libpq 异步 API，打通 epoll，实现无锁 DB 连接池。拿下 Single query 赛道。

里程碑 2 (JIT 渲染)： 扩展 JIT 序列化器，支持动态变量插入和 HTML 转义。拿下 Fortunes 赛道。

里程碑 3 (DB Pipelining)： 激活 PG 14 流水线模式，优化高密度查询。拿下 Multiple queries 和 Data updates 赛道。

最终里程碑 (kTLS 闭环)： 完成 BoringSSL 到内核的密钥注入。确保生产环境 HTTPS 性能零损耗。