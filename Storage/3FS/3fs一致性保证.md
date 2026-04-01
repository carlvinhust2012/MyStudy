# 3FS 一致性保证机制全面分析

## 概述

3FS（Fire-Flyer File System）是 DeepSeek 开源的高性能分布式文件系统，专为 AI 训练和推理工作负载设计。其架构包含四个核心组件：**集群管理器（mgmtd）**、**元数据服务（meta）**、**存储服务（storage）** 和 **客户端（client）**。

3FS 采用**强一致性**模型，通过多层机制保证数据与元数据的一致性。本文从以下五个维度进行系统分析：

1. 数据副本一致性 — 基于 CRAQ 协议
2. 元数据一致性 — 基于 FoundationDB SSI 事务
3. 集群状态一致性 — 基于 FDB 租约 + 状态机
4. 客户端一致性 — 重试、版本校验与 Failover
5. 故障恢复一致性 — 数据同步与链重构

---

## 一、数据副本一致性（CRAQ 协议）

### 1.1 协议核心：Chain Replication with Apportioned Queries

3FS 的存储层实现了 **CRAQ（Chain Replication with Apportioned Queries）** 协议，采用"写全读任一"（write-all-read-any）策略。每个文件 chunk 被复制到一条由多个存储目标（Storage Target）组成的链上。

**关键设计**：
- **写请求**发送到链头（head），沿链逐级传播到链尾（tail）
- **读请求**可以发送到链上任意节点，充分利用所有副本的读带宽
- 每条链有**单调递增的链版本号**（chain version），链重构时递增

### 1.2 写流程的一致性保证

写请求在链上的执行流程如下（代码见 `src/storage/service/ReliableUpdate.cc`、`src/storage/store/ChunkReplica.cc`）：

```
步骤 1: 链版本校验
   - 存储服务检查写请求中的 chain version 是否与本地最新已知版本匹配
   - 若不匹配，拒绝请求（防止过期写操作）

步骤 2: 数据拉取
   - 通过 RDMA Read 操作从客户端/前驱节点拉取写数据
   - 若客户端/前驱失败，RDMA Read 超时后中止写操作

步骤 3: Chunk 锁获取
   - 从锁管理器获取待更新 chunk 的锁
   - 并发写同一 chunk 被阻塞（所有写操作在 head 串行化）

步骤 4: 版本管理（COW）
   - 读取 chunk 的 committed version 到内存
   - 应用更新，存储为 pending version
   - 每个 chunk 最多保存两个版本：committed（v）和 pending（u = v + 1）

步骤 5: 链式传播
   - 若当前节点是 tail：原子地将 committed version 替换为 pending version，向前驱发送 ACK
   - 若不是 tail：将写请求转发给后继节点
   - 更新 committed version 时，将当前 chain version 存入 chunk 元数据

步骤 6: ACK 回传
   - ACK 到达后，将 committed version 替换为 pending version
   - 继续向前驱传播 ACK
   - 释放本地 chunk 锁
```

**一致性要点**：
- **链版本校验**防止在链重构后发送的旧写请求被错误接受
- **Chunk 级锁**在 head 节点串行化所有写操作，避免写冲突
- **COW（Copy-on-Write）** 语义：旧 committed version 在所有读操作完成后才被回收
- **Tail 原子提交**：只有 tail 节点执行 committed -> pending 的原子替换，确保写操作在链上全量达成后才算完成

### 1.3 读流程的一致性保证

读请求的处理逻辑（`src/storage/store/ChunkReplica.cc` 中的 `kChunkNotCommit` 状态码处理）：

1. **仅有 committed version**：直接返回给客户端（强一致性读）
2. **同时存在 committed 和 pending version**：
   - 返回特殊状态码 `kChunkNotCommit` 通知客户端
   - 客户端可选择等待短时间后重试（获取最新 committed version）
   - 或发起**放松读**（relaxed read）直接获取 pending version（弱一致性读，需要客户端主动选择 `allowReadUncommitted` 选项）

这种设计相比标准 CRAQ 做了简化：不向 tail 查询最新版本号，而是让客户端在遇到 pending version 时自行决策。

### 1.4 故障场景下的一致性

当链中某个节点（如 B）在转发写请求后立即故障：
- Cluster Manager 检测到 B 的故障后，将 B 标记为 offline 并移到链尾
- 广播更新后的 chain table（chain version 递增）
- Head（A）收到新的 chain table 后，向新的后继（C）重新转发
- C 可能尚未收到最新 chain table 而拒绝请求，A 会持续重试直到 C 接受

---

