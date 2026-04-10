# Apache Doris QoS (服务质量) 机制详解

> 基于早期版本，Pre-Nereids，无 Pipeline 引擎

## 一、总体架构

```
+----------------------------------------------------------------------+
|                         FE (Frontend)                                |
|                                                                      |
|  QoS 入口点:                                                          |
|  +-- ConnectScheduler: 连接限流 (全局/按用户)                          |
|  +-- SessionVariable: 查询级资源限制 (内存/超时/CPU)                    |
|  +-- UserProperty: 用户级资源属性                                      |
|  +-- ResourceGroup: 资源组 (CPU Share / IO Share / IO 限额)            |
|  +-- DatabaseTransactionMgr: 事务并发控制                               |
|  +-- LoadJobScheduler: Load 任务队列与并发控制                          |
|  +-- QeProcessorImpl: 查询实例数限制                                   |
|  +-- Coordinator: 资源信息传递 + 扫描范围分配                           |
|                                                                      |
+----------------------------------------------------------------------+
         | TQueryOptions + TResourceInfo         | Heartbeat (资源更新)
         | (mem_limit, timeout, cpu_limit,         | (cpu_share, io_share,
         |  user, group)                          |  io_throttle)
         v                                        v
+----------------------------------------------------------------------+
|                         BE (Backend)                                 |
|                                                                      |
|  QoS 执行层:                                                          |
|  +-- FragmentMgr: Fragment 线程池 + 超时检查 + 取消                    |
|  +-- MemTracker: 层级化内存追踪 (Process->Query->Fragment->ExecNode)   |
|  +-- CgroupsMgr: Linux cgroups v1 (CPU Share + IO 限速)                |
|  +-- PriorityThreadPool: 优先级扫描调度                                |
|  +-- ThreadPoolToken: 按 CPU Limit 限制扫描并发                        |
|  +-- ThreadResourceMgr: 线程资源令牌池                                 |
|  +-- DiskIoMgr: 磁盘 I/O 调度                                         |
+----------------------------------------------------------------------+
```

## 二、QoS 维度总览

| QoS 维度 | FE 控制 | BE 执行 | 机制 |
|---------|--------|--------|------|
| **连接限流** | ConnectScheduler | - | 全局/用户级最大连接数 |
| **查询超时** | SessionVariable | FragmentMgr.cancel_worker() | 1 秒周期检查 |
| **查询内存** | SessionVariable.mem_limit | MemTracker 层级树 | 超限触发 GC/报错 |
| **查询 CPU** | UserProperty.cpu_resource_limit | ThreadPoolToken + cgroups | 限制扫描线程并发 |
| **CPU Share** | ResourceGroup.cpu_share | cgroups cpu.shares | 用户间按比例分配 |
| **IO 限速** | ResourceGroup.io_*_mbps/iops | cgroups blkio.throttle | 按设备限速 |
| **IO Share** | ResourceGroup.io_share | cgroups blkio.weight | 用户间按比例分配 |
| **事务并发** | max_running_txn_num_per_db | - | 每库最大运行事务数 |
| **Load 并发** | LoadJobScheduler | - | 线程池 + 队列 |
| **查询实例数** | max_query_instances | QeProcessorImpl | 用户级限制 |
| **扫描优先级** | - | PriorityThreadPool | 基于扫描范围自动分级 |
| **BE 可用性** | isQueryDisabled/isLoadDisabled | HeartbeatMgr | 心跳检测 |

## 三、连接限流

### 3.1 时序流程

```
  MySQL Client         ConnectScheduler           ConnectContext
       |                    |                           |
       | TCP 连接请求        |                           |
       |------------------->|                           |
       |                    | registerConnection()       |
       |                    |                           |
       |                    +-- 全局连接数检查            |
       |                    | numberConnection++        |
       |                    | > qe_max_connection(1024)?|
       |                    |   YES -> 拒绝连接          |
       |                    |   ERR_USER_LIMIT_REACHED  |
       |                    |                           |
       |                    +-- 用户连接数检查            |
       |                    | connByUser[user]++        |
       |                    | > max_user_connections?   |
       |                    |   YES -> 拒绝连接          |
       |                    |                           |
       |                    +-- 提交到线程池             |
       |                    | (max 4096 线程)            |
       |                    |                           |
       |<-------------------|                           |
       | 连接成功             |                           |
       |                    |                           |
       | [1秒后] TimeoutChecker (定期检查)
       |                    | checkTimeout(now)        |
       |                    | COM_SLEEP > wait_timeout?|
       |                    |   YES -> kill 连接         |
       |                    | 活动查询 > query_timeout? |
       |                    |   YES -> kill 查询         |
```

