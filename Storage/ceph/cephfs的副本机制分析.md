# CephFS 核心概念：PG 与副本机制

> 基于 CephFS 源码分析，解释 Placement Group 和副本的工作原理。

---

## 1. PG 是什么

**PG = Placement Group（归置组）**，是一个纯逻辑概念，不是独立进程或实体。

### 1.1 为什么需要 PG

```
没有 PG: 对象 → hash → OSD
  问题: OSD Map 变更 → 几百万对象全要重新计算归属

有 PG:   对象 → hash → PG → CRUSH → OSD Set
  好处: OSD Map 变更 → 只需迁移变化的 PG（几千个）
```

PG 的本质作用：**对象到 OSD 的中间映射层，减少数据迁移量。**

### 1.2 PG 分布在哪里

| 组件 | 持有什么 | 说明 |
|------|---------|------|
| **Monitor** | CRUSH Map | PG → OSD Set 的映射算法 |
| **Monitor** | PGMap | 每个 PG 的状态、acting set、primary |
| **librados（客户端）** | 缓存映射 | 对象 → PG → Primary OSD，做路由用 |
| **OSD（服务端）** | PG 实例 | 每个 OSD 上运行着该 OSD 负责的所有 PG 状态机 |

```
┌───────────────────────────────────────────────────────────┐
│                                                           │
│  CRUSH Map (Monitor)                                       │
│    定义 PG → OSD Set 的映射算法                            │
│    例: PG 1.0 → [osd.1, osd.3, osd.7]                    │
│                                                           │
│  PGMap (Monitor)                                           │
│    记录每个 PG 的状态、acting set、primary                 │
│    例: pg 1.0 state=active+clean primary=osd.1             │
│                                                           │
│  librados (客户端)                                         │
│    缓存 PG → OSD 映射，做路由                              │
│    对象 "100.00000002" → hash → pg 1.2f                   │
│      → 查 OSDMap → primary = osd.3                        │
│    只需要知道 Primary 是谁，不需要知道 PG 内部状态          │
│                                                           │
│  OSD (服务端)                                              │
│    运行 PG 实例，每个 PG 是一个独立状态机                   │
│    例: osd.3 上运行 PG 1.0 (primary) + PG 1.5 (replica)   │
│    PG 状态机: peering → active → clean/degraded/recovering │
│    PG 内部维护: pg_log、missing set、对象索引               │
│                                                           │
└───────────────────────────────────────────────────────────┘
```

### 1.3 PG 状态机

```
PG 生命周期:
  creating → peering → active → clean
                        │
                        ├── degraded      (副本不足)
                        ├── recovering    (正在恢复副本)
                        ├── undersized    (副本数低于 min_size)
                        └── peering       (重新协商成员)
```

---

## 2. 对象写入的完整路径

保存一个对象的完整过程：**hash → PG → Primary OSD → 副本复制。**

```
对象 "100.00000002"
  │
  ▼ 1. hash
  pg_hash("100.00000002") → pg 1.2f
  │  (pool_id.pg_num)
  │
  ▼ 2. CRUSH 查 PG → OSD Set
  CRUSH(pg 1.2f) → [osd.1, osd.3, osd.7]
  │
  ▼ 3. 找 Primary
  OSDMap → pg 1.2f 的 primary = osd.1
  │
  ▼ 4. 发送请求
  librados → TCP → osd.1 (只和 Primary 通信)
  │
  ▼ 5. Primary 负责复制
  osd.1 → osd.3 (replica)
  osd.1 → osd.7 (replica)
  全部 ONDISK → 回复客户端
```

关键点：**客户端只和 Primary 通信**，副本复制是 Primary 的责任。客户端不需要知道有多少副本、副本在哪些 OSD 上。

---

## 3. 条带与副本的关系

条带（Stripe）和副本（Replica）是两个不同的概念：

- **条带**：把大文件切成小块，解决单对象大小限制
- **副本**：同一个小块的完整拷贝，解决数据可靠性

