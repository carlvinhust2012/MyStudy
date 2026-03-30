# Apache Doris StreamLoad 流程分析

> 基于早期版本，Pre-Nereids，无 Pipeline 引擎

## 一、总体架构

StreamLoad 采用 **HTTP Redirect** 架构：FE 不处理实际数据，仅负责认证、选择目标 BE 并返回重定向，所有数据写入由 BE 完成。

```
+-------------------+       HTTP PUT        +-------------------+
|   MySQL Client    |  --- (data+header) -->|     FE Node       |
|   (curl / SDK)    |  <-- 307 Redirect --- |  LoadAction v1/v2 |
+-------------------+                        +-------------------+
       |                                             |
       |  HTTP PUT (data + header,                   |  Thrift RPC
       |   with redirect target BE)                  |  (loadTxnBegin / streamLoadPut / loadTxnCommit)
       v                                             v
+------------------------------------------------------------------+
|                         BE Node (目标)                              |
|                                                                  |
|  StreamLoadAction (HTTP Handler)                                   |
|       |                                                          |
|       +-- StreamLoadPipe (生产者-消费者缓冲)                       |
|       |       |                                                  |
|       |       +-- on_chunk_data() -> put()                       |
|       |       |                                                  |
|       v       v                                                  |
|  PlanFragmentExecutor                                              |
|       |                                                          |
|       +-- StreamLoadScanNode (从 Pipe 读取数据)                    |
|       |                                                          |
|       +-- OlapTableSink (数据路由)                                 |
|              |                                                   |
|              +-- LoadChannelMgr                                   |
|                     |                                            |
|                     +-- LoadChannel (per transaction)             |
|                            |                                     |
|                            +-- TabletsChannel (per tablet)        |
|                                   |                              |
|                                   +-- DeltaWriter                 |
|                                          |                       |
|                                          +-- MemTable (SkipList) |
|                                          |                       |
|                                          +-- RowsetWriter         |
|                                                 |                |
|                                                 v                |
|                                          Segment 文件 (不可变)    |
+------------------------------------------------------------------+
```

## 二、完整执行流程时序图

