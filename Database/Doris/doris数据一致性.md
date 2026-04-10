# Apache Doris 数据一致性保证机制

> 基于早期版本，Pre-Nereids，无 Pipeline 引擎

## 一、总体架构

Doris 通过**两层一致性架构**来保证数据一致性：

```
+----------------------------------------------------------------------+
|                         FE (Frontend)                                |
|                                                                      |
|  BDBJE 复制组 (Raft 式共识)                                           |
|  +----------+   +----------+   +----------+                          |
|  |  MASTER  |   | FOLLOWER |   | OBSERVER |                          |
|  | (唯一写) |<=>| (选举者) |<=>| (只读)   |                          |
|  +----------+   +----------+   +----------+                          |
|       |                                                     |
|       | Edit Log (WAL)                                        |
|       v                                                     |
|  GlobalTransactionMgr                                          |
|  DatabaseTransactionMgr                                         |
|  PublishVersionDaemon                                            |
+----------------------------------------------------------------------+
         | Thrift RPC                  | Thrift RPC
         v                             v
+-------------------+          +-------------------+
|   BE Node 1       |  Quorum  |   BE Node 2       |
|   Tablet Replica  |<-------->|   Tablet Replica  |
|   Rowset [1-N]    |          |   Rowset [1-N]    |
|   RocksDB Meta    |          |   RocksDB Meta    |
+-------------------+          +-------------------+
         |                             |
         +-------------+---------------+
                       |
              +-------------------+
              |   BE Node 3       |
              |   Tablet Replica  |
              |   Rowset [1-N]    |
              |   RocksDB Meta    |
              +-------------------+
```

## 二、FE 元数据一致性（Master-Follower 复制）

### 2.1 BDBJE 基础设施

**核心机制**: Berkeley DB Java Edition (BDBJE) 提供 Raft 式共识协议

| 配置项 | 说明 |
|--------|------|
| 复制组名 | `PALO_JOURNAL_GROUP` |
| `master_sync_policy` | Master 写入磁盘的策略 |
| `replica_sync_policy` | Follower 写入磁盘的策略 |
| `replica_ack_policy` | `SIMPLE` / `QUORUM` / `ALL` |

**FE 节点类型**:
- **MASTER** -- 唯一可写节点，处理所有元数据变更
- **FOLLOWER** -- 可选举节点，同步 Master 日志，可提升为 Master
- **OBSERVER** -- 不可选举节点，只同步日志，用于扩展读

### 2.2 Edit Log（元数据 WAL）

所有元数据变更通过 `EditLog` -> `BDBJEJournal` 持久化到 BDBJE：

```
Catalog.createDatabase()  --> EditLog.logCreateDb()    --> BDBJEJournal.write()
Catalog.createTable()     --> EditLog.logCreateTable()  --> BDBJEJournal.write()
Coordinator.commitTxn()   --> EditLog.logLoadDone()    --> BDBJEJournal.write()
...
```

**关键操作类型**: `OP_CREATE_DB`, `OP_DROP_DB`, `OP_CREATE_TABLE`, `OP_DROP_TABLE`, `OP_ADD_REPLICA`, `OP_UPDATE_REPLICA`, `OP_DELETE_REPLICA`, `OP_LOAD_DONE`, `OP_ADD_BACKEND`, `OP_DROP_BACKEND` 等

**写入失败处理**: 如果 BDBJE 写入失败（3 次重试后），FE 进程直接退出（`OP_TIMESTAMP` 除外，防止 Follower 不可用时 Master 退出）

### 2.3 Fencing（防脑裂）

```
FOLLOWER -> MASTER 转换流程:
    |
    +-- 停止 Replayer 线程
    +-- 打开 Edit Log 写权限
    +-- BDBHA.fencing()
    |     +-- 使用 BDBJE epoch DB
    |     +-- putNoOverwrite() 确保只有一个 Master 写入
    +-- 重放剩余日志
    +-- 启动后台线程, isReady = true

MASTER -> 其他状态:
    +-- 直接退出进程 (防止 split-brain)
```

### 2.4 FE-to-FE 心跳

`HeartbeatMgr` 定期执行：
1. 向所有 BE 发送心跳（Thrift `heartbeat()` RPC）
2. 向所有 FE 发送心跳（Thrift `ping()` RPC），Follower 返回 `replayedJournalId`
3. 监控 Follower 日志追赶进度，发现延迟过大的节点

**关键源文件**:
- `fe/.../journal/bdbje/BDBEnvironment.java` - BDBJE 环境管理
- `fe/.../journal/bdbje/BDBJEJournal.java` - 元数据 WAL 实现
- `fe/.../ha/BDBHA.java` - Fencing 实现
- `fe/.../ha/BDBStateChangeListener.java` - 状态监听
- `fe/.../persist/EditLog.java` - 元数据变更日志
- `fe/.../catalog/Catalog.java` - 状态转移控制
- `fe/.../system/HeartbeatMgr.java` - 心跳管理

