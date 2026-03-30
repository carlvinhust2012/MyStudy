# Apache Doris 元数据管理机制

> 基于早期版本，Pre-Nereids，无 Pipeline 引擎

FE 侧元数据管理：

WAL + Image 检查点架构（与 HDFS NameNode 同构）
BDBJE 存储细节（B-Tree KV、滚动数据库、Raft 式共识）
JournalEntity 序列化格式与 60+ 操作类型分类
启动流程时序图（loadImage → editLog.open → 状态转移）
Image 文件格式（25 类元数据固定顺序、XOR Checksum）
Follower Replayer 回放时序图（含 OP_TIMESTAMP 延迟检测）
Master-Follower 状态转移时序图
写入与复制时序图
Checkpoint 五阶段时序图（生成 Image → 推送 Follower → 删除旧日志）

BE 侧元数据管理：

RocksDB 双层存储架构（Level 1 即时 Rowset Meta + Level 2 延迟 Tablet Meta Checkpoint）
OlapMeta 封装（3 个 Column Family、4 字节前缀提取器）
TabletMeta / RowsetMeta 完整字段说明与状态机
启动加载三阶段时序图（Rowset Meta → Tablet Meta → 协调恢复）
Tablet Meta Checkpoint 时序图
BE-FE Tablet Report 时序图
Compaction 元数据原子更新流程
崩溃恢复时序图与后台修复线程

## 一、总体架构

Doris 采用**两层元数据架构**：FE 管理全局元数据（WAL + Image 检查点），BE 管理本地元数据（RocksDB 双层存储）。

```
+========================================================================+
|                            FE (Frontend)                                |
|                                                                        |
|   +------------------+     +------------------+     +----------------+ |
|   |    EditLog       |     |   BDBJE Journal  |     | Image Checkpoint| |
|   | (60+ logXxx方法) |---> | (WAL + 共识复制)  |     | (定期快照)      | |
|   +------------------+     +------------------+     +----------------+ |
|            |                        |                       |          |
|            v                        v                       v          |
|   +--------------------------------------------------------------+     |
|   |                     Catalog (内存)                            |     |
|   |  Database -> Table -> Partition -> Tablet(Replica)           |     |
|   |  User/Role/Priv, TransactionState, LoadJob, AlterJob ...    |     |
|   +--------------------------------------------------------------+     |
|            | Master 写入 (EditLog)  | Follower 回放 (Replayer)         |
+============|========================|=================================+
             |                        |
             v                        v
      BDBJE 复制组 (Raft 式共识)  BDBJE 复制 (日志同步)
      +----------+  +----------+   +----------+  +----------+
      | MASTER   |  | FOLLOWER |   | FOLLOWER |  | OBSERVER |
      | (唯一写) |<=>| (选举者) |<=>| (选举者) |  | (只读)   |
      +----------+  +----------+   +----------+  +----------+


+========================================================================+
|                            BE (Backend)                                |
|                                                                        |
|   DataDir 1                          DataDir 2                         |
|   +---------------------------+      +---------------------------+      |
|   | OlapMeta (RocksDB)        |      | OlapMeta (RocksDB)        |      |
|   |   Column Family "meta":   |      |   Column Family "meta":   |      |
|   |     "tabletmeta_<id>_<sh>"|      |     "tabletmeta_<id>_<sh>"|      |
|   |     "rst_<uid>_<rowset>"  |      |     "rst_<uid>_<rowset>"  |      |
|   +---------------------------+      +---------------------------+      |
|            |                                                        |
|            v                                                        |
|   +---------------------------+      +---------------------------+      |
|   | TabletManager (内存)      |      | TabletManager (内存)      |      |
|   |   tablet_map (分片锁)      |      |   tablet_map (分片锁)      |      |
|   |   Tablet -> TabletMeta    |      |   Tablet -> TabletMeta    |      |
|   |   _rs_version_map         |      |   _rs_version_map         |      |
|   +---------------------------+      +---------------------------+      |
+========================================================================+
             |
             | Tablet Report (Thrift RPC, 定期)
             v
        FE HeartbeatMgr / ReportHandler
```

## 二、FE 元数据管理

### 2.1 WAL + Image 检查点架构

FE 采用与 HDFS NameNode 相同的 **WAL (Write-Ahead Log) + Image Checkpoint** 模式：

```
                    元数据变更流
    ================          ================

    Master FE 内存            BDBJE (磁盘)
    (Catalog 对象)            (WAL 日志)
        |                         |
        | logCreateDb()           |
        | logAddReplica()         |
        | ...                     |
        +---------> EditLog ---> BDBJEJournal.write()
                    |               |
                    |               v
                    |          Journal DB (B-Tree KV)
                    |          Key: journalId (递增 long)
                    |          Value: JournalEntity (opCode + data)
                    |
                    |        (定期, 仅 Master)
                    |               |
                    |               v
                    |          Checkpoint
                    |          saveImage()
                    |               |
                    v               v
               image.50000    (旧日志可删除)
```