```
Client              FE                  BE(Target)          LoadChannelMgr       TabletsChannel       DeltaWriter
  |                   |                     |                     |                    |                   |
  |                   |   === Phase 1: HTTP 请求 FE (认证 + 重定向) ===               |                   |
  |                   |                     |                     |                    |                   |
  |  HTTP PUT         |                     |                     |                    |                   |
  |  /api/{db}/{tbl}  |                     |                     |                    |                   |
  |  (with auth)      |                     |                     |                    |                   |
  |------------------>|                     |                     |                    |                   |
  |                   |  LoadAction.on_req  |                     |                    |                   |
  |                   |  +-- 认证检查         |                     |                    |                   |
  |                   |  +-- 权限校验         |                     |                    |                   |
  |                   |  +-- 选择目标 BE      |                     |                    |                   |
  |                   |  |  (选择副本健康且   |                     |                    |                   |
  |                   |  |   有磁盘空间的 BE)  |                     |                    |                   |
  |                   |                     |                     |                    |                   |
  |  HTTP 307         |                     |                     |                    |                   |
  |  Redirect         |                     |                     |                    |                   |
  |  (BE endpoint)    |                     |                     |                    |                   |
  |<------------------|                     |                     |                    |                   |
  |                   |                     |                     |                    |                   |
  |                   |   === Phase 2: HTTP 请求 BE (开始事务) ===                     |                   |
  |                   |                     |                     |                    |                   |
  |  HTTP PUT         |                     |                     |                    |                   |
  |  (data + header)  |                     |                     |                    |                   |
  |------------------>|                     |                     |                    |                   |
  |                   |                     |  on_header()        |                    |                   |
  |                   |                     |------------------->|                    |                   |
  |                   |                     |                     |                    |                   |
  |                   |                     |  loadTxnBegin RPC   |                    |                   |
  |                   |                     |  (begin transaction)|                   |                   |
  |                   |                     |------------------->|                    |                   |
  |                   |  GlobalTransactionMgr                   |                    |                   |
  |                   |  .beginTransaction()                     |                    |                   |
  |                   |  分配 txnId        |                     |                    |                   |
  |                   |<-------------------|                     |                    |                   |
  |                   |                     |                     |                    |                   |
  |                   |   === Phase 3: 执行计划生成 (RPC 到 FE) ===                    |                   |
  |                   |                     |                     |                    |                   |
  |                   |                     |  streamLoadPut RPC  |                    |                   |
  |                   |                     |  (发送 plan 请求)    |                   |                   |
  |                   |                     |------------------->|                    |                   |
  |                   |  StreamLoadPlanner  |                     |                    |                   |
  |                   |  .plan()            |                     |                    |                   |
  |                   |  生成执行计划:       |                     |                    |                   |
  |                   |  StreamLoadScanNode |                     |                    |                   |
  |                   |  -> OlapTableSink   |                     |                    |                   |
  |                   |  -> ExchangeNode    |                     |                    |                   |
  |                   |  (如跨节点)         |                     |                    |                   |
  |                   |<-------------------|                     |                    |                   |
  |                   |                     |                     |                    |                   |
  |                   |   === Phase 4: 数据接收与管道写入 ===                           |                   |
  |                   |                     |                     |                    |                   |
  |  HTTP Body chunks |                     |                     |                    |                   |
  |  (CSV/JSON/Parquet)|                    |                     |                    |                   |
  |------------------>|                     |                     |                    |                   |
  |                   |                     |  on_chunk_data()    |                    |                   |
  |                   |                     |  StreamLoadPipe     |                    |                   |
  |                   |                     |  .put(block)        |                    |                   |
  |  |                |                     |  [生产者]            |                   |                   |
  |  |                |                     |                     |                    |                   |
  |  |                |                     |   === Phase 5: BE 端执行 (并行) ===                      |                   |
  |  |                |                     |                     |                    |                   |
  |  |                |                     |  PlanFragmentExecutor.open()              |                   |
  |  |                |                     |  StreamLoadScanNode.open()               |                   |
  |  |                |                     |  从 Pipe.get() 拉取数据 [消费者]         |                   |
  |  |                |                     |                     |                    |                   |
  |  |                |                     |  OlapTableSink.send()                     |                   |
  |  |                |                     |  (按分桶键路由)      |                   |                   |
  |  |                |                     |-------------------->|                    |                   |
  |  |                |                     |                     |  open()            |                   |
  |  |                |                     |                     |-------------------->|                   |
  |  |                |                     |                     |                    |  DeltaWriter      |
  |  |                |                     |                     |                    |  .write(tuple)    |
  |  |                |                     |                     |                    |  .init()          |
  |  |                |                     |                     |                    |  -> prepare_txn   |
  |  |                |                     |                     |                    |  -> create        |
  |  |                |                     |                     |                    |     RowsetWriter  |
  |  |                |                     |                     |                    |  -> create        |
  |  |                |                     |                     |                    |     MemTable      |
  |  |                |                     |                     |                    |                   |
  |  |                |                     |                     |  add_row()          |                   |
  |  |                |                     |                     |-------------------->|  MemTable.insert()|
  |  |                |                     |                     |                    |  (排序+聚合)      |
  |  |                |                     |                     |                    |                   |
  |  |                |                     |                     |                    |  MemTable 满:      |
  |  |                |                     |                     |                    |  -> flush()        |
  |  |                |                     |                     |                    |  -> RowsetWriter   |
  |  |                |                     |                     |                    |     -> Segment文件 |
  |  |                |                     |                     |                    |  -> 新 MemTable    |
  |  |                |                     |                     |                    |                   |
  |  |                |                     |                     |  close()           |                   |
  |  |                |                     |                     |-------------------->|                   |
  |  |                |                     |                     |                    |  等待刷盘         |
  |  |                |                     |                     |                    |  commit_txn()     |
  |  |                |                     |                    |                    |  -> RowsetMeta    |
  |  |                |                     |                     |                    |     -> RocksDB    |
  |  |                |                     |                     |                    |  上报 CommitInfo  |
  |  |                |                     |                     |<--------------------|                   |
  |  |                |                     |                     |                    |                   |
  |                   |                     |  close_wait()       |                    |                   |
  |                   |                     |  等待所有数据写入完成 |                    |                   |
  |                   |                     |                     |                    |                   |
  |                   |   === Phase 6: 事务提交 ===                                          |                   |
  |                   |                     |                     |                    |                   |
  |                   |                     |  loadTxnCommit RPC  |                    |                   |
  |                   |                     |  (txnId + CommitInfos)                    |                   |
  |                   |                     |------------------->|                    |                   |
  |                   |  DatabaseTransactionMgr                  |                    |                   |
  |                   |  .commitTransaction()                     |                    |                   |
  |                   |  +-- checkCommitStatus():                 |                    |                   |
  |                   |  |   对每个 Tablet 统计成功副本数          |                    |                   |
  |                   |  |   successReplicaNum >= quorum ?        |                    |                   |
  |                   |  |     YES -> 分配版本号, COMMITTED       |                    |                   |
  |                   |  |     NO  -> TabletQuorumFailedException |                    |                   |
  |                   |  +-- 持久化到 Edit Log (BDBJE)           |                    |                   |
  |                   |                     |                     |                    |                   |
  |                   |   === Phase 7: 版本发布 (PublishVersionDaemon) ===               |                   |
  |                   |                     |                     |                    |                   |
  |                   |  PublishVersionDaemon (定期)              |                    |                   |
  |                   |  获取 COMMITTED 事务                       |                    |                   |
  |                   |  向所有 BE 发送 PublishVersionTask        |                    |                   |
  |                   |                     |                     |                    |                   |
  |                   |                     |  publish_txn RPC    |                    |                   |
  |                   |                     |<-------------------+                    |                   |
  |                   |                     |  Rowset.make_visible()                  |                   |
  |                   |                     |  Tablet.add_rowset()                     |                   |
  |                   |                     |  save_meta() -> RocksDB                  |                   |
  |                   |                     |                     |                    |                   |
  |                   |                     |  loadTxnCommit 响应 |                    |                   |
  |                   |                     |------------------->|                    |                   |
  |                   |                     |                     |                    |                   |
  |                   |   === Phase 8: 返回结果 ===                                        |                   |
  |                   |                     |                     |                    |                   |
  |  HTTP 200 OK      |                     |                     |                    |                   |
  |  {"TxnId": 123,   |                     |                     |                    |                   |
  |   "Status":"OK"}  |                     |                     |                    |                   |
  |<------------------|                     |                     |                    |                   |
  |                   |                     |                     |                    |                   |
```