## 三、BE 数据副本一致性（Quorum 协议）

### 3.1 写事务完整生命周期

```
                        FE (Master)
                          |
    ======== Phase 1: BEGIN ========
    Client 请求 Load (Stream Load / Broker Load / Insert)
    GlobalTransactionMgr.beginTransaction()
    分配 txnId, TransactionState = PREPARE
    持久化到 Edit Log
                          |
    ======== Phase 2: DATA WRITE (各 BE 并行) ========
    FE -> 各 BE: 下发数据
                          |
    BE: DeltaWriter.init()
    +-- TxnManager.prepare_txn() (注册事务)
    +-- 创建 RowsetWriter (PREPARED 状态)
    +-- 创建 MemTable (SkipList 内存缓冲)
                          |
    BE: DeltaWriter.write(tuple)
    +-- 插入 MemTable (排序 + 聚合)
    +-- MemTable 满 -> FlushToken 异步刷盘
         +-- MemTable::flush() -> RowsetWriter -> Segment 文件 (不可变)
         +-- 创建新 MemTable
                          |
    BE: DeltaWriter.close_wait()
    +-- 等待所有 MemTable 刷盘完成
    +-- RowsetWriter::build() -> 产生 Rowset
    +-- TxnManager.commit_txn()
    |     +-- RowsetMetaManager::save() -> RocksDB (持久性关键步骤)
    |     +-- 更新内存 txn_tablet_map
    +-- 上报 TabletCommitInfo(tabletId, backendId) 到 FE
                          |
    ======== Phase 3: COMMIT (FE 仲裁) ========
    Client 调用 commitTransaction(List<TabletCommitInfo>)
    DatabaseTransactionMgr.commitTransaction()
    +-- checkCommitStatus():
    |     对每个 Tablet, 统计成功副本数
    |     successReplicaNum >= quorumReplicaNum ?
    |       (quorum = replicationNum / 2 + 1)
    |         YES -> 分配版本号, 状态 -> COMMITTED, 持久化到 Edit Log
    |          NO -> 抛出 TabletQuorumFailedException, 事务回滚
                          |
    ======== Phase 4: PUBLISH (广播版本) ========
    PublishVersionDaemon 定期执行
    +-- 获取所有 COMMITTED 状态的事务
    +-- 向【所有 BE】发送 PublishVersionTask (包括未参与写入的 BE)
    |     BE: TxnManager.publish_txn()
    |     +-- Rowset::make_visible(version) -> _is_pending = false
    |     +-- RowsetMetaManager::save() -> RocksDB
    |     +-- Tablet::add_rowset() -> _rs_version_map (在 _meta_lock 写锁下)
    +-- 等待 Quorum 个 BE 响应成功
    +-- finishTransaction() -> 状态 -> VISIBLE
                          |
    ======== Phase 5: CLEANUP ========
    FE: removeExpiredAndTimeoutTxns() 清理过期事务状态
    FE: 发送 delete_txn 到 BE 清理未使用 Rowset Meta
```

### 3.2 原子性保证

#### 原子可见性切换

Rowset 在写入后处于 `_is_pending = true` 状态（对查询完全不可见）。`make_visible()` 将其原子切换为可见：

```
写入中:                    切换后:
  _is_pending = true        _is_pending = false
  对查询不可见              对查询可见
  version = 未分配          version = [10-10]

切换过程 (在 Tablet._meta_lock 写锁下):
  Rowset::make_visible(version)
    +-- 设置 _is_pending = false
    +-- 分配 version
  Tablet::add_rowset(rowset)
    +-- 获取 _meta_lock (写锁)
    +-- 加入 TabletMeta._rs_metas
    +-- 加入 Tablet._rs_version_map[version]
    +-- 更新 TimestampedVersionTracker
    +-- Tablet::save_meta() -> RocksDB
    +-- 释放锁

读查询 (在 Tablet._meta_lock 读锁下):
  Tablet::capture_consistent_rowsets()
    +-- 获取 _meta_lock (读锁)
    +-- 返回连续版本链
    +-- 要么看到旧状态，要么看到新状态，永远不会看到中间状态
```

#### Compaction 原子性

```
Compaction 流程:
    |
    +-- 1. pick_rowsets_to_compact()      # 选择候选 Rowset
    +-- 2. do_compaction_impl()            # 合并生成新 Rowset (新 Segment 文件)
    +-- 3. modify_rowsets()                # 原子替换
    |     +-- 获取 _meta_lock (写锁)
    |     +-- 从 _rs_version_map 移除旧 Rowset
    |     +-- 加入 _stale_rs_version_map
    |     +-- 将新 Rowset 加入 _rs_version_map
    |     +-- save_meta() -> RocksDB
    |     +-- 释放锁
    +-- 4. 失败时: gc_output_rowset()      # 清理新 Rowset
```

