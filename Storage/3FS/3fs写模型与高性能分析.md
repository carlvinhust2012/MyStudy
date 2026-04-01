# 3FS 写模型与数据布局

## 一、写模式支持

### 1.1 随机写与追加写

3FS **同时支持随机写和顺序追加写**，不限于 append-only。这由其基于偏移量的 Chunk 映射机制决定。

**关键证据**：

**写入按偏移量直接定位 Chunk**（`src/fbs/meta/Schema.cc`）：

```cpp
// 任意 offset → 对应 chunk
Result<ChunkId> File::getChunkId(InodeId id, uint64_t offset) const {
    auto chunk = offset / layout.chunkSize;
    return ChunkId(id, 0, chunk);
}

// 任意 chunk → 对应链
Result<ChainId> File::getChainId(const Inode &inode, size_t offset, ...) const {
    auto chunkIndex = offset / layout.chunkSize;
    auto stripe = chunkIndex % stripeSize;
    return chains[stripe];
}
```

offset 可以是文件中的**任意位置**，无需等于文件末尾。

**FUSE write 接口传入任意 offset**（`src/fuse/FuseOps.cc`）：

```cpp
void hf3fs_write(fuse_req_t req, fuse_ino_t fino, const char *data,
                 size_t size, off_t off, struct fuse_file_info *fi)
```

`off_t off` 由内核传入，可以是文件任意偏移，这是标准 POSIX `pwrite()` 语义。

**支持 Truncate**：允许缩小或扩大文件，截断后可在中间位置重新写入——这是随机写的典型特征。

**支持 O_DIRECT**：绕过页缓存直接写入存储，也是随机写的典型场景。

### 1.2 支持的写模式总结

| 写模式 | 支持 | 说明 |
|--------|------|------|
| 随机写 | 是 | 任意 offset → 任意 chunk → 任意链 |
| 顺序写/追加 | 是 | offset = file_length 的特例 |
| 覆盖写 | 是 | 同一 offset 多次写入，通过 `updateVer` 版本号管理 |
| 截断后重写 | 是 | Truncate + 后续随机写 |

---

## 二、文件 → Inode → Chunk 映射模型

### 2.1 两级映射

3FS 的核心数据模型是**文件 → Inode → Chunk** 的两级映射：

- **Inode**（元数据）：存储在 FoundationDB 中，通过 Meta Server 管理
- **Chunk**（数据）：存储在 Storage Server 的磁盘上，通过 Chain Replication 复制

### 2.2 完整映射链路

```
文件 (POSIX path)
 │
 │  路径解析: /dir1/dir2/file.txt
 │  DENT: root + "dir1" → Inode(100)
 │  DENT: Inode(100) + "dir2" → Inode(200)
 │  DENT: Inode(200) + "file.txt" → Inode(300)
 ▼
Inode (元数据, 存储在 FDB)
 │
 │  包含: type, acl, length, Layout{chunkSize, stripeSize, chains}
 │
 │  文件偏移 → Chunk 映射:
 │  chunkIndex = offset / chunkSize
 │  chainIndex = chunkIndex % stripeSize
 ▼
Chunk (数据, 存储在 Storage Server)
 │
 │  ChunkId = (InodeId, track, chunkIndex) — 16 字节唯一标识
 │  每个 Chunk 通过 Chain 复制到多个 Storage Target
 ▼
Chain (复制单元)
 │
 │  Chain 0: [Target A → Target B → Target C]  3 副本
 │  Chain 1: [Target D → Target E → Target F]
 ▼
Storage Target (实际磁盘上的数据块)
```

### 2.3 具体示例

以一个 4 MiB chunkSize、3 条链条带化的 12 MiB 文件为例：

```
File (12 MiB)
├── offset 0 ~ 4MiB      → chunk 0 → Chain 0 → [T_A → T_B → T_C]
├── offset 4MiB ~ 8MiB    → chunk 1 → Chain 1 → [T_D → T_E → T_F]
├── offset 8MiB ~ 12MiB   → chunk 2 → Chain 2 → [T_G → T_H → T_I]
├── offset 12MiB ~ 16MiB  → chunk 3 → Chain 0 → [T_A → T_B → T_C]  (循环复用)
└── ...
```

### 2.4 核心映射公式

整个映射由两条公式驱动：

```
chunkIndex  = offset / chunkSize        // 文件偏移 → chunk 编号
chainIndex  = chunkIndex % stripeSize   // chunk 编号 → 链编号
```

Inode 的 `Layout` 字段记录了这个映射规则（chunkSize、stripeSize、使用了哪些链），有了 Inode 就能计算出任意 offset 对应的 chunk 在哪个 Storage Target 上。

### 2.5 ChunkId 格式