## 三、各阶段详细说明

### 3.1 Phase 1: FE 认证与重定向

FE 的 `LoadAction`（v1/v2）负责接收客户端的 HTTP 请求，完成认证和路由选择：

```
LoadAction.on_req()
    |
    +-- 1. 认证检查
    |     +-- 解析 Basic Auth (user:password)
    |     +-- PasswordAuthenticator.authenticate()
    |
    +-- 2. 权限校验
    |     +-- 检查用户对目标表的 LOAD 权限
    |     +-- 检查集群资源限制 (mem_limit, running_load jobs 等)
    |
    +-- 3. 选择目标 BE
    |     +-- LoadAction (v1): 随机选择可用 BE
    |     +-- LoadAction (v2): 基于副本健康度 + 磁盘可用空间选择最优 BE
    |     +-- 确保选择持有目标表分桶副本的 BE
    |
    +-- 4. 返回 HTTP 307 Redirect
          +-- Location: http://{be_host}:{be_http_port}
            /api/{db}/{tbl}/_stream_load
          +-- 携带原始请求参数 (label, columns, format 等)
```

**FE 不处理实际数据**，仅做一次轻量级认证和路由，这种设计：
- 避免数据经过 FE 造成瓶颈
- BE 直接写入本地磁盘，减少网络跳数
- FE 可横向扩展，不受数据吞吐量限制

### 3.2 Phase 2: BE 开始事务

