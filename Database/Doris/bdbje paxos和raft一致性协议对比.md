# BDBJE Paxos vs Raft 一致性协议对比

> 基于 Apache Doris 使用的 BDBJE 7.3.7 (Berkeley DB Java Edition) 与 Raft 协议的对比分析

## 一、背景

Apache Doris 的 FE 元数据复制使用 **BDBJE** (Berkeley DB Java Edition) 提供的分布式共识机制。BDBJE 内部实现了一个基于 **Multi-Paxos** 的复制协议。Doris 代码中并无显式的 Paxos 实现代码——共识逻辑完全封装在 BDBJE 库内部，Doris 仅通过 API 与之交互。

```
Doris FE 层                    BDBJE 库 (黑盒)                    底层共识
+-------------------+     +-----------------------------+     +------------------+
| Catalog           |     | ReplicatedEnvironment       |     | Multi-Paxos      |
| EditLog           |---> |   - 日志复制                  |---> |   - 选举         |
| BDBHA (Fencing)   |     |   - 选举通知                  |     |   - 日志复制      |
| StateChangeListener|    |   - Feeder 模型               |     |   - 多数派确认    |
+-------------------+     +-----------------------------+     +------------------+
                            Oracle Berkeley DB JE 7.3.7
```

## 二、BDBJE 共识协议概览

### 2.1 核心概念映射

| Paxos 概念 | BDBJE 对应 | Doris 对应 |
|-----------|-----------|-----------|
| Proposer | Master (当前领导者) | FE Master 节点 |
| Acceptor | Electable Replica (选举者) | FE Follower 节点 |
| Learner | Secondary (观察者) | FE Observer 节点 |
| Proposal | Database Transaction (put 操作) | EditLog.write() |
| Proposal ID | 内部 LSN (Log Sequence Number) | journalId |
| Quorum | SIMPLE_MAJORITY / ALL / NONE | replica_ack_policy 配置 |
| Leader | Master | FE Master |
| Epoch / Term | Election Term (内部) | BDBHA fencing epoch |

### 2.2 BDBJE 复制模型

```
                    BDBJE Feeder 模型
                    ===============

  Master (Feeder)             Replica 1                Replica 2
  +----------------+         +----------------+       +----------------+
  | Log Entry      |  ACK    | Log Entry      | ACK   | Log Entry      |
  | [seq=1,data]   |-------->| [seq=1,data]   |------>| [seq=1,data]   |
  | [seq=2,data]   |-------->| [seq=2,data]   |------>| [seq=2,data]   |
  +----------------+         +----------------+       +----------------+
         |                         |                       |
         | fsync                  | fsync                | fsync
         v                         v                       v
       磁盘                      磁盘                    磁盘
         |                         |                       |
         +--------- ACK -----------+-------- ACK ----------+
                     |
              多数派确认 (SIMPLE_MAJORITY)
                     |
                     v
              写入成功返回
```

### 2.3 关键配置参数

| 参数 | 默认值 | 含义 |
|------|--------|------|
| `master_sync_policy` | SYNC | Master 写入后 fsync 再 ACK |
| `replica_sync_policy` | SYNC | Replica 写入后 fsync 再 ACK |
| `replica_ack_policy` | SIMPLE_MAJORITY | 多数派 Replica 确认即提交 |
| `bdbje_heartbeat_timeout_second` | 30s | 心跳超时, 触发选举 |
| `bdbje_replica_ack_timeout_second` | 10s | 等待 Replica ACK 超时 |
| `max_bdbje_clock_delta_ms` | 5000ms | 节点间最大时钟偏差 |

## 三、Raft 协议概览

### 3.1 核心概念映射

| Raft 概念 | 说明 |
|-----------|------|
| Leader | 处理所有客户端请求, 复制日志到 Follower |
| Candidate | 选举期间的过渡状态 |
| Follower | 接收 Leader 复制的日志 |
| Term | 逻辑时钟, 每次选举递增 |
| Log Entry | 带有 term 索引的日志条目 |
| AppendEntries | Leader -> Follower 的心跳和日志复制 RPC |
| RequestVote | Candidate -> 所有节点的选举 RPC |
| CommitIndex | 已提交的最高日志索引 |
| LastApplied | 已应用到状态机的最高日志索引 |

### 3.2 Raft 复制模型