## 二、元数据一致性（FoundationDB SSI 事务）

### 2.1 FoundationDB 作为一致性基石

3FS 使用 **FoundationDB（FDB）** 作为元数据存储后端，利用其提供的 **Serializable Snapshot Isolation（SSI）** 事务保证：

- 所有元数据操作（inode、目录项、文件会话等）作为键值对存储在 FDB 中
- Meta 服务无状态，可水平扩展，故障转移时客户端可连接任意 meta 服务
- FDB SSI 保证事务的**可串行化**，即所有事务的效果等价于某种串行执行顺序

### 2.2 事务执行模型：OperationDriver

所有元数据操作通过 `OperationDriver`（`src/meta/store/Operation.h`）执行，核心循环为：

```
while (true) {
    if (timeout) break;
    result = runAndCommit(txn, operation);
    if (success) break;
    operation.retry(error);  // 清除已累积的事件
    retry = strategy.onError(txn, error);
    if (not retryable) break;
}
```

**关键特性**：
- **读操作**不提交事务，可使用 GRV 缓存（`FDB_TR_OPTION_USE_GRV_CACHE`）降低延迟
- **写操作**执行后提交，失败时整个操作从头重试（`retry()` 清除累积状态）
- **指数退避重试**：`FDBRetryStrategy` 对 `not_committed`（冲突）错误使用 FDB 内置的智能退避策略

### 2.3 Snapshot Read vs Serializable Read

这是 3FS 元数据一致性最核心的设计模式：

| 读类型 | API | 冲突集合 | 用途 |
|--------|-----|---------|------|
| Serializable Read | `get()` / `getRange()` | 自动加入读冲突集 | 需要防止并发修改的场景 |
| Snapshot Read | `snapshotGet()` / `snapshotGetRange()` | 不加入读冲突集 | 路径解析、批量扫描等 |

**`PathResolveOp`**（`src/meta/store/PathResolve.h`）显式使用 snapshot read 进行路径遍历，避免在遍历大量 inode 和目录项时产生不必要的冲突。调用方（create、remove、rename）在关键操作前手动将关键 key 加入读冲突集：

```cpp
// 防止并发创建同名文件
entry.addIntoReadConflict(txn);
// 防止父目录被并发删除
Inode(parentId).addIntoReadConflict(txn);
```

### 2.4 文件创建一致性

文件创建操作（`src/meta/store/ops/BatchOperation.cc`）：

1. **父 inode 保护**：`Inode(parentId).addIntoReadConflict(txn)` — 防止父目录被并发删除
2. **目录项冲突检测**：`entry.addIntoReadConflict(txn)` — 防止两个并发 `create("/dir/file")` 同时成功
3. **原子创建**：inode 和目录项在同一事务中创建，若并发创建同名文件，一个事务会因目录项冲突而重试
4. **幂等性**：UUID 字段用于检测 `commit_unknown_result` 后的重复创建

### 2.5 文件删除一致性

删除操作（`src/meta/store/ops/Remove.cc`）：

1. 通过 snapshot read 进行路径解析
2. 对父目录进行权限检查（sticky bit）
3. 将目录项和 inode 加入读冲突集后原子删除：
   ```cpp
   co_await entry.addIntoReadConflict(txn);
   co_await inode.addIntoReadConflict(txn);
   co_await entry.remove(txn);
   co_await inode.remove(txn);
   ```
4. **递归删除**：祖先 inode 通过 `Inode::loadAncestors()` 以 serializable read 加载（加入冲突集），同时检测循环目录
5. **幂等性**：通过 `Idempotent` 系统处理 `commit_unknown_result`

### 2.6 文件重命名（Rename）一致性

Rename 是最复杂的元数据操作（`src/meta/store/ops/Rename.cc`）：

1. **并发解析**：源路径和目标路径通过 `collectAll` 并行 snapshot read 解析
2. **重复检测**：检查 `dstResult.dirEntry.uuid == req.uuid` 检测已完成的 rename
3. **循环检测**：`checkLoop()` 加载目标路径所有祖先（serializable read），验证源目录不是目标目录的后代
4. **nlink 检查**：检查祖先 `nlink == 0`（已删除），防止 rename 到已删除的目录
5. **四键冲突集**：
   ```cpp
   Inode(srcParentId).addIntoReadConflict(txn);
   srcDirEntry.addIntoReadConflict(txn);
   Inode(dstParentId).addIntoReadConflict(txn);
   DirEntry(dstParentId, destName).addIntoReadConflict(txn);
   ```
6. **原子交换**：在同一事务中删除源目录项、替换/删除目标目录项、创建新目标目录项

