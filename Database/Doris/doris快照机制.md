# Apache Doris 快照机制详解

> 基于早期版本，Pre-Nereids，无 Pipeline 引擎

## 一、Doris 中的两种"快照"概念

Doris 中"快照"指两种不同的概念：

| 概念 | 用途 | 实现方式 | 关键类 |
|------|------|---------|--------|
| **查询快照 (MVCC Snapshot)** | 查询读取一致性视图 | 版本号 + RowsetReader shared_ptr 引用 | `VersionGraph`, `TimestampedVersionTracker`, `Tablet::capture_rs_readers()` |
| **物理快照 (Tablet Snapshot)** | 备份/恢复、Clone、迁移 | 硬链接文件目录 | `SnapshotManager`, `SnapshotLoader`, `SnapshotTask` |

## 二、查询快照（MVCC 读取一致性）

### 2.1 核心原理

Doris **没有**传统的 MVCC 多版本元组机制。每个 Rowset 是不可变的，带有一个版本号范围 `[start, end]`。查询快照本质上是：**在查询开始时确定一个版本号，然后找到覆盖该版本号的最小 Rowset 集合**。

```
查询快照 = 一个版本号 + 一组 RowsetReader (shared_ptr 引用)

version 1    version 2    version 3    version 5
  |            |            |            |
  v            v            v            v
[Rowset A]  [Rowset B]  [Rowset C]  [Rowset D]
 [1-1]        [2-2]        [3-4]        [5-5]

查询版本 5 的快照:  Rowset A + Rowset C + Rowset D
查询版本 2 的快照:  Rowset A + Rowset B
查询版本 3 的快照:  Rowset A + Rowset C
```

### 2.2 查询快照获取流程

```
FE (规划时)                    BE (执行时)
    |                              |
    | Partition.getVisibleVersion()|
    | (获取最新已发布版本号)         |
    |                              |
    | OlapScanNode 设置             |
    | paloRange.setVersion(5)      |
    |                              |
    | [发送 Fragment 到 BE]          |
    |                              |
    |                    OlapScanner.prepare()
    |                              |
    |                    _version = 5
    |                              |
    |                    [获取 _meta_lock 读锁]
    |                              |
    |                    Tablet::capture_rs_readers(
    |                      Version(0, 5)
    |                    )
    |                              |
    |                    +-> VersionGraph::capture_consistent_versions()
    |                    |     寻找从 0 到 5 的最短路径
    |                    |     -> [1-1, 3-4, 5-5]
    |                    |
    |                    +-> _capture_consistent_rowsets_unlocked()
    |                    |     在 _rs_version_map 中查找
    |                    |     -> Rowset A, C, D
    |                    |
    |                    +-> 为每个 Rowset 创建 RowsetReader
    |                    |     (持有 shared_ptr 引用)
    |                    |
    |                    [释放 _meta_lock 读锁]
    |                              |
    |                    扫描数据...
    |                    (Rowset 的 shared_ptr 保证不被删除)
    |                              |
    |                    扫描完成, 释放所有 shared_ptr
    |                    Rowset 可被清理
```

### 2.3 VersionGraph 与最短路径

**数据结构**: 版本图 (邻接表)

```
每个版本 [start, end] 创建两个顶点: start 和 end+1
边: start <-> end+1 (双向边)
每个顶点的边按目标顶点值【降序】排列

示例 (版本 [1-1], [2-2], [3-4], [5-5]):

顶点图:
  1 --- 2 --- 3 --- 5 --- 6
                  |
                  4 (从 3 到 4)

查询版本 5 (即 [0, 5]):
  从顶点 1 开始, 目标顶点 6
  顶点 1 的边: [2] -> 选择 2 (最大 <= 6)
  顶点 3 的边: [5, 4] -> 跳过 4 (<= 5 不行), 选择... 取 5? 不, 边是 3->5 和 3->4
  实际: 从顶点 1, 目标 6, BFS 最短路径:
    1 -> 2 (Version [1-1])
    3 -> 5 (Version [3-4])
    6 (到达目标, Version [5-5])
  路径: [1-1], [3-4], [5-5] (3 个 Rowset, 最优)
```