### 2.2 BDBJE 存储细节

**BDBJE (Berkeley DB Java Edition)** 同时提供**存储**和**分布式共识**：

```
BDBJE ReplicatedEnvironment
    |
    +-- 数据库名 = 最小 journalId
    |     例: "100" 包含 journal 100~200
    |         "201" 包含 journal 201~400
    |
    +-- Key:   journalId (单调递增, long)
    +-- Value: JournalEntity
    |     {
    |       opCode: short,        // 操作类型
    |       data:   Writable      // 操作负载 (序列化)
    |     }
    |
    +-- 写入: put(key, value)
    |     失败重试 3 次
    |     致命失败 -> System.exit(-1) (OP_TIMESTAMP 除外)
    |
    +-- 滚动: 每 edit_log_roll_num 次写入创建新数据库
    |     允许删除旧日志范围
    |
    +-- 复制: Raft 式共识, Master 写入自动复制到所有节点
    +-- 选举: 自动选举新 Master
    +-- Fencing: putNoOverwrite(epoch) 防脑裂
```

### 2.3 JournalEntity 序列化

每个 journal entry 包含一个 `JournalEntity`，由 `opCode` (short) + `Writable` data 组成：

```
写入:  out.writeShort(opCode); data.write(out);
读取:  switch(opCode) {
          case OP_CREATE_DB:      new DbCreateData();
          case OP_DROP_TABLE:     new DropTableData();
          case OP_ADD_REPLICA:    new AddReplicaData();
          case OP_UPSERT_TXN:     new TransactionState();
          ... (100+ 操作类型)
        }
```

### 2.4 EditLog 体系

`EditLog` 是所有元数据变更的统一入口，提供 60+ `logXxx()` 方法：

```
Catalog.createDatabase()  --> EditLog.logCreateDb()    --> BDBJEJournal.write()
Catalog.createTable()     --> EditLog.logCreateTable()  --> BDBJEJournal.write()
Coordinator.commitTxn()   --> EditLog.logLoadDone()     --> BDBJEJournal.write()
TabletChecker.addReplica()--> EditLog.logAddReplica()   --> BDBJEJournal.write()
```

**回放** (`EditLog.loadJournal()`): 根据每个 opCode 调用对应的 `catalog.replayXxx()` 方法。

### 2.5 操作类型分类

| 类别 | 操作类型 (OP_*) | 数量 |
|------|----------------|------|
| **Database** | CREATE_DB, DROP_DB, ALTER_DB, RENAME_DB, ERASE_DB, RECOVER_DB, UPDATE_DB | 7 |
| **Table** | CREATE_TABLE, DROP_TABLE, RENAME_TABLE, TRUNCATE_TABLE, REPLACE_TABLE, MODIFY_VIEW_DEF, ERASE_TABLE, RECOVER_TABLE | 8 |
| **Partition** | ADD_PARTITION, DROP_PARTITION, MODIFY_PARTITION, RENAME_PARTITION, REPLACE_TEMP_PARTITION, BATCH_MODIFY_PARTITION | 6 |
| **Replica** | ADD_REPLICA, DELETE_REPLICA, UPDATE_REPLICA, SET_REPLICA_STATUS | 4 |
| **Schema Change** | START/FINISH/CANCEL_ROLLUP, START/FINISH/CANCEL_SCHEMA_CHANGE, ALTER_JOB_V2 | 7 |
| **User/Privilege** | CREATE_USER, DROP_USER, GRANT_PRIV, REVOKE_PRIV, SET_PASSWORD, CREATE_ROLE, DROP_ROLE | 7 |
| **Transaction** | UPSERT_TXN_STATE, SAVE_TXN_ID, BATCH_REMOVE_TXNS | 3 |
| **Load Job** | LOAD_START/ETL/LOADING/QUORUM/DONE/CANCEL, CREATE/END/UPDATE_LOAD_JOB, STREAM_LOAD_RECORD | 10 |
| **Node** | ADD/DROP/REMOVE_FRONTEND, ADD/DROP/MODIFY_BACKEND, BACKEND_STATE_CHANGE | 7 |
| **Cluster** | COLOCATE_TABLE_INDEX, META_VERSION, GLOBAL_VARIABLE, MASTER_INFO_CHANGE, TIMESTAMP | 5 |
| **其他** | Export, Sync, Backup/Restore, Resource, Plugin, UDF, SmallFile, EncryptKey, SQL Block Rule | 15+ |

## 三、FE 启动与元数据加载

### 3.1 启动流程时序图

