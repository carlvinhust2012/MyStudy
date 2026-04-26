# ClickHouse 分布式部署分析

> 总结 4 个关键问题: 分布式模块、分片机制、副本配置、ZK/Keeper 一致性协议

## 一、ClickHouse 分布式部署模块总览

### 完整架构图

```mermaid
flowchart TB
    Client["客户端<br/>clickhouse-client / JDBC / HTTP"]

    subgraph LB["负载均衡层 (可选)"]
        Proxy["CHProxy / Nginx / HAProxy"]
    end

    subgraph Cluster["ClickHouse Cluster"]
        subgraph S1["Shard 1"]
            R1["Replica 1 (Server 1)"]
            R2["Replica 2 (Server 2)"]
        end
        subgraph S2["Shard 2"]
            R3["Replica 3 (Server 3)"]
            R4["Replica 4 (Server 4)"]
        end
    end

    subgraph Coord["元数据协调 (3~5 节点)"]
        K1["Keeper/ZK 1"]
        K2["Keeper/ZK 2"]
        K3["Keeper/ZK 3"]
    end

    subgraph Monitor["监控体系"]
        Sys["system.* 系统表"]
        Prometheus["Prometheus Exporter"]
        Grafana["Grafana Dashboard"]
    end

    Client --> Proxy --> Cluster
    Cluster --> Coord
    Cluster --> Monitor
```

### 各模块职责

| 模块 | 职责 | 是否必须 |
|------|------|---------|
| **ClickHouse Server** | 数据存储 + 查询计算 | 必须 |
| **ClickHouse Keeper / ZooKeeper** | 副本协调, 复制日志, DDL 同步, block 去重 | 使用 ReplicatedMergeTree 时必须 |
| **Distributed 表引擎** | 跨分片查询路由, 结果汇总 | 分布式查询时必须 |
| **CHProxy / Nginx** | 负载均衡, 限流, 路由 | 可选, 推荐生产使用 |
| **clickhouse-backup** | 备份恢复 (S3/磁盘) | 可选 |
| **clickhouse-copier** | 跨集群数据迁移 | 可选 |
| **Prometheus + Grafana** | 监控告警 | 推荐 |

### 集群配置示例

```xml
<clickhouse>
    <remote_servers>
        <my_cluster>
            <shard>
                <replica><host>node1</host><port>9000</port></replica>
                <replica><host>node2</host><port>9000</port></replica>
            </shard>
            <shard>
                <replica><host>node3</host><port>9000</port></replica>
                <replica><host>node4</host><port>9000</port></replica>
            </shard>
        </my_cluster>
    </remote_servers>
</clickhouse>
```

- **Shard**: 水平分片, 每个 Shard 存储不同数据子集
- **Replica**: 同一 Shard 内的副本, 保证高可用

### 典型部署拓扑

| 场景 | 节点数 | 配置 |
|------|--------|------|
| 开发测试 | 1 | 单节点, 无副本 |
| 最小高可用 | 7 | 2 分片 x 2 副本 + 3 Keeper |
| 中等规模 | 10 | 3 分片 x 2 副本 + 1 负载均衡 |
| 大规模生产 | 15+ | 3 分片 x 3 副本 + 3 Keeper + 负载均衡 |

## 二、Part 分片机制: 数据如何分布到不同节点

### 核心结论

**Part 不会跨节点存储。每个 Part 完整存在于一个 Shard 内, 不同 Shard 的 Part 是不同的数据子集。**

### 未分片: 所有 Part 在同一节点

```
Server 1
└── table_A (local_table)
    ├── 202504_1_1_0   (10MB)
    ├── 202504_2_2_0   (10MB)
    ├── 202504_3_3_0   (10MB)
    └── 202504_4_6_1   (30MB, 合并后)
```

### 有分片: 数据按分片键打散

```mermaid
flowchart TB
    Client["客户端 INSERT"] --> Dist["Distributed 表<br/>(路由代理, 不存数据)"]
    Dist -->|"intHash32(user_id) % 2 = 0"| S1["Shard 1 (Server 1, Server 2)"]
    Dist -->|"intHash32(user_id) % 2 = 1"| S2["Shard 2 (Server 3, Server 4)"]

    S1 --> P1["local_table_A<br/>202504_1_1_0 (5MB)<br/>202504_3_3_0 (5MB)"]
    S2 --> P2["local_table_A<br/>202504_2_2_0 (5MB)<br/>202504_4_4_0 (5MB)"]
```

### 分片写入流程