BE 接收重定向后的 HTTP 请求，在 `on_header()` 阶段通过 Thrift RPC 向 FE 申请事务：

```
StreamLoadAction.on_header()
    |
    +-- 解析 HTTP Header (label, columns, format, db, tbl 等)
    +-- 创建 StreamLoadContext
    +-- 调用 FE: beginTxn RPC
    |     +-- GlobalTransactionMgr.beginTransaction(db, tbl, label)
    |     +-- 分配 txnId
    |     +-- 检查 label 唯一性 (防止重复导入)
    |     +-- TransactionState = PREPARE
    |     +-- 持久化到 Edit Log
    +-- 返回 txnId
```

**Label 幂等性**: `label` 是 StreamLoad 的唯一标识。如果 FE 发现相同 label 的事务已存在：
- `VISIBLE` / `COMMITTED` → 返回成功（幂等）
- `PREPARED` / `LOADING` → 返回错误（正在处理中）

### 3.3 Phase 3: 执行计划生成

BE 通过 Thrift RPC 请求 FE 生成执行计划：

```
StreamLoadAction._process_put()
    |
    +-- streamLoadPut RPC -> FE
    |     |
    |     +-- StreamLoadPlanner.plan(txnId, db, tbl, columns)
    |     |     |
    |     |     +-- 1. 解析 columns 定义
    |     |     |     +-- 简单列映射: "k1,k2,v1"
    |     |     |     +-- 列转换: "columns=k1,tmp1,k2=tmp2" (重命名)
    |     |     |     +-- 表达式列: "columns=k1,ts=from_unixtime(dt)"
    |     |     |     +-- WHERE 过滤: "where k1 > 10"
    |     |     |
    |     |     +-- 2. 构建 PlanNode 树
    |     |     |     +-- StreamLoadScanNode (从 Pipe 读取原始数据)
    |     |     |     +-- SelectNode (列转换/表达式计算)
    |     |     |     +-- OlapTableSink (数据路由 + 写入)
    |     |     |     +-- [可选] ExchangeNode (如分桶跨节点)
    |     |     |
    |     |     +-- 3. 序列化为 TPlan (Thrift)
    |     |           +-- 描述符表 (DescriptorTable)
    |     |           +-- ScanRange
    |     |           +-- DataSink 信息
    |     |
    |     +-- 返回 TExecPlanFragmentParams
    |
    +-- PlanFragmentExecutor.prepare()
          +-- ExecNode::create_tree() (TPlan -> ExecNode 树)
```

### 3.4 Phase 4: 数据接收（StreamLoadPipe）

数据接收与执行并行进行，通过 `StreamLoadPipe` 解耦：

```
                     StreamLoadPipe (有界阻塞队列)
                     ============================

  HTTP Chunk Reader    Buffer[block_0, block_1, ..., block_N]    PlanFragmentExecutor
  (生产者)                            |                          (消费者)
       |                               |
       +-- on_chunk_data()             |
       +-- 将原始数据解析为 Block       |
       +-- pipe->put(block) ---------> put() [队列满则阻塞]
                                       |
                                   get() <--+
                                             |
                                   StreamLoadScanNode.get_next()
```

**StreamLoadPipe 关键参数**:
- 缓冲区大小可配置 (`buffer_size`)
- 队列满时阻塞生产者（反压）
- EOF 标记：`pipe->finish()` 通知消费者数据结束
- 错误传播：`pipe->cancel()` 取消整个流程

### 3.5 Phase 5: BE 端数据写入

这是 StreamLoad 的核心执行阶段，数据从 Pipe 流经执行计划最终写入存储引擎：