### 2.7 幂等性系统

`Idempotent` 系统（`src/meta/store/Idempotent.h`）保证 remove/rename 操作的幂等性：

- 在 FDB 中存储 `Record<clientId, requestId, timestamp, result>`
- Key 格式：`requestId + clientId`（requestId 在前以分散 FDB 分片负载）
- 执行前检查是否有历史结果，有则直接返回
- 成功后将结果与元数据变更原子写入同一事务
- 依赖 `retryMaybeCommitted` 标志在 FDB 返回 `commit_unknown_result` 时仍重试

### 2.8 文件长度一致性

文件长度跟踪为 `VersionedLength(length, truncateVer)` 对：

- **定期上报**：客户端每 5 秒向 meta 服务上报各写打开文件的最大写入位置
- **fsync/close 精确查询**：从存储服务查询最后一个 chunk 的实际长度
- **Rendezvous Hashing 分发**：文件长度更新任务通过 inode ID 和 rendezvous hash 分散到不同 meta 服务，避免并发更新同一文件长度导致的事务冲突
- **一致性不变式**：在相同 `truncateVer` 下，文件长度只能增大不能缩小

---

## 三、集群状态一致性（Mgmtd）

### 3.1 单写者模型：FDB 租约（Lease）

Cluster Manager 采用**单写者**架构，通过 FDB 租约保证全局唯一主节点：

- 只有租约持有者处理写请求
- 租约校验在 FDB 事务提交时再次验证，防止"脑裂"（即使内存中租约看似有效）
- 非主节点在租约过期后自动提升为主节点

### 3.2 写入串行化：Writer Mutex + Store-then-Memory

所有集群状态的变更遵循"写 FDB 先于更新内存"模式：

1. 通过协程互斥锁（coroutine mutex）串行化所有变更
2. 先将变更原子写入 FDB 单个事务
3. 再更新内存状态
4. 在 FDB 和内存中同时递增版本计数器

### 3.3 存储目标状态机

存储目标有两套状态——**公共状态**（Public State，分发到所有服务/客户端）和**本地状态**（Local State，仅 Cluster Manager 和存储服务知道）：

| 公共状态 | 可读 | 可写 | 含义 |
|---------|------|------|------|
| serving | Y | Y | 正常服务 |
| syncing | N | Y | 数据恢复中 |
| waiting | N | N | 等待恢复 |
| lastsrv | N | N | 服务下线，原为最后一个服务节点 |
| offline | N | N | 下线或介质故障 |

状态转换由 `generateNewChain()` 确定性状态机驱动：
- 每个 update 周期最多提升一个节点（at-most-one promotion per cycle）
- 本地状态作为触发事件，驱动公共状态转换
- 离线节点被移到链尾

### 3.4 防止重启期间的不一致决策

- **新生链宽限期**（newborn chain grace period）：新创建的链在短时间内不做状态转换
- **旧心跳拒绝**：Cluster Manager 拒绝过期的旧心跳，防止已重启节点用旧状态干扰新决策
- **可疑租约间隔**：检测租约是否可疑地快速续约
- **启动标志**：启动时的标记防止在领导权转移期间做出不一致决策

---

## 四、客户端一致性机制

### 4.1 链版本校验与路由

客户端通过 `RoutingInfo`（`src/client/mgmtd/RoutingInfo.h`）缓存和监听集群配置变更：

- 通过 `MgmtdClient` 从 Cluster Manager 获取路由信息，包含 chain table 和目标状态
- 写操作前检查 chain version，若不匹配则等待最新路由信息到达后重试
- `RoutingInfoListener` 模式允许订阅者实时感知路由变更

### 4.2 读重试与 Pending Version 处理

当客户端收到 `kChunkNotCommit`（chunk 有 pending version）时：

1. 默认行为：等待短时间后重试（最终获取 committed version，强一致性）
2. 可选行为：设置 `allowReadUncommitted` 选项发起放松读，直接获取 pending version（适用于可容忍短暂数据陈旧的场景）

### 4.3 存储目标选择

`TargetSelection`（`src/client/storage/TargetSelection.cc`）实现目标选择策略：

- 读操作：在链上所有 serving 状态的节点中均匀分配读流量（实现读带宽聚合）
- 写操作：始终发送到链 head 节点

### 4.4 元数据客户端 Failover

`MetaClient`（`src/client/meta/MetaClient.cc`）和 `ServerSelectionStrategy`（`src/client/meta/ServerSelectionStrategy.cc`）：