```mermaid
sequenceDiagram
    participant C as Client
    participant Dist as Distributed 表
    participant S1 as Shard 1
    participant S2 as Shard 2
    participant ZK as ZooKeeper/Keeper

    C->>Dist: INSERT INTO table_A VALUES (1, 'alice'), (2, 'bob')

    rect rgb(255, 245, 230)
        Note right of Dist: Step 1: 按分片键路由
        Dist->>Dist: intHash32(user_id) % 2
        Note over Dist: row(1, alice): hash=0 -> Shard 1
        Note over Dist: row(2, bob): hash=1 -> Shard 2
    end

    rect rgb(240, 255, 230)
        Note right of Dist: Step 2: 分发到各 Shard
        Dist->>S1: INSERT row(1, alice)
        Dist->>S2: INSERT row(2, bob)
    end

    rect rgb(245, 240, 255)
        Note right of S1: Step 3: 各 Shard 独立写入
        S1->>S1: 写入本地 Part 202504_5_5_0
        S1->>ZK: multi() 提交 (block_number + log + parts)
        S2->>S2: 写入本地 Part 202504_6_6_0
        S2->>ZK: multi() 提交
    end
```

### 分片查询流程

```mermaid
sequenceDiagram
    participant C as Client
    participant Dist as Distributed 表
    participant S1 as Shard 1
    participant S2 as Shard 2

    C->>Dist: SELECT a, b FROM table_A WHERE a > 0

    rect rgb(255, 245, 230)
        Note right of Dist: Step 1: 广播到所有 Shard
        Dist->>S1: SELECT a, b FROM local_table WHERE a > 0
        Dist->>S2: SELECT a, b FROM local_table WHERE a > 0
    end

    rect rgb(240, 255, 230)
        Note right of S1: Step 2: 各 Shard 并行执行
        Note over S1: 读本地 Part, 多级索引过滤
        Note over S2: 读本地 Part, 多级索引过滤
        S1-->>Dist: 结果集 1
        S2-->>Dist: 结果集 2
    end

    rect rgb(245, 240, 255)
        Note right of Dist: Step 3: 汇总返回
        Dist->>Dist: 合并结果集 1 + 2
        Dist-->>C: 最终结果
    end
```

### 常见分片键选择

| 分片键 | 分布方式 | 适用场景 |
|--------|---------|---------|
| `intHash32(user_id)` | 哈希均匀分布 | 用户行为表, 按用户查询 |
| `toYYYYMM(event_date)` | 时间范围 | 日志表, 按时间查询 |
| `rand()` | 随机分布 | 无特定查询模式 |
| `''` (空) | 全部到一个 Shard | 相当于不分片 |

## 三、副本机制: Part 的副本数如何决定

### 核心结论

**ClickHouse 没有默认副本数。副本数 = 集群配置中每个 Shard 下的 replica 标签数量。**

### 副本配置方式

```xml
<shard>
    <replica><host>node1</host><port>9000</port></replica>  <!-- 副本 1 -->
    <replica><host>node2</host><port>9000</port></replica>  <!-- 副本 2 -->
    <replica><host>node5</host><port>9000</port></replica>  <!-- 副本 3 -->
</shard>
```

### 建表语句

```sql
-- 本地表 (每个 Shard 的每个副本上都创建)
CREATE TABLE local_table ON CLUSTER my_cluster (
    a UInt32, b String, event_date Date
) ENGINE = ReplicatedMergeTree(
    '/clickhouse/tables/{cluster}/local_table',  -- ZK 路径, 同 Shard 副本共享
    '{replica}'                                    -- 副本标识, 自动替换
)
PARTITION BY toYYYYMM(event_date)
ORDER BY a

-- 分布式表 (所有节点上都创建)
CREATE TABLE distributed_table ON CLUSTER my_cluster (
    a UInt32, b String, event_date Date
) ENGINE = Distributed(my_cluster, db, local_table, intHash32(a))
```

### 副本与 Part 的关系

```mermaid
flowchart TB
    Shard["Shard 1"]
    Shard --> R1["Replica 1 (Server 1)"]
    Shard --> R2["Replica 2 (Server 2)"]

    R1 --> P1["local_table<br/>202504_1_1_0 (10MB)<br/>202504_2_2_0 (10MB)"]
    R2 --> P2["local_table<br/>202504_1_1_0 (10MB) 完整拷贝<br/>202504_2_2_0 (10MB) 完整拷贝"]

    ZK["ZooKeeper/Keeper<br/>/log 复制日志<br/>/replicas/*/parts 注册"]

    R1 -.->|"写入 + 拉取"| ZK
    R2 -.->|"拉取 + 注册"| ZK

    style P1 fill:#E3F2FD
    style P2 fill:#E8F5E9
```