**关键**: 因为边按降序排列，贪心算法总能找到覆盖最大范围的第一条合法边，产生最少 Rowset 的路径。

### 2.4 TimestampedVersionTracker（版本生命周期追踪）

**作用**: 追踪哪些过期版本路径仍被进行中的查询需要，决定何时可以清理。

```
初始化 (Tablet 加载时):
  construct_versioned_tracker(rs_metas, stale_metas)
    +-- 从活跃 Rowset 构建版本图
    +-- 从过期 Rowset 构建 stale_version_path_map

新增版本 (写入/Load):
  add_version([2-2])
    +-- 在版本图中添加边

新增过期路径 (Compaction):
  add_stale_path_version(stale_rs_metas)
    +-- 包装为 TimestampedVersion (version + create_time)
    +-- 存入 _stale_version_path_map[path_id]

过期检查 (后台清理线程):
  capture_expired_paths(now - retention_sec)
    +-- 遍历 _stale_version_path_map
    +-- 如果 max_create_time < now - retention_sec
    +-- 标记为可删除
```

**注意**: 这是基于时间的保留策略，不是引用计数。默认保留时间 `tablet_rowset_stale_sweep_time_sec`。

### 2.5 过期 Rowset 清理

```
TabletManager 后台线程 (定期)
    |
    +-- Tablet::delete_expired_stale_rowset()
          |
          +-- 1. 计算 expired_sweep_endtime = now - retention_sec
          |
          +-- 2. _timestamped_version_tracker.capture_expired_paths()
          |     找到所有过期的 path_id
          |
          +-- 3. 对每个过期 path:
          |     +-- 从 _stale_version_path_map 移除
          |     +-- 从 _version_graph 移除边
          |
          +-- 4. 安全检查:
          |     模拟删除后, 验证主版本链 [0, max_version] 仍完整
          |     不完整 -> recover_versioned_tracker() 恢复
          |
          +-- 5. 实际删除:
          |     +-- 从 _stale_rs_version_map 移除
          |     +-- StorageEngine::add_unused_rowset() -> _unused_rowsets
          |
          v
StorageEngine::start_delete_unused_rowset() (最终物理删除)
    |
    +-- 遍历 _unused_rowsets
    +-- 如果 use_count() == 1 (无任何引用)
    |     +-- rowset->remove() (删除磁盘文件)
    +-- 如果 use_count() > 1 (仍有 Scanner 持有 shared_ptr)
          +-- 保留, 下次再检查
```

**安全网**: 即使 Rowset 被移到 `_unused_rowsets`，只要任何扫描器仍持有 `shared_ptr`（`use_count() > 1`），文件就不会被物理删除。

### 2.6 并发模型

```
写入 (Compaction/Load):          读取 (查询扫描):
                                |
获取 _meta_lock (写锁)           获取 _meta_lock (读锁)
  |                               |
modify_rowsets()                 capture_rs_readers()
  |                               |
  +-- 旧 Rowset -> _stale         +-- 查找 Rowset
  +-- 新 Rowset -> _rs_version    +-- 创建 RowsetReader (shared_ptr)
  |                               |
释放 _meta_lock (写锁)           释放 _meta_lock (读锁)
                                |
                                独立扫描 (shared_ptr 保证 Rowset 不被删除)
                                |
                                扫描完成, 释放 shared_ptr
```

读写锁隔离：多个读可以并发，写阻塞所有读写，但写锁持有时间很短（仅修改内存数据结构）。

**关键源文件**:
- `be/src/olap/version_graph.h/.cpp` - 版本图和生命周期追踪
- `be/src/olap/tablet.h/.cpp` - 快照捕获和过期清理
- `be/src/olap/tablet_meta.h` - 持久化的 Rowset 元数据
- `be/src/olap/storage_engine.h/.cpp` - _unused_rowsets 管理
- `be/src/olap/tablet_manager.cpp` - 后台清理线程
- `fe/.../planner/OlapScanNode.java` - 版本号设置
- `fe/.../catalog/Partition.java` - visibleVersion 水位线

