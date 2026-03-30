# Apache Doris 监控实现机制

> 基于早期版本，Pre-Nereids，无 Pipeline 引擎

指标框架：

FE 双注册表架构（Codahale Histogram + 自定义 DorisMetricRegistry，LongAdder 高并发计数器）
BE 三层设计（MetricRegistry → MetricEntity → Metric，CoreLocal 无锁计数器 + Hook 延迟计算 Gauge）
指标类型对比表（Counter/Gauge/Histogram 的 Java vs C++ 实现差异）
读查询监控时序图（以 SELECT 为例）：

FE 侧：request_total → query_begin → query_total → query_err / HISTO_QUERY_LATENCY → 审计日志
BE 侧：fragment_requests_total → segment_read_total → query_scan_bytes/rows → fragment_request_duration_us
包含每查询 RuntimeProfile 计数器（scan_timer、filtered_rows、cached_pages 等）
写操作监控时序图（以 StreamLoad 为例）：

BE 侧完整链路：txn_begin → stream_receive_bytes → txn_exec_plan → memtable_flush → bytes_written → txn_commit → stream_load_rows → push_success
FE 侧：txn_begin/success/failed + load_finished + 审计日志
其他监控：

系统级指标（CPU/磁盘/网络/TCP/FD/负载，来源 /proc）
派生速率指标（15s 计算：QPS/RPS/scan_throughput/disk_io_util）
Compaction 监控（base/cumulative 请求/成功/失败/字节数）
审计日志体系（4 类日志：query/slow_query/load/stream_load）
HTTP 暴露端点（Prometheus/JSON/Core 三种格式）
读 vs 写监控对比表

## 一、总体架构

Doris 的监控系统分为 **FE 监控** 和 **BE 监控** 两个独立子系统，均通过 HTTP `/metrics` 端点暴露 Prometheus 格式的指标，同时支持 JSON 和 Core 文本格式。

```
  +---------------------------------------------------------------+
  |                         监控数据消费层                          |
  |  Prometheus (定时拉取 /metrics)  /  Grafana (可视化)           |
  +---------------------------------------------------------------+
         |                                    |
         | HTTP GET /metrics                  | HTTP GET /metrics
         v                                    v
  +-------------------+              +-------------------+
  |     FE Node       |              |     BE Node       |
  |                   |              |                   |
  | MetricRepo        |              | DorisMetrics      |
  | +-- Codahale      |              | +-- MetricRegistry |
  | |   (Histogram)   |              | +-- MetricEntity   |
  | +-- DorisRegistry  |              | +-- CoreLocal<64>  |
  |     (Counter/     |              |     (无锁计数器)    |
  |      Gauge)       |              | +-- AtomicGauge    |
  |                   |              | +-- Histogram      |
  | MetricCalculator  |              | +-- Hook Metrics   |
  | (QPS/RPS 15s)     |              | (延迟计算 15s)     |
  |                   |              |                   |
  | SystemMetrics     |              | SystemMetrics     |
  | (/proc/meminfo,   |              | (/proc/stat,      |
  |  /proc/net/snmp)  |              |  /proc/diskstats, |
  |                   |              |  /proc/net/dev)    |
  | JvmService        |              |                   |
  | (Heap/GC/Thread)  |              |                   |
  +-------------------+              +-------------------+
```

## 二、FE 监控系统

### 2.1 指标框架

FE 采用**双注册表**架构：自定义 `DorisMetricRegistry` + Codahale `MetricRegistry`