### 3.3 持久性保证（无传统 WAL）

Doris **没有**传统 WAL，采用**直接写不可变文件 + RocksDB 元数据持久化**：

| 层级 | 机制 | 作用 |
|------|------|------|
| **Segment 文件** | `RowsetWriter` 直接写入 | 数据落盘，带 Adler32 校验，写入后不可变 |
| **Rowset Meta** | `RowsetMetaManager::save()` | RocksDB 自带 WAL 保证元数据持久 |
| **Tablet Meta** | `Tablet::save_meta()` | 保存完整 Rowset 版本链到 RocksDB |
| **FE 事务状态** | `EditLog` + BDBJE | 事务状态持久化到 BDBJE |
| **崩溃恢复** | BE 启动扫描 RocksDB | 发现 pending Rowset 重新注册到 TxnManager，FE 决定 publish 或清理 |

### 3.4 Tablet 锁层次

`Tablet` 类使用多把锁实现并发控制：

| 锁 | 类型 | 保护对象 |
|----|------|---------|
| `_meta_lock` | RWMutex | `_rs_version_map`、`_stale_rs_version_map`、`TabletMeta` |
| `_ingest_lock` | Mutex | 数据写入序列化，防止并发写同一 Tablet |
| `_base_lock` | Mutex | 确保 Base Compaction 串行 |
| `_cumulative_lock` | Mutex | 确保 Cumulative Compaction 串行 |
| `_migration_lock` | RWMutex | 磁盘迁移保护 |

### 3.5 幂等性和重试安全

- `TxnManager::commit_txn()`: 检查重复提交，相同 rowset_id 返回成功（幂等）
- `TxnManager::prepare_txn()`: 允许重新 prepare，支持 BE 端重试
- `DeltaWriter::init()`: 获取 push lock + prepare_txn，确保每个事务只有一个 writer

## 四、副本健康检查与自修复

### 4.1 副本版本追踪

每个副本维护以下版本信息：

```
Replica (FE 侧):
  version            -- 当前可查询版本
  versionHash        -- 版本哈希（完整性校验）
  lastFailedVersion  -- 最高失败版本
  lastSuccessVersion -- 最高成功版本
  schemaHash         -- Schema 版本

版本更新协议 (updateReplicaInfo, 5 种情况):
  Case 1: LFV > LSV      -- LSV 回退到 version (中间版本无效)
  Case 2: LFV 变大        -- LSV 重置为 version
  Case 3: LFV 不变        -- 仅更新 LSV
  Case 4: version >= LFV  -- 重置 LFV (副本恢复)
  Case 5: Bug 恢复情况
```

### 4.2 副本健康检查

`Tablet.getHealthStatusWithPriority()` 采用严格的级联检查：

```
检查流程:
  |
  +-- 1. 存活副本数 vs replicationNum
  |     +-- alive == 0                -> UNRECOVERABLE (VERY_HIGH)
  |     +-- alive < quorum            -> REPLICA_MISSING (HIGH)
  |     +-- alive < replicationNum    -> REPLICA_MISSING (NORMAL)
  |
  +-- 2. 版本完整性
  |     +-- (alive && LFV <= 0 && version >= visibleVersion) 的副本数
  |     +-- 完整副本 == 0              -> UNRECOVERABLE
  |     +-- 完整副本 < quorum          -> VERSION_INCOMPLETE (HIGH)
  |
  +-- 3. 调度可用性 (BE 未下线等)
  +-- 4. 集群/标签放置检查
  +-- 5. Compaction 健康检查 (版本差距过大)
```

**健康副本判定条件**: BE 可用 && `lastFailedVersion <= 0` && `version >= visibleVersion`

### 4.3 自动修复机制

```
HeartbeatMgr (定期)
    |
    +-- BE 心跳 -> 检测 BE 存活
    +-- Tablet Report -> BE 上报副本版本
    |     +-- ReportHandler.tabletReport()
    |     |     +-- Diff: 交集 / BE独有 / FE独有
    |     |     +-- Sync: 版本同步, schema 变更时忽略 SCHEMA_CHANGE 状态副本
    |     |     +-- Recover: 需要修复的 Tablet
    |     +-- 处理: 更新/删除/恢复/重发布版本
    |
    v
TabletChecker (定期遍历所有 Partition 的 Tablet)
    |
    +-- tablet.getHealthStatusWithPriority()
    +-- 不健康 -> 创建 TabletSchedCtx (REPAIR)
    |     +-- 优先级: VERY_HIGH(立即) / HIGH(1x延迟) / NORMAL(2x) / LOW(3x)
    |
    v
TabletScheduler (每秒执行)
    |
    +-- 维护 PriorityQueue<TabletSchedCtx>
    +-- 选择源副本 (版本最高, 健康, 不在同一主机)
    +-- 选择目标 BE (有磁盘空间, 正确集群/标签)
    +-- 创建 CloneTask (版本范围, 超时)
    +-- AgentTaskExecutor 分发到 BE 执行
    |
    v
BE: TabletSyncService
    +-- fetch_rowset()       # 从远端拉取 Rowset 数据
    +-- push_rowset_meta()   # 推送 Rowset Meta 到远端
    +-- fetch_tablet_meta()  # 拉取完整 Tablet Meta
```