```
文件 "bigfile" (16MB)
  │
  ▼ Striper 切分成 4 个条带（4 个对象）
  │  默认: stripe_unit=4MB, stripe_count=1, object_size=4MB
  │
  ├── 对象 "100.00000000" → 3 个副本 (默认 size=3)
  │     ├── osd.1 (Primary, 副本1)
  │     ├── osd.3 (副本2)
  │     └── osd.7 (副本3)
  │
  ├── 对象 "100.00000001" → 3 个副本
  │     ├── osd.2 (Primary, 副本1)
  │     ├── osd.5 (副本2)
  │     └── osd.9 (副本3)
  │
  ├── 对象 "100.00000002" → 3 个副本
  │     └── ...
  │
  └── 对象 "100.00000003" → 3 个副本
        └── ...

结果: 4 个对象 × 3 副本 = 12 份数据
      分布在不同 OSD 上（可能有重叠）
```

### 3.1 副本策略

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `osd_pool_default_size` | 3 | 总副本数 |
| `osd_pool_default_min_size` | 0（即 size - size/2 = 2） | 最低可写入副本数 |

```
副本丢失场景:
  3 个副本 → 丢 1 个 → degraded（降级，但仍可读写）
  3 个副本 → 丢 2 个 → undersized（低于 min_size=2，只读）
  3 个副本 → 丢 3 个 → 数据丢失

  丢失后: Recovery 自动从存活副本拉取数据，重建新副本
```

### 3.2 条带与副本的完整映射

```
                    Striper (客户端)
                        │
        ┌───────────────┼───────────────┐
        ▼               ▼               ▼
    条带1(4MB)      条带2(4MB)      条带3(4MB)
    对象 A          对象 B          对象 C
        │               │               │
    ┌───┼───┐       ┌───┼───┐       ┌───┼───┐
    ▼   ▼   ▼       ▼   ▼   ▼       ▼   ▼   ▼
  R1  R2  R3      R1  R2  R3      R1  R2  R3   ← 副本

  ───────┬────── ───────┬────── ───────┬──────
         │               │               │
        PG 1.0          PG 1.1          PG 1.2  ← 对象→PG→OSD
```

### 3.3 总结

| 维度 | 条带（Stripe） | 副本（Replica） |
|------|---------------|----------------|
| 目的 | 切分大文件 | 保证数据可靠性 |
| 粒度 | 文件 → 对象 | 对象 → 多个拷贝 |
| 执行位置 | 客户端（Striper） | Primary OSD（ReplicatedBackend） |
| 默认数量 | 取决于文件大小 | 3（由 pool size 决定） |
| 位置分布 | 同一文件的不同条带可能在同一 OSD | 同一对象的副本一定在不同 OSD |

---

## 4. 为什么只和 Primary 通信就能保证多副本一致性

**Primary 是 PG 内的唯一写入协调者**，副本复制和一致性保证都由 Primary 负责。

### 4.1 Primary-Replica 协议

```
                    客户端
                      │
                      │ 只发一次请求
                      ▼
                 Primary OSD
                ┌─────────────────────────┐
                │  1. 分配全局 version      │
                │  2. 写入本地 pg_log       │
                │  3. 并行发给所有副本       │
                │  4. 等待全部 ONDISK       │
                │  5. 全部完成 → 回复客户端  │
                └───┬───────────┬──────────┘
                    │           │
              ┌─────▼───┐ ┌────▼────┐
              │ Replica1 │ │ Replica2│
              │ 写 pg_log│ │ 写 pg_log│
              │ 写 Blue  │ │ 写 Blue  │
              │ 回复 ONDISK│ │ 回复 ONDISK│
              └─────────┘ └─────────┘
```

客户端只管和 Primary 交互，Primary 保证了所有副本的最终一致。

### 4.2 一致性的三个保证

**（1）全序 — pg_log**

```
每个操作分配一个单调递增的 version:
  op1: version 1.100
  op2: version 1.101
  op3: version 1.102

Primary 和所有 Replica 的 pg_log 必须完全一致
→ 同一对象同一版本的数据，在所有副本上相同
```

**（2）原子性 — 全部或全部不**

```
Primary 的 waiting_for_commit 集合:
  op = {osd.1, osd.3, osd.7}  // 3 个副本

  osd.1 回复 ONDISK → erase(osd.1)  → 还剩 {osd.3, osd.7}
  osd.7 回复 ONDISK → erase(osd.7)  → 还剩 {osd.3}
  osd.3 回复 ONDISK → erase(osd.3)  → 集合为空
  → on_commit → 回复客户端 ACK

  任何一个副本失败 → 集合永远不为空 → 客户端收不到 ACK → 客户端重试
  → 不存在"Primary 成功但 Replica 失败"的情况
```

