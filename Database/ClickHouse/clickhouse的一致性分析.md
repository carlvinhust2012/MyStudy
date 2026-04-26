# ClickHouse 元数据与副本一致性分析

> 基于 ClickHouse 源码 (ClickHouse-master) 分析，聚焦 ReplicatedMergeTree + ZooKeeper 的一致性机制

## 一、一致性架构总览

```mermaid
flowchart TB
    subgraph ZK["ZooKeeper (唯一真相源)"]
        Meta["/metadata (表结构)"]
        Cols["/columns (列定义)"]
        Log["/log (复制日志)"]
        Replicas["/replicas (副本列表)"]
        Mutations["/mutations (变更定义)"]
        Quorum["/quorum (仲裁写入)"]
        Blocks["/block_numbers (去重)"]
    end

    subgraph R1["Replica 1"]
        R1Queue["/queue (本地任务队列)"]
        R1Parts["/parts (本地 Part 列表)"]
        R1MetaVer["metadata_version"]
    end

    subgraph R2["Replica 2"]
        R2Queue["/queue"]
        R2Parts["/parts"]
        R2MetaVer["metadata_version"]
    end

    subgraph R3["Replica 3"]
        R3Queue["/queue"]
        R3Parts["/parts"]
        R3MetaVer["metadata_version"]
    end

    Log -->|"GET_PART / MERGE_PARTS"| R1Queue
    Log -->|"GET_PART / MERGE_PARTS"| R2Queue
    Log -->|"GET_PART / MERGE_PARTS"| R3Queue

    R1 -->|"写入"| ZK
    R2 -->|"写入"| ZK
    R3 -->|"写入"| ZK
```

核心设计: **ZooKeeper 是所有副本的唯一真相源 (Single Source of Truth)**, `/log` 是共享的复制状态机, 每个副本的 `/queue` 是本地已处理的检查点。

## 二、ZooKeeper Znode 完整结构

```mermaid
flowchart TB
    Root["{zookeeper_path} (表根路径)"]

    subgraph Shared["共享元数据 (所有副本一致)"]
        Meta["metadata: ReplicatedMergeTreeTableMetadata"]
        Cols["columns: 列描述"]
        MetaVer["metadata_version: 全局 schema 版本号"]
        Leader["leader_election/leader (临时节点)"]
    end

    subgraph Log["/log (共享复制日志)"]
        L0["log-0000000000: GET_PART"]
        L1["log-0000000001: MERGE_PARTS"]
        L2["log-0000000002: ALTER_METADATA"]
        L3["log-0000000003: MUTATE_PART"]
        L4["log-0000000004: DROP_RANGE"]
    end

    subgraph BlockNum["/block_numbers (去重)"]
        BN1["partition_202504: 42"]
        BN2["partition_202505: 13"]
    end

    subgraph Mut["/mutations (变更)"]
        M0["mutation-0000000000: ALTER UPDATE"]
        M1["mutation-0000000001: ALTER DELETE"]
    end

    subgraph Quorum["/quorum (仲裁)"]
        QS["status: ReplicatedMergeTreeQuorumEntry"]
        QP["parallel/{part_name}"]
        QF["failed_parts/"]
    end

    subgraph PerReplica["/replicas/{name} (每个副本)"]
        Host["host: host:port"]
        Active["is_active (临时节点, 存活标记)"]
        LogPtr["log_pointer: 已处理到 /log 的位置"]
        Queue["queue/: 本地任务队列"]
        Parts["parts/: 本地拥有哪些 Part"]
        MetaVer2["metadata_version: 本地 schema 版本"]
        IsLost["is_lost: 是否标记为丢失"]
        MutPtr["mutation_pointer: 已处理到的 mutation"]
    end

    Root --> Shared
    Root --> Log
    Root --> BlockNum
    Root --> Mut
    Root --> Quorum
    Root --> PerReplica
```

### 复制日志条目类型

| 类型 | 含义 | 说明 |
|------|------|------|
| `GET_PART` | 从其他副本拉取 Part | 最常见的条目 |
| `ATTACH_PART` | 挂载本地 Part | 可能来自 /detached |
| `MERGE_PARTS` | 合并多个 Part | 只记录合并结果, 各副本自行执行 |
| `DROP_RANGE` | 删除一个范围的 Part | 副本执行本地删除 |
| `REPLACE_RANGE` | 替换一个范围 | DROP + 插入 |
| `MUTATE_PART` | 对 Part 应用 Mutation | ALTER UPDATE/DELETE |
| `ALTER_METADATA` | Schema 变更 | 所有副本更新表结构 |

