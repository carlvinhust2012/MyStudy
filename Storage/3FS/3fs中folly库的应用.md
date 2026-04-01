# 3FS 中 Folly 库的使用分析

## 概述

3FS 全面依赖 Facebook 的 **folly** 库作为基础设施层。folly 解决了 C++ 标准库在高性能分布式存储场景中的关键能力缺失。

本文档按功能领域分类，总结 3FS 使用每个 folly 组件的目的、用法以及选型理由。

---

## 一、协程与异步模型（核心）

### 1.1 问题

C++20 协程仅提供了语言级的 `co_await`/`co_return` 语法，缺少生产级异步编程所需的**组合子（combinator）**——如并发扇出、超时、错误传播、取消等。

### 1.2 Folly 组件

| 组件 | 头文件 | 用途 | 使用频率 |
|------|-------|------|---------|
| `Task<T>` | `coro/Task.h` | 通用异步返回类型，别名 `CoTask<T>` | 全项目，数千处 |
| `collectAllRange` | `coro/Collect.h` | 并发扇出（最常用） | 20+ 处 |
| `collectAll` | `coro/Collect.h` | 固定数量扇出 | `Rename.cc`, `GcManager.cc` 等 |
| `collectAllWindowed` | `coro/Collect.h` | 有界并发扇出 | `DumpChunkMeta.cc` |
| `co_awaitTry` | `coro/FutureUtil.h` | 错误传播式 await | RPC 调用、重试逻辑 |
| `timeout` | `coro/Timeout.h` | 异步超时 | `InodeIdAllocator.cc` |
| `co_withCancellation` | `coro/WithCancellation.h` | 取消传播 | `ScanTree.cc` |
| `collectAnyNoDiscard` | `coro/Collect.h` | 竞速（首个完成胜出） | `CoroutinesPool.h` |
| `Baton` | `coro/Baton.h` | 协程感知的轻量通知 | 30+ 处 |
| `SharedMutex` | `coro/SharedMutex.h` | 协程感知读写锁 | `CoroSynchronized.h` |
| `Mutex` | `coro/Mutex.h` | 协程感知互斥锁 | `MgmtdState.h`, `FuseClients.h` |
| `sleep` | `coro/Sleep.h` | 异步休眠 | 重试策略、心跳 |
| `blockingWait` | `coro/BlockingWait.h` | 同步→协程桥接 | `StorageTargets.cc`, `BufferPool.cc` |

### 1.3 核心用法：并发扇出

`collectAllRange` 是 3FS 异步模型的核心模式，用于并行处理多个分片/服务器/链的请求：

```cpp
// MetaClient.cc:298 - 并发向多个 Meta Server 发起请求
CoTryTask<BatchStatRsp> MetaClient::batchStat(const std::vector<StatReq> &reqs) {
    auto results = co_await folly::coro::collectAllRange(
        folly::coro::co_invoke([&]() -> CoTryTask<StatRsp> { ... }),
        folly::coro::co_invoke([&]() -> CoTryTask<StatRsp> { ... }),
        ...
    );
}

// DirEntry.cc:265 - 并发列出多个子目录
// BatchOperation.cc:386 - 并发处理批量操作
// StorageTargets.cc:67 - 并发向多个 Storage Target 发起请求
```

### 1.4 核心用法：错误传播

```cpp
// FDB 事务重试
co_await folly::coro::co_awaitTry(
    folly::coro::co_invoke([&]() -> CoTask<Inode> { ... })
);

// RPC 调用重试
co_await folly::coro::co_awaitTry(
    serde::callRpc<...>(client, request)
);
```

### 1.5 选型理由

| 对比项 | C++20 协程 | folly::coro |
|--------|-----------|-------------|
| 基础语法 | `co_await`/`co_return` | 相同 |
| 并发扇出 | 需自行实现 | `collectAllRange`/`collectAll` |
| 超时 | 需自行实现 | `coro::timeout` |
| 取消 | 需自行实现 | `CancellationToken` + `co_withCancellation` |
| 错误传播 | 需自行包装 | `co_awaitTry` |
| 协程锁 | `std::mutex` 阻塞 | `coro::Mutex`/`coro::SharedMutex` |
| 协程通知 | `std::condition_variable` 阻塞 | `coro::Baton` |

---

## 二、并发原语