```
  MetricRepo
  |
  +-- METRIC_REGISTER (Codahale MetricRegistry)
  |     |
  |     +-- Histogram: query.latency.ms
  |     +-- Histogram: editlog.write.latency.ms
  |     (仅用于需要分位统计的延迟指标)
  |
  +-- PALO_METRIC_REGISTER (DorisMetricRegistry, 自定义)
        |
        +-- LongCounterMetric  (java.util.concurrent.LongAdder, 高并发)
        |     +-- request_total
        |     +-- query_total, query_err
        |     +-- load_add, load_finished
        |     +-- txn_begin, txn_success, txn_failed
        |     +-- edit_log_write, edit_log_read
        |     +-- routine_load_rows, routine_load_error_rows
        |
        +-- GaugeMetricImpl<Double>  (动态计算值)
        |     +-- qps, rps, query_err_rate
        |     +-- connection_total
        |     +-- max_journal_id
        |     +-- max_tablet_compaction_score
        |
        +-- GaugeMetric<?>  (回调函数动态获取)
              +-- job{job="load", type="BROKER", state="PENDING"}
              +-- job{job="load", type="ROUTINE_LOAD", state="RUNNING"}
              +-- tablet_num{backend="xxx"}
              +-- snmp{name="tcp_retrans_segs"}
              +-- meminfo{name="memory_available"}
```

### 2.2 指标类型

| 类型 | Java 类 | 存储机制 | 线程安全 |
|------|--------|---------|---------|
| Counter | `LongCounterMetric` | `LongAdder` (CAS 分段累加) | 高并发友好 |
| Counter | `DoubleCounterMetric` | `DoubleAdder` | 高并发友好 |
| Gauge | `GaugeMetricImpl<T>` | 直接 `setValue()`/`getValue()` | 非线程安全, 仅单线程写 |
| Gauge | `GaugeMetric<T>` | 子类通过 `getValue()` 回调动态获取 | 取决于回调实现 |
| Histogram | Codahale `Histogram` | 指数衰减采样, 分位计算 | 内部同步 |

### 2.3 MetricCalculator (派生指标)

```
  MetricCalculator (TimerTask, 每 15 秒执行一次)
  |
  +-- QPS  = (request_total - lastRequestTotal) / 15
  +-- RPS  = (query_total - lastQueryTotal) / 15
  +-- ERR_RATE = (query_err - lastQueryErr) / 15
  +-- MAX_COMPACTION_SCORE = max(所有 BE 的 max_compaction_score)
```

## 三、BE 监控系统

### 3.1 指标框架 (三层设计)

```
  DorisMetrics (单例)
  |
  +-- MetricRegistry ("doris_be")
        |
        +-- MetricEntity ("server")  <-- 全局 BE 指标
        |     |
        |     +-- IntCounter (CoreLocalCounter<int64_t>)
        |     |     query_scan_bytes, query_scan_rows
        |     |     push_requests_success_total, load_bytes
        |     |     fragment_requests_total
        |     |     (每个 CPU Core 一个计数器, 无锁)
        |     |
        |     +-- IntGauge (AtomicGauge<int64_t>)
        |     |     memory_pool_bytes_total
        |     |     unused_rowsets_count
        |     |     load_channel_count
        |     |
        |     +-- UIntGauge (AtomicGauge<uint64_t>)
        |     |     query_mem_consumption (hook)
        |     |     compaction_mem_consumption (hook)
        |     |     load_mem_consumption (hook)
        |     |
        |     +-- HistogramMetric
        |           tablet_version_num_distribution
        |           (109 个原子桶 + SpinLock)
        |
        +-- MetricEntity ("Disk.sda")  <-- 每个磁盘设备
        |     +-- disk_reads_completed, disk_bytes_read
        |     +-- disk_writes_completed, disk_bytes_written
        |     +-- disk_io_time_ms
        |
        +-- MetricEntity ("Network.eth0")  <-- 每个网卡
        |     +-- network_receive_bytes, network_send_bytes
        |
        +-- MetricEntity ("Tablet.10000")  <-- 每个 Tablet (可选)
              +-- query_scan_bytes, query_scan_rows, query_scan_count
```

### 3.2 指标类型