## 三、INSERT 数据复制流程

```mermaid
sequenceDiagram
    participant C as Client
    participant R1 as Replica 1 (写入方)
    participant ZK as ZooKeeper
    participant R2 as Replica 2
    participant R3 as Replica 3

    C->>R1: INSERT INTO t VALUES (...)

    rect rgb(230, 255, 230)
        Note right of R1: Step 1: 本地写入 Part
        R1->>R1: 写入 tmp Part (排序 + 压缩)
        R1->>R1: commit: tmp → Active
    end

    rect rgb(255, 245, 230)
        Note right of R1: Step 2: ZooKeeper 原子提交
        R1->>ZK: multi() 原子操作:
        Note over ZK: 1. 检查 block_numbers/{partition} 无冲突
        Note over ZK: 2. block_numbers/{partition} + 1
        Note over ZK: 3. /log 新增 GET_PART 条目
        Note over ZK: 4. /replicas/R1/parts 新增 Part 元数据
        Note over ZK: 5. /replicas/R1/log_pointer 更新
        Note over ZK: 全部成功或全部失败 (原子性)
        ZK-->>R1: 提交成功
    end

    rect rgb(240, 240, 255)
        Note right of ZK: Step 3: 其他副本发现新条目
        ZK-->>R2: watch 通知 /log 变化
        ZK-->>R3: watch 通知 /log 变化
    end

    rect rgb(255, 240, 230)
        Note right of R2: Step 4: 副本拉取数据
        R2->>ZK: 读 /log 新条目
        R2->>ZK: 复制到 /replicas/R2/queue/
        R2->>R1: HTTP GET /replicas/fetch (interserver 协议)
        R1-->>R2: Part 数据 + checksums
        R2->>R2: 校验 checksums
        R2->>ZK: /replicas/R2/parts 新增 Part 元数据
    end

    rect rgb(255, 230, 240)
        Note right of R3: Step 5: 副本 3 同样拉取
        R3->>ZK: 读 /log 新条目
        R3->>ZK: 复制到 /replicas/R3/queue/
        R3->>R1: HTTP GET /replicas/fetch
        R1-->>R3: Part 数据
        R3->>R3: 校验 + 注册到 ZK
    end

    rect rgb(230, 255, 255)
        Note right of ZK: Step 6: 所有副本完成
        Note over ZK: /replicas/R1/parts: 有 Part
        Note over ZK: /replicas/R2/parts: 有 Part
        Note over ZK: /replicas/R3/parts: 有 Part
        Note over ZK: 数据一致
    end
```

**关键保证**: `multi()` 原子操作确保 block_numbers 分配 + log 写入 + parts 注册要么全部成功要么全部失败, 不会出现分配了 block number 但 log 未写入的不一致状态。

## 四、Quorum INSERT 流程

```mermaid
sequenceDiagram
    participant C as Client
    participant R1 as Replica 1
    participant ZK as ZooKeeper
    participant R2 as Replica 2
    participant R3 as Replica 3

    C->>R1: INSERT ... SETTINGS insert_quorum=2

    rect rgb(255, 240, 230)
        Note right of R1: 前置检查
        R1->>ZK: 列出 /replicas/ 所有副本
        R1->>ZK: 检查每个副本的 is_active
        ZK-->>R1: 活跃副本数 = 3 >= quorum (2)
        R1->>ZK: 检查 /quorum/status 是否存在
        Note over ZK: 非并行模式: 上一轮 quorum 未完成则拒绝
    end

    rect rgb(230, 255, 230)
        Note right of R1: 本地写入 + ZK 提交
        R1->>R1: 写入本地 Part
        R1->>ZK: multi() 写入 /log + /parts
        R1->>ZK: 创建 /quorum/parallel/{part_name}
        Note over ZK: ReplicatedMergeTreeQuorumEntry:<br/>part_name, required_replicas=2,<br/>actual_replicas=0, replicas=[]
    end

    rect rgb(240, 240, 255)
        Note right of ZK: 其他副本确认 quorum
        R2->>ZK: 读取 /log, 拉取 Part
        R2->>R1: fetch Part 数据
        R2->>ZK: 注册 /replicas/R2/parts
        R2->>ZK: 更新 /quorum/parallel/{part_name}<br/>actual_replicas=2

        R3->>ZK: 读取 /log, 拉取 Part
        R3->>ZK: 注册 /replicas/R3/parts
        R3->>ZK: 更新 /quorum/parallel/{part_name}<br/>actual_replicas=3
    end

    rect rgb(255, 255, 230)
        Note right of R1: Quorum 达成
        R1->>ZK: waitForQuorum() 轮询
        Note over ZK: actual_replicas >= required_replicas
        ZK-->>R1: Quorum 达成!
        R1-->>C: INSERT 成功
    end
```