```
  Catalog.initialize()
  |
  +-- 1. 设置 meta 目录 (metaDir, bdbDir, imageDir)
  +-- 2. 获取本地节点信息 (ip, port, nodeType)
  +-- 3. 检查/创建目录 (meta/, bdb/, image/)
  +-- 4. 获取 clusterId 和 role (从 VERSION/ROLE 文件或 helper 节点)
  +-- 5. 创建 EditLog (内部创建 BDBJEJournal)
  +-- 6. loadImage(imageDir)           <<<< 加载快照
  |     |
  |     +-- 找到最新 image 文件 (如 image.100000)
  |     +-- replayedJournalId = 100000
  |     +-- MetaReader.read() 按固定顺序加载 25 类元数据
  |
  +-- 7. editLog.open()                <<<< 打开 BDBJE
  |     |
  |     +-- 打开 ReplicatedEnvironment
  |     +-- 加入 BDBJE 复制组
  |
  +-- 8. BDBJE StateChangeListener 注册
  |     |
  |     +-- 监听角色变化: MASTER / FOLLOWER / OBSERVER / UNKNOWN
  |     +-- 通知 Catalog 状态转移
  |
  +-- 9. 状态转移 (取决于 BDBJE 选举结果)
         |
         +-- [成为 MASTER]  -> transferToMaster()
         +-- [成为 FOLLOWER] -> transferToNonMaster()
         +-- [成为 OBSERVER]  -> transferToNonMaster()
         +-- [UNKNOWN]       -> 等待选举完成
```

### 3.2 Image 文件格式

```
image.100000
  |
  +- Magic String (4 bytes)
  +- Header Length (4 bytes)
  |
  +- [Header] (JSON)
  |   +-- meta_version
  |   +-- replayedJournalId (100000)
  |   +-- nextId (全局自增 ID)
  |   +-- isDefaultClusterCreated
  |
  +- [Body] (25 类元数据, 固定顺序)
  |   +--  1. loadHeader          (版本, 自增 ID)
  |   +--  2. loadMasterInfo      (Master IP, 端口)
  |   +--  3. loadFrontends       (FE 节点列表)
  |   +--  4. loadBackends        (BE 节点列表)
  |   +--  5. loadDb              (所有数据库, 嵌套表/分区/副本)
  |   +--  6. recreateTabletIndex (重建 tablet 倒排索引)
  |   +--  7. loadLoadJob         (Hadoop Load 任务)
  |   +--  8. loadAlterJob        (Schema Change / Rollup 任务)
  |   +--  9. loadRecycleBin      (回收站)
  |   +-- 10. loadGlobalVariable   (全局变量)
  |   +-- 11. loadCluster          (集群元数据)
  |   +-- 12. loadBrokers          (Broker 节点)
  |   +-- 13. loadResources        (外部资源)
  |   +-- 14. loadExportJob        (导出任务)
  |   +-- 15. loadSyncJobs         (同步任务)
  |   +-- 16. loadBackupHandler    (备份仓库)
  |   +-- 17. loadPaloAuth         (用户/角色/权限)
  |   +-- 18. loadTransactionState (进行中事务)
  |   +-- 19. loadColocateIndex    (Colocate 组)
  |   +-- 20. loadRoutineLoadJobs  (例行导入)
  |   +-- 21. loadLoadJobsV2       (Broker/Spark Load)
  |   +-- 22. loadSmallFiles       (小文件)
  |   +-- 23. loadPlugins          (插件)
  |   +-- 24. loadDeleteHandler    (删除信息)
  |   +-- 25. loadSqlBlockRule     (SQL 阻断规则)
  |
  +- [Footer]
  |   +-- Checksum (8 bytes, XOR)
  |   +-- Object Index (名称 -> 偏移量)
  |
  +- Footer Length (8 bytes)
  +- Magic String (4 bytes)
```

### 3.3 Journal 回放时序图

```
Follower/Observer FE
    |
    +-- Replayer 线程 (每 REPLAY_INTERVAL_MS 执行一次)
    |
    +-- replayJournal(-1)
    |     |
    |     +-- getMaxJournalId()           // 从 BDBJE 获取最新日志 ID
    |     +-- toJournalId = maxJournalId
    |     |
    |     +-- JournalCursor cursor = editLog.read(replayedJournalId + 1, toJournalId)
    |     |
    |     +-- while (entity = cursor.next()) != null:
    |     |     |
    |     |     +-- EditLog.loadJournal(catalog, entity)
    |     |     |     |
    |     |     |     +-- switch (entity.opCode):
    |     |     |           case OP_CREATE_DB:    catalog.replayCreateDb(data)
    |     |     |           case OP_DROP_TABLE:   catalog.replayDropTable(data)
    |     |     |           case OP_ADD_REPLICA:  catalog.replayAddReplica(data)
    |     |     |           case OP_TIMESTAMP:    replayedJournalId++ (心跳, 检测延迟)
    |     |     |           ... (100+ 操作类型)
    |     |     |
    |     |     +-- replayedJournalId.incrementAndGet()
    |     |
    |     +-- 回放成功 -> metaReplayState.setOk()
    |     +-- 回放失败 -> canRead = false, isReady = false
    |
    +-- 延迟检测
    |     |
    |     +-- currentTime - synchronizedTimeMs > meta_delay_toleration_second
    |     +-- AND (有新日志 OR 节点状态为 UNKNOWN)
    |     +--   -> 标记元数据过期, 禁止读服务
    |     +--   -> FE 拒绝 SQL 查询, 避免读取过期数据
    |
    +-- OP_TIMESTAMP 机制
          |
          +-- Master 每 10 秒写入 OP_TIMESTAMP
          +-- Follower 回放时更新 synchronizedTimeMs
          +-- 用于检测 Follower 是否落后太多
```