```
                      Raft AppendEntries 模型
                      ======================

  Leader                         Follower 1               Follower 2
  +----------------+            +----------------+       +----------------+
  | Log:           | AppendEntries| Log:           |  AppendEntries | Log:        |
  | [1,1] [2,1]   | RPC -------->| [1,1] [2,1]   |  RPC --------->| [1,1] [2,1] |
  | [3,1] NEW     |            | [3,1] NEW     |               | [3,1] NEW   |
  +----------------+            +----------------+       +----------------+
         |                           |                       |
         | fsync                     | fsync                | fsync
         v                           v                       v
       磁盘                        磁盘                    磁盘
         |                           |                       |
         +-------- AppendEntries Response (Success) ---------+
         +-------- AppendEntries Response (Success) ---------+
                     |
              多数派确认
                     |
                     v
           commitIndex = 3
           Apply to State Machine
```

## 四、核心机制对比

### 4.1 选举 (Leader Election)

```
                     Paxos 选举                           Raft 选举
                     =========                           ==========

  1. Master 心跳停止 (FEEDER_TIMEOUT)    1. Leader 心跳停止 (election_timeout)

  2. Replica 超时, 进入 UNKNOWN 状态       2. Follower 超时, 自增 term

  3. 各节点广播选举消息 (内部协议)          3. 转为 Candidate, 广播 RequestVote RPC

  4. 选出拥有最新日志的节点成为 Master      4. 获得多数派投票的 Candidate 成为 Leader
     (最高 LSN 优先)                       (先到先得, 不比较日志)

  5. 通过 StateChangeListener 通知 Doris   5. 新 Leader 开始发送 AppendEntries 心跳

  6. Doris 执行 transferToMaster():        6. (应用层自行处理状态转移)
     - fencing (putNoOverwrite epoch)
     - replayJournal(-1)
     - 启动 Master 守护线程
```

**关键区别**:

| 维度 | Paxos (BDBJE) | Raft |
|------|--------------|------|
| **选举触发** | 心跳超时 (FEEDER_TIMEOUT) | 选举超时 (election_timeout, 随机化) |
| **候选人选择** | 拥有最新完整日志的节点优先 | 任何节点都可以成为 Candidate |
| **选举通信** | 内部协议 (黑盒) | 显式 RequestVote RPC |
| **选举结果通知** | StateChangeListener 回调 | 应用层自行检测 |
| **Term/Epoch** | 内部 Election Term | 显式 term 编号 |
| **超时随机化** | 无明确随机化 | 必须随机化 (150-300ms) 避免活锁 |

### 4.2 日志复制 (Log Replication)

```
                     Paxos 日志复制 (BDBJE Feeder)          Raft 日志复制 (AppendEntries)
                     ================================     =============================

  Master: put(key, value)                  Leader: AppendEntries(entries=[...])
     |                                         |
     v                                         v
  BDBJE 内部:                                 发送 RPC 到所有 Follower:
    通过 Feeder 流式推送到所有 Replica            +-- prevLogIndex, prevLogTerm (一致性检查)
    每个 Replica 独立持久化并 ACK               +-- entries[] (新日志条目)
    收集 ACK, 多数派确认后提交                   +-- leaderCommit (Leader 的 commitIndex)
     |                                         |
     v                                         v
  Replica: 写入本地 BDB                       Follower:
    (自动持久化, 无需显式 RPC)                    +-- 检查 prevLogIndex/Term 匹配
    ACK 到 Master                               +-- 追加不冲突的 entries
                                               +-- 截断冲突的 entries (如果 Leader 更新)
                                               +-- 返回 Success/Failure
                                                    |
                                                    v
                                               Leader: 多数派 Success
                                                    commitIndex = index
```

**关键区别**:

| 维度 | Paxos (BDBJE) | Raft |
|------|--------------|------|
| **复制模型** | Feeder 流式推送 | AppendEntries RPC (拉取式) |
| **一致性检查** | 内部隐式保证 | 显式 prevLogIndex/Term 检查 |
| **冲突处理** | 内部协商回滚 | Leader 覆盖 Follower 冲突日志 |
| **通信模式** | 长连接流式 (TCP) | RPC 请求-响应 |
| **批量传输** | 逐条提交 (group_commit=0) | 可批量发送多条 entries |
| **日志编号** | 单调递增 journalId | (term, index) 二维编号 |