### Quorum 超时处理

```mermaid
flowchart TD
    Start["INSERT with quorum"] --> Check{"活跃副本 >= quorum?"}
    Check -->|"否"| Err1["TOO_FEW_LIVE_REPLICAS"]
    Check -->|"是"| Write["本地写入 + ZK 提交"]
    Write --> Wait["waitForQuorum() 轮询"]
    Wait --> Timeout{"超时 (默认 600s)?"}
    Timeout -->|"未超时"| CheckQuorum{"actual >= required?"}
    CheckQuorum -->|"是"| Success["INSERT 成功"]
    CheckQuorum -->|"否"| Wait
    Timeout -->|"超时"| Fail["UNKNOWN_STATUS_OF_INSERT"]
    Fail --> Note["数据已写入本地副本<br/>但 quorum 状态未知<br/>/quorum/status 节点残留<br/>阻塞后续非并行 quorum 写入"]
```

## 五、select_sequential_consistency 流程

```mermaid
sequenceDiagram
    participant C as Client
    participant R as Replica
    participant ZK as ZooKeeper

    C->>R: SELECT ... SETTINGS select_sequential_consistency=1

    rect rgb(255, 245, 230)
        Note right of R: Step 1: 查询本地最大 block number
        R->>R: 扫描所有本地 Part
        R->>R: 计算每个 partition 的 max_added_blocks
        Note over R: partition_202504: max_block=42
        Note over R: partition_202505: max_block=13
    end

    rect rgb(240, 255, 230)
        Note right of R: Step 2: 查询 ZK quorum 状态
        R->>ZK: 读取 /quorum/parallel/{part_name}
        R->>ZK: 读取 /replicas/self/quorum/parallel/
        ZK-->>R: QuorumEntry 数据
        Note over R: 对比: quorum 确认的 block number<br/>vs 本地拥有的 block number
    end

    rect rgb(255, 240, 230)
        Note right of R: Step 3: 过滤未达到 quorum 的 Part
        R->>R: foreachActiveParts()
        Note over R: Part A: max_block=42, quorum=42 → 保留
        Note over R: Part B: max_block=44, quorum=42 → 跳过!
        Note over R: Part B 是非 quorum INSERT 的数据<br/>可能其他副本没有
    end

    R-->>C: 只返回 quorum 确认的数据

    Note over R,C: 注意: insert_quorum_parallel 开启时<br/>sequential_consistency 不生效<br/>因为并行 INSERT 可能写入不同副本子集
```

## 六、DDL ON CLUSTER 复制流程

```mermaid
sequenceDiagram
    participant C as Client (R1)
    participant DDL as DDLWorker (R1)
    participant ZK as ZooKeeper
    participant R2 as DDLWorker (R2)
    participant R3 as DDLWorker (R3)

    C->>DDL: CREATE TABLE t ON CLUSTER cluster

    rect rgb(230, 255, 230)
        Note right of DDL: Step 1: 入队
        DDL->>DDL: 创建 DDLLogEntry
        Note over DDL: 包含: SQL 语句 + 目标 host 列表
        DDL->>ZK: createSequential(/clickhouse/task_queue/ddl/query-)
        Note over ZK: query-0000000001
        ZK-->>DDL: 创建成功
    end

    rect rgb(255, 245, 230)
        Note right of ZK: Step 2: 所有副本发现新任务
        Note over ZK: ZK watch 通知
        ZK-->>DDL: 子节点变化通知
        ZK-->>R2: 子节点变化通知
        ZK-->>R3: 子节点变化通知
    end

    rect rgb(240, 240, 255)
        Note right of DDL: Step 3: 各副本执行 DDL
        par 并行执行
            DDL->>DDL: 解析 SQL, 匹配本地 host
            DDL->>DDL: 执行 CREATE TABLE 本地
            DDL->>ZK: 写入 finished/R1 状态
        and
            R2->>R2: 解析 SQL, 匹配本地 host
            R2->>R2: 执行 CREATE TABLE 本地
            R2->>ZK: 写入 finished/R2 状态
        and
            R3->>R3: 解析 SQL, 匹配本地 host
            R3->>R3: 执行 CREATE TABLE 本地
            R3->>ZK: 写入 finished/R3 状态
        end
    end

    rect rgb(255, 255, 230)
        Note right of C: Step 4: 等待所有副本完成
        DDL->>ZK: 轮询 finished/ 下所有 host 状态
        ZK-->>DDL: R1=OK, R2=OK, R3=OK
        DDL-->>C: CREATE TABLE 成功 (所有副本)
    end

    Note over ZK: 任务生命周期: 7 天后自动清理<br/>max_tasks_in_queue = 1000
```