| 类型 | C++ 类 | 存储机制 | 适用场景 |
|------|--------|---------|---------|
| Counter | `IntCounter` = `CoreLocalCounter<int64_t>` | 每 CPU Core 原子值, 读时求和 | 高频累加 (scan_bytes/rows) |
| Counter | `IntAtomicCounter` = `AtomicCounter<int64_t>` | `std::atomic<int64_t>` | 中频累加 |
| Counter | `DoubleCounter` = `LockCounter<double>` | `SpinLock` + `double` | 浮点累加 |
| Gauge | `IntGauge` = `AtomicGauge<int64_t>` | `std::atomic<int64_t>` | 瞬时值 |
| Gauge | `DoubleGauge` = `LockGauge<double>` | `SpinLock` + `double` | 浮点瞬时值 |
| Histogram | `HistogramMetric` | 109 原子桶 + `HistogramStat` + SpinLock | 分布统计 |

### 3.3 Hook Metrics (延迟计算)

```
  DorisMetrics::initialize()
  |
  +-- 注册 Hook 函数到 MetricEntity
        |
        +-- query_mem_consumption      <- ExecEnv::_mem_tracker->consumption()
        +-- compaction_mem_consumption <- StorageEngine compaction tracker
        +-- load_mem_consumption       <- LoadChannelMgr::_mem_tracker->consumption()
        +-- schema_change_mem_consumption
        +-- tablet_meta_mem_consumption
        +-- unused_rowsets_count       <- StorageEngine unused set
        +-- load_channel_count         <- LoadChannelMgr map size
        +-- plan_fragment_count        <- FragmentMgr map size
        +-- result_buffer_block_count  <- ResultBufferMgr
        +-- scanner_thread_pool_queue_size
        +-- send_batch_thread_pool_queue_size
        +-- ...
```

Hook 触发方式:
- `enable_metric_calculator = true` (默认): 后台线程每 15 秒触发一次
- `false`: 在 `/metrics` HTTP 请求时内联触发

## 四、读查询监控时序流程

### 4.1 FE 侧读监控

```
  MySQL Client
       |
       | COM_QUERY (SELECT ...)
       v
  ConnectProcessor.handleQuery()
       |
       +-- request_total ++                    <<<< Counter: 总请求数
       |
       +-- StmtExecutor.execute()
             |
             +-- [QueryStmt]
                   +-- query_begin ++          <<<< Counter: 查询开始
                   |
                   +-- Coordinator.exec()
                         |
                         +-- FE 分析/规划/调度 (不直接记录指标)
                         |
                         +-- coord.getNext() (循环获取结果)
                               |
                               +-- fetch_data RPC -> BE
                               +-- 收到所有结果
                         |
                         +-- [查询完成]
                               |
                               +-- elapseMs = endTime - startTime
                               |
                               +-- isQuery? YES:
                                     +-- query_total ++       <<<< Counter: 总查询数
                                     |
                                     +-- [成功]:
                                     |     query_err 不变
                                     |     HISTO_QUERY_LATENCY.update(elapseMs)
                                     |     <<<< Histogram: 查询延迟 (成功)
                                     |
                                     +-- [失败]:
                                           query_err ++       <<<< Counter: 查询错误
                                           不记录延迟
  ConnectProcessor
       |
       +-- [审计日志]
             AuditLogBuilder.build()
             +-- timestamp, clientIp, user, db, state
             +-- queryTime, scanBytes, scanRows, returnRows
             +-- cpuTimeMs, peakMemoryBytes, queryId
             +-- 写入 audit.query (所有查询)
             +-- queryTime > qe_slow_log_ms?
                   YES -> 写入 audit.slow_query (慢查询日志)
```

### 4.2 BE 侧读监控