### 2.1 问题

高性能存储系统需要在协程和同步线程两种上下文中使用并发控制原语，标准库的 `std::counting_semaphore` 不支持协程 `co_wait`，也不支持动态调整容量。

### 2.2 Folly 组件

| 组件 | 头文件 | 用途 | 使用频率 |
|------|-------|------|---------|
| `Semaphore` | `fibers/Semaphore.h` | 同时支持同步+异步的信号量 | 10+ 处 |
| `BatchSemaphore` | `fibers/BatchSemaphore.h` | 批量获取信号量 | `IBSocket.h` (RDMA WR 流控) |
| `Baton` | `fibers/Baton.h` | 同步上下文轻量通知 | `DestructionGuard.h` 等 |
| `Synchronized<T>` | `Synchronized.h` | RAII 加锁包装器 | ~40 处 |

### 2.3 核心用法：双模式信号量

3FS 在 `Semaphore.h` 中封装了 `folly::fibers::Semaphore`：

```cpp
class Semaphore : public folly::fibers::Semaphore {
  void co_wait() { folly::coro::blockingWait(wait()); }  // 协程上下文
  bool wait(Duration timeout) { ... }                      // 同步上下文
  void changeUsableTokens(uint32_t value) { ... }          // 动态调整容量
};
```

应用场景：
- **RDMA 缓冲池**（`RDMABuf.h:346`）：限制并发 RDMA Buffer 分配数
- **连接池**（`BoundedQueue.h:98`）：限制入队/出队并发
- **批量操作**（`IBSocket.h:551`）：`rdmaSem_` BatchSemaphore 限制每个 Socket 挂起的 RDMA WR 数
- **协程池**（`CoroutinesPool.h`）：限制并发协程数

### 2.4 核心用法：Synchronized

```cpp
// 会话管理 (SessionManager.h)
folly::Synchronized<std::map<Uuid, FileSession>> sessions;

// 路由信息缓存 (MgmtdData.h)
folly::Synchronized<std::optional<flat::RoutingInfo>> routingInfoCache;

// 动态属性 (FuseClients.h)
folly::Synchronized<DynamicAttr> dynamicAttr;

// 错误节点集合 (MetaClient.h)
folly::Synchronized<std::set<flat::NodeId>> errNodes_;

// 使用方式
{
  auto guard = sessions.wlock();
  sessions->emplace(key, value);
}
```

**选型理由**：`Synchronized<T>` 强制所有访问都通过 `wlock()`/`rlock()`，避免忘记加锁的 bug。C++ 标准库没有等价物。

---

## 三、原子共享指针

### 3.1 问题

路由信息、配置选项等**读多写少**的共享状态，需要无锁读取以保证性能。`std::atomic<std::shared_ptr>` 在 C++20 中性能不佳（全局引用计数锁）。

### 3.2 Folly 组件：`folly::concurrency::AtomicSharedPtr`

| 使用位置 | 类型 | 说明 |
|---------|------|------|
| `ClientContext.h` | `atomic_shared_ptr<const CoreRequestOptions>` | 连接配置无锁读取 |
| `MgmtdClient.cc` | `atomic_shared_ptr<RoutingInfo>` | 路由信息无锁读取 |
| `StorageClientImpl.h` | `atomic_shared_ptr<const RoutingInfo>` | 客户端路由表 |
| `TargetMap.h` | `atomic_shared_ptr<const TargetMap>` | Target 映射 |
| `ConfigBase.h` | `atomic_shared_ptr<T>` | 通用配置热更新 |
| `FuseOps.cc` | `atomic_shared_ptr<vector<DirEntry>>` | 根目录缓存 |

### 3.3 核心用法：无锁配置读取

```cpp
// Client.h - 每次发请求时无锁读取选项
serdeCtx(serverAddr) {
  return serdeCtx(ioWorker_, config_.force_use_tcp() ? serverAddr.tcp() : serverAddr, options_);
  // options_ 是 atomic_shared_ptr<const CoreRequestOptions>
}
```

```cpp
// StorageClientImpl.h - 路由信息快照
auto routingInfo = routingInfo_.load();  // 原子加载, 无锁
auto chainInfo = routingInfo->getChainInfo(chainId);
```

---

## 四、错误处理

### 4.1 问题