每个 Chunk 由 16 字节的 `ChunkId` 唯一标识：

```
[0]      : tenant     (1 byte, 固定 0x00)
[1]      : reserved   (1 byte, 固定 0x00)
[2..9]   : inode      (8 bytes, big-endian InodeId)
[10..11] : track      (2 bytes, big-endian track_id)
[12..15] : chunk      (4 bytes, big-endian chunk_num)
```

使用 big-endian 编码保证排序性，支持范围扫描：扫描某 Inode 的所有 chunk 只需 `ChunkId(inode, 0, begin)` 到 `ChunkId(inode, 1, 0)`。

### 2.6 Chunk 与 Chain 的对应关系

每个 chunk 精确映射到**一条** chain，而一条 chain 由多个 Storage Target 组成，承载该 chunk 的多个副本。映射关系是**多对一**的——多个 chunk 共享同一条 chain（按 `chunkIndex % stripeSize` 循环分配），但每个 chunk 在这条 chain 的每个 Target 上都有一份完整的数据副本。

```
chunk 0 → Chain 0 → [T_A → T_B → T_C]  ← chunk 0 的 3 个副本
chunk 1 → Chain 1 → [T_D → T_E → T_F]  ← chunk 1 的 3 个副本
chunk 2 → Chain 2 → [T_G → T_H → T_I]  ← chunk 2 的 3 个副本
chunk 3 → Chain 0 → [T_A → T_B → T_C]  ← chunk 3 复用 Chain 0，也是 3 个副本
```

写入时通过链式复制协议保证所有副本一致：

```
Client → [Head: 写入] → [Member 2: 写入] → [Member 3: 写入]
                                                  ↓ commit
Client ← [Head: commit] ← [Member 2: ACK]   ←──────┘
```

读取时任选链中一个 Target（默认选负载最低的），无需读所有副本。

### 2.7 动态条带

3FS 支持动态条带扩展（`dynStripe`），避免创建文件时预分配所有链：

```
初始: dynStripe = 1 (仅使用 Chain 0)
写入 chunk 0 → Chain 0  ✓
写入 chunk 1 → 需要 Chain 1 → beginWrite() 触发 extendStripe()
                                    Meta Server 将 dynStripe 更新为 2
写入 chunk 1 → Chain 1  ✓
写入 chunk 2 → 需要 Chain 0  ✓ (循环复用)
```

### 2.8 本质

3FS 把一个文件当作一块**虚拟的连续地址空间**，以 chunkSize 为粒度切分，然后通过条带化分散到多条链上。这种映射是纯算术运算（除法取模），任意 offset 都能直接定位到对应的 chunk 和 Storage Target，因此天然支持随机读写。

---

## 三、AI 训练场景下的 I/O 模式分析

### 3.1 AI 训练的典型 I/O 工作负载

| 场景 | 操作 | I/O 模式 | 特点 |
|------|------|---------|------|
| 训练数据加载 | read | 顺序读 | 大批量顺序读取训练样本（图片、文本、token） |
| Checkpoint 保存 | write | 顺序写（追加） | 定期 dump 模型权重、优化器状态，单次几百 GB |
| Checkpoint 恢复 | read | 顺序读 | 训练启动时加载 checkpoint |
| 模型权重加载 | read | 顺序读 | 推理服务启动时加载权重 |

这些场景**全部是大块顺序 I/O**，随机写几乎没有用武之地。

### 3.2 随机写的必要性

对于 AI 训练场景，随机写**不是必需的**。真正重要的能力是：

| 能力 | 重要程度 | 说明 |
|------|---------|------|
| 高顺序读带宽 | 核心 | 多 GPU 同时读取训练数据，GB/s 级吞吐 |
| 高顺序写吞吐 | 核心 | Checkpoint 保存，几百 GB 需在几秒内完成 |
| 大文件支持 | 高 | 模型权重文件可达数百 GB |
| 高并发 | 高 | 成百上千 GPU 同时读写 |
| RDMA 零拷贝 | 高 | 减少数据拷贝和 CPU 开销 |
| 随机写 | 低 | 几乎不使用 |
| 随机读 | 低 | Checkpoint 通常是整体加载 |
| 小文件/元数据操作 | 低 | 训练数据文件数量大但访问模式固定 |

### 3.3 3FS 为什么仍然支持随机写

不是因为 AI 场景需要，而是**架构设计的自然结果**：

```
offset / chunkSize = chunkIndex   ← 纯算术映射，天然支持任意 offset
```

3FS 采用基于偏移量的 chunk 寻址模型，这套机制天然就是随机寻址的——支持随机写不需要额外实现任何特殊逻辑，反而是限制为 append-only 需要额外增加约束检查。