```
  FE: exec_plan_fragment RPC
       |
       v
  BE: FragmentMgr::exec_plan_fragment()
       |
       +-- fragment_requests_total ++         <<<< Counter: Fragment 请求数
       |
       +-- 创建 PlanFragmentExecutor
       +-- 线程池提交
       |
       v
  PlanFragmentExecutor.open()
       |
       +-- ExecNode::create_tree() -> OlapScanNode
       +-- OlapScanNode.open()
             |
             +-- 启动多个 VOlapScanner 线程
             |
             v
  VOlapScanner -> OlapScanner -> BlockReader
       |
       +-- 每次读取 Segment:
             |
             +-- [Segment V2 索引裁剪]
             |     segment_read_total ++          <<<< 读过的 Segment 数
             |     segment_row_total += rows      <<<< 索引裁剪前总行数
             |     segment_rows_by_short_key += rows  <<<< 短键索引选中行
             |     segment_rows_read_by_zone_map += rows  <<<< Zone Map 选中行
             |
             +-- BlockReader.next_block()
                   |
                   +-- [RuntimeProfile 计数器 - 每查询粒度, 非 Prometheus]
                   |     _scan_timer               (扫描耗时)
                   |     _read_compressed_counter   (压缩读字节数)
                   |     _raw_rows_counter          (原始行数)
                   |     _stats_filtered_counter    (统计信息过滤行)
                   |     _bf_filtered_counter       (BloomFilter 过滤行)
                   |     _del_filtered_counter      (删除谓词过滤行)
                   |     _block_load_counter        (加载 Block 次数)
                   |     _total_pages_num_counter   (总 Page 数)
                   |     _cached_pages_num_counter  (缓存命中 Page 数)
                   |     _num_scanners              (Scanner 数)
                   |
                   +-- [Scanner 关闭时 _update_counter()]
                         query_scan_bytes += _compressed_bytes_read  <<<< 全局+Tablet级
                         query_scan_rows += _raw_rows_read          <<<< 全局+Tablet级
                         query_scan_count += 1                     <<<< 全局+Tablet级
  PlanFragmentExecutor.close()
       |
       +-- duration_us = endTime - startTime
       +-- fragment_request_duration_us += duration_us  <<<< Counter: Fragment 执行耗时
```

### 4.3 读监控指标汇总

```
  +-- FE 指标 -------------------+
  | request_total    (Counter)   |  总请求数 (包含非查询)
  | query_total      (Counter)   |  查询总数
  | query_begin      (Counter)   |  查询开始数
  | query_err        (Counter)   |  查询错误数
  | qps              (Gauge)     |  每秒查询数 (15s 计算)
  | rps              (Gauge)     |  每秒请求数 (15s 计算)
  | query_err_rate   (Gauge)     |  每秒查询错误数 (15s 计算)
  | query.latency.ms (Histogram) |  查询延迟分位 (75/95/98/99/999)
  | cache_hit_sql    (Counter)   |  SQL 缓存命中
  | cache_hit_partition (Counter)|  分区缓存命中
  +-------------------------------+

  +-- BE 指标 ---------------------------------------+
  | fragment_requests_total    (IntCounter)           |  Fragment 请求数
  | fragment_request_duration_us (IntCounter)         |  Fragment 总耗时(us)
  | query_scan_bytes           (IntCounter)           |  扫描字节数 (全局)
  | query_scan_rows            (IntCounter)           |  扫描行数 (全局)
  | query_scan_bytes_per_second (IntGauge, 15s计算)   |  扫描吞吐 (B/s)
  | segment_read_total         (IntCounter)           |  读取 Segment 数
  | segment_row_total          (IntCounter)           |  索引裁剪前总行数
  | segment_rows_by_short_key  (IntCounter)           |  短键索引选中行数
  | segment_rows_read_by_zone_map (IntCounter)        |  Zone Map 选中行数
  | [Tablet 级] query_scan_bytes/rows/count (可选)    |  每个 Tablet 扫描统计
  +---------------------------------------------------+

  +-- 审计日志 (每查询) --------------------+
  | timestamp, clientIp, user, db            |
  | queryTime, scanBytes, scanRows, returnRows|
  | cpuTimeMs, peakMemoryBytes, queryId     |
  | stmt (完整 SQL)                          |
  +-----------------------------------------+
```

## 五、写操作监控时序流程

### 5.1 StreamLoad 写入监控