```
PlanFragmentExecutor.open() / get_next()
    |
    +-- StreamLoadScanNode
    |     +-- 从 StreamLoadPipe.get() 拉取数据
    |     +-- 数据格式解析 (CSV/JSON/Parquet)
    |     +-- 输出为 Block (列式内存格式)
    |
    +-- SelectNode (可选)
    |     +-- 列重命名、表达式计算、WHERE 过滤
    |
    +-- OlapTableSink.send(block)
          |
          +-- 根据分桶键计算 hash(bucket_num)
          +-- 路由到对应的 TabletsChannel
          |
          v
    LoadChannelMgr
          |
          +-- get_or_create_load_channel(txnId)
          +-- LoadChannel.add_batch(tablet_id, block)
                |
                +-- TabletsChannel[tablet_id].add_block(block)
                      |
                      v
                DeltaWriter.write(tuple)
                      |
                      +-- MemTable.insert()
                      |     +-- SkipList 排序
                      |     +-- AGGREGATE KEY 模式下合并相同 key
                      |     +-- MemTable 满阈值 -> 触发 flush
                      |
                      +-- MemTable.flush() (异步)
                      |     +-- FlushToken
                      |     +-- RowsetWriter->add_block()
                      |     +-- 生成 Segment 文件 (不可变)
                      |     +-- 创建新 MemTable
                      |
                      +-- DeltaWriter.close_wait()
                            +-- 等待所有 MemTable flush 完成
                            +-- RowsetWriter.build() -> 产生 Rowset
                            +-- TxnManager.commit_txn()
                            |     +-- RowsetMetaManager::save() -> RocksDB
                            |     +-- 更新内存 txn_tablet_map
                            +-- 上报 TabletCommitInfo(tabletId, backendId) 到 FE
```

#### 数据路由详情 (OlapTableSink)

```
OlapTableSink
    |
    +-- open():
    |     +-- _load_channel = LoadChannelMgr.get_or_create(txnId)
    |     +-- 为每个 Tablet 创建 TabletsChannel
    |
    +-- send(block):
    |     +-- 遍历 block 中每一行
    |     +-- 根据分布类型路由:
    |     |     +-- HASH 分布: hash(distribution_columns) % bucket_num
    |     |     +-- RANDOM 分布: random() % bucket_num
    |     +-- TabletsChannel[tablet_id].add_block(row_block)
    |
    +-- close():
          +-- TabletsChannel.close() -> DeltaWriter.close_wait()
          +-- 收集所有 TabletCommitInfo
```

### 3.6 Phase 6: 事务提交

BE 完成数据写入后，调用 FE 的 commit RPC：

```
StreamLoadAction
    |
    +-- 收集所有 TabletCommitInfo
    +-- loadTxnCommit RPC -> FE
          |
          v
    DatabaseTransactionMgr.commitTransaction(txnId, commitInfos)
          |
          +-- checkCommitStatus():
          |     |
          |     +-- 对每个 Tablet:
          |     |     +-- 统计成功写入的副本数 (来自 commitInfos)
          |     |     +-- successReplicaNum >= quorumReplicaNum ?
          |     |     |     (quorum = replicationNum / 2 + 1)
          |     |     |       YES -> 副本数据完整
          |     |     |       NO  -> TabletQuorumFailedException
          |     |     |             事务标记为 ABORTED
          |     |
          |     +-- 所有 Tablet 通过 quorum 检查
          |
          +-- 分配版本号 (单调递增)
          +-- TransactionState -> COMMITTED
          +-- EditLog.logCommitTxn() 持久化到 BDBJE
```

### 3.7 Phase 7: 版本发布

`PublishVersionDaemon` 异步将 COMMITTED 事务发布为可见：

```
PublishVersionDaemon (FE, 定期执行)
    |
    +-- 获取所有 COMMITTED 状态的事务
    +-- 对每个事务:
          |
          +-- 向【所有 BE】发送 PublishVersionTask
          |     (包括未参与写入的 BE, 确保全局可见)
          |
          +-- BE: TxnManager.publish_txn(version)
          |     +-- Rowset.make_visible(version)
          |     |     +-- _is_pending = false
          |     |     +-- 分配 version 号
          |     +-- Tablet.add_rowset(rowset)
          |     |     +-- 获取 _meta_lock (写锁)
          |     |     +-- 加入 _rs_version_map
          |     |     +-- Tablet::save_meta() -> RocksDB
          |     |     +-- 释放锁
          |     +-- 返回成功
          |
          +-- 等待 Quorum 个 BE 响应成功
          +-- finishTransaction() -> TransactionState = VISIBLE
```