## 三、物理快照（备份/恢复/Clone）

### 3.1 总体架构

```
+----------------------------------------------------------------------+
|                         FE (Frontend)                                |
|                                                                      |
|  BackupHandler (MasterDaemon, 3s 轮询)                               |
|  +-- BackupJob / RestoreJob (状态机)                                  |
|  +-- Repository / RepositoryMgr (远程存储管理)                         |
|  +-- S3Storage / BrokerStorage (存储后端)                              |
|                                                                      |
+----------------------------------------------------------------------+
         | Agent Task Protocol (Thrift RPC)              | Progress Report
         v                                                v
+-------------------+                              +-------------------+
|   BE Node         |                              |   Remote Storage  |
|   SnapshotManager |<-- upload/download ------>   |   (S3 / HDFS /   |
|   SnapshotLoader  |                              |    BOS / Broker)  |
|                   |                              |                   |
|   /data/snapshot/ |                              |                   |
+-------------------+                              +-------------------+
```

### 3.2 备份流程时序图

```
  User              BackupHandler        BackupJob          BE                  Remote Storage
    |                    |                    |               |                      |
    | BACKUP db.tbl      |                    |               |                      |
    | TO repo LABEL lbl  |                    |               |                      |
    |------------------->|                    |               |                      |
    |                    |                    |               |                      |
    |                    | backup()            |               |                      |
    |                    | 验证 repo/db/table  |               |                      |
    |                    |-------------------->|               |                      |
    |                    | 创建 BackupJob      |               |                      |
    |                    |   state = PENDING   |               |                      |
    |                    | 持久化到 Edit Log   |               |                      |
    |                    |                    |               |                      |
    |   ============ PENDING -> SNAPSHOTING ============     |                      |
    |                    | run() [3s 轮询]     |               |                      |
    |                    |                    |               |                      |
    |                    | prepareAndSendSnapshotTask()       |                      |
    |                    |                    |               |                      |
    |                    | 遍历 table -> partition -> index -> tablet                      |
    |                    | chooseReplica(): 选版本 >= visibleVersion 的健康副本            |
    |                    |                    |               |                      |
    |                    | 创建 SnapshotTask   |               |                      |
    |                    | (MAKE_SNAPSHOT)     |               |                      |
    |                    |-------------------->|               |                      |
    |                    |                    | SnapshotTask  |                      |
    |                    |                    | (tablet_id,   |                      |
    |                    |                    |  schema_hash, |                      |
    |                    |                    |  version,     |                      |
    |                    |                    |  version_hash)|                      |
    |                    |                    |-------------->|                      |
    |                    |                    |               |                      |
    |                    |                    |               | SnapshotManager::    |
    |                    |                    |               |  make_snapshot()      |
    |                    |                    |               |   |                   |
    |                    |                    |               |   +-- 生成快照路径:    |
    |                    |                    |               |   |  /data/snapshot/    |
    |                    |                    |               |   |   <ts>.<seq>.<tm>/ |
    |                    |                    |               |   +-- 获取 header 读锁|
    |                    |                    |               |   +-- capture_         |
    |                    |                    |               |   |  consistent_       |
    |                    |                    |               |   |  rowsets(0, ver)   |
    |                    |                    |               |   +-- 对每个 Rowset:   |
    |                    |                    |               |   |  HARD LINK 文件     |
    |                    |                    |               |   |  到快照目录 (零拷贝) |
    |                    |                    |               |   +-- 复制 tablet     |
    |                    |                    |               |   |  meta (.hdr 文件)   |
    |                    |                    |               |   +-- 释放 header 读锁|
    |                    |                    |               |                      |
    |                    |                    |  snapshot     |                      |
    |                    |                    |  path + files |                      |
    |                    |                    |<--------------|                      |
    |                    | finishTablet       |               |                      |
    |                    | SnapshotTask()     |               |                      |
    |                    |<-------------------|               |                      |
    |                    | 记录 SnapshotInfo  |               |                      |
    |                    |   state = SNAPSHOTING              |                      |
    |                    |                    |               |                      |
    |   ============ SNAPSHOTING -> UPLOAD_SNAPSHOT ============                      |
    |                    | waitingAll         |               |                      |
    |                    | SnapshotsFinished() |               |                      |
    |                    |-------------------->|               |                      |
    |                    |                    | 全部快照完成   |                      |
    |                    |<-------------------|               |                      |
    |                    |                    |               |                      |
    |                    | uploadSnapshot()    |               |                      |
    |                    |-------------------->|               |                      |
    |                    | 按 BE 分组, 创建    |               |                      |
    |                    | UploadTask (x3/BE) |               |                      |
    |                    |                    |               |                      |
    |                    |                    | UploadTask    |                      |
    |                    |                    | (src, dest,   |                      |
    |                    |                    |  broker/s3)   |                      |
    |                    |                    |-------------->|                      |
    |                    |                    |               |                      |
    |                    |                    |               | SnapshotLoader::     |
    |                    |                    |               |  upload()            |
    |                    |                    |               |   |                   |
    |                    |                    |               |   +-- 列出本地文件    |
    |                    |                    |               |   +-- 计算每个文件    |
    |                    |                    |               |   |  MD5 校验和        |
    |                    |                    |               |   +-- 上传到远程存储   |
    |                    |                    |               |   |  文件名加 MD5 后缀: |
    |                    |                    |               |   |  file.md5hash      |
    |                    |                    |               |   +-- 定期汇报进度    |
    |                    |                    |               |                      |
    |                    |                    | 上传完成       |    文件 + MD5        |
    |                    |                    |<--------------|<---------------------|
    |                    | finishUploadTask()  |               |                      |
    |                    |<-------------------|               |                      |
    |                    |                    |               |                      |
    |   ============ UPLOADING -> SAVE_META -> UPLOAD_INFO -> FINISHED ===========
    |                    |                    |               |                      |
    |                    | saveMetaInfo()      |               |                      |
    |                    |-------------------->|               |                      |
    |                    | +-- 写 BackupMeta    |               |                      |
    |                    | |   (表 Schema)     |               |                      |
    |                    | +-- 写 BackupJobInfo |               |                      |
    |                    | |   (JSON 清单)     |               |                      |
    |                    |                    |               |                      |
    |                    | 发送 ReleaseSnapshotTask -> 清理 BE 本地快照目录           |
    |                    |                    |               |                      |
    |                    | uploadMetaAnd       |               |                      |
    |                    | JobInfoFile()       |               |                      |
    |                    |-------------------->|               |                      |
    |                    |                    |               | 上传 __meta + __info  |
    |                    |                    |               |----------->           |
    |                    |                    |               |                      |
    |                    | state = FINISHED    |               |                      |
    |<-------------------|                    |               |                      |
    | 备份完成             |                    |               |                      |
```