```
  MySQL Client (curl/SDK)
       |
       | HTTP PUT -> FE (认证+重定向)
       | HTTP PUT -> BE (数据)
       v
  BE: StreamLoadAction.on_header()
       |
       +-- stream_load_executor()->begin_txn()
       |     |
       |     +-- loadTxnBegin RPC -> FE
       |     |     txn_begin ++           <<<< FE Counter: 事务开始
       |     |
       |     +-- [BE 侧]
       |           txn_begin_request_total ++   <<<< BE Counter: 事务开始
       |
       +-- _process_put() -> streamLoadPut RPC -> FE
             |
             +-- stream_load_executor()->execute_plan_fragment()
                   |
                   +-- txn_exec_plan_total ++     <<<< BE Counter: 执行计划

  BE: StreamLoadAction.on_chunk_data()  (持续接收数据块)
       |
       +-- stream_receive_bytes_total += chunk_size  <<<< BE Counter: 接收字节数

  BE: PlanFragmentExecutor (数据写入管道)
       |
       +-- OlapTableSink -> LoadChannelMgr -> TabletsChannel -> DeltaWriter
       |     |
       |     +-- MemTable 满 -> flush
       |     |     memtable_flush_total ++         <<<< BE Counter: 刷盘次数
       |     |     memtable_flush_duration_us += t <<<< BE Counter: 刷盘耗时
       |     |
       |     +-- Block Manager (写入 Segment)
       |           bytes_written_total += size      <<<< BE Counter: 磁盘写入字节
       |           blocks_created_total ++           <<<< BE Counter: 创建 Block 数
       |           disk_sync_total ++                <<<< BE Counter: fsync 次数
       |
       v
  [写入完成]

  BE: StreamLoadExecutor.commit_txn()
       |
       +-- txn_commit_request_total ++     <<<< BE Counter: 事务提交
       |
       +-- loadTxnCommit RPC -> FE
             |
             +-- [FE 侧]
             |     txn_success ++            <<<< FE Counter: 事务成功
             |     load_finished ++          <<<< FE Counter: 负载完成
             |
             +-- [失败时]
                   txn_failed ++             <<<< FE Counter: 事务失败

  BE: StreamLoadContext 收集本次统计
       |
       +-- stream_load_rows_total += loaded_rows  <<<< BE Counter: 导入行数
       +-- load_rows += loaded_rows               <<<< BE Counter: 总导入行数
       +-- load_bytes += loaded_bytes              <<<< BE Counter: 总导入字节
       +-- push_requests_success_total ++          <<<< BE Counter: 推送成功

  BE: 返回 HTTP Response (JSON)
       |
       +-- { "TxnId", "Label", "Status", "NumberTotalRows",
       |     "NumberLoadedRows", "NumberFilteredRows",
       |     "LoadBytes", "LoadTimeMs",
       |     "BeginTxnTimeMs", "StreamLoadPutTimeMs",
       |     "ReadDataTimeMs", "WriteDataTimeMs",
       |     "CommitAndPublishTimeMs" }

  FE: 审计日志
       |
       +-- AuditLogBuilder.build(STREAM_LOAD_FINISH)
             +-- Label, Db, Table, User, ClientIp
             +-- Status, Message, Url
             +-- TotalRows, LoadedRows, FilteredRows
             +-- LoadBytes, StartTime, FinishTime
             +-- 写入 audit.stream_load
```

### 5.2 事务状态监控

```
  FE: DatabaseTransactionMgr
       |
       +-- beginTransaction()
       |     txn_begin ++              <<<< Counter
       |
       +-- [Label 冲突 / 资源不足]
       |     txn_reject ++             <<<< Counter: 事务拒绝
       |
       +-- commitTransaction() [成功]
       |     txn_success ++            <<<< Counter: 事务成功
       |
       +-- commitTransaction() [失败]
       |     txn_failed ++             <<<< Counter: 事务失败


  BE: StreamLoadExecutor
       |
       +-- begin_txn()
       |     txn_begin_request_total ++      <<<< Counter
       |
       +-- commit_txn()
       |     txn_commit_request_total ++     <<<< Counter
       |
       +-- rollback_txn() [失败/超时]
             txn_rollback_request_total ++   <<<< Counter
```