### 3.2 配置

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| `qe_max_connection` | 1024 | FE 最大连接数 |
| `max_connection_scheduler_threads_num` | 4096 | 连接处理线程池大小 |
| `max_user_connections` | 100 | 单用户最大连接数 (SET PROPERTY) |
| `wait_timeout` | 28800s (8h) | 空闲连接超时 |
| `query_timeout` | 300s (5min) | 查询执行超时 |

## 四、查询级资源控制

### 4.1 资源限制传递链

```
FE 用户属性/Session变量
    |
    +-- UserProperty: cpu_resource_limit, max_query_instances
    +-- SessionVariable: exec_mem_limit, query_timeout, resource_group
    |
    v
StmtExecutor -> Coordinator
    |
    +-- Coordinator.setFromUserProperty()
    |     覆盖 TResourceLimit.cpuLimit
    |
    +-- SessionVariable.toThrift() -> TQueryOptions
    |     +-- mem_limit (单查询实例内存上限)
    |     +-- query_timeout
    |     +-- load_mem_limit
    |     +-- resource_limit.cpu_limit
    |     +-- max_reservation (内存预分配)
    |     +-- buffer_pool_limit
    |
    +-- TResourceInfo (user + group)
    |
    v
Fragment RPC (TExecPlanFragmentParams)
    |
    v
BE: FragmentMgr::exec_plan_fragment()
    |
    +-- 创建 QueryFragmentsCtx (共享上下文)
    |     +-- 设置 timeout_second
    |     +-- 创建 ThreadPoolToken (如果 cpu_limit > 0)
    |
    +-- 创建 FragmentExecState
    |
    v
BE: PlanFragmentExecutor::prepare()
    |
    +-- 创建 MemTracker (query_mem_limit, 父节点=process_mem_tracker)
    +-- 创建 RuntimeState
    |     +-- _query_mem_tracker (查询级)
    |     +-- _instance_mem_tracker (实例级)
    |
    v
BE: FragmentExecState::execute()
    |
    +-- CgroupsMgr::apply_system_cgroup()
    |     将线程分配到用户/组的 cgroup
    |
    +-- PlanFragmentExecutor::open() / get_next() / close()
```

### 4.2 内存控制时序

```
  PlanFragmentExecutor          MemTracker              ExecNode (Scan等)
       |                          |                         |
       | prepare()                |                         |
       +------------------------->|                         |
       |                          |                         |
       | CreateTracker(          |                         |
       |   mem_limit,            |                         |
       |   parent=process_tracker |                         |
       | )                        |                         |
       |                          |                         |
       | open() / get_next()      |                         |
       +-- RuntimeState.init()   |                         |
       |   query_mem_tracker  -> |                         |
       |   instance_mem_tracker ->|                         |
       |                          |                         |
       | ExecNode 分配内存        |                         |
       +------------------------->|------------------------->|
       |                          |                         |
       |                    TryConsume(bytes)            |
       |                          |                         |
       |                    检查本层 limit              |
       |                    检查父层 limit              |
       |                    检查祖先 limit             |
       |                          |                         |
       |                    未超限 -> 返回 OK          |
       |                    超限   -> 触发 GC            |
       |                              -> 重试                |
       |                              -> 仍超限:             |
       |                                MemoryLimitExceeded  |
       |                                报错/取消查询        |
```

