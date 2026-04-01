# 3FS 存储模型详解

> 基于 [3FS (Fire-Flyer File System)](https://github.com/deepseek-ai/3fs) 源码分析

## 1. 系统总览

3FS 是一个面向 AI 训练和推理场景设计的高性能分布式文件系统。其存储模型采用**存算分离架构**，通过 RDMA 网络连接四大组件：

| 组件 | 职责 |
|------|------|
| **mgmtd**（管理守护进程） | 集群管理、链表维护、目标状态管理、路由信息分发 |
| **meta**（元数据服务） | 文件/目录元数据（Inode、目录项）、Inode 分配、链分配 |
| **storage**（存储服务） | Chunk I/O、数据复制（CRAQ）、物理存储管理 |
| **client**（客户端） | FUSE 集成、I/O 路径编排、数据布局计算 |

```
                          ┌──────────┐
                          │  Client   │
                          │ (FUSE/    │
                          │  Native)  │
                          └────┬──────┘
                               │ RDMA
                    ┌──────────┼──────────┐
                    ▼          ▼          ▼
             ┌──────────┐ ┌──────────┐ ┌──────────┐
             │  Meta    │ │ Mgmtd    │ │  Storage  │
             │ Service  │ │ Service  │ │ Service   │
             └────┬─────┘ └──────────┘ └─────┬─────┘
                  │                            │
                  ▼                            ▼
            FoundationDB               SSD (Chunk Engine)
                                       + RocksDB (Chunk Meta)
```

## 2. 数据组织层级

3FS 将用户数据按以下层级组织，从逻辑到物理：

```
File (文件)
  └── Layout (布局: chainTableId + chunkSize + stripeSize + chain分配)
        └── Stripe (条带: stripeSize 个 Chain)
              └── Chain (复制链: 一组有序的 Target)
                    └── Target (存储目标: 一个节点上的存储实例)
                          └── Chunk (数据块: 固定大小的数据单元)
                                └── Position (物理位置: cluster file 中的偏移)
```

### 2.1 ChainTable（链表）

ChainTable 是一组 Chain 的集合，定义了数据复制和条带化的拓扑。关键属性：

- **chainTableId**: 链表标识
- **chainTableVersion**: 版本号（成员变更时递增）
- **chains**: ChainId 的有序列表

生产环境中可以创建多个 ChainTable（如离线任务表、在线服务表），使用互不交叉的节点和磁盘。

### 2.2 Chain（复制链）

每个 Chain 是一组**有序的 Target 列表**，采用 CRAQ（Chain Replication with Apportioned Queries）协议实现强一致性复制：

```
Chain: [Target0 (head)] → [Target1] → [Target2] → ... → [TargetN (tail)]
```

- **写入**: 客户端发送到 head，沿链逐级转发到 tail，tail 提交后逐级确认返回
- **读取**: 可从任意 serving 状态的 Target 读取（充分利用所有副本带宽）

### 2.3 数据放置（Data Placement）

Chain 的构建通过**数学优化**实现，目标是平衡节点故障时的恢复流量：

- 使用 Pyomo + HiGHS 整数规划求解器
- 约束：每对节点共享的 Chain 数量有上限
- 特殊情况：BIBD（平衡不完全区组设计），使任意节点对共享相同数量的 Chain
- 每块磁盘上创建多个 Target（通常 5 个），分别加入不同的 Chain

**示例**（6 节点，每节点 1 SSD，5 targets/SSD，3 副本）：

| Chain | Target 1 (head) | Target 2 | Target 3 (tail) |
|-------|-----------------|----------|-----------------|
| 1     | A1              | B1       | C1              |
| 2     | D1              | E1       | F1              |
| ...   | ...             | ...      | ...             |

## 3. 文件元数据模型

### 3.1 Inode（索引节点）

每个文件/目录/符号链接对应一个 64 位全局唯一 InodeId，核心结构：

```cpp
// 文件 Inode
struct File {
    uint64 length;        // 文件长度
    uint64 truncateVer;   // 截断版本
    Layout layout;        // 数据布局
    BitFlags flags;       // kHasHole 等标志
    uint32 dynStripe;     // 动态条带 (0 = 禁用)
};

// 目录 Inode
struct Directory {
    InodeId parent;       // 父目录 InodeId
    Layout layout;        // 目录自身的链分配
    string name;
    uint32 chainAllocCounter; // 链分配计数器 (-1 = 全局轮询)
};
```

### 3.2 Layout（数据布局）

Layout 定义了文件的条带化策略：

```cpp
struct Layout {
    ChainTableId tableId;      // 使用的链表
    ChainTableVersion tableVersion;
    uint32 chunkSize;          // 最大 chunk 大小
    uint32 stripeSize;         // 条带宽度（跨多少个 Chain）
    ChainRange or ChainList;   // Chain 分配方式
};
```

- **ChainRange**: 存储 `(baseIndex, shuffle, seed)`，用 MT19937 伪随机数生成器确定性计算 Chain 分配
- **ChainList**: 显式列出所有 Chain 索引

### 3.3 ChunkId（块标识）

ChunkId 是 16 字节的大端编码：

```
[2B tenant] [2B reserved] [8B inode_id] [2B track] [4B chunk_num]
```

- `inode_id`: 文件的 InodeId
- `chunk_num`: 由 `file_offset / chunk_size` 计算
- 同一文件的所有 chunk 按条带分布在多个 Chain 上

### 3.4 元数据持久化（FoundationDB）

3FS 使用 FoundationDB（API v710）存储元数据，支持 SSI 事务：

| 数据类型 | Key 格式 | Value |
|----------|---------|-------|
| Inode | `INOD + InodeId(LE)` | Inode 序列化数据 |
| 目录项 | `DENT + parent_InodeId + name` | `{child_InodeId, type}` |
| 文件会话 | `kFileSessionPrefix + inodeId + uuid` | SessionInfo |

Meta 服务是无状态的，多个实例可并行处理请求。FDB 的冲突检测机制自动处理并发写入冲突。

## 4. Chunk 存储引擎

3FS 的存储引擎有新（Rust）旧（C++）两套实现，新引擎为 Chunk Engine。

### 4.1 物理存储层级

```
Node (节点)
  └── Disk (磁盘, 10-20 块/节点, 单盘 30TB)
        └── Target (存储目标, 多个/磁盘)
              └── Chunk Size Class (11 种: 64KB ~ 64MB, 2 的幂次)
                    └── Cluster (256 个文件/尺寸类, 编号 0x00 ~ 0xFF)
                          └── Group (256 个 Position)
                                └── Position (单个 chunk 的物理位置)
```

**关键参数**：
- 每个 Chunk Size Class 有 256 个 Cluster 文件
- 每个 Group 包含 256 个 Position
- 单台机器最多支持约 12 亿个 Chunk、500 万个 Group

### 4.2 地址编码

**GroupId**（64 位）：
```
[32-bit chunk_size][24-bit group_index][8-bit cluster]
```

**Position**（64 位）：
```
[24-bit chunk_size][8-bit cluster][24-bit group][8-bit index_in_group]
```

字节偏移量直接由 `chunk_size * position_value` 计算，无需额外的查表。

### 4.3 Chunk Engine（Rust 实现）

Chunk Engine 是新版的存储引擎，通过 CXX bridge 暴露给 C++ 调用。

#### 4.3.1 分配器（Allocator）

三级分配器结构：

```
Allocators (11 个 Allocator，每种 chunk size 一个)
  └── Allocator (管理 Cluster 文件 I/O)
        └── ChunkAllocator (内存中的分配状态)
              ├── full_groups      // 已满的组
              ├── active_groups    // 部分使用的组（按 4 级填充度排序）
              └── frozen_groups    // 压缩中冻结的组
                    └── GroupAllocator (管理组的分配/回收)
                          ├── allocated_groups   // 已分配空间的空组
                          └── unallocated_groups // 需要新 fallocate 的组
```

**分配流程**：
1. 优先在 `active_groups` 中通过位运算（`__builtin_ctz`）找到空闲位置
2. 若 `active_groups` 为空，从 `allocated_groups` 获取新组
3. 若 `allocated_groups` 为空，从 `unallocated_groups` 获取，同步调用 `fallocate()` 分配磁盘空间
4. 后台线程维护 `active_groups` 的目标数量，确保分配效率

#### 4.3.2 GroupState（组状态）

256 位位图（4 个 `u64`），跟踪组内 256 个位置的分配状态：
- 4 个填充级别（每 64 位 = 64 个位置为一级）
- 使用 RocksDB MergeOp 实现原子位图更新

#### 4.3.3 ChunkMeta（块元数据）

```rust
struct ChunkMeta {
    pos: Position,          // 物理位置（64 位）
    chain_ver: u32,         // 写入时的链版本
    chunk_ver: u32,         // 块版本（单调递增）
    len: u32,               // 数据长度（字节）
    checksum: u32,          // CRC32C 校验和
    timestamp: u64,         // 时间戳（微秒）
    last_request_id: u64,   // 幂等重试支持
    last_client_low: u64,   // 客户端 UUID 低 32 位
    last_client_high: u64,  // 客户端 UUID 高 32 位
    etag: ETag,             // 实体标签
    uncommitted: bool,      // 是否已提交到链
}
```

### 4.4 MetaStore（RocksDB 元数据存储）

Chunk Engine 使用本地 RocksDB 存储块元数据，Key 前缀设计：

| 前缀 | Key | Value | 用途 |
|------|-----|-------|------|
| 0x01 | `1 \|\| inverted(chunk_id)` | ChunkMeta | 块元数据（倒序存储便于 GC 扫描） |
| 0x02 | `2 \|\| group_id_bytes` | GroupState (32B) | 组分配位图（MergeOp 原子更新） |
| 0x03 | `3 \|\| position_bytes` | chunk_id | 物理位置到块 ID 的映射 |
| 0x04 | `4 \|\| prefix` | i64 delta | 空间用量统计（merge-sum） |
| 0x06 | `6 \|\| prefix \|\| ts \|\| chunk_id` | chunk_id | 时间戳索引（GC 排序） |
| 0x09 | `9 \|\| chunk_id` | ChunkMeta | 写入中块日志（崩溃恢复） |

### 4.5 Copy-on-Write（写时复制）

Chunk Engine 使用 CoW 语义实现安全更新：

```
写入现有 Chunk 的流程：
1. 分配新 Position
2. 读取旧数据到缓冲区
3. 在缓冲区上叠加新数据
4. 将合并后的缓冲区写入新 Position
5. 原子更新 RocksDB 中的 ChunkMeta 和旧/新 Position 状态
6. 旧 Position 引用计数归零后释放空间
```

**追加优化**：当写入是纯追加时，可就地写入（不分配新位置），使用 `crc32c_combine` 高效计算增量校验和。

## 5. 数据复制协议（CRAQ）

### 5.1 写入流程

```
Client                Head (T0)           T1                Tail (Tn)
  │                      │                  │                  │
  │── UpdateReq(WRITE) ─▶│                  │                  │
  │                      │── 本地更新 ──     │                  │
  │                      │── Forward ──────▶│                  │
  │                      │                  │── 本地更新 ──     │
  │                      │                  │── Forward ──────▶│
  │                      │                  │                  │── 本地更新
  │                      │                  │                  │── Commit
  │                      │                  │◀── CommitMsg ────│
  │                      │                  │── 本地 Commit     │
  │                      │◀── CommitMsg ────│                  │
  │                      │── 本地 Commit     │                  │
  │◀── UpdateRsp ────────│                  │                  │
```

**关键机制**：
- **幂等写入**：每个客户端维护 `(ChainId, ChannelId)` → `RequestId` 映射，重复请求直接返回缓存结果
- **链版本校验**：写入请求携带 `chain_ver`，与当前版本不匹配则拒绝
- **块锁**：同一 Chunk 的并发写入在 head 节点串行化
- **双版本存储**：每个 Target 可存储 committed 和 pending 两个版本（`pending_ver = committed_ver + 1`）

### 5.2 读取流程

```
Client           Serving Target
  │                      │
  │── BatchReadReq ─────▶│
  │                      │── 查询 ChunkMeta
  │                      │── 计算物理偏移
  │                      │── pread from cluster file
  │◀── ReadData (RDMA) ──│
```

- 读取可从任意 serving 状态的 Target 发起
- 客户端支持多种目标选择策略：负载均衡、轮询、随机、固定尾部等
- 支持流量区域（Traffic Zone）过滤

### 5.3 版本追踪

| 版本 | 说明 |
|------|------|
| `ChainVer` | 链成员变更时递增，存储在 ChunkMeta 中 |
| `ChunkVer` | 每个 Chunk 更新时递增 |
| `commitVer` | 所有副本确认写入的版本号 |
| `updateVer` | 当前更新返回的版本号 |

### 5.4 故障恢复

Target 的状态机：

```
                    OFFLINE ──(服务重启)──▶ WAITING
                      ▲                        │
                      │                        ▼
                      │                     SYNCING
                      │                        │
                      │            (前驱 serving)│
                      │                        ▼
                      └──(前驱 offline)── SERVING
```

**恢复流程**：
1. 离线 Target 重启后进入 WAITING 状态，等待数据恢复
2. 前驱 Target（serving 状态）开始转发正常写入（full-chunk-replace）到恢复节点
3. 前驱发送 `dump-chunkmeta` 请求，收集两端的 Chunk 元数据
4. 比较两端元数据，决定需要传输的 Chunk
5. 传输完成后发送 `sync-done`，Target 进入 SERVING 状态

**恢复流量均衡**：通过 BIBD 数据放置优化，确保任意节点故障时恢复流量均匀分散到其他所有节点。

## 6. 磁盘上的存储布局

每个 Storage Target 的目录结构：

```
<path>/                          # Target 根目录
├── target.toml                  # 配置文件（target_id, chain_id, chunk_size_list 等）
├── meta/                        # RocksDB 元数据存储
└── <chunk_size>/                # 每个 chunk size class 一个子目录（如 "524288"）
    ├── 00                       # Cluster 文件 0
    ├── 01                       # Cluster 文件 1
    ├── ...                      # ...
    └── FF                       # Cluster 文件 255
```

每个 Cluster 文件是一个稀疏文件：
- 通过 `fallocate()` 按需扩展（预分配 `chunk_size * 256` 字节）
- 通过 `pread`/`pwrite` 直接在计算出的偏移位置读写 Chunk
- 通过 `FALLOC_FL_PUNCH_HOLE` 释放已回收的物理空间
- 支持 `O_DIRECT`（EXT4/XFS）和 `O_SYNC`（ZFS）两种 I/O 模式

## 7. 零拷贝 I/O 接口（USRBIO）

为绕过 FUSE 的性能瓶颈，3FS 提供了 USRBIO（User Space Ring-Based IO）接口：

```
┌──────────────────┐          ┌──────────────────┐
│  User Process    │          │  FUSE Daemon     │
│                  │          │                  │
│  ┌─── Iov ────┐  │  Shared  │  ┌── Iov ────┐   │
│  │IB-registered│◄─┼──────────┼─►│Memory     │   │
│  │memory       │  │  Memory  │  │Region     │   │
│  └─────────────┘  │          │  └───────────┘   │
│                  │          │                  │
│  ┌─── Ior ────┐  │  Ring    │  ┌── Ior ────┐   │
│  │Submit/Wait │◄─┼─ Buffer ─┼─►│Dequeue/   │   │
│  │IO Requests │  │          │  │Complete   │   │
│  └─────────────┘  │          │  └───────────┘   │
└──────────────────┘          └──────────────────┘
```

- **Iov**: IB 注册的共享内存区域，支持零拷贝数据传输
- **Ior**: 类似 io_uring 的环形缓冲区，用户进程入队请求，FUSE 守护进程出队执行
- 支持批处理（`io_depth` 参数控制批大小）和多 NUMA 节点亲和性

## 8. 一致性与事务模型

### 8.1 元数据一致性

- FoundationDB 提供 **SSI（Serializable Snapshot Isolation）** 事务
- 并发写入相同 Key 时，FDB 自动检测冲突并重试
- Meta 服务利用事务保证原子性操作：create、link、unlink、rename 等

### 8.2 数据一致性

- CRAQ 协议保证 **写全读任意** 的强一致性
- 写入沿链传播到所有副本后才返回成功
- 客户端维护 `commitVer`，保证读到自己写入的数据

### 8.3 文件长度一致性

- 写入模式下客户端每 5 秒上报最大写入位置
- close/fsync 时精确查询最后一个 Chunk 的 ID 和长度
- 使用 Rendezvous Hash 将长度更新任务分散到不同 Meta 服务，避免热点

## 9. 形式化验证

3FS 使用 Microsoft Research 的 P 框架对核心协议进行形式化验证：

- **DataStorage**: 验证 CRAQ 复制协议的正确性
  - `WriteComplete`: 所有提交的写请求最终完成
  - `MonotoneIncreasingVersionNumber`: 版本号单调递增
  - `AllReplicasOnChainUpdated`: 所有副本最终一致
- **RDMASocket**: 验证 RDMA 传输层的正确性
  - `RecvComplete`: 所有发送的数据最终被接收
  - `NoDuplicatePostedBuffers`: 无缓冲区重复投递

测试覆盖：无故障场景、不可靠故障检测器、单/双节点故障、短链（2 副本）、长链（4 副本）。

## 10. 关键配置参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `heartbeat_interval` | 10s | 心跳间隔 |
| `lease_length` | 1min | 租约长度 |
| `chunk_size_list` | 512K, 1M, 2M, 4M, 16M, 64M | Chunk 大小等级 |
| `physical_file_count` | 256 | 每个 Size Class 的 Cluster 文件数 |
| `stripe_size` | 200 | 文件条带宽度 |
| `removed_chunk_expiration` | 3 天 | 已删除 Chunk 的过期时间 |
| `removed_chunk_force_recycled_time` | 1 小时 | 强制回收时间 |
| `disk_low_space_threshold` | 96% | 磁盘空间不足阈值 |
| `emergency_recycling_ratio` | 95% | 紧急回收触发比例 |
| `max_batch_size` | 128 | 最大批量 I/O 数 |
| `max_concurrent_requests` | 32 | 最大并发请求数 |
| `kv_store_type` | RocksDB | 元数据 KV 存储 |
| `rocksdb_block_cache_size` | 8GB | RocksDB 块缓存大小 |