### 4.3 安全性保证 (Safety)

```
  +---------------------------+     +---------------------------+
  |  Paxos 安全性              |     |  Raft 安全性               |
  +---------------------------+     +---------------------------+
  |                            |     |                            |
  | 选举安全性:                |     | 选举安全性 (Election Safety):|
  |   任何已提交的值必须在     |     |   每个 term 最多一个 Leader   |
  |   未来的选举中被保留       |     |   (通过 term 编号保证)        |
  |                            |     |                            |
  | 多数派交集:                |     | Leader Append-Only:          |
  |   两个多数派至少有一个     |     |   Leader 不会覆盖已提交日志   |
  |   共同节点 (保证一致性)    |     |   (通过 prevLogTerm 检查)     |
  |                            |     |                            |
  | 值唯一性:                 |     | Log Matching:                |
  |   同一个 slot 最多被选    |     |   相同 index+term 的日志     |
  |   中一个值                 |     |   完全相同                   |
  |                            |     |                            |
  | (内部实现, Doris 不感知)   |     | Leader Completeness:         |
  |                            |     |   已提交日志必然存在于新     |
  |                            |     |   Leader 的日志中            |
  +---------------------------+     +---------------------------+
```

### 4.4 持久化与恢复

| 维度 | Paxos (BDBJE) | Raft |
|------|--------------|------|
| **Master 持久化** | `master_sync_policy`: SYNC/NO_SYNC/WRITE_NO_SYNC | Leader fsync 后才发送 AppendEntries |
| **Replica 持久化** | `replica_sync_policy`: SYNC/NO_SYNC/WRITE_NO_SYNC | Follower fsync 后才返回 ACK |
| **ACK 策略** | `replica_ack_policy`: SIMPLE_MAJORITY/ALL/NONE | 多数派确认 (硬编码) |
| **写入失败处理** | 重试 3 次, 非时间戳写失败 -> System.exit(-1) | 重试, 持续失败 -> 触发新选举 |
| **日志截断** | `txn_rollback_limit` 控制最大回滚事务数 | Leader 覆盖 Follower 冲突日志 |
| **落后节点恢复** | NetworkRestore 从其他节点拉取缺失日志 | InstallSnapshot RPC 发送完整快照 |
| **状态机快照** | Doris Image Checkpoint (独立机制) | Raft Snapshot (内置机制) |

### 4.5 网络分区处理

```
        网络分区场景 (3 节点: Master + 2 Follower)
        =========================================

  分区前:              分区后:
  +-------+           +-------+         +-------+
  |Master |           |Master |         |F1  F2 |
  +-------+           +-------+         +-------+
       |                  |  <网络断开>       |
  +-------+              |                 |
  |  F1   |              |                 |
  +-------+              |                 |
       |                  |                 |
  +-------+              |                 |
  |  F2   |              |                 |
  +-------+              |                 |


  BDBJE (Paxos):                      Raft:
  ===============                      ======

  Master 侧 (少数派):                 Leader 侧 (少数派):
    无法达到 SIMPLE_MAJORITY            无法获得多数派 AppendEntries Response
    OP_TIMESTAMP 写入可继续             退化为 Candidate
    其他写入: 重试 3 次 -> System.exit(-1)  无法获得多数派投票
    Master 主动退出, 防止脑裂           无法成为新 Leader
                                       (但不会自动退出, 等待分区恢复)

  F1 + F2 侧 (多数派):                F1 + F2 侧 (多数派):
    心跳超时, 触发新选举                 选举超时, 成为 Candidate
    选出新的 Master                     获得多数派投票, 选出新 Leader
    新 Master 开始接受写入              新 Leader 开始接受写入
    分区恢复后:                         分区恢复后:
      旧 Master 重新加入                  旧 Leader 退化为 Follower
      作为 Follower 追赶日志             截断未提交日志, 追赶新 Leader
      日志追赶: NetworkRestore           日志追赶: AppendEntries + InstallSnapshot
```

### 4.6 成员变更

| 维度 | Paxos (BDBJE) | Raft |
|------|--------------|------|
| **添加节点** | addHelperSocket + ReplicatedEnvironment 自动加入 | Joint Consensus (两阶段) 或单节点变更 |
| **移除节点** | ReplicationGroupAdmin.removeElectableNode() | 同上, Joint Consensus |
| **角色变更** | Electable <-> SECONDARY (需要重建) | Learner (单向, 不投票) |
| **动态配置** | 通过 ReplicationGroupAdmin API | 通过 CofnChange RPC |