### 4.4 一致性校验

`CheckConsistencyJob` 按需触发：
- 向所有副本所在 BE 发送 `CheckConsistencyTask`
- 各 BE 计算指定版本和 schema 的 checksum
- 对比所有副本 checksum
- 全部一致 -> 标记一致
- 不一致 -> 记录日志，触发修复

## 五、Schema 变更一致性

```
Schema Change / Rollup 流程:
    |
    +-- 1. 创建影子副本 (新 schema_hash), 状态 = ALTER
    +-- 2. 从旧副本转换数据到新副本
    +-- 3. ReportHandler.sync() 忽略 SCHEMA_CHANGE 状态副本上报
    |     (防止版本信息被污染)
    +-- 4. Quorum 个影子副本完成
    +-- 5. 原子切换: 旧索引替换为新索引
    +-- 6. 发送 UpdateTabletMetaInfoTask 到所有 BE
    +-- 7. ClearAlterTask 清理旧副本
```

## 六、Delta Write + Compaction 模型

### 6.1 Rowset 与版本链

每次写入产生一个不可变的 **Rowset**（Segment 文件集合），带有版本号：

```
Tablet._rs_version_map:
  Version [1-1]  -> Rowset A (初始基线)
  Version [2-2]  -> Rowset B (Load 1)
  Version [3-3]  -> Rowset C (Load 2)
  Version [4-4]  -> Rowset D (Load 3)
  Version [5-7]  -> Rowset E (Cumulative Compaction: B+C+D)
  Version [1-7]  -> Rowset F (Base Compaction: A+E)

_cumulative_point = 7 (累积层分界点)
  Rowset end_version <= 7  -> Base 区域
  Rowset start_version > 7 -> Cumulative 区域 (增量写入)
```

### 6.2 两级 Compaction

| 类型 | 操作对象 | 频率 | 说明 |
|------|---------|------|------|
| **Cumulative Compaction** | 累积层以上 Rowset | 高频 | 合并近期小 Rowset，处理"热"数据 |
| **Base Compaction** | 版本 1 到累积层分界点 | 低频 | 生成大基线 Rowset，处理"冷"数据，受 `CompactionPermitLimiter` 限流 |

两种 Compaction 都遵循相同的原子替换模式：写锁 -> 替换 Rowset -> 持久化 Meta -> 释放锁。

## 七、一致性保证汇总

| 维度 | 机制 | 一致性级别 | 关键类/文件 |
|------|------|-----------|------------|
| **FE 元数据复制** | BDBJE 复制组 | 强一致性（日志回放） | `BDBEnvironment`, `BDBJEJournal` |
| **FE Leader 选举** | BDBJE ReplicatedEnvironment | 单主（Fencing 防脑裂） | `BDBHA`, `Catalog` |
| **FE 元数据持久化** | Edit Log (BDBJE) | WAL 保证 | `EditLog` |
| **数据写入** | Quorum 两阶段提交 | 多数派成功即提交 | `DatabaseTransactionMgr` |
| **版本发布** | PublishVersionDaemon 广播 | 全节点广播 | `PublishVersionDaemon` |
| **原子可见性** | _is_pending + _meta_lock | 读写锁隔离 | `Rowset`, `Tablet` |
| **数据持久化** | 直接写文件 + RocksDB Meta | 文件不可变 + RocksDB WAL | `RowsetWriter`, `RowsetMetaManager` |
| **Compaction** | _meta_lock 写锁原子替换 | 读不阻塞，原子生效 | `Compaction`, `BaseCompaction` |
| **副本健康** | TabletChecker + HeartbeatMgr | 周期秒级检测 | `TabletChecker`, `HeartbeatMgr` |
| **副本修复** | TabletScheduler + CloneTask | 自动按优先级修复 | `TabletScheduler`, `TabletSyncService` |
| **一致性校验** | CheckConsistencyJob | 按需 Checksum 校验 | `CheckConsistencyJob` |
| **Schema 变更** | 影子副本 + Quorum + 原子切换 | 一致性切换 | `SchemaChangeJobV2` |
| **BE-FE 版本同步** | ReportHandler 周期同步 | 版本协调 | `ReportHandler` |