**MemTracker 层级**:
```
Process MemTracker (mem_limit = 80% 物理内存)
  |
  +-- Query MemTracker (limit = session mem_limit, 默认 2GB)
  |     |
  |     +-- Fragment Instance MemTracker (limit = -1, 继承查询级)
  |           |
  |           +-- ExecNode MemTracker (Aggregation Hash Table 等)
  |           +-- DataSink MemTracker (DataStreamSender Buffer 等)
  |
  +-- Buffer Pool MemTracker
  +-- Storage Page Cache MemTracker
  +-- Tablet Reader MemTracker
```

### 4.3 CPU 控制时序

```
  FE: UserProperty                  BE: FragmentMgr            BE: OlapScanNode
       |                                 |                           |
       | cpu_resource_limit = 4         |                           |
       |-----------------------------> TQueryOptions              |
       |                                 |                           |
       |                                 | QueryFragmentsCtx           |
       |                                 | set_thread_token(4)        |
       |                                 |                           |
       |                                 | ThreadPoolToken            |
       |                                 | max_concurrency = 4         |
       |                                 | (共享于同一查询的所有         |
       |                                 |  Fragment 实例)             |
       |                                 |                           |
       |                                 | FragmentExecState          |
       |                                 | execute()                  |
       |                                 |                           |
       |                                 | CgroupsMgr                 |
       |                                 | apply_system_cgroup()      |
       |                                 | 线程 -> /cgroup/user/group |
       |                                 |                           |
       |                                 |               PlanFragmentExecutor.open()
       |                                 |                           |
       |                                 |               OlapScanNode.start_scan()
       |                                 |               |            |
       |                                 |               | 计算 nice 值:
       |                                 |               | nice = 18 +
       |                                 |               |   max(0, 2 - scanners/5)
       |                                 |               |            |
       |                                 |               | 提交 Scanner 任务
       |                                 |               |------------->|
       |                                 |               |            |
       |                                 |               | 如果 cpu_limit > 0:
       |                                 |               |   ThreadPoolToken.offer()
       |                                 |               |   并发上限 = cpu_limit
       |                                 |               |            |
       |                                 |               | 如果 cpu_limit <= 0:
       |                                 |               |   PriorityThreadPool.offer()
       |                                 |               |   按 nice 值优先调度
       |                                 |               |   (小查询优先)
```

### 4.4 优先级扫描调度

```
  PriorityThreadPool + BlockingPriorityQueue
  ===========================================

  提交 Scanner 任务:
    +-- 计算 nice 值:
    |     扫描范围少 -> nice 值高 -> 优先级高 (小查询)
    |     扫描范围多 -> nice 值低 -> 优先级低 (大查询)
    |
    +-- BlockingPriorityQueue:
          +-- 按优先级排序
          +-- 防饿死机制:
                每隔 N 次取任务, 所有任务优先级 +1
                (等待越久, 优先级越高)
          +-- 等待超时: 5ms (doris_blocking_priority_queue_wait_timeout_ms)

  配置:
    doris_scanner_thread_pool_thread_num = 48
    doris_scanner_thread_pool_queue_size  = 102400
```

## 五、Cgroups 资源隔离

### 5.1 Cgroups 层次结构

```
{cgroups_root_path}/
  |
  +-- default/              (默认用户)
  |     +-- low/
  |     +-- normal/
  |     +-- high/
  |
  +-- user_a/               (用户 A)
  |     +-- low/
  |     +-- normal/
  |     +-- high/
  |
  +-- user_b/               (用户 B)
        +-- low/
        +-- normal/
        +-- high/
```

### 5.2 资源控制项

| cgroup 控制项 | TResourceType | 说明 |
|---------------|---------------|------|
| `cpu.shares` | CPU_SHARE | CPU 时间片比例 (100-1000) |
| `blkio.weight` | IO_SHARE | IO 权重 (100-1000) |
| `blkio.throttle.read_bps_device` | SSD_READ_MBPS / HDD_READ_MBPS | SSD/HDD 读带宽限制 |
| `blkio.throttle.write_bps_device` | SSD_WRITE_MBPS / HDD_WRITE_MBPS | SSD/HDD 写带宽限制 |
| `blkio.throttle.read_iops_device` | SSD_READ_IOPS / HDD_READ_IOPS | SSD/HDD 读 IOPS 限制 |
| `blkio.throttle.write_iops_device` | SSD_WRITE_IOPS / HDD_WRITE_IOPS | SSD/HDD 写 IOPS 限制 |