分布式存储系统需要统一且高效的错误传播机制。C++ 异常开销大且不易与协程集成，`std::expected` 直到 C++23 才引入。

### 4.2 Folly 组件：`folly::Expected`

3FS 在 `Result.h` 中将整个错误模型建立在 `folly::Expected` 之上：

```cpp
template <typename T>
using Result = folly::Expected<T, Status>;

using Void = folly::Unit;
```

配套宏覆盖同步和异步两种场景：

```cpp
RETURN_ON_ERROR(result);     // 同步: return makeError on failure
CO_RETURN_ON_ERROR(result);  // 异步: co_return makeError on failure
CHECK_RESULT(result);        // 同步: assert + return
```

**选型理由**：`folly::Expected` 在 C++20 即可用（`std::expected` 需 C++23），且与 `folly::coro::Task` 天然兼容——`Result<T>` 作为 `Task<T>` 的返回值，错误通过 `co_return makeError()` 传播。

---

## 五、线程池与执行器

### 5.1 问题

分布式存储服务需要多种线程池（CPU 密集、I/O、连接管理、后台任务），C++ 标准库的 `std::thread` 缺乏命名、监控、工作窃取等生产级特性。

### 5.2 Folly 组件

| 组件 | 头文件 | 用途 |
|------|-------|------|
| `CPUThreadPoolExecutor` | `executors/CPUThreadPoolExecutor.h` | CPU 密集工作线程池（25+ 处） |
| `IOThreadPoolExecutor` | `executors/IOThreadPoolExecutor.h` | I/O 事件循环线程池 |
| `EventBase` | `EventBase.h` | 事件循环核心 |
| `ViaIfAsync` | `coro/ViaIfAsync.h` | 协程跨执行器调度优化 |

### 5.3 核心用法：4 级线程池架构

3FS 的 `ThreadPoolGroup` 创建 4 类线程池：

```
ThreadPoolGroup
├── procThreadPool_ (CPUThreadPoolExecutor)  → RPC 请求处理
├── ioThreadPool_   (CPUThreadPoolExecutor)  → I/O 相关 CPU 工作
├── bgThreadPool_   (CPUThreadPoolExecutor)  → 后台任务 (GC, 扫描)
└── connThreadPool_ (IOThreadPoolExecutor)   → 连接管理 (epoll 事件)
```

**Storage Worker 示例**（每种操作类型一个 CPU 线程池）：
```
DumpWorker.h:42      → DumpChunkMeta 操作线程池
CheckWorker.h:42     → CheckChunk 操作线程池
AllocateWorker.h:37  → Chunk 分配线程池
UpdateWorker.h:43    → Chunk 写入线程池
AioReadWorker.h:73   → AIO 读操作线程池
```

---

## 六、并发容器

### 6.1 问题

元数据服务需要在高并发下安全访问哈希表、队列等容器。`std::unordered_map` + `std::mutex` 的方案在读写密集场景下性能不足。

### 6.2 Folly 组件

| 组件 | 使用位置 | 说明 |
|------|---------|------|
| `ConcurrentHashMap` | `ChunkStore.h`, `Recorder.h` | 无锁哈希表 |
| `MPMCQueue` | `BoundedQueue.h`, `IBSocket.h` | 多生产者多消费者队列 |
| `ProducerConsumerQueue` | `Monitor.h` | 单生产者单消费者队列 |
| `EvictingCacheMap` | `AclCache.h` | 带 TTL 的 LRU 缓存 |
| `small_vector` | `IBSocket.cc` | 栈上向量，避免堆分配 |
| `sorted_vector_map` | `Sample.h` | 针对小集合优化的有序 map |

### 6.3 核心用法

```cpp
// ChunkStore.h - Chunk 元数据无锁查找
folly::ConcurrentHashMap<ChunkId, ChunkInfo> chunks_;

// IBSocket.h - RDMA 接收完成事件队列
folly::MPMCQueue<std::pair<uint32_t, uint32_t>> queue_;

// AclCache.h - ACL 条目缓存
folly::EvictingCacheMap<Path, AclEntry> cache_;
```

---

## 七、网络与 I/O

### 7.1 问题

需要处理 IP 地址解析、异步 Socket、网络字节序、零拷贝 Buffer 管理等底层网络操作。

### 7.2 Folly 组件