## 四、FE Master-Follower 元数据同步

### 4.1 状态转移时序图

```
         BDBJE 选举完成
              |
              v
  +----- StateChangeListener -----+
  |                               |
  v                               v
  transferToMaster()    transferToNonMaster()
  |                           |
  +-- 停止 Replayer 线程      +-- isReady = false
  +-- canRead = false          |
  +-- editLog.open() (写模式)   +-- [从 UNKNOWN 转来]:
  |                             |    保持 canRead, 允许读
  +-- BDBHA.fencing()           |
  |   (putNoOverwrite 防脑裂)   +-- [首次成为 Follower]:
  |                             |    添加 helper sockets
  +-- replayJournal(-1)         |
  |   (回放所有剩余日志)         +-- 创建/启动 Replayer 线程
  |                             |
  +-- editLog.rollEditLog()     +-- 等待元数据回放完成
  |   (开始新日志数据库)         |
  |                             +-- 启动非 Master 守护线程
  +-- 启动 Master 守护线程:       |   (不启动 checkpoint, heartbeat,
  |   Checkpoint                |    tabletChecker, loadChecker)
  |   HeartbeatMgr              |
  |   TabletChecker             +-- canRead = true
  |   LoadChecker               +-- isReady = true
  |   PublishVersionDaemon         (可提供读服务)
  |
  +-- canRead = true
  +-- isReady = true
```

### 4.2 写入与复制时序图

```
  Client SQL (DDL / Load Commit)
       |
       v
  Master FE
       |
       +-- Catalog.createTable()
       |     |
       |     +-- 内存更新: _idToDb, _idToTable ...
       |     +-- EditLog.logCreateTable(data)
       |           |
       |           +-- BDBJEJournal.write(OP_CREATE_TABLE, data)
       |                 |
       |                 +-- 序列化 JournalEntity
       |                 +-- BDBJE put(journalId++, entity)
       |                 |     |
       |                 |     +-- [Raft 式共识]
       |                 |           Master 写入本地 BDB
       |                 |           复制到 FOLLOWER 1 (ACK)
       |                 |           复制到 FOLLOWER 2 (ACK)
       |                 |           Quorum 确认 -> 返回成功
       |                 |
       |                 v
       |           写入成功 (或 System.exit(-1))
       |
       v
  BDBJE 复制 (自动)
       |
       +-- FOLLOWER 1: 日志写入本地 BDB
       +-- FOLLOWER 2: 日志写入本地 BDB
       +-- OBSERVER:   日志写入本地 BDB (不参与选举)
       |
       v
  Follower Replayer 线程
       |
       +-- 检测到新日志
       +-- replayJournal() -> 回放到内存 Catalog
       +-- Follower 可提供查询服务 (数据已同步)
```

## 五、FE 检查点 (Checkpoint) 机制

### 5.1 检查点时序图

```
  Checkpoint 守护线程 (仅 Master, 定期执行)
  |
  +-- Phase 1: 检查是否需要检查点
  |     |
  |     +-- imageVersion = Storage.getImageSeq() (当前 image 的 journal ID)
  |     +-- checkPointVersion = editLog.getFinalizedJournalId()
  |     +-- imageVersion >= checkPointVersion ? -> 跳过 (已最新)
  |     +-- JVM 内存压力过大 ? -> 跳过
  |
  +-- Phase 2: 生成新 Image (独立 Catalog 实例)
  |     |
  |     +-- 创建 Checkpoint Catalog (独立内存空间)
  |     +-- checkpointCatalog.loadImage(imageDir)
  |     +-- checkpointCatalog.replayJournal(checkPointVersion)
  |     +-- checkpointCatalog.fixBugAfterMetadataReplayed()
  |     +-- checkpointCatalog.saveImage(image.ckpt)
  |     |     |
  |     |     +-- 写入 25 类元数据
  |     |     +-- 计算 XOR Checksum
  |     |     +-- image.ckpt -> rename -> image.<journalId>  (原子重命名)
  |     |
  |     +-- 销毁 Checkpoint Catalog (释放内存)
  |
  +-- Phase 3: 推送 Image 到所有 Follower
  |     |
  |     +-- for each Follower FE:
  |     |     HTTP PUT http://<fe>:<port>/put?version=<journalId>&port=<port>
  |     +-- 追踪成功/失败数量
  |
  +-- Phase 4: 删除旧日志
  |     |
  |     +-- [所有 Follower 都成功] ?
  |     |     YES -> 查询各 Follower 的 /journal_id
  |     |            deleteVersion = min(minFollowerJournalId, checkPointVersion)
  |     |            editLog.deleteJournals(deleteVersion + 1)
  |     |            (删除 BDBJE 中 journalId < deleteVersion 的数据库)
  |     |
  |     |     NO  -> 保留旧日志, 下次重试
  |
  +-- Phase 5: 删除旧 Image 文件
        |
        +-- 删除 image.<version - 1> (保留最近一个)
```