### 5.3 资源更新流程

```
  FE (MasterDaemon)              BE (UserResourceListener)
       |                              |
       | HeartbeatMgr (定期)         |
       | publishResource()           |
       | (topic: resource_update)   |
       +----------------------------->|
       |                              |
       |                    TopicListener.onEvent()
       |                              |
       |                    UserResourceListener.update()
       |                              |
       |                    CgroupsMgr.update_local_cgroups()
       |                    |              |
       |                    |  创建/更新/删除 cgroup 目录
       |                    |  写入 cpu.shares
       |                    |  写入 blkio.weight
       |                    |  写入 blkio.throttle.*_device
       |                    |              |
       |                    后续查询线程自动应用新 cgroup
```

## 六、事务与 Load 并发控制

### 6.1 事务并发

```
  Client                 DatabaseTransactionMgr         FE Config
    |                          |                           |
    | BEGIN TRANSACTION       |                           |
    |------------------------>|                           |
    |                          | runningTxnNums++          |
    |                          |                           |
    |                          | checkRunningTxnExceedLimit()
    |                          | runningTxnNums >=         |
    |                          | max_running_txn_num_per_db |
    |                          |   (= 100)?                |
    |                          |     YES -> 拒绝事务       |
    |                          |     NO  -> 允许            |
    |<------------------------|                           |
    |                          |                           |
    | COMMIT/ROLLBACK         |                           |
    |------------------------>|                           |
    |                          | runningTxnNums--          |
```

### 6.2 Load 任务调度

```
  LoadManager           LoadJobScheduler           LoadingLoadTaskScheduler
       |                      |                              |
       | 提交 Load Job       |                              |
       |--------------------->|                              |
       |                      | needScheduleJobs 队列      |
       |                      | (LinkedBlockingQueue)      |
       |                      |                              |
       |                      | isQueueFull()?              |
       |                      | size > desired_max_waiting  |
       |                      | _jobs (100)?                |
       |                      |   YES -> 拒绝新 Job         |
       |                      |                              |
       |                      | isQueueFull() == NO         |
       |                      | LoadingLoadTaskScheduler     |
       |                      | 有空闲线程?                  |
       |                      |   YES -> 取出 Job 执行      |
       |                      |   NO  -> 等待                |
       |                      |                              |
       |                      | 并发限制:                    |
       |                      | async_pending_load_task_   |
       |                      |   pool_size = 10            |
       |                      | async_loading_load_task_   |
       |                      |   pool_size = 10            |
```

## 七、超时与取消机制

### 7.1 超时检查时序

```
  FE: ConnectScheduler          BE: FragmentMgr
       |                           |
       | [每 1 秒]                 | [每 1 秒]
       | TimeoutChecker            | cancel_worker
       |                           |
       | checkTimeout(now)         | 遍历 _fragment_map
       |   |                       |   |
       |   +-- COM_SLEEP?          | is_timeout(now)?
       |   |   delta > wait_timeout? |   |
       |   |     YES -> kill 连接   |   YES -> cancel(fragment_id)
       |   |                       |         PPlanFragmentCancelReason::TIMEOUT
       |   +-- 活动查询?           |         |
       |       delta > query_      | cancel() 流程:
       |       timeout?            |   RuntimeState.is_cancelled = true
       |         YES -> kill 查询   |   VStreamMgr.cancel()
       |           StmtExecutor.    |   stream_mgr.cancel()
       |           cancel()        |   (解除 ExchangeNode 阻塞)
       |           Coordinator.     |   清理资源, 报告状态
       |           cancel()         |
       |                           |
       | 遍历 _fragments_ctx_map   | 遍历超时的 QueryFragmentsCtx
       |                           |   清理共享上下文
```

### 7.2 取消原因

| 原因 | 枚举值 | 触发方式 |
|------|--------|---------|
| 超时 | `TIMEOUT` | FragmentMgr.cancel_worker() 定期检查 |
| 限制达到 | `LIMIT_REACH` | 内存超限等 |
| 内部错误 | `INTERNAL_ERROR` | 执行异常 |
| 用户取消 | - | FE 调用 cancel RPC 或 Ctrl+C |