## 五、BDBJE 在 Doris 中的封装方式

```
  Doris 与 BDBJE 的接口边界
  ========================

  +-- Doris 调用 BDBJE ----------------------------------------------+
  |                                                                   |
  |  BDBEnvironment                                                   |
  |    +-- 创建 ReplicatedEnvironment (复制组, 端口, 超时, 持久化策略)   |
  |    +-- 获取 epochDB (用于 fencing)                                  |
  |    +-- 获取 ReplicationGroupAdmin (节点管理)                         |
  |                                                                   |
  |  BDBJEJournal                                                     |
  |    +-- write(op, data) -> currentJournalDB.put(null, key, value)   |
  |    +-- read(journalId) -> currentJournalDB.get(key)                |
  |    +-- read(from, to) -> BDBJournalCursor 范围扫描                 |
  |    +-- deleteJournals(threshold) -> 删除旧数据库                     |
  |    +-- getMaxJournalId() -> 遍历数据库找最大 ID                     |
  |    +-- getFinalizedJournalId() -> 当前数据库最小 ID - 1             |
  |                                                                   |
  |  BDBHA                                                             |
  |    +-- fencing() -> epochDB.putNoOverwrite(epoch, data)            |
  |    +-- getLeader() -> ReplicationGroupAdmin.getLeader()            |
  |    +-- isLeader() -> ReplicationGroupAdmin.isLeader()              |
  |                                                                   |
  |  BDBStateChangeListener                                           |
  |    +-- stateChange(MASTER/REPLICA/UNKNOWN) -> 通知 Catalog        |
  |                                                                   |
  +-- BDBJE 内部 (黑盒, Doris 不可见) --------------------------------+
  |                                                                   |
  |    Multi-Paxos 实现:                                               |
  |      - Leader 选举 (心跳 + 投票)                                    |
  |      - 日志复制 (Feeder 流式推送)                                   |
  |      - 多数派确认 (SIMPLE_MAJORITY/ALL/NONE)                        |
  |      - 日志压缩与恢复 (NetworkRestore)                              |
  |      - 时钟同步 (max_clock_delta_ms)                                |
  |                                                                   |
  +-------------------------------------------------------------------+
```

## 六、协议复杂度对比

### 6.1 理论复杂度

| 维度 | Paxos (Multi-Paxos) | Raft |
|------|---------------------|------|
| **核心思想** | 通过 Propose/Promise/Accept 达成共识 | 通过选举 Leader 简化日志管理 |
| **状态空间** | Proposer/Acceptor/Learner + 槽位 | Leader/Candidate/Follower + Term |
| **消息类型** | Prepare, Promise, Accept, Accepted | AppendEntries, RequestVote, InstallSnapshot |
| **正常写入** | 1 轮 RTT (已选出 Leader 时) | 1 轮 RTT |
| **选举轮数** | 不确定 (可能多轮协商) | 通常 1 轮 (随机化超时) |
| **冲突解决** | 多数派自然去重 | Leader Append-Only 强制一致 |
| **活锁风险** | 存在 (提案冲突可无限重试) | 不存在 (Leader 制 + 随机化) |

### 6.2 实现复杂度

| 维度 | Paxos (BDBJE) | Raft |
|------|--------------|------|
| **可理解性** | 较低 (论文偏理论, 实现变体多) | 较高 (论文面向工程实现, 伪代码清晰) |
| **开源实现** | BDBJE (Oracle, 黑盒) | etcd, Consul, TiKV (开源, 可审计) |
| **可定制性** | 低 (依赖 BDBJE 库) | 高 (可自行实现或选择多种开源库) |
| **依赖复杂度** | 引入整个 BDBJE 库 (较重) | 核心算法简洁, 依赖轻 |
| **调试难度** | 困难 (内部黑盒) | 相对容易 (协议透明) |
| **社区活跃度** | BDBJE 已停止维护 (Oracle) | Raft 实现广泛, 社区活跃 |

### 6.3 性能对比