### 3.3 恢复流程时序图

```
  User              BackupHandler        RestoreJob          BE                  Remote Storage
    |                    |                    |               |                      |
    | RESTORE db.tbl     |                    |               |                      |
    | FROM repo LABEL lbl|                    |               |                      |
    |------------------->|                    |               |                      |
    |                    |                    |               |                      |
    |   ============ PENDING (checkAndPrepareMeta) ============                   |
    |                    |                    |               |                      |
    |                    |                    | 下载 __info    |                      |
    |                    |                    |--------------->|                      |
    |                    |                    | 下载 __meta    |                      |
    |                    |                    |--------------->|                      |
    |                    |                    |               |                      |
    |                    |                    | 创建新表结构   |                      |
    |                    |                    | (新 ID)        |                      |
    |                    |                    | 创建新副本     |                      |
    |                    |                    | (CreateReplica |                      |
    |                    |                    |  Task -> BE)   |                      |
    |                    |                    |               |                      |
    |                    |                    | 发送 SnapshotTask (空快照)            |
    |                    |                    |-------------->|                      |
    |                    |                    | state = SNAPSHOTING              |                      |
    |                    |                    |               |                      |
    |   ============ SNAPSHOTING -> DOWNLOAD ============     |                      |
    |                    |                    |               |                      |
    |                    |                    | downloadSnapshots()              |                      |
    |                    |                    |-------------->|                      |
    |                    |                    | DownloadTask   |                      |
    |                    |                    | (remote_path, |                      |
    |                    |                    |  local_path)   |                      |
    |                    |                    |               |                      |
    |                    |                    |               | SnapshotLoader::     |
    |                    |                    |               |  download()          |
    |                    |                    |               |   +-- 列出远程文件   |
    |                    |                    |               |   +-- 下载 + MD5 校验 |
    |                    |                    |               |   +-- 替换文件中的    |
    |                    |                    |               |     tablet ID         |
    |                    |                    | state = DOWNLOADING             |                      |
    |                    |                    |               |                      |
    |   ============ DOWNLOADING -> COMMIT ============     |                      |
    |                    |                    |               |                      |
    |                    |                    | DirMoveTask    |                      |
    |                    |                    | (snapshot ->   |                      |
    |                    |                    |  tablet data)  |                      |
    |                    |                    |-------------->|                      |
    |                    |                    |               | SnapshotLoader::     |
    |                    |                    |               |  move()              |
    |                    |                    |               |   +-- convert_rowset  |
    |                    |                    |               |   |  _ids() (新 rowset |
    |                    |                    |               |   |  ID, 重命名文件)   |
    |                    |                    |               |   +-- HARD LINK 快照  |
    |                    |                    |               |     文件到 tablet 目录 |
    |                    |                    |               |   +-- 原子替换数据目录 |
    |                    |                    | state = COMMITTING             |                      |
    |                    |                    |               |                      |
    |                    |                    | PublishVersionTask              |
    |                    |                    |-------------->|                      |
    |                    |                    |               |                      |
    |                    |                    | state = FINISHED               |                      |
    |<-------------------|                    |               |                      |
    | 恢复完成             |                    |               |                      |
```