## 七、ALTER TABLE Schema 变更复制

```mermaid
sequenceDiagram
    participant C as Client
    participant R1 as Replica 1 (发起 ALTER)
    participant ZK as ZooKeeper
    participant R2 as Replica 2
    participant R3 as Replica 3

    C->>R1: ALTER TABLE t ADD COLUMN new_col String

    rect rgb(255, 240, 230)
        Note right of R1: Step 1: Compare-And-Swap metadata_version
        R1->>ZK: 读取 /metadata_version (当前值=N)
        R1->>ZK: multi() CAS 操作:
        Note over ZK: 1. /metadata_version = N+1 (CAS, 版本必须一致)
        Note over ZK: 2. 更新 /metadata (新表结构)
        Note over ZK: 3. 更新 /columns (新列定义)
        Note over ZK: 4. /log 新增 ALTER_METADATA 条目 (带 alter_version=N+1)
    end

    alt CAS 成功
        ZK-->>R1: ZOK, ALTER 提交成功
        R1->>R1: 本地应用 schema 变更
        R1->>ZK: 更新 /replicas/R1/metadata_version = N+1
        R1-->>C: ALTER TABLE 成功
    else CAS 失败 (并发 ALTER)
        ZK-->>R1: ZBADVERSION
        Note over R1: 其他副本已提交了 ALTER
        R1->>R1: 重试循环 (读取新版本, 重新执行)
    end

    rect rgb(240, 255, 240)
        Note right of ZK: Step 2: 其他副本应用 ALTER
        R2->>ZK: watch /log → 发现 ALTER_METADATA 条目
        R2->>ZK: 读取 /metadata, /columns (新版本)
        R2->>R2: 本地应用 schema 变更
        R2->>ZK: 更新 /replicas/R2/metadata_version = N+1

        R3->>ZK: watch /log → 发现 ALTER_METADATA 条目
        R3->>ZK: 读取新 metadata
        R3->>R3: 本地应用
        R3->>ZK: 更新 /replicas/R3/metadata_version = N+1
    end
```

### 并发 ALTER 冲突解决

```mermaid
flowchart TD
    A1["Replica 1: ALTER ADD col_a"] --> CAS1{"CAS /metadata_version?<br/>期望 N → N+1"}
    A2["Replica 2: ALTER ADD col_b"] --> CAS2{"CAS /metadata_version?<br/>期望 N → N+1"}

    CAS1 -->|"ZOK (先到)"| Success1["提交成功, version=N+1"]
    CAS2 -->|"ZBADVERSION"| Retry["重试: 读取 N+1, 提交 N+2"]

    Success1 --> Final["最终: 两个 ALTER 都生效"]
    Retry --> Final
```

## 八、后台合并协调