### 5.2 元数据目录结构

```
<doris_meta_dir>/
  |
  +-- VERSION              # clusterId=<id>, token=<token>
  +-- ROLE                 # role=FOLLOWER/OBSERVER, name=<ip>_<port>_<uid>
  |
  +-- image/
  |     +-- image.0        # 初始空镜像
  |     +-- image.50000    # 检查点 (journal 50000)
  |     +-- image.100000   # 最新检查点 (journal 100000)
  |     +-- image.ckpt     # 检查点写入临时文件
  |
  +-- bdbje/               # BDBJE 复制环境
        +-- *.jdb          # BDBJE 数据库文件
        +-- je.info.0      # BDBJE 环境信息
        +-- ...
```

## 六、BE 元数据管理

### 6.1 RocksDB 双层存储架构

BE 采用**两层元数据持久化**模式，平衡崩溃恢复与稳态效率：

```
  Rowset 级别 (Level 1, 即时写入)         Tablet 级别 (Level 2, 延迟检查点)
  =============================          ================================

  RocksDB "meta" Column Family           RocksDB "meta" Column Family
  Key: "rst_<tablet_uid>_<rowset_id>"    Key: "tabletmeta_<tablet_id>_<schema_hash>"
  Value: RowsetMetaPB (单个 rowset)      Value: TabletMetaPB (包含所有 rowset meta)
  |
  | 写入时机:                             | 写入时机:
  |   TxnManager.commit_txn()             |   do_tablet_meta_checkpoint() (定期)
  |   TxnManager.publish_txn()            |
  |   Tablet.add_rowset(need_persist)     |
  |                                       |
  | 优点: 崩溃安全, 单个 rowset 独立恢复   | 优点: 合并存储, 减少碎片, 加速加载
  | 缺点: 条目多, RocksDB 膨胀            | 缺点: 写入延迟, 单次序列化开销大
  |
  |         检查点后删除 (Level 1 -> Level 2)
  |         <--------------------------------
```

### 6.2 OlapMeta (RocksDB 封装)

```
DataDir (每个存储路径)
  |
  +-- OlapMeta
        |
        +-- RocksDB 实例 (<data_path>/meta/)
        +-- 3 个 Column Family:
              |  Index | Name       | 用途                  |
              |--------|------------|-----------------------|
              |    0   | "default"  | 默认 (未用于元数据)      |
              |    1   | "doris"    | Doris 通用操作 (未用)   |
              |    2   | "meta"     | 所有 tablet/rowset 元数据 |
        |
        +-- 4 字节前缀提取器 (prefix extractor)
        |     "rst_" 前缀覆盖 (4 字节)
        |     "tabletmeta_" 前缀部分覆盖
        |     -> 优化前缀范围扫描
        |
        +-- API: get(), put(), remove(), iterate(), key_may_exist()
        +-- 可选同步写: config::sync_tablet_meta -> WriteOptions.sync (fsync)
```

### 6.3 Tablet 元数据

```
TabletMeta (Protobuf: TabletMetaPB)
  |
  +-- 标识信息
  |     +-- _table_id          (int64)   逻辑表 ID
  |     +-- _partition_id      (int64)   分区 ID
  |     +-- _tablet_id         (int64)   Tablet ID
  |     +-- _schema_hash       (int32)   Schema 哈希
  |     +-- _tablet_uid        (TabletUid) 128位唯一标识 (防歧义)
  |     +-- _shard_id          (int32)   磁盘分片
  |     +-- _creation_time     (int64)   创建时间
  |
  +-- 状态信息
  |     +-- _tablet_state      (TabletState) NOTREADY/RUNNING/TOMBSTONED/STOPPED/SHUTDOWN
  |     +-- _tablet_type       (TabletTypePB) DISK / MEMORY
  |     +-- _cumulative_layer_point (int64) 累积层分界点
  |
  +-- Schema
  |     +-- _schema (shared_ptr<TabletSchema>)
  |           +-- keys_type: DUP_KEYS / AGG_KEYS / UNIQUE_KEYS
  |           +-- columns: 列定义列表
  |           +-- short_key_columns: 短键列数
  |           +-- sort_key_idxes: 排序键索引
  |
  +-- Rowset 版本链
  |     +-- _rs_metas: vector<RowsetMeta>      (活跃 rowset)
  |     +-- _stale_rs_metas: vector<RowsetMeta> (已合并, 待过期)
  |
  +-- 其他
        +-- _del_pred_array    (DeletePredicateArray) 条件删除谓词
        +-- _in_restore_mode   (bool) 恢复模式 (抑制 compaction)
        +-- _preferred_rowset_type (BETA_ROWSET)

状态机:
  NOTREADY -> RUNNING -> TOMBSTONED -> STOPPED -> SHUTDOWN
     |           |            |           ^^^
     |           |            +----------++|
     |           +------------------------+|
     +-------------------------------------+
```