| 维度 | BDBJE (Paxos) | Raft |
|------|--------------|------|
| **写入延迟** | 中等 (SYNC fsync + 网络往返) | 中等 (fsync + 网络往返) |
| **批量写入** | 支持 (group_commit) | 内置 (批量 AppendEntries) |
| **吞吐量** | 中等 | 中等 |
| **选举延迟** | FEEDER_TIMEOUT (30s, 可配) | election_timeout (150-300ms) |
| **快照** | 应用层实现 (Doris Checkpoint) | 协议内置 (InstallSnapshot) |
| **日志压缩** | 外部日志滚动 (edit_log_roll_num) | 协议内置 (日志截断) |

## 七、BDBJE 在 Doris 中的优缺点

### 7.1 优点

| 优点 | 说明 |
|------|------|
| **开箱即用** | 无需自行实现共识协议, BDBJE 封装了选举、复制、持久化 |
| **成熟稳定** | BDBJE 经过多年验证, Oracle 商业支持 (虽已停维护) |
| **持久化可配** | master/replica_sync_policy 和 replica_ack_policy 灵活可调 |
| **Observer 支持** | NodeType.SECONDARY 提供了不参与选举的读副本 |
| **Fencing 内置** | epochDB 的 putNoOverwrite 提供了简洁的防脑裂机制 |
| **Java 原生** | 与 FE 的 Java 技术栈天然契合, 无跨语言开销 |

### 7.2 缺点

| 缺点 | 说明 |
|------|------|
| **黑盒协议** | 内部共识逻辑不可审计, 出现问题时难以定位 |
| **停止维护** | Oracle 已停止 BDBJE 维护, 修复安全/性能问题困难 |
| **重量级依赖** | 引入整个 BDBJE 库 (包含 B-Tree 存储引擎), 体积大 |
| **选举延迟高** | 默认 30s 心跳超时, 故障切换慢 |
| **无快照机制** | BDBJE 不提供内置快照, Doris 需自行实现 Checkpoint |
| **协议不透明** | 无法利用协议层优化 (如 batch、pipeline replication) |
| **Learner 有限** | SECONDARY 节点不参与投票, 功能较 Raft Learner 更受限 |

## 八、Doris 后续演进

在现代版本的 Apache Doris 中，BDBJE 已被替换为 **`bdb-je` 社区版本** (从 Oracle 转为 LGPL 协议), 并且社区正在推进使用 **Raft** 替代 BDBJE 的方案 (基于 Brpc 的 Raft 实现), 主要驱动因素：

```
BDBJE -> Raft 迁移动机:

  1. BDBJE 停止维护    ->  需要可持续维护的共识组件
  2. 协议黑盒           ->  Raft 协议透明, 可审计可优化
  3. 选举延迟高         ->  Raft 亚秒级故障切换
  4. Java 依赖重        ->  C++ Raft 实现更轻量, 与 BE 技术栈统一
  5. 无内置快照         ->  Raft InstallSnapshot 更优雅
  6. 社区生态           ->  Raft 有丰富的开源实现 (etcd, braft 等)
```

## 九、总结

| 对比维度 | BDBJE Paxos | Raft |
|----------|------------|------|
| **一致性级别** | 等效 (多数派确认) | 等效 (多数派确认) |
| **选举机制** | 最高日志优先 | 先到先得 + 随机化超时 |
| **复制模型** | Feeder 流式推送 | AppendEntries RPC |
| **安全性** | 多数派交集保证 | term + prevLogIndex 保证 |
| **可理解性** | 偏理论, 变体多 | 面向工程, 伪代码清晰 |
| **实现透明度** | 黑盒 (BDBJE 内部) | 白盒 (多种开源实现) |
| **故障切换速度** | 秒级 (~30s) | 亚秒级 (~300ms) |
| **快照支持** | 应用层实现 | 协议内置 |
| **社区生态** | 已停维护 | 活跃 (etcd/braft/HashiCorp) |
| **适用场景** | 需要 Java 原生, 快速集成 | 需要可控、可审计、高性能 |

**核心结论**: 两者在一致性保证上是等效的 (都基于多数派确认), 主要区别在于**工程实现方式**。Paxos 偏理论, 实现变体多, BDBJE 作为黑盒封装了复杂性但牺牲了可控性; Raft 面向工程实现, 协议步骤明确, 实现透明度高。Doris 早期选择 BDBJE 是出于快速集成的考虑, 后续迁移到 Raft 是为了解决可维护性和性能问题。