| 组件 | 用途 |
|------|------|
| `IPAddressV4/V6` | IP 地址安全解析 |
| `NetworkSocket` | Socket 句柄抽象 |
| `AsyncSocket` | TCP 异步 Socket |
| `coro::ServerSocket` | 协程式 TCP 服务端 |
| `IOBuf` / `IOBufQueue` | 零拷贝 Buffer 链 |
| `File` / `FileUtil` | 文件操作封装 |
| `RequestContext` | 请求上下文传播（故障注入等） |
| `SocketAddress` | 地址管理 |

### 7.3 IOBuf 在网络栈中的核心地位

```cpp
// Network.h - 全局类型别名
using IOBuf = folly::IOBuf;
using IOBufPtr = std::unique_ptr<folly::IOBuf>;
using IOBufQueue = folly::IOBufQueue;
```

IOBuf 的链式结构支持零拷贝传输——多个 Buffer 片段无需合并即可作为一个消息发送，与 RDMA 和 TCP sendmsg 配合使用。

**选型理由**：`std::vector<char>` 是单一连续内存块，跨 Buffer 拼接需要 memcpy。IOBuf 的链式引用计数支持真正的零拷贝。

---

## 八、日志系统

### 8.1 问题

分布式系统需要分级日志、异步写入、文件轮转、零成本编译期开关。

### 8.2 Folly 组件：`folly::logging`

3FS 基于 folly logging 构建了完整的日志栈：

| 组件 | 3FS 扩展 | 说明 |
|------|---------|------|
| `XLOG` / `XLOGF` | 全项目使用 | 编译期日志级别，关闭时零开销 |
| `FileWriterFactory` | 3FS 自定义 | 带大小限制和轮转的文件写入器 |
| `AsyncFileWriter` | 3FS 自定义 | 异步日志写入，不阻塞业务 |
| `LogFormatter` | 3FS 自定义 | 自定义日志格式 |
| `LoggerDB` | 3FS 自定义 | 日志器数据库管理 |

```cpp
// 典型用法
XLOGF(INFO, "Created inode {}, parent {}", inodeId, parentId);
XLOGF(DBG, "Route request to node {}", nodeId);
XLOGF_IF(ERR, chunkSize == 0, "Invalid chunk size");
XLOGF(WARNING, "Retry attempt {} for operation", retryCount);
```

**选型理由**：`XLOG` 宏在编译期可通过日志级别开关完全消除，生产环境关闭 DEBUG 日志后零性能开销。`spdlog`/`glog` 均不具备此特性。

---

## 九、统计与监控

### 9.1 问题

存储系统需要精确的延迟分位监控（P50/P99/P999），以保障 SLO。

### 9.2 Folly 组件

| 组件 | 用途 |
|------|------|
| `TDigest` | 延迟分位数估算（P50/P99/P999） |
| `DigestBuilder` | 高效合并多个 TDigest |
| `ThreadLocal` | 每线程本地统计缓冲区 |

```cpp
// Recorder.h:189 - 每个 Recorder 维护一个 TDigest
DigestBuilder digest_;
std::mutex digestMu_;

// 每 30 秒输出一次分位数摘要
XLOGF(INFO, "recorder {}: count {}, P50 {:.1f}us, P99 {:.1f}us, P999 {:.1f}us",
       name_, count, p50, p99, p999);
```

---

## 十、其他工具

### 10.1 字节序与位操作

```cpp
// InodeId 小端编码 (Common.h:176)
auto le = folly::Endian::little(val_);
return folly::bit_cast<Key>(le);

// RDMA 协议头大端编码 (IBSocket.h:352)
ImmData(uint32_t val) : val(folly::Endian::big32(val)) {}

// 类型双关 (IBSocket.h:282)
using Base = folly::StampedPtr<void>;
```

### 10.2 哈希函数

```cpp
// ChunkId 哈希
folly::hash::twang_mix64(offset);    // 快速整数哈希
folly::hash::hash_combine(a, b);      // 组合哈希
folly::hash::hash_128_to_64(hi, lo);  // 128→64 位哈希
```

### 10.3 线程本地存储

```cpp
// Recorder.h - 每线程统计缓冲区
folly::ThreadLocal<RecorderTls> tls_;

// ConnectionPool.h - 每线程连接缓存
folly::ThreadLocal<ConnectionMap> connections_;

// ChunkFileStore.h - 每线程文件描述符缓存
folly::ThreadLocal<fd_map> fdMap_;
```