换言之，随机写是 3FS 地址空间映射模型的一个**免费副产品**，而非为 AI 场景特意优化的功能。3FS 针对 AI 场景的核心优化方向是高并发、高吞吐、RDMA 零拷贝和大文件顺序 I/O 性能。

---

## 四、高带宽与高 IOPS 的技术实现

3FS 的高性能源于**全栈分层的批量化与并行化**设计，从 FUSE 层到磁盘层每一级都做了优化。以下是各层的关键技术。

### 4.1 总体架构：五级流水线

```
┌─────────────────────────────────────────────────────────────────────┐
│  Layer 5: FUSE / IoRing 层                                        │
│  写缓冲、读预取、批量提交、splice 零拷贝                            │
├─────────────────────────────────────────────────────────────────────┤
│  Layer 4: 客户端 I/O 调度层                                       │
│  批量 RPC (128 IO/4MB)、多级并发控制、负载均衡、目标分片           │
├─────────────────────────────────────────────────────────────────────┤
│  Layer 3: RDMA 网络传输层                                         │
│  零拷贝数据传输、连接池 + 线程本地缓存、Signal Batching             │
├─────────────────────────────────────────────────────────────────────┤
│  Layer 2: 存储服务层                                              │
│  32 线程 AIO Worker、RDMA Buffer Pool、批处理、流控               │
├─────────────────────────────────────────────────────────────────────┤
│  Layer 1: 磁盘 I/O 层                                             │
│  O_DIRECT、io_uring (registered files/buffers)、多文件分片           │
└─────────────────────────────────────────────────────────────────────┘
```

### 4.2 磁盘 I/O 层（Layer 1）

| 技术 | 说明 | 效果 |
|------|------|------|
| **O_DIRECT** | 所有 AIO 读和已对齐写绕过页缓存 | 消除双缓冲，减少内存拷贝 |
| **io_uring** | 使用 `io_uring_prep_read_fixed` + registered files/buffers | 消除每次 I/O 的 fd 查找和 buffer 地址转换 |
| **256 物理文件分片** | 每个 Storage Target 按 chunk size 创建 256 个物理文件 | 分散单文件 inode 锁竞争 |
| **fallocate 预分配** | 写入前预分配空间 | 避免写入时 COW 导致的延迟抖动 |
| **pread/pwrite** | 使用偏移量读写，避免 lseek | 同一 fd 多线程并发安全 |
| **32 AIO Worker 线程** | 每线程独立 collect→submit→reap 循环 | 线程间无调度器竞争 |
| **批量 reaping** | `min_complete=128`，每次 syscall 至少完成 128 个 I/O | 摊销 syscall 开销 |

### 4.3 RDMA 网络传输层（Layer 3）

| 技术 | 说明 | 效果 |
|------|------|------|
| **RDMA WRITE (读响应)** | 服务端直接写入客户端注册内存 | 读数据零 CPU 拷贝 |
| **RDMA READ (写请求)** | 服务端直接从客户端内存拉取数据 | 写数据零 CPU 拷贝 |
| **预注册内存池** | 服务端 4MB×1024 + 64MB×64 的 RDMA Buffer Pool | 运行时零 ibv_reg_mr |
| **Signal Batching** | 每 8 个 SEND WR 才触发一次 CQ 信号 | CQ 中断减少 ~8x |
| **Unsignaled RDMA Batch** | 批次中仅最后一个 WR 设信号 | 大批量 RDMA 开销趋近 O(1) |
| **连接池 + 线程本地缓存** | 32 分片全局池 + ThreadLocal 一级缓存 | 连接复用，消除锁竞争 |
| **多连接** | 同一地址支持多条连接（随机选择） | 链路聚合 / Multi-Rail |
| **ZSTD 压缩** | >128KB 的 RPC 消息可压缩 | 减少网络带宽消耗 |

### 4.4 客户端 I/O 调度层（Layer 4）

| 技术 | 说明 | 效果 |
|------|------|------|
| **批量 RPC** | 最多 128 个 IO / 4MB 合并为一个 BatchReadReq | 单次 RPC 完成大量 I/O |
| **按目标节点分组** | 同一 Storage Node 的 IO 合并为一个批次 | 减少跨节点 RPC 次数 |
| **多级并发控制** | 全局信号量(32) + 每节点信号量(8) | 防止服务端过载 |
| **跨节点并行** | 不同节点的批次通过 `collectAllRange` 并发执行 | 水平扩展吞吐 |
| **负载均衡** | LoadBalance 策略选择最空闲的副本 Target | 均衡存储节点负载 |
| **快速故障转移** | 单次失败后立即切换到备用副本 | 减少故障延迟 |
| **小数据内联** | < max_inline_read_bytes 的读直接嵌入 RPC 响应 | 小 I/O 跳过 RDMA 建立开销 |
| **IO 拆分** | max_read_io_bytes 将大读拆分为对齐的子 IO | 优化磁盘访问模式 |