### 副本对写入的影响

```mermaid
sequenceDiagram
    participant C as Client
    participant R1 as Replica 1 (写入方)
    participant ZK as ZooKeeper/Keeper
    participant R2 as Replica 2
    participant R3 as Replica 3

    C->>R1: INSERT INTO local_table VALUES (...)

    rect rgb(230, 255, 230)
        Note right of R1: Step 1: 本地写入 (极快)
        R1->>R1: 排序 + 压缩, 写入 Part
        Note over R1: 写入延迟 = 排序 + 写磁盘 + rename
    end

    rect rgb(255, 245, 230)
        Note right of R1: Step 2: ZK 提交 (阻塞等待)
        R1->>ZK: multi() 原子操作
        Note over ZK: block_numbers + /log + /parts
        ZK-->>R1: 提交成功
    end

    R1-->>C: INSERT 成功 (不等副本拉取)

    rect rgb(240, 240, 255)
        Note right of ZK: Step 3: 异步拉取 (客户端无感)
        ZK-->>R2: watch 通知
        ZK-->>R3: watch 通知
        R2->>R1: HTTP fetch Part 数据
        R3->>R1: HTTP fetch Part 数据
        Note over R2: 拉取完成后可查到数据
    end
```

### 副本数选择建议

| 副本数 | 可容忍故障 | 存储开销 | 适用场景 |
|--------|-----------|---------|---------|
| 1 | 0 | 1x | 开发测试, 可重建数据 |
| 2 | 1 | 2x | 中小规模生产 (最常见) |
| 3 | 2 | 3x | 金融级, 大规模生产 |
| 3+ | 2 | 3x+ | 超过 3 副本意义不大 |

### 不需要副本的场景

```sql
-- MergeTree (非 Replicated), 无副本, 不依赖 ZK/Keeper
CREATE TABLE temp_table (
    a UInt32, b String
) ENGINE = MergeTree()
ORDER BY a
```

适用场景: 临时表, ETL 中间表, 可重建的数据。

## 四、ZooKeeper / ClickHouse Keeper 一致性协议

### 核心结论

**两者都使用 ZAB 协议 (ZooKeeper Atomic Broadcast), 基于事务日志 (Transaction Log) + 多数派确认 (Quorum) 保证一致性。**

### ZAB 协议写入流程

```mermaid
sequenceDiagram
    participant C as ClickHouse 节点
    participant Leader as Leader
    participant F1 as Follower 1
    participant F2 as Follower 2
    participant F3 as Follower 3

    C->>Leader: multi() 写入请求

    rect rgb(255, 245, 230)
        Note right of Leader: Step 1: Leader 生成 ZXID
        Leader->>Leader: 分配 ZXID = 0x0000000000000005
        Leader->>Leader: 写本地事务日志 + fsync
        Note over Leader: 磁盘 I/O: fsync 确保日志落盘
    end

    rect rgb(240, 255, 230)
        Note right of Leader: Step 2: 广播 PROPOSAL
        Leader->>F1: PROPOSAL(ZXID=5, data)
        Leader->>F2: PROPOSAL(ZXID=5, data)
        Leader->>F3: PROPOSAL(ZXID=5, data)
    end

    rect rgb(245, 240, 255)
        Note right of F1: Step 3: Follower 写日志
        F1->>F1: 写事务日志 + fsync
        F1-->>Leader: ACK(ZXID=5)
        F2->>F2: 写事务日志 + fsync
        F2-->>Leader: ACK(ZXID=5)
        Note over F3: F3 宕机, 无响应
    end

    rect rgb(255, 230, 245)
        Note right of Leader: Step 4: Quorum 确认
        Leader->>Leader: 收到 2/3 ACK, 达到多数派
        Leader->>Leader: 提交事务, 应用到内存
    end

    rect rgb(230, 255, 255)
        Note right of Leader: Step 5: 广播 COMMIT
        Leader->>F1: COMMIT(ZXID=5)
        Leader->>F2: COMMIT(ZXID=5)
        F1->>F1: 应用到内存数据树
        F2->>F2: 应用到内存数据树
        Leader-->>C: multi() 成功
    end
```

### ZXID 严格有序