### 6.4 Rowset 元数据

```
RowsetMeta (Protobuf: RowsetMetaPB)
  |
  +-- rowset_id:        (RowsetId) 唯一标识
  +-- tablet_id:        (int64) 所属 Tablet
  +-- tablet_uid:       (TabletUid) 所属 Tablet UID
  +-- txn_id:           (int64) 创建事务 ID
  +-- schema_hash:      (int32) Schema 哈希
  +-- rowset_type:      BETA_ROWSET / ALPHA_ROWSET
  +-- rowset_state:     PREPARED -> COMMITTED -> VISIBLE
  +-- start_version:    (int64) 起始版本 (VISIBLE 后分配)
  +-- end_version:      (int64) 结束版本 (VISIBLE 后分配)
  +-- num_rows:         (int64) 行数
  +-- total_disk_size:  (int64) 磁盘占用 (数据+索引)
  +-- data_disk_size:   (int64) 数据文件大小
  +-- index_disk_size:  (int64) 索引文件大小
  +-- zone_maps:        (repeated ZoneMap) 列级 min/max 统计
  +-- delete_predicate: (DeletePredicatePB) 条件删除
  +-- num_segments:     (int64) Segment 数量
  +-- segments_overlap: (SegmentsOverlapPB) OVERLAPPING / NONOVERLAPPING
  +-- empty:            (bool) 空 rowset
  +-- load_id:          (PUniqueId) 导入任务 ID
  +-- creation_time:    (int64) 创建时间
```

## 七、BE 启动与元数据加载

### 7.1 启动加载时序图

```
  StorageEngine::open()
  |
  +-- _init_store_map()
  |     创建 DataDir 对象 (每个存储路径一个)
  |     每个 DataDir 打开自己的 OlapMeta (RocksDB)
  |
  +-- _check_all_root_path_cluster_id()
  |     验证所有路径 cluster ID 一致
  |
  +-- load_data_dirs(dirs)  [并行, 每个路径一个线程]
        |
        v
  DataDir::load()
  |
  +-- Phase 1: 加载所有 Rowset Meta
  |     |
  |     +-- OlapMeta.iterate() prefix="rst_"
  |     +-- 对每条记录:
  |     |     反序列化 -> AlphaRowsetMeta
  |     |     收集到 dir_rowset_metas
  |     |
  |     +-- [结果: 所有 rowset 元数据列表]
  |
  +-- Phase 2: 加载所有 Tablet Meta
  |     |
  |     +-- OlapMeta.iterate() prefix="tabletmeta_"
  |     +-- 对每条记录:
  |     |     解析 tablet_id, schema_hash
  |     |     反序列化 -> TabletMeta
  |     |     TabletManager::load_tablet_from_meta()
  |     |           |
  |     |           +-- Tablet::create_tablet_from_meta()
  |     |           +-- tablet->init()
  |     |           |     加载 meta 中的所有 rowset meta
  |     |           |     填充 _rs_version_map + _stale_rs_version_map
  |     |           +-- 加入 tablet_map (分片锁)
  |     |
  |     +-- 失败处理:
  |           ignore_load_tablet_failure=false -> FATAL 退出
  |
  +-- Phase 3: 协调 Rowset 到 Tablet
        |
        +-- 对 Phase 1 中的每个 rowset meta:
              |
              +-- 通过 tablet_id + schema_hash 找到 Tablet
              +-- [未找到] -> 跳过 (Tablet 已删除)
              |
              +-- [状态: COMMITTED, tablet_uid 匹配]
              |     -> 崩溃恢复: 重新注册到 TxnManager
              |     -> TxnManager::commit_txn(is_recovery=true)
              |     -> FE 后续会 publish 或 clear
              |
              +-- [状态: VISIBLE, tablet_uid 匹配]
              |     -> tablet->add_rowset(rowset, false)
              |     -> 加入 _rs_version_map (不持久化, 已在 RocksDB)
              |
              +-- [状态: VISIBLE, 已在 Tablet Meta 中]
                    -> 跳过 (Level 2 已包含, 无需重复加载)
```

### 7.2 Tablet Meta Checkpoint 时序图