## 六、系统级监控

### 6.1 BE 系统指标采集

```
  SystemMetrics (每 15 秒采集一次, 通过 Hook 触发)
  |
  +-- CPU 指标 (来源: /proc/stat)
  |     cpu_user, cpu_system, cpu_idle, cpu_iowait
  |     cpu_irq, cpu_soft_irq, cpu_steal
  |
  +-- 内存指标 (来源: tcmalloc/mallinfo)
  |     memory_allocated_bytes
  |
  +-- 磁盘指标 (来源: /proc/diskstats, 每设备一个 Entity)
  |     disk_reads_completed, disk_bytes_read
  |     disk_writes_completed, disk_bytes_written
  |     disk_read_time_ms, disk_write_time_ms
  |     disk_io_time_ms
  |
  +-- 网络指标 (来源: /proc/net/dev, 每网卡一个 Entity)
  |     network_receive_bytes, network_receive_packets
  |     network_send_bytes, network_send_packets
  |
  +-- TCP 指标 (来源: /proc/net/snmp)
  |     snmp_tcp_in_errs, snmp_tcp_retrans_segs
  |     snmp_tcp_in_segs, snmp_tcp_out_segs
  |
  +-- FD 指标 (来源: /proc/sys/fs/file-nr)
  |     fd_num_limit, fd_num_used
  |
  +-- 负载指标 (来源: /proc/loadavg)
        load_average_1/5/15_minutes
```

### 6.2 派生速率指标

```
  Daemon::calculate_metrics_thread() (每 15 秒)
  |
  +-- trigger_all_hooks(true)     // 触发所有 Hook 更新
  |
  +-- push_request_write_bytes_per_second
  |     = delta(push_request_write_bytes) / 15
  |
  +-- query_scan_bytes_per_second
  |     = delta(query_scan_bytes) / 15
  |
  +-- max_disk_io_util_percent
  |     = max(delta(disk_io_time_ms) / device) / 15 / 10
  |
  +-- max_network_send_bytes_rate
  |     = max(delta(send_bytes) / interface) / 15
  |
  +-- max_network_receive_bytes_rate
        = max(delta(receive_bytes) / interface) / 15
```

### 6.3 FE 系统指标采集

```
  JvmService.stats() (每次 /metrics 请求时采集)
  |
  +-- 内存
  |     jvm_heap_size_bytes{type="max|committed|used"}
  |     jvm_non_heap_size_bytes{type="committed|used"}
  |     jvm_young_size_bytes{type="used|peak_used|max"}
  |     jvm_old_size_bytes{type="used|peak_used|max"}
  |
  +-- GC
  |     jvm_young_gc{type="count|time"}
  |     jvm_old_gc{type="count|time"}
  |
  +-- 线程
  |     jvm_thread{type="count|peak_count|runnable|blocked|waiting|...}


  SystemMetrics (每次 /metrics 请求时更新)
  |
  +-- TCP (/proc/net/snmp)
  |     snmp{name="tcp_retrans_segs|tcp_in_errs|tcp_in_segs|tcp_out_segs"}
  |
  +-- 内存 (/proc/meminfo)
        meminfo{name="memory_total|memory_free|memory_available|buffers|cached"}
```

## 七、监控指标暴露

### 7.1 HTTP 端点

| 端点 | 节点 | 格式 | 说明 |
|------|------|------|------|
| `GET /metrics` | FE+BE | Prometheus (默认) | 完整指标输出 |
| `GET /metrics?type=json` | FE+BE | JSON 数组 | 程序化访问 |
| `GET /metrics?type=core` | FE+BE | 文本 (name value) | 核心指标子集 |
| `GET /metrics?with_tablet=true` | BE | Prometheus | 包含 Tablet 级指标 |
| `GET /api/query_detail?event_time=T` | FE | JSON | 最近查询详情 |
| `GET /api/health` | FE | JSON | 集群健康状态 |