- 当一个 meta 服务不可用时，客户端自动切换到其他可用 meta 服务
- **无粘性会话**：任意 meta 服务可处理任意请求
- 写请求通过 Distributor 的 rendezvous hash 被路由到负责特定 inode 的 meta 服务

### 4.5 写去重通道

`UpdateChannelAllocator`（`src/client/storage/UpdateChannelAllocator.cc`）：

- 为写操作分配去重通道
- 同一 chunk 的并发写操作在同一通道排队，避免重复写

---

## 五、故障恢复一致性

### 5.1 存储目标恢复流程

当存储服务重启或介质故障恢复后（`src/storage/sync/ResyncWorker.cc`）：

**恢复节点（Returning Service）视角：**
1. 周期性从 Cluster Manager 拉取最新 chain table
2. 不发送心跳，直到所有本地目标在最新 chain table 中被标记为 offline
3. 恢复期间的写请求始终是全 chunk 替换写（full-chunk-replace），直接更新 committed version
4. 接收前驱的 dump-chunkmeta 请求，收集本地 chunk 元数据返回
5. 收到 sync-done 消息后，将目标本地状态设为 up-to-date

**前驱节点（Existing Predecessor）视角：**
1. 开始向恢复中的后继转发正常写请求（转发的是全 chunk 替换写）
2. 发送 dump-chunkmeta 请求收集后继的 chunk 元数据
3. 比较本地和远端元数据，决定需要传输的 chunk

**Chunk 传输决策规则：**
- 仅本地存在的 chunk：需要传输
- 仅远端存在的 chunk：需要删除
- 本地 chain version > 远端：需要传输
- chain version 相同但 committed version 不等于远端 pending version：需要传输
- 否则：两个副本相同或正在被进行中的写请求更新

### 5.2 恢复期间的锁保护

Chunk 传输过程中：
1. 先获取 chunk 锁
2. 读取 chain version、committed version number 和 chunk 内容
3. 发送全 chunk 替换请求
4. 释放 chunk 锁

这确保了恢复期间的 chunk 传输不会与并发写操作冲突。

### 5.3 Chunk 引擎的 COW 语义

Chunk Engine（`src/storage/chunk_engine/`）在本地存储层面也采用 COW 语义：

1. **open/close**：从 RocksDB 加载元数据，重建 chunk allocator 状态
2. **get**：通过哈希表缓存获取 chunk 元数据和引用计数句柄，O(1) 平均复杂度
3. **update**：分配新 chunk 后再修改数据，旧 chunk 在所有句柄释放前保持可读
4. **commit**：通过 RocksDB write batch 原子提交 chunk 元数据，同步刷新内存缓存

物理块使用位图管理，回收时仅清除位图标志（设置 0），实际存储空间保留并优先用于后续分配，减少磁盘碎片。

---

## 六、一致性保证总结

| 层次 | 机制 | 一致性级别 |
|------|------|-----------|
| **数据副本** | CRAQ 协议（链式复制 + tail 原子提交） | 强一致性（默认读） |
| **数据副本** | Pending version + 客户端重试 | 可选放松一致性 |
| **元数据** | FoundationDB SSI 事务 | 可串行化 |
| **元数据** | Snapshot read + 手动冲突集 | 精细冲突控制 |
| **元数据** | 幂等性系统（Idempotent） | 重复请求安全 |
| **元数据** | Rendezvous Hashing 分发 | 减少热点冲突 |
| **集群状态** | FDB 租约 + 单写者 | 强一致性 |
| **集群状态** | Store-then-Memory 模式 | 持久化优先 |
| **集群状态** | 确定性状态机 | 可重现的状态转换 |
| **客户端** | Chain version 校验 | 防止过期操作 |
| **客户端** | Meta 服务 Failover | 无状态透明切换 |
| **故障恢复** | 全 chunk 替换写 + 锁保护 | 恢复期间无数据丢失 |
| **故障恢复** | Chunk Engine COW + RocksDB 原子提交 | 本地存储一致性 |

### 核心设计原则

1. **写串行化、读并行化**：写操作在链头通过 chunk 锁串行化，读操作可并行访问任意副本
2. **持久化优先**：所有关键状态变更先写 FDB/RocksDB，再更新内存
3. **尾节点提交**：只有链尾节点执行原子提交，确保写操作在全链路达成后才算成功
4. **冲突检测优于预防**：依赖 FDB SSI 的冲突检测和自动重试，而非悲观锁
5. **幂等设计**：remove/rename 等关键操作支持安全重试，应对 `commit_unknown_result`
6. **无状态服务**：Meta 服务无状态，任意节点可处理任意请求，极大简化故障恢复