```mermaid
sequenceDiagram
    participant R1 as Replica 1 (Leader)
    participant ZK as ZooKeeper
    participant R2 as Replica 2
    participant R3 as Replica 3

    rect rgb(255, 245, 230)
        Note right of R1: Leader 执行合并
        R1->>R1: 选择 Part A+B 合并
        R1->>R1: 多路归并 → Part D (30MB)
        R1->>R1: commit: Part D Active, A+B Outdated
    end

    rect rgb(240, 255, 230)
        Note right of R1: 通知其他副本
        R1->>ZK: /log 新增 MERGE_PARTS 条目
        Note over ZK: source_parts=[A,B], result_part=D
    end

    rect rgb(255, 240, 240)
        Note right of R2: Replica 2 独立合并
        R2->>ZK: 发现 MERGE_PARTS 条目
        R2->>R2: 本地合并 A+B → D
        R2->>R2: commit: D Active, A+B Outdated
        R2->>ZK: /log 新增 DROP_RANGE [A,B]
    end

    rect rgb(240, 230, 255)
        Note right of R3: Replica 3 可能拉取或合并
        alt R3 已有 A+B
            R3->>R3: 本地合并 → D (和 R2 一样)
        else R3 缺少某些 Part
            R3->>ZK: 发现 MERGE_PARTS
            R3->>ZK: 发现自己缺少 Part
            R3->>R1: fetch Part D (直接拉取合并结果)
            R3->>R3: 注册 Part D
        end
    end

    rect rgb(255, 255, 230)
        Note right of ZK: 清理
        Note over ZK: /replicas/*/parts: 只有 Part D
        Note over ZK: Part A, B 的记录被覆盖
        Note over ZK: 所有副本最终一致
    end
```

**关键设计**: 合并是**各副本独立执行**的, 不是由 Leader 推送合并后的数据。每个副本根据 `/log` 中的 `MERGE_PARTS` 条目自行执行合并。这避免了大数据量的网络传输。

## 九、副本恢复流程

```mermaid
sequenceDiagram
    participant R as 新/恢复中的 Replica
    participant ZK as ZooKeeper
    participant S as 源 Replica (健康)

    rect rgb(255, 240, 230)
        Note right of R: Step 1: 检查是否需要 clone
        R->>ZK: 检查 /replicas/self/is_lost
        alt is_lost = "1" (标记为丢失)
            R->>R: 需要完整重建, 调用 cloneReplica()
        else is_lost 不存在 (正常)
            R->>R: 只需增量同步
        end
    end

    rect rgb(240, 255, 230)
        Note right of R: Step 2: cloneReplica (完整重建)
        R->>ZK: 读取源 replica 的 log_pointer
        Note over ZK: 用 multi() 原子读取
        Note over ZK: 包含 version 检查, 防止读取过程中源 replica 状态变化
        ZK-->>R: 源 replica 的队列 + log_pointer

        R->>ZK: 过滤掉已有 Part, 复制队列条目到本地
        R->>ZK: 清理本地 ZK 中多余的 Part 记录
        R->>ZK: 为缺失 Part 创建 GET_PART 队列条目
        R->>ZK: 读取源 replica 的 metadata + columns + metadata_version
        R->>ZK: 本地队列添加 ALTER_METADATA 条目
        R->>ZK: 设置 is_lost = "0"
    end

    rect rgb(255, 245, 240)
        Note right of R: Step 3: fetchPart (逐个拉取)
        loop 每个缺失的 Part
            R->>ZK: 选择拥有该 Part 的健康副本
            R->>ZK: 验证源 replica is_active
            R->>S: HTTP GET /replicas/fetch (interserver 协议)
            S-->>R: Part 数据文件

            alt 零拷贝 (远程磁盘, S3/HDFS)
                R->>ZK: 只复制元数据指针 (无需传输数据)
            else 正常拉取
                R->>R: 写入本地 Part 文件
                R->>R: 校验 checksums (与 ZK 中的 PartHeader 对比)
            end

            R->>ZK: 注册 /replicas/self/parts/{part_name}
        end
    end

    rect rgb(230, 255, 255)
        Note right of R: Step 4: 恢复完成
        R->>ZK: 更新 log_pointer
        R->>ZK: is_active 节点存在
        Note over R: 副本完全恢复, 可正常读写
    end
```

### ReplicatedMergeTreeAttachThread 后台恢复