```
  _tablet_checkpoint_tasks_producer_thread (定期触发)
  |
  +-- Tablet::do_tablet_meta_checkpoint()
        |
        +-- 检查: _newly_created_rowset_num > 0 ?
        |     NO  -> 跳过
        |     YES ->
        |
        +-- save_meta()
        |     |
        |     +-- TabletMeta -> TabletMetaPB (Protobuf 序列化)
        |     |   (包含所有活跃 + stale rowset meta)
        |     +-- TabletMetaManager::save()
        |     |     +-- key = "tabletmeta_<id>_<schema_hash>"
        |     |     +-- OlapMeta::put() (RocksDB 写入)
        |     |
        |     v
        |   Level 2 写入完成 (Tablet Meta 含所有 rowset)
        |
        +-- 删除 Level 1 (Rowset Meta)
              |
              +-- 对每个 _is_removed_from_rowset_meta == false 的 rowset:
                    RowsetMetaManager::remove()
                    设置 _is_removed_from_rowset_meta = true
              |
              v
            Level 1 条目已清理
            (RocksDB 中不再有独立的 rowset meta)
```

## 八、BE-FE 元数据同步

### 8.1 Tablet Report 时序图

```
  BE (TaskWorkerPool)
  |
  +-- _report_tablet_worker_thread_callback()
  |     每 config::report_tablet_interval_seconds 唤醒
  |     (或在 FE Master 变更时立即唤醒)
  |
  +-- TabletManager::build_all_report_tablets_info()
  |     |
  |     +-- TxnManager::build_expire_txn_map()
  |     |     收集所有过期事务 -> TabletInfo -> [txn_ids]
  |     |
  |     +-- 遍历所有 Tablet (分片):
  |           +-- Tablet::build_tablet_report_info()
  |                 +-- tablet_id, schema_hash
  |                 +-- row_count, data_size, version_count
  |                 +-- version: 最大连续版本
  |                 +-- version_miss: max_version != max_continuous_version
  |                 +-- used: RUNNING 但无版本 -> false
  |                 +-- partition_id, storage_medium, path_hash
  |                 +-- 附加过期事务 ID
  |
  +-- _handle_report() -> Thrift RPC -> FE
  |
  v
  FE ReportHandler
  |
  +-- tabletReport()
        +-- Diff: 交集 / BE独有 / FE独有
        +-- Sync: 版本同步
        +-- Recover: 版本不完整时创建修复任务
```

### 8.2 FE Master 变更检测

```
  FE -> BE: heartbeat RPC
       |
       +-- 包含 epoch (Master 标识)
       |
  BE HeartbeatServer
       |
       +-- 比较 epoch
       +-- [epoch 变化] -> Master 发生切换
       +-- StorageEngine::notify_listeners()
       +-- 唤醒 tablet report 线程
       +-- 立即上报所有 Tablet 信息到新 Master
```

## 九、Compaction 元数据更新

```
  Compaction::do_compaction_impl()
  |
  +-- 选择输入 Rowset, 执行合并 -> 生成新 Rowset (新 Segment 文件)
  |
  +-- modify_rowsets()
        |
        +-- 获取 Tablet._meta_lock (写锁)
        |
        +-- Tablet::modify_rowsets(output, input)
        |     |
        |     +-- 从 _rs_version_map 移除旧 Rowset
        |     +-- 移动到 _stale_rs_version_map
        |     +-- 将新 Rowset 加入 _rs_version_map
        |     +-- TabletMeta::modify_rs_metas() (更新元数据)
        |           +-- _rs_metas: 删除旧, 添加新
        |           +-- _stale_rs_metas: 添加旧
        |
        +-- Tablet::save_meta()
        |     +-- TabletMeta -> TabletMetaPB
        |     +-- TabletMetaManager::save() -> RocksDB
        |
        +-- 释放锁
        |
        +-- [新 Rowset 需独立持久化]
              RowsetMetaManager::save() -> RocksDB (Level 1)
```

## 十、崩溃恢复与自修复

### 10.1 崩溃恢复时序图

```
  BE 崩溃后重启
  |
  +-- DataDir::load()
        |
        +-- Phase 1: 加载所有 Rowset Meta
        +-- Phase 2: 加载所有 Tablet Meta
        +-- Phase 3: 协调
              |
              +-- [COMMITTED Rowset, tablet 存在]
              |     -> 重新注册到 TxnManager
              |     -> 等待 FE 发送 publish_version 或 clear_transaction
              |
              +-- [VISIBLE Rowset, 不在 Tablet Meta 中]
              |     -> Level 1 独立存在的 rowset
              |     -> 重新加载到 _rs_version_map
              |
              +-- [VISIBLE Rowset, 已在 Tablet Meta 中]
                    -> 已通过 Level 2 加载, 跳过


  FE 崩溃后重启
  |
  +-- Catalog.initialize()
        |
        +-- loadImage() -> 加载最新 Image (内存恢复到检查点时刻)
        +-- editLog.open() -> 加入 BDBJE 复制组
        |
        +-- BDBJE 选举:
              +-- [成为 Master]
              |     -> replayJournal(-1) 回放所有剩余日志
              |     -> 恢复到崩溃前最新状态
              |     -> isReady = true
              |
              +-- [成为 Follower]
                    -> 启动 Replayer 线程
                    -> 回放日志到最新
                    -> isReady = true
```

### 10.2 后台修复线程