**（3）写入顺序 — 同 PG 内串行**

```
OpSequencer 保证同一 PG 的事务严格串行执行:

  op1 (write A)  ───────────────────── 完成
  op2 (write B)  ────────────── 等待 op1 ────── 完成
  op3 (write A)  ────── 等待 op2 ──────────────── 完成

  所有副本以相同顺序执行 → 不会出现 A 在 Replica1 上旧而在 Replica2 上新的情况
```

### 4.3 总结

| 保证机制 | 实现方式 | 代码位置 |
|---------|---------|---------|
| 全序 | pg_log + 单调递增 version | `ReplicatedBackend.cc:648` `log_operation()` |
| 原子性 | waiting_for_commit 集合为空后才回复 | `ReplicatedBackend.cc:670-693` `op_commit()` |
| 顺序性 | OpSequencer 同 PG 事务串行化 | `BlueStore.cc:14401` `_txc_state_proc()` |

---

## 5. pg_log 与 Ceph 日志体系

### 5.1 pg_log 存放位置

pg_log 存放在**每个 OSD 的 BlueStore 中**（RocksDB 内），每个 OSD 只存自己负责的 PG 的日志。

```
  osd.1 上的 pg_log:
    pg 1.0: [v1.100 (write A), v1.101 (write B), v1.102 (delete C)]
    pg 1.5: [v1.50 (write X), v1.51 (write Y)]

  osd.3 上的 pg_log:
    pg 1.0: [v1.100 (write A), v1.101 (write B), v1.102 (delete C)]  ← 同 PG 完全一致

  同一 PG 的 pg_log 在 Primary 和所有 Replica 上必须完全相同
```

### 5.2 Ceph 中的全部日志

| 日志 | 保护什么 | 存在哪 | 粒度 | 代码 |
|------|---------|--------|------|------|
| **pg_log** | 对象数据操作（读/写/删） | OSD (RocksDB) | 单 PG | `src/osd/PGLog.cc` |
| **MDS Journal** | 元数据操作（inode/dentry/目录） | RADOS 对象 (journal_mds.{rank}) | 单 MDS | `src/mds/MDLog.cc` |
| **BlueStore WAL** | 存储引擎事务 | block.wal 或 block 设备 (BlueFS) | 单 OSD | `src/os/bluestore/BlueFS.cc` |
| **BlueRocksEnv WAL** | RocksDB MemTable 持久化 | BlueFS（与 BlueStore WAL 共享磁盘） | 单 OSD | `src/os/bluestore/BlueRocksEnv.cc` |
| **Monitor Store** | 集群地图（OSDMap/PGMap/CRUSH Map） | Monitor 本地磁盘 (RocksDB) | 全集群 | `src/mon/MonitorStore.cc` |

### 5.3 日志之间的协作关系

```
客户端写入一个文件:
  │
  ▼ MDS Journal
  元数据变更 (创建 inode、更新 size) → 先写 MDS Journal
  │
  ▼ pg_log
  数据写入 (对象 WRITE) → Primary 分配 version → 写入 pg_log
  │
  ▼ BlueStore WAL + BlueRocksEnv WAL
  对象落盘 → BlueStore 两阶段提交 → RocksDB WAL 同步

恢复场景:
  MDS 崩溃 → 重放 MDS Journal → 重建元数据
  OSD 崩溃 → 重放 pg_log + BlueStore WAL → 重建对象状态
  Monitor 崩溃 → 读取 Monitor Store → 恢复集群地图
```

### 5.4 日志的裁剪策略

```
pg_log 裁剪 (PGLog.cc:56):
  条件: 日志头部超过 can_rollback_to → 从头部裁剪旧条目
  限制: 不能裁剪过回滚点，保证可以处理 Peering 时的日志合并

MDS Journal 裁剪 (journal.cc:125):
  条件: LogSegment 中的 dirty dirfrag 已提交到 RADOS 元数据存储
  条件: 所有 scatterlock 已 gather
  安全性: 日志内容已持久化后才可裁剪

BlueStore WAL 裁剪:
  由 RocksDB 自动管理，MemTable 刷盘为 SSTable 后回收 WAL 空间
```