```
ZXID = 64 位整数
  高 32 位: epoch (每次重新选举 Leader 递增)
  低 32 位: counter (每次写操作递增)

时间线:
  ZXID 0x0000000000000001  → 第一个事务
  ZXID 0x0000000000000002  → 第二个事务
  ZXID 0x0000000000000003  → 第三个事务
  ... Leader 重新选举 ...
  ZXID 0x0000000100000004  → 新 Leader 的第一个事务
  ZXID 0x0000000100000005  → 新 Leader 的第二个事务
```

所有 Follower 按 ZXID 顺序回放日志, 保证全局线性一致性。

### Quorum 多数派

| 节点数 | Quorum | 可容忍故障 | 写性能 |
|--------|--------|-----------|--------|
| 3 | 2 | 1 | 高 (推荐) |
| 5 | 3 | 2 | 中 |
| 7 | 4 | 3 | 低 |

生产环境推荐 3 或 5 个节点。超过 5 个写性能下降严重, 容错提升有限。

### ZK/Keeper 在 ClickHouse 中的角色

```mermaid
flowchart TB
    subgraph ZK["ZooKeeper / ClickHouse Keeper"]
        Log["/log: 全局复制日志 (共享状态机)"]
        BN["/block_numbers: 去重计数器"]
        Meta["/metadata: 表结构"]
        MetaVer["/metadata_version: schema 版本"]
        Replicas["/replicas: 副本列表"]
        Quorum["/quorum: 仲裁写入"]
    end

    subgraph R1["Replica 1"]
        R1Q["/queue: 本地任务队列"]
        R1P["/parts: 本地 Part 注册"]
    end

    subgraph R2["Replica 2"]
        R2Q["/queue: 本地任务队列"]
        R2P["/parts: 本地 Part 注册"]
    end

    subgraph R3["Replica 3"]
        R3Q["/queue: 本地任务队列"]
        R3P["/parts: 本地 Part 注册"]
    end

    Log --> R1Q
    Log --> R2Q
    Log --> R3Q

    style ZK fill:#FFF3E0
```

### ZAB 协议如何保证 ClickHouse 数据一致性

```mermaid
sequenceDiagram
    participant CH1 as ClickHouse Replica 1
    participant Leader as ZK/Keeper Leader
    participant F1 as ZK Follower 1
    participant F2 as ZK Follower 2

    CH1->>Leader: multi() 原子操作
    Note over Leader: 操作内容:<br/>1. check block_numbers/{partition} 无冲突<br/>2. block_numbers/{partition} + 1<br/>3. /log 新增 GET_PART 条目<br/>4. /replicas/R1/parts 新增 Part<br/>5. /replicas/R1/log_pointer 更新

    Leader->>Leader: 写事务日志 (ZXID=100)
    Leader->>F1: PROPOSAL(ZXID=100)
    Leader->>F2: PROPOSAL(ZXID=100)
    F1-->>Leader: ACK
    F2-->>Leader: ACK

    Leader->>Leader: Quorum 达成, COMMIT
    Leader->>F1: COMMIT
    Leader->>F2: COMMIT

    Leader-->>CH1: multi() 成功

    Note over Leader: 保证:<br/>全部成功 或 全部失败<br/>不会出现分配了 block_number<br/>但 /log 未写入的不一致状态
```

### ZooKeeper vs ClickHouse Keeper 对比

| 维度 | ZooKeeper | ClickHouse Keeper |
|------|-----------|-------------------|
| 协议 | ZAB | ZAB (兼容) |
| 实现 | Java | C++ |
| 事务日志 | 磁盘文件 log.XX | 磁盘文件 log.XX |
| 快照 | 定期 snap.XX | 定期 snap.XX |
| fsync | 每次事务 | 每次事务 |
| 写性能 | ~10k TPS | ~50k~100k TPS |
| GC 停顿 | Java GC, 调优复杂 | 无 GC |
| 运维 | 独立部署, Java 环境 | 内嵌或独立, 无额外依赖 |
| 推荐 | 老集群 / 已有基础设施 | 新集群首选 (21.8+) |

### 为什么 ClickHouse 需要 ZK/Keeper

| 场景 | ZK/Keeper 的作用 |
|------|-----------------|
| INSERT 复制 | multi() 原子提交, 分配 block_number, 写 /log |
| Part 去重 | block_numbers 防止重复 INSERT |
| 副本同步 | /log 复制日志, 各副本异步拉取 |
| ALTER 同步 | CAS metadata_version, /log 写 ALTER_METADATA |
| DDL ON CLUSTER | /clickhouse/task_queue/ddl 队列 |
| Leader 选举 | /leader_election/leader 临时节点 |
| Quorum INSERT | /quorum/parallel 跟踪多副本确认 |
| 副本恢复 | cloneReplica, is_lost 标记 |