## 八、BE 节点选择与负载均衡

```
  Coordinator.computeScanRangeAssignment()
    |
    +-- Colocate Join:
    |     相同 Bucket 的 Tablet 分配到同一 BE (数据本地化)
    |
    +-- Bucket Shuffle Join:
    |     根据 Bucket 信息保持数据本地化
    |
    +-- 通用调度:
          round-robin + least-bytes-assigned
          |
          +-- selectBackendsByRoundRobin()
          |     每个 Scan Range 选择 assignedBytesPerHost 最小的 BE
          |     实现基本的负载均衡
          |
          +-- 过滤不可用 BE:
                +-- isAlive == false -> 排除
                +-- isQueryDisabled == true -> 排除
                +-- diskExceedLimit() -> 排除 (所有磁盘已满)
```

## 九、资源标签隔离

```
  FE: UserProperty.resource_tags  ->  ConnectContext.resourceTags
                                        |
                                        v
  Coordinator 发送 Fragment 时:
    TResourceInfo.user = "user_a"
    TResourceInfo.group = "normal"
    +-- TagManager 查找用户可访问的 BE (匹配 resource_tags)
    +-- 仅将 Fragment 分发到匹配标签的 BE

  BE 集合:
    +-- TagManager 维护 bidirectional index:
          tag -> resource (哪些 BE 有这个标签)
          resource -> tag (某个 BE 有哪些标签)
    +-- 用户设置 resource_tags 后, 查询仅路由到对应 BE
```

## 十、完整 QoS 配置汇总

### FE 配置

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| `qe_max_connection` | 1024 | FE 最大连接数 |
| `max_connection_scheduler_threads_num` | 4096 | 连接处理线程池 |
| `max_running_txn_num_per_db` | 100 | 每库最大事务数 |
| `desired_max_waiting_jobs` | 100 | Load 等待队列上限 |
| `async_pending_load_task_pool_size` | 10 | 并发 Pending Load 数 |
| `async_loading_load_task_pool_size` | 10 | 并发 Loading Load 数 |
| `load_pending_thread_num_high_priority` | 3 | 高优先级 Load 并发 |
| `load_pending_thread_num_normal_priority` | 10 | 普通优先级 Load 并发 |
| `max_routine_load_job_num` | 100 | Routine Load 任务上限 |
| `max_routine_load_task_num_per_be` | 5 | 每 BE Routine Load 上限 |
| `export_running_job_num_limit` | 5 | 并发 Export 上限 |
| `query_colocate_join_memory_limit_penalty_factor` | 1 | Colocate Join 内存惩罚因子 |
| `default_max_query_instances` | -1 | 用户最大查询实例数 |

### BE 配置

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| `mem_limit` | 80% | 进程内存上限 |
| `soft_mem_limit_frac` | 0.9 | 软内存限制比例 |
| `fragment_pool_thread_num_min` | 64 | Fragment 线程池最小线程 |
| `fragment_pool_thread_num_max` | 512 | Fragment 线程池最大线程 |
| `fragment_pool_queue_size` | 2048 | Fragment 队列大小 |
| `doris_scanner_thread_pool_thread_num` | 48 | Scanner 线程池大小 |
| `doris_scanner_thread_pool_queue_size` | 102400 | Scanner 队列大小 |
| `doris_cgroups` | "" | cgroup 根路径 (空=禁用) |
| `doris_blocking_priority_queue_wait_timeout_ms` | 5 | 优先级队列等待超时 |
| `doris_enable_scanner_thread_pool_per_disk` | false | 每磁盘独立 Scanner 线程池 |

### 用户级属性 (SET PROPERTY)

| 属性 | 说明 | 默认值 |
|------|------|--------|
| `max_user_connections` | 单用户最大连接数 | 100 |
| `max_query_instances` | 最大查询实例数 | -1 (无限制) |
| `cpu_resource_limit` | CPU 核数限制 | -1 (无限制) |
| `resource_tags` | 资源标签 (BE 隔离) | 空 |
| `sql_block_rules` | SQL 阻断规则 | 空 |