### 7.2 指标暴露时序图

```
  Prometheus Server
       |
       | GET /metrics (每 15s/30s 拉取)
       v
  BE: MetricsAction.handle()
       |
       +-- 判断 type 参数
       |     "core" -> metric_registry->to_core_string()
       |     "json" -> metric_registry->to_json()
       |     默认    -> metric_registry->to_prometheus()
       |
       +-- metric_registry->to_prometheus()
             |
             +-- trigger_all_hooks()      <<<< 触发 Hook 更新 Gauge 值
             |
             +-- 遍历所有 MetricEntity:
                   for each entity:
                     for each metric:
                       +-- Counter/Gauge:
                       |     # TYPE doris_be_<name> counter|gauge
                       |     doris_be_<name><labels> <value>
                       |
                       +-- Histogram:
                             # TYPE doris_be_<name> histogram
                             doris_be_<name>{quantile="0.50"} <p50>
                             doris_be_<name>{quantile="0.75"} <p75>
                             doris_be_<name>{quantile="0.90"} <p90>
                             doris_be_<name>{quantile="0.95"} <p95>
                             doris_be_<name>{quantile="0.99"} <p99>
                             doris_be_<name>_sum <sum>
                             doris_be_<name>_count <count>
             |
             v
       Content-Type: text/plain; version=0.0.4
       返回 Prometheus 格式文本
```

## 八、Compaction 监控

```
  Compaction 执行 (BE)
       |
       +-- [Base Compaction]
       |     base_compaction_request_total ++        <<<< Counter: 请求次数
       |     [成功]
       |     base_compaction_deltas_total += N       <<<< Counter: 合并 Rowset 数
       |     base_compaction_bytes_total += size     <<<< Counter: 合并字节数
       |     [失败]
       |     base_compaction_request_failed ++       <<<< Counter: 失败次数
       |     tablet_base_max_compaction_score = max  <<<< Gauge: 最大压缩分数
       |
       +-- [Cumulative Compaction]
             cumulative_compaction_request_total ++
             cumulative_compaction_deltas_total += N
             cumulative_compaction_bytes_total += size
             cumulative_compaction_request_failed ++
             tablet_cumulative_max_compaction_score = max
```

## 九、审计日志体系

```
  审计日志流程
  ================

  ConnectProcessor.handleQuery() / StreamLoadAction.handle()
       |
       +-- AuditLogBuilder.build(event)
       |     +-- 填充字段 (user, db, queryTime, scanBytes, ...)
       |
       +-- AuditEventProcessor.handleAuditEvent(event)
             |
             +-- BlockingQueue<AuditEvent> (容量 10,000)
             |
             +-- [后台线程消费]
                   |
                   +-- AuditLogBuilder.exec(event)
                         |
                         +-- [AFTER_QUERY]
                         |     写入 audit.query (所有查询)
                         |     queryTime > qe_slow_log_ms ?
                         |       YES -> 写入 audit.slow_query
                         |
                         +-- [LOAD_SUCCEED]
                         |     写入 audit.load (批量导入)
                         |
                         +-- [STREAM_LOAD_FINISH]
                               写入 audit.stream_load (流式导入)


  审计日志文件:
  +-- fe/log/audit.query.log       (所有查询, pipe 分隔 key=value)
  +-- fe/log/audit.slow_query.log  (慢查询, 超过 qe_slow_log_ms)
  +-- fe/log/audit.load.log        (批量导入完成)
  +-- fe/log/audit.stream_load.log (流式导入完成)
```

## 十、读监控 vs 写监控对比