```mermaid
flowchart TB
    Start["AttachThread 启动"] --> Watch["watch /log 变化"]
    Watch --> Poll["轮询 /queue 中的条目"]

    Poll --> Entry{"读取队列条目"}
    Entry -->|"GET_PART"| Check{"本地已有该 Part?"}
    Check -->|"有"| Next["跳过, 更新 log_pointer"]
    Check -->|"没有"| Fetch["调用 fetchPart()"]
    Fetch --> Verify{"checksums 匹配?"}
    Verify -->|"是"| Commit["注册到 ZK"]
    Verify -->|"否"| Retry["重试 + 指数退避"]
    Retry --> Fetch
    Commit --> Next
    Entry -->|"MERGE_PARTS"| Merge["本地执行合并"]
    Merge --> Next
    Entry -->|"ALTER_METADATA"| Alter["应用 schema 变更"]
    Alter --> Next
    Entry -->|"MUTATE_PART"| Mutate["应用 mutation"]
    Mutate --> Next
    Entry -->|"DROP_RANGE"| Drop["本地删除 Part"]
    Drop --> Next
    Next --> Poll
```

## 十、ZooKeeper Session 过期处理

```mermaid
sequenceDiagram
    participant R as Replica
    participant ZK as ZooKeeper
    participant Restart as ReplicatedMergeTreeRestartingThread
    participant Other as 其他 Replica

    rect rgb(255, 230, 230)
        Note right of ZK: Session 过期
        Note over ZK: is_active 临时节点被删除
        Note over R: 本地收到 ZK 连接断开通知
        R->>R: 停止接受写入
        R->>R: 停止后台合并
    end

    rect rgb(240, 255, 230)
        Note right of R: 自动重连
        R->>ZK: 尝试重新建立 Session
        alt 重连成功
            ZK-->>R: Session 恢复
            R->>R: RestartingThread 触发
            Restart->>ZK: 检查 /metadata_version
            Restart->>ZK: 检查本地 metadata_version
            alt 版本一致
                Note over Restart: 无需修复
            else 版本不一致
                Restart->>Restart: 应用缺失的 ALTER_METADATA
            end
            Restart->>ZK: 重新创建 is_active 临时节点
            Note over R: 恢复正常
        else 重连失败 (ZK 集群不可用)
            Note over R: 持续重试
            Note over Other: 其他副本继续正常工作
        end
    end
```

## 十一、多级一致性保证总结

```mermaid
mindmap
    root((ClickHouse 一致性保证))
        数据一致性
            INSERT: multi() 原子提交
            Quorum INSERT: 多副本确认
            select_sequential_consistency: 读取仲裁确认数据
            去重: block_numbers 防重复
            checksums: Part 级数据完整性
        元数据一致性
            CREATE ON CLUSTER: DDL 队列 + 状态跟踪
            ALTER: CAS metadata_version
            ALTER_METADATA log 条目
            .sql 文件: 启动时加载 + crash 恢复
        副本一致性
            /log: 全局复制日志 (共享状态机)
            /queue: 本地检查点 (每副本独立)
            cloneReplica: 完整重建
            fetchPart: 增量同步 + 校验
            AttachThread: 后台持续追赶
        故障恢复
            Session 过期: 自动重连 + 元数据修复
            副本丢失: is_lost 标记 + clone
            Part 损坏: 从其他副本重新拉取
            ZK 短暂不可用: 本地缓冲 + 重试
        设计权衡
            最终一致: 不是强一致
            异步复制: 其他副本异步拉取
            独立合并: 不推送合并数据, 各副本自行计算
            R=W=All: Quorum INSERT 实现读写都确认
```

## 十二、关键源码文件索引

| 文件 | 职责 |
|------|------|
| `Storages/StorageReplicatedMergeTree.h/cpp` | 副本引擎核心实现, clone/fetch/ALTER/quorum |
| `Storages/MergeTree/ReplicatedMergeTreeLogEntry.h` | 复制日志条目类型定义 |
| `Storages/MergeTree/ReplicatedMergeTreeSink.cpp` | INSERT + quorum 实现 |
| `Storages/MergeTree/ReplicatedMergeTreeQuorumEntry.h` | Quorum 状态数据结构 |
| `Storages/MergeTree/ReplicatedMergeTreeAttachThread.cpp` | 后台 Part 拉取线程 |
| `Storages/MergeTree/ReplicatedMergeTreeRestartingThread.cpp` | Session 恢复 + 元数据修复 |
| `Interpreters/DDLWorker.h/cpp` | DDL ON CLUSTER 队列处理 |
| `Interpreters/executeDDLQueryOnCluster.cpp` | DDL 分布式执行入口 |
| `Databases/DatabaseOnDisk.cpp` | .sql 元数据文件存储与加载 |
| `Common/ZooKeeper/ZooKeeper.h/cpp` | ZooKeeper 客户端封装 |