**注意**: 版本发布是异步的，BE 返回的 HTTP 响应中 `Status: OK` 表示事务已提交（COMMITTED），但不一定已经可见（VISIBLE）。客户端可通过 `SHOW LOAD` 查看事务的 `TransactionStatus`。

### 3.8 Phase 8: 结果返回

```
BE StreamLoadAction
    |
    +-- 构建 JSON 响应
    |     {
    |       "TxnId": 123,
    |       "Label": "my_load_label",
    |       "Status": "OK",              // OK / PublishTimeout / Fail
    |       "Message": "...",
    |       "NumberTotalRows": 100000,
    |       "NumberLoadedRows": 99998,
    |       "NumberFilteredRows": 2,
    |       "NumberUnselectedRows": 0,
    |       "LoadBytes": 10485760,
    |       "LoadTimeMs": 3200,
    |       "BeginTxnTimeMs": 5,
    |       "StreamLoadPutTimeMs": 10,
    |       "ReadDataTimeMs": 3000,
    |       "WriteDataTimeMs": 3100,
    |       "CommitAndPublishTimeMs": 85
    |     }
    |
    +-- HTTP 200 OK
```

## 四、数据格式支持

| 格式 | 说明 | 处理方式 |
|------|------|---------|
| **CSV** | 默认格式，逗号分隔 | `CsvParser` 逐行解析 |
| **JSON** | 每行一个 JSON 对象 (ndjson) | `JsonParser` 解析 |
| **Parquet** | 列式二进制格式 | `ParquetScanner` 直接读取 |

**数据转换流程**:
```
原始数据 (CSV/JSON/Parquet)
    |
    v
StreamLoadScanNode (格式解析)
    |
    v
SelectNode (columns 映射 / 表达式 / WHERE 过滤)
    |
    v
OlapTableSink (按分桶路由)
    |
    v
DeltaWriter -> MemTable -> RowsetWriter -> Segment 文件
```

## 五、错误处理与重试

```
错误场景                     处理方式
=======================     =======================
BE 写入失败 (部分 Tablet)     FE commit 时 quorum 检查失败 -> ABORTED
Label 冲突 (已 VISIBLE)      返回成功 (幂等)
Label 冲突 (LOADING)         返回错误, 客户端重试
BE 宕机 (写入过程中断)        FE 超时检测 -> ABORTED, 清理残留 Rowset
FE 宕机 (提交过程中)          BDBJE 保证已提交事务持久化, 新 Master 恢复
数据格式错误                 Where/MaxFilterRatio 过滤, 超阈值 -> Fail
管道消费者阻塞超时            StreamLoadPipe 超时 -> 取消, 返回错误
MemTable 内存超限            触发 flush, flush 失败 -> ABORTED
```

## 六、两阶段提交与 2PC

对于单次 StreamLoad，使用标准的 **Two-Phase Commit (2PC)** 协议：

```
       Phase 1: Prepare                    Phase 2: Commit
    ========================            ========================
    BE: loadTxnBegin RPC -> FE           BE: loadTxnCommit RPC -> FE
    FE: 分配 txnId, PREPARE             FE: checkCommitStatus (quorum)
    BE: DeltaWriter.write(data)           FE: COMMITTED -> BDBJE 持久化
    BE: DeltaWriter.commit_txn()         FE: PublishVersion (异步)
    BE: 上报 TabletCommitInfo            BE: make_visible() (各副本)
```

**FE 事务状态流转**:
```
                    beginTxn
                       |
                       v
                   PREPARE
                    /      \
              commit      abort/timeout
                /              \
               v                v
         COMMITTED          ABORTED
              |
              v (PublishVersionDaemon)
         VISIBLE
```

## 七、StreamLoad vs Broker Load vs Insert