### 4.5 FUSE / IoRing 层（Layer 5）

| 技术 | 说明 | 效果 |
|------|------|------|
| **写回缓存** | 可选的 FUSE writeback cache，委托内核合并写 | 减少小写次数 |
| **FUSE Splice** | 零拷贝数据传输 (SPLICE_READ/WRITE/MOVE) | 管道/Socket 直接传输 |
| **Per-Inode 写缓冲** | 每个 Inode 最大 1MB 写缓冲，RDMA 注册内存 | 批量刷写减少 RPC |
| **IoRing 高性能路径** | 自定义共享内存 Ring，128 个并发协程 | 绕过 FUSE，用户态直连 |
| **PioV 批量引擎** | 将任意文件区间拆分为 chunk 对齐的批量 IO | 一次调用完成多 chunk I/O |
| **批量元数据** | readdirplus 使用 batchStat 批量查询 Inode | 减少 meta RPC 轮次 |

### 4.6 存储服务层（Layer 2）

| 技术 | 说明 | 效果 |
|------|------|------|
| **批处理拆分** | batch_read_job_split_size=1024，大批次拆分到多个 AIO Worker | 并行化磁盘 I/O |
| **RDMA Buffer Pool** | 服务端 4MB×1024 + 64MB×64 预注册 buffer | RDMA 传输无需运行时分配 |
| **Per-Device RDMA 流控** | 每张 IB 网卡独立信号量 (max 256 并发) | 防止单网卡过载 |
| **Per-Chunk 协程锁** | `CoLockManager` 细粒度异步锁 | 不同 Chunk 的更新互不阻塞 |
| **32 分片元数据** | ChunkStore 使用 32 个 ConcurrentHashMap 分片 | 减少元数据锁竞争 |
| **1024 分片客户端状态** | ReliableUpdate 客户端状态 1024 分片 | 高并发链式复制 |
| **4 级线程池** | proc / io / bg / connect 独立线程池 | 隔离不同类型的工作负载 |

### 4.7 关键参数默认值

| 参数 | 默认值 | 层级 | 说明 |
|------|-------|------|------|
| `AIO Worker 线程数` | 32 | 磁盘 | 每个 Target 的读线程 |
| `AIO 批量 reaping` | 128 | 磁盘 | 每次 syscall 最少完成数 |
| `物理文件数` | 256 | 磁盘 | 每个 Target 的分片文件数 |
| `Chunk 大小` | 512K ~ 64M | 磁盘 | 支持 6 种规格 |
| `RDMA buffer pool (小)` | 4MB × 1024 | 网络 | 共 4GB |
| `RDMA buffer pool (大)` | 64MB × 64 | 网络 | 共 4GB |
| `Signal Batching` | 8 | 网络 | 每 N 次 SEND 触发一次 CQ 信号 |
| `ACK Batching` | 8 | 网络 | 每 N 次 RECV 发送一次 ACK |
| `客户端 batch_size` | 128 | 调度 | 每个 RPC 最多 IO 数 |
| `客户端 batch_bytes` | 4MB | 调度 | 每个 RPC 最大字节数 |
| `全局并发数` | 32 | 调度 | 全局最大并发 RPC 数 |
| `每节点并发数` | 8 | 调度 | 单 Storage Node 最大并发数 |
| `写缓冲大小` | 1MB | FUSE | Per-Inode 写缓冲 |
| `IoRing 并发协程` | 128 | FUSE | 用户态 I/O 并发数 |
| `服务端 RDMA 写并发` | 256 | 服务 | Per-Device 最大 RDMA WRITE 并发 |
| `ChunkStore 分片` | 32 | 服务 | 元数据哈希表分片数 |

### 4.8 核心设计思想

3FS 的高性能源于一个统一的设计哲学：**在每一层都做批量化，在每一层都做并行化**。

```
用户写入 1MB 数据的完整路径:

FUSE 层: memcpy 到 1MB 写缓冲 → 立即返回 (异步)
                          ↓ 后续 flush
调度层:  1MB 拆为若干 chunk IO → 按目标节点分组 → batchWrite RPC
网络层:  RPC 消息走 RDMA SEND → 数据走 RDMA READ (服务端拉取) → 零拷贝
服务层:  写请求入队 → Worker 线程处理 → RDMA Buffer → AIO 提交
磁盘层:  io_uring registered file + fixed buffer → O_DIRECT pread/pwrite
```

每一层都在做合并和并行，而不是简单地传递请求，这就是 3FS 能够支撑高带宽高 IOPS 的根本原因。