### 10.4 其他常用工具

| 组件 | 用途 |
|------|------|
| `ScopeGuard` / `makeGuard` | RAII 清理守卫（计时器、计数器、资源释放） |
| `Random` | 链分配随机化、重试抖动、故障注入 |
| `dynamic` / `json` | 动态配置、日志配置、事件系统 |
| `MoveOnly` | 非拷贝可移动基类（IBSocket, StorageClient, RDMABufPool） |
| `Overload` | `std::variant` 访问器 |
| `Subprocess` | 调用 IB 设备命令（`ibv_devinfo`） |
| `base64` | 用户 Token 编解码 |
| `split` / `Conv` | 字符串分割和类型转换 |
| `CancellationToken` | 关闭/取消信号传播 |
| `Unit` | 空类型（`Void = folly::Unit`） |

---

## 十一、为什么选择 Folly

### 11.1 Folly 解决的核心问题

```
C++ 标准库的不足                      Folly 的补齐
─────────────────                    ──────────────
C++20 协程无组合子                    → folly::coro (collectAll, timeout, Baton, Mutex)
std::counting_semaphore 不支持协程    → folly::fibers::Semaphore (wait + co_wait)
std::atomic<shared_ptr> 性能差       → folly::AtomicSharedPtr (无锁读)
无统一错误处理                        → folly::Expected (Result<T>)
无并发哈希表                          → folly::ConcurrentHashMap
无零拷贝 Buffer                       → folly::IOBuf
日志关闭时有开销                      → XLOG 编译期零开销
无延迟分位监控                        → TDigest
无生产级线程池                        → CPUThreadPoolExecutor + IOThreadPoolExecutor
```

### 11.2 使用量统计

| 类别 | 唯一组件数 | include 次数 | 重要性 |
|------|-----------|-------------|--------|
| 协程 (coro/) | 20+ | ~200 | 核心 |
| 纤程 (fibers/) | 3 | ~30 | 高 |
| 同步原语 (Synchronized, AtomicSharedPtr) | 2 | ~70 | 高 |
| 线程池 (executors/) | 4 | ~40 | 高 |
| 并发容器 | 6 | ~20 | 中 |
| 网络 (net/, io/) | 10+ | ~50 | 高 |
| 日志 (logging/) | 14 | ~100 | 高 |
| I/O 缓冲 (IOBuf) | 2 | ~30 | 中 |
| 工具类 | 40+ | ~300 | 中 |

### 11.3 与 std:: 的共存

3FS 并非完全依赖 Folly，标准库仍广泛使用：
- `std::mutex`, `std::shared_mutex` — 用于非协程上下文
- `std::optional`, `std::variant` — 类型系统
- `std::string`, `std::string_view` — 字符串（与 folly::StringPiece 共存）
- `std::unordered_map`, `std::map` — 非热路径容器
- `std::chrono` — 时间处理
- `std::span` — 内存视图

Folly 专注于**标准库缺失的高性能能力**（协程组合子、无锁数据结构、零拷贝 I/O、生产级线程池），标准库用于通用数据类型和基础功能。

---

## 十二、关键文件索引

| 文件 | Folly 用途 |
|------|-----------|
| `src/common/utils/Coroutine.h` | `CoTask<T>` = `folly::coro::Task<T>` |
| `src/common/utils/Result.h` | `Result<T>` = `folly::Expected<T, Status>` |
| `src/common/utils/Semaphore.h` | 封装 `folly::fibers::Semaphore` |
| `src/common/utils/CoroSynchronized.h` | 封装 `folly::coro::SharedMutex` |
| `src/common/net/ThreadPoolGroup.h` | 4 级线程池架构 |
| `src/common/net/Network.h` | `IOBuf`, `EventBase` 类型别名 |
| `src/common/net/Client.h` | `AtomicSharedPtr<CoreRequestOptions>` |
| `src/common/net/ib/IBSocket.h` | `BatchSemaphore`, `StampedPtr` |
| `src/common/monitor/Recorder.h` | `ThreadLocal`, `TDigest`, `ConcurrentHashMap` |
| `src/common/logging/LogInit.cc` | `folly::initLogging()` |