| 维度 | 读 (SELECT) | 写 (StreamLoad) |
|------|-------------|----------------|
| **FE 入口** | ConnectProcessor.handleQuery() | FE LoadAction (仅重定向) |
| **BE 入口** | FragmentMgr::exec_plan_fragment() | StreamLoadAction.on_header() |
| **FE 计数器** | request_total, query_total, query_err, query.latency.ms | txn_begin, txn_success/failed, load_finished |
| **BE 计数器** | fragment_requests_total, query_scan_bytes/rows | stream_receive_bytes_total, stream_load_rows_total, load_bytes |
| **BE 延迟** | fragment_request_duration_us | push_request_duration_us |
| **BE 系统指标** | query_scan_bytes_per_second | push_request_write_bytes_per_second |
| **每查询 Profile** | RuntimeProfile (scan_timer, rows_read, filtered_*) | StreamLoadContext (各阶段耗时) |
| **审计日志** | audit.query + audit.slow_query | audit.stream_load |
| **Tablet 级指标** | query_scan_bytes/rows/count (可选) | 无 |

## 十一、关键源文件

### FE 侧

| 文件 | 说明 |
|------|------|
| `fe/.../metric/MetricRepo.java` | 指标仓库 (双注册表, 所有指标定义) |
| `fe/.../metric/DorisMetricRegistry.java` | 自定义指标注册表 |
| `fe/.../metric/Metric.java` | 指标基类 (GAUGE/COUNTER 枚举) |
| `fe/.../metric/LongCounterMetric.java` | Long 计数器 (LongAdder) |
| `fe/.../metric/GaugeMetricImpl.java` | Gauge 实现 |
| `fe/.../metric/MetricCalculator.java` | 派生指标计算 (15s) |
| `fe/.../metric/MetricLabel.java` | 指标标签 (key=value) |
| `fe/.../metric/PrometheusMetricVisitor.java` | Prometheus 格式输出 |
| `fe/.../metric/SimpleCoreMetricVisitor.java` | 核心指标格式输出 |
| `fe/.../metric/JsonMetricVisitor.java` | JSON 格式输出 |
| `fe/.../metric/SystemMetrics.java` | Linux 系统指标 (/proc) |
| `fe/.../monitor/jvm/JvmService.java` | JVM 指标采集 |
| `fe/.../http/rest/MetricsAction.java` | /metrics HTTP 端点 |
| `fe/.../qe/AuditLogBuilder.java` | 审计日志构建器 |
| `fe/.../qe/AuditEventProcessor.java` | 审计事件处理器 |
| `fe/.../plugin/AuditEvent.java` | 审计事件数据 |

### BE 侧

| 文件 | 说明 |
|------|------|
| `be/src/util/metrics.h` | 指标基类 (MetricRegistry/MetricEntity/Metric) |
| `be/src/util/metrics.cpp` | 指标序列化 (Prometheus/JSON/Core) |
| `be/src/util/doris_metrics.h` | 全局指标单例 (所有指标声明) |
| `be/src/util/doris_metrics.cpp` | 全局指标注册 + Hook 定义 |
| `be/src/util/histogram.h` | Histogram 实现 (109 桶) |
| `be/src/util/system_metrics.h` | 系统指标 (CPU/磁盘/网络/TCP) |
| `be/src/common/daemon.cpp` | calculate_metrics_thread (15s) |
| `be/src/http/action/metrics_action.cpp` | /metrics HTTP 端点 |
| `be/src/exec/olap_scanner.cpp` | 扫描计数器更新 |
| `be/src/runtime/fragment_mgr.cpp` | Fragment 执行计数器 |
| `be/src/runtime/stream_load/stream_load_executor.cpp` | StreamLoad 计数器 |
| `be/src/olap/memtable.cpp` | MemTable 刷盘计数器 |
| `be/src/olap/task/engine_batch_load_task.cpp` | Push Load 计数器 |
| `be/src/olap/fs/block_manager_metrics.cpp` | Block I/O 计数器 |
| `be/src/runtime/load_channel_mgr.cpp` | Load Channel Hook 指标 |