```
  StorageEngine 后台线程
  |
  +-- _unused_rowset_monitor_thread (定期)
  |     |
  |     +-- _clean_unused_rowset_metas()
  |     |     扫描 RocksDB 中所有 "rst_" 条目
  |     |     检查是否仍被 Tablet 引用 (rowset_meta_is_useful)
  |     |     未引用 -> 删除 (孤儿 rowset meta)
  |     |
  |     +-- _clean_unused_txns()
  |           扫描 TxnManager 中所有事务
  |           对应 Tablet 不存在 -> 强制回滚
  |
  +-- _garbage_sweeper_thread (定期)
  |     清理 trash 目录和过期 snapshot
  |
  +-- _path_gc_threads (定期)
        清理孤儿路径
```

## 十一、元数据管理汇总

| 维度 | FE | BE |
|------|-----|-----|
| **存储引擎** | BDBJE (B-Tree KV) | RocksDB (LSM-Tree) |
| **持久化模式** | WAL + Image Checkpoint | 双层: Rowset Meta (即时) + Tablet Meta (检查点) |
| **序列化** | JournalEntity (Java Writable) | Protobuf (TabletMetaPB, RowsetMetaPB) |
| **写入方** | 仅 Master | 本地 BE |
| **复制方式** | BDBJE Raft 式共识 | 无复制, 由 FE 驱动 Clone |
| **崩溃恢复** | Image + 日志回放 | Level 1 Rowset Meta 独立恢复 |
| **一致性** | 强一致性 (日志回放) | 最终一致性 (FE 驱动修复) |
| **检查点** | 定期 Image (25 类元数据) | 定期 Tablet Meta Checkpoint |
| **Fencing** | BDBHA.putNoOverwrite(epoch) | 无 (单点写入) |
| **元数据大小** | 数万 Tablet 元数据 | 数千本地 Tablet 完整元数据 |

## 十二、关键源文件

### FE 侧

| 文件 | 说明 |
|------|------|
| `fe/.../journal/Journal.java` | Journal 接口定义 |
| `fe/.../journal/bdbje/BDBJEJournal.java` | BDBJE Journal 实现 (WAL) |
| `fe/.../journal/bdbje/BDBEnvironment.java` | BDBJE 环境管理 |
| `fe/.../journal/bdbje/BDBJournalCursor.java` | 日志游标 (范围读取) |
| `fe/.../journal/JournalEntity.java` | 日志实体 (opCode + data 序列化/反序列化) |
| `fe/.../persist/EditLog.java` | 元数据日志 (60+ logXxx 方法, 回放分发) |
| `fe/.../persist/OperationType.java` | 操作类型常量 (100+) |
| `fe/.../persist/Storage.java` | Image 文件管理 |
| `fe/.../common/MetaReader.java` | Image 读取 (25 类元数据固定顺序) |
| `fe/.../common/MetaWriter.java` | Image 写入 (XOR Checksum) |
| `fe/.../persist/gson/GsonUtils.java` | GSON 序列化配置 |
| `fe/.../catalog/Catalog.java` | 全局元数据管理 (启动/回放/状态转移) |
| `fe/.../master/Checkpoint.java` | 检查点守护线程 |
| `fe/.../ha/BDBHA.java` | Fencing (防脑裂) |
| `fe/.../ha/BDBStateChangeListener.java` | BDBJE 角色变化监听 |

### BE 侧

| 文件 | 说明 |
|------|------|
| `be/src/olap/olap_meta.h/.cpp` | OlapMeta (RocksDB 封装) |
| `be/src/olap/tablet_meta.h/.cpp` | Tablet 元数据 (Protobuf) |
| `be/src/olap/tablet_meta_manager.h/.cpp` | Tablet Meta 读写 (RocksDB Key-Value) |
| `be/src/olap/rowset/rowset_meta.h` | Rowset 元数据 |
| `be/src/olap/rowset/rowset_meta_manager.h/.cpp` | Rowset Meta 读写 |
| `be/src/olap/rowset/beta_rowset_meta.h` | Beta Rowset Meta 实现 |
| `be/src/olap/tablet.h/.cpp` | Tablet (内存状态 + save_meta) |
| `be/src/olap/tablet_manager.h/.cpp` | Tablet 管理 (加载/报告/分片锁) |
| `be/src/olap/data_dir.h/.cpp` | DataDir (加载三阶段) |
| `be/src/olap/storage_engine.h/.cpp` | 存储引擎 (初始化/后台线程) |
| `be/src/olap/txn_manager.h/.cpp` | 事务管理 (prepare/commit/publish) |
| `be/src/olap/compaction.cpp` | Compaction 元数据更新 |
| `be/src/agent/task_worker_pool.cpp` | Tablet Report 工作线程 |
| `be/src/agent/heartbeat_server.cpp` | 心跳服务 (Master 变更检测) |
| `gensrc/proto/olap_file.proto` | TabletMetaPB, RowsetMetaPB 定义 |