### 3.4 文件处理方式

| 操作 | 方式 | 说明 |
|------|------|------|
| **创建快照** | **Hard Link** | `BetaRowset::link_files_to()` / `AlphaRowset::link_files_to()` 零拷贝创建硬链接 |
| **Tablet Meta** | **Copy** | `.hdr` 文件从内存元数据序列化生成（非硬链接） |
| **上传到远程** | **Full Copy** | 读取本地文件，写入远程存储 (Broker / S3) |
| **从远程下载** | **Full Copy** | 从远程读取，写入本地快照目录 |
| **恢复到 Tablet** | **Hard Link** | `SnapshotLoader::move()` 硬链接快照文件到 tablet 数据目录 |
| **Rowset ID 转换** | **Rename** | 恢复时生成新 Rowset ID，重命名 Segment 文件，重写 `.hdr` |
| **释放快照** | **Delete** | `SnapshotManager::release_snapshot()` 删除整个快照目录 |

**远程存储目录结构**:
```
<location>/__palo_repository_<repo_name>/
  __repo_info                          # 仓库元数据 (JSON)
  __ss_<label>/                        # 备份快照目录
    __meta__<md5>                      # 表 Schema 元数据
    __info_<timestamp>.<md5>           # 备份清单 (JSON)
    __ss_content/                      # 数据文件
      __db_<db_id>/
        __tbl_<tbl_id>/
          __part_<part_id>/
            __idx_<idx_id>/
              __<tablet_id>/
                __<schema_hash>/
                  <data_files>.<md5>  # 文件名 + MD5 校验后缀
```

### 3.5 FE-BE 任务协议

| 任务类型 | FE 创建 | BE 处理 | 方向 |
|---------|---------|---------|------|
| `MAKE_SNAPSHOT` | `SnapshotTask` | `SnapshotManager::make_snapshot()` | FE -> BE |
| `UPLOAD` | `UploadTask` | `SnapshotLoader::upload()` | FE -> BE |
| `DOWNLOAD` | `DownloadTask` | `SnapshotLoader::download()` | FE -> BE |
| `MOVE` | `DirMoveTask` | `SnapshotLoader::move()` | FE -> BE |
| `RELEASE_SNAPSHOT` | `ReleaseSnapshotTask` | `SnapshotManager::release_snapshot()` | FE -> BE |
| 进度汇报 | - | `snapshotLoaderReport()` RPC | BE -> FE |
| 任务完成 | - | `finishTask()` RPC | BE -> FE |