| 特性 | StreamLoad | Broker Load | Insert Into |
|------|-----------|-------------|-------------|
| **协议** | HTTP PUT | Thrift RPC | MySQL Protocol |
| **数据源** | 客户端直传 | 外部存储 (S3/HDFS) | SQL 语句内嵌 |
| **FE 角色** | 认证 + 重定向 | 调度 + 状态管理 | 解析 + 调度 |
| **数据路径** | Client → BE | Broker → BE | FE → BE |
| **吞吐量** | 高 (HTTP 流式) | 高 (并行 Broker) | 低 (SQL 大小受限) |
| **适用场景** | 实时/近实时导入 | 批量离线导入 | 小批量/测试 |
| **事务管理** | 标准两阶段提交 | 标准两阶段提交 | 单次事务 |

## 八、关键源文件

### FE 侧

| 文件 | 说明 |
|------|------|
| `fe/.../httpv2/rest/LoadAction.java` | HTTP v2 入口, 认证 + 重定向 |
| `fe/.../httpv2/rest/StreamLoadAction.java` | HTTP v1 入口 (早期版本) |
| `fe/.../service/FrontendServiceImpl.java` | Thrift RPC 实现 (loadTxnBegin/Put/Commit) |
| `fe/.../planner/StreamLoadPlanner.java` | 生成 StreamLoad 执行计划 |
| `fe/.../transaction/GlobalTransactionMgr.java` | 全局事务管理器 |
| `fe/.../transaction/DatabaseTransactionMgr.java` | 数据库级事务管理, quorum 检查 |
| `fe/.../transaction/TransactionState.java` | 事务状态定义 |
| `fe/.../transaction/PublishVersionDaemon.java` | 版本发布守护线程 |
| `fe/.../load/LoadRecordMgr.java` | 导入记录管理 |
| `fe/.../load/StreamLoadTask.java` | StreamLoad 任务封装 |

### BE 侧

| 文件 | 说明 |
|------|------|
| `be/src/http/action/stream_load_action.cpp` | HTTP 处理器 (on_header/on_chunk_data) |
| `be/src/http/action/stream_load_action.h` | StreamLoadAction 定义 |
| `be/src/runtime/stream_load_context.h` | StreamLoad 上下文 |
| `be/src/runtime/stream_load_context.cpp` | 上下文实现 |
| `be/src/runtime/stream_load_pipe.h` | 生产者-消费者管道 |
| `be/src/runtime/stream_load_executor.h/.cpp` | StreamLoad 执行器 |
| `be/src/runtime/load_channel_mgr.h/.cpp` | LoadChannel 管理 |
| `be/src/runtime/load_channel.h/.cpp` | 单事务 LoadChannel |
| `be/src/runtime/tablets_channel.h/.cpp` | Tablet 通道 |
| `be/src/runtime/fragment_mgr.h/.cpp` | Fragment 执行管理 |
| `be/src/runtime/plan_fragment_executor.h/.cpp` | Fragment 执行器 |
| `be/src/vec/sink/olap_table_sink.h/.cpp` | 数据路由 Sink |
| `be/src/vec/sink/vtablet_sink.h/.cpp` | Tablet Sink 实现 |
| `be/src/vec/exec/vstream_load_scan_node.h` | StreamLoad 扫描节点 |
| `be/src/olap/delta_writer.h/.cpp` | Delta Writer |
| `be/src/olap/memtable.h/.cpp` | MemTable (内存排序+聚合) |
| `be/src/olap/rowset/rowset_writer.h/.cpp` | RowsetWriter (Segment 文件写入) |
| `be/src/olap/txn_manager.h/.cpp` | BE 端事务管理 |

## 九、一致性保证汇总

| 维度 | 机制 | 说明 |
|------|------|------|
| **事务原子性** | Quorum 两阶段提交 | 多数派副本成功才提交 |
| **数据持久性** | Segment 不可变文件 + RocksDB WAL | RowsetMeta 持久化到 RocksDB |
| **幂等性** | Label 唯一性约束 | 相同 label 重复请求安全 |
| **FE 元数据持久化** | Edit Log + BDBJE | 事务状态不丢失 |
| **版本可见性** | _is_pending + _meta_lock | 原子切换，读写不冲突 |
| **崩溃恢复** | BE 启动扫描 RocksDB pending Rowset | FE 决定 publish 或清理 |
| **反压** | StreamLoadPipe 有界队列 | 消费不过来时阻塞生产者 |