### 3.6 快照清理

| 清理方式 | 触发条件 | 实现 |
|---------|---------|------|
| **显式释放** | BackupJob/RestoreJob 完成后 | FE 发送 `ReleaseSnapshotTask` -> BE 删除快照目录 |
| **自动 GC** | 快照超过 `snapshot_expire_time_sec` (默认 48h) | `StorageEngine` 后台 GC 线程扫描 `/data/snapshot/` 删除过期目录 |
| **取消清理** | Job 被取消 | 发送 `ReleaseSnapshotTask`，清理本地临时目录 |

### 3.7 BackupJob / RestoreJob 状态机

**BackupJob**:
```
PENDING --[发送 SnapshotTask]--> SNAPSHOTING
SNAPSHOTING --[全部快照完成]--> UPLOAD_SNAPSHOT
UPLOAD_SNAPSHOT --[发送 UploadTask]--> UPLOADING
UPLOADING --[全部上传完成]--> SAVE_META
SAVE_META --[保存元数据到本地]--> UPLOAD_INFO
UPLOAD_INFO --[上传 __meta + __info]--> FINISHED
(任何错误或超时) --> CANCELLED
```

**RestoreJob**:
```
PENDING --[下载 __info, 创建表结构]--> SNAPSHOTING
SNAPSHOTING --[全部快照就绪]--> DOWNLOAD
DOWNLOAD --[发送 DownloadTask]--> DOWNLOADING
DOWNLOADING --[全部下载完成]--> COMMIT
COMMIT --[发送 DirMoveTask]--> COMMITTING
COMMITTING --[PublishVersion]--> FINISHED
(任何错误或超时) --> CANCELLED
```

**并发控制**:
- `BackupHandler` 使用 `ReentrantLock` (seqlock)，同一数据库同一时间只允许一个备份/恢复任务
- FE 重启后，从 Edit Log 恢复任务；若处于 `SNAPSHOTING`/`UPLOADING` 状态，重置为 `PENDING` 重新执行

**关键源文件**:
- `fe/.../backup/BackupHandler.java` - 备份恢复调度器
- `fe/.../backup/BackupJob.java` - 备份任务状态机
- `fe/.../backup/RestoreJob.java` - 恢复任务状态机
- `fe/.../backup/SnapshotInfo.java` - 快照描述信息
- `fe/.../backup/Repository.java` - 远程存储管理
- `fe/.../backup/S3Storage.java` - S3 存储后端
- `fe/.../backup/BrokerStorage.java` - Broker 存储后端
- `fe/.../task/SnapshotTask.java` - 创建快照任务
- `fe/.../task/UploadTask.java` - 上传任务
- `fe/.../task/DownloadTask.java` - 下载任务
- `fe/.../task/DirMoveTask.java` - 目录移动任务
- `fe/.../task/ReleaseSnapshotTask.java` - 释放快照任务
- `be/src/olap/snapshot_manager.h/.cpp` - 快照创建与释放
- `be/src/runtime/snapshot_loader.h/.cpp` - 上传/下载/移动
- `be/src/olap/rowset/beta_rowset.cpp` - 硬链接实现
- `be/src/olap/rowset/alpha_rowset.cpp` - 硬链接实现

## 四、两种快照的关系

```
                     查询快照 (MVCC)
                          |
                          | capture_consistent_rowsets()
                          v
                   Tablet._rs_version_map
                   Tablet._stale_rs_version_map
                          |
                          | link_files_to() (Hard Link)
                          v
                   物理快照目录 (/data/snapshot/...)
                          |
            +-------------+-------------+
            |             |             |
       Clone 使用    Backup 上传   Restore 下载
            |             |             |
            v             v             v
       BE 间复制    远程存储      远程 -> 本地
```

物理快照的创建依赖查询快照的 `capture_consistent_rowsets()` 方法来获取一致的 Rowset 集合，然后通过 Hard Link 零拷贝创建文件目录。
