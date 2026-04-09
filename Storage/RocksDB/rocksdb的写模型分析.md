# RocksDB 写模型：追加写与随机写

## 目录

1. [核心问题](#1-核心问题)
2. [RocksDB 写入路径分析](#2-rocksdb-写入路径分析)
3. [随机写如何变成顺序写](#3-随机写如何变成顺序写)
4. [LSM-Tree 三阶段模型](#4-lsm-tree-三阶段模型)
5. [与 B-Tree 的对比](#5-与-b-tree-的对比)
6. [原地更新模式（实验性）](#6-原地更新模式实验性)
7. [为什么追加写更快](#7-为什么追加写更快)
8. [源码索引](#8-源码索引)

---

## 1. 核心问题

- **RocksDB 是追加写模型吗？** — 是的，底层 WAL 和 SST 文件都是追加写，不存在原地修改（in-place update）。
- **能否支持随机写？** — 完全支持。用户可以写入任意 key，RocksDB 在内部将随机写转换为顺序写。

RocksDB 的 LSM-Tree 架构的核心价值就是：**用写放大换取写性能，将随机写转化为顺序写。**

---

## 2. RocksDB 写入路径分析

### 2.1 单次写入的完整路径

```
用户调用 db->Put("z_key", value)
            │
            ▼
┌──────────────────────────────────────────────────────────┐
│                    WriteThread                           │
│  1. 分配序列号: VersionSet::FetchAddLastAllocatedSequence│
│  2. 构造 WriteBatch: [seq, Put, "z_key", value]        │
│  3. 加入 Writer 队列（支持组提交）                       │
└──────────┬───────────────────────────────────────────────┘
           ▼
┌──────────────────────────────────────────────────────────┐
│                    WAL 写入（磁盘顺序追加）                │
│  WriteBatch 序列化后 append 到 WAL 文件尾部              │
│  调用 fsync/fdatasync 确保持久化                         │
│                                                          │
│  WAL 文件布局:                                           │
│  ┌────────────────────────────────────────────────┐      │
│  │ [batch1][batch2][batch3][batch4][batch5]...    │      │
│  │  ↑ 纯追加，永不原地修改                         │      │
│  └────────────────────────────────────────────────┘      │
└──────────┬───────────────────────────────────────────────┘
           ▼
┌──────────────────────────────────────────────────────────┐
│               MemTable 写入（内存有序插入）               │
│  WriteBatchInternal::InsertInto() 将每个 KV 插入跳表    │
│                                                          │
│  Skiplist 内部按 user_key 排序:                          │
│  ┌─────────────────────────────────────────┐             │
│  │  "a_key" → (seq=5, Put, "v1")          │             │
│  │  "m_key" → (seq=3, Put, "v2")          │             │
│  │  "z_key" → (seq=8, Put, value)  ← 新插入│             │
│  └─────────────────────────────────────────┘             │
└──────────┬───────────────────────────────────────────────┘
           ▼
     SetLastPublishedSequence(8)
     返回成功
```

### 2.2 关键代码路径

```cpp
// 写入入口 → WriteBatch → WAL → MemTable
DBImpl::Write()
  → WriteThread::Enter()              // 加入写入队列
  → WriteBatchInternal::SetSequence() // 分配序列号
  → WriteToWAL()                      // WAL 追加写入
  → WriteBatchInternal::InsertInto()  // 插入 MemTable
  → SetLastPublishedSequence()        // 更新已发布序列号
```

### 2.3 没有原地修改

整个写入路径中没有任何一步涉及"找到磁盘上的某个位置并修改它"：

| 组件 | 操作类型 | 是否原地修改 |
|------|---------|-------------|
| WAL 文件 | 追加写入 | 否 |
| MemTable（跳表） | 内存有序插入 | 否（内存分配新节点） |
| SST 文件（Flush/Compaction） | 创建新文件 | 否（新文件写入） |
| 旧 SST 文件 | 标记删除 | 否（仅标记 obsolete，异步删除） |

---

## 3. 随机写如何变成顺序写

### 3.1 用户视角 vs 存储视角

```
用户视角（随机写）:
    Put("user:1001", "alice")
    Put("user:999", "bob")
    Put("user:1", "charlie")
    Delete("user:999")

RocksDB 内部转换:

    WAL 上（顺序追加）:
    ┌────────────────────────────────────────────────────────────┐
    │ offset=0:    [seq=1, Put, "user:1001", "alice"]          │
    │ offset=128:  [seq=2, Put, "user:999", "bob"]             │
    │ offset=256:  [seq=3, Put, "user:1", "charlie"]           │
    │ offset=384:  [seq=4, Delete, "user:999"]                 │
    └────────────────────────────────────────────────────────────┘
    ↑ 4 次写入全部是顺序 append，没有任何 seek

    MemTable 中（有序组织）:
    ┌──────────────────────────────────────┐
    │ "user:1"    → seq=3, Put, "charlie" │
    │ "user:999"  → seq=4, Delete         │  ← 删除覆盖了 seq=2 的 Put
    │ "user:1001" → seq=1, Put, "alice"   │
    └──────────────────────────────────────┘
    ↑ 跳表按 key 排序存储，方便后续顺序输出
```

### 3.2 组提交优化

RocksDB 支持组提交（Group Commit），多个并发写入可以合并为一次 WAL 写入：

```
线程 A: Put("k1", "v1")  ─┐
线程 B: Put("k2", "v2")  ─┤ → 合并为一个 WriteGroup
线程 C: Put("k3", "v3")  ─┘

WAL 一次写入: [batchA][batchB][batchC]  ← 一次 fsync
```

这进一步提高了写入吞吐量，减少了 WAL fsync 次数。

### 3.3 序列号机制保证顺序性

```cpp
// db/dbformat.h:181-186
inline uint64_t PackSequenceAndType(uint64_t seq, ValueType t) {
    return (seq << 8) | type;  // seq 占高 56 位, type 占低 8 位
}

// 内部键按 (user_key ASC, seq DESC, type DESC) 排序
// 同一 key 的多个版本按 seq 降序排列
// 最新版本排在最前面
```

每个写操作获得一个单调递增的序列号，这个序列号是理解"随机写转顺序写"的关键：

- **WAL 中**：序列号随写入顺序递增（自然顺序）
- **MemTable 中**：按 user_key 排序，同 key 按序列号降序（最新版本在前）
- **SST 文件中**：同样按 (key, seq desc) 排序
- **读取时**：只返回每个 key 的最新可见版本

---

## 4. LSM-Tree 三阶段模型

### 4.1 写入-重组织-读取

```
┌──────────────────────────────────────────────────────────────────┐
│                      阶段 1: 写入（全部顺序）                      │
│                                                                  │
│  WAL: 纯顺序追加写入（磁盘）                                      │
│  MemTable: 内存有序插入（O(log N) 查找/插入）                      │
│                                                                  │
│  特点: 写入极快，仅受 WAL fsync 和内存分配限制                     │
└──────────────────────────┬───────────────────────────────────────┘
                           ▼ MemTable 满
┌──────────────────────────────────────────────────────────────────┐
│                      阶段 2: 重组织（后台顺序）                    │
│                                                                  │
│  Flush: MemTable → L0 SST（顺序写磁盘）                           │
│  Compaction: L0 → L1 → L2 → ... → L6（顺序读+顺序写）           │
│                                                                  │
│  特点: 后台异步执行，不影响前台写入                                 │
│  代价: 写放大（同一数据可能被重写多次）                             │
└──────────────────────────┬───────────────────────────────────────┘
                           ▼ 读取
┌──────────────────────────────────────────────────────────────────┐
│                      阶段 3: 读取（需要查找多层）                  │
│                                                                  │
│  Get: MemTable → ImmutableMemTable → L0 SST → L1 → ... → L6    │
│  优化: Bloom Filter、Block Cache、索引块                          │
│                                                                  │
│  特点: 需要搜索最多 7 层，每层最多 1 次 SST 查找                   │
│  代价: 读放大                                                    │
└──────────────────────────────────────────────────────────────────┘
```

### 4.2 数据生命周期

```
写入 → WAL（磁盘顺序追加）
     → MemTable（内存有序结构）
         → ImmutableMemTable（等待 Flush）
             → L0 SST（磁盘，文件间可能重叠）
                 → L1 SST（磁盘，文件间不重叠）
                     → L2 SST
                         → ...
                             → L6 SST（最底层）
                                 → Compaction 清理后删除
```

---

## 5. 与 B-Tree 的对比

### 5.1 架构差异

```
B-Tree（如 MySQL InnoDB）:
┌─────────────────────────────────────────┐
│  写入: WAL 追加 + 修改 B-Tree 叶子页      │
│                                         │
│  ┌───────────┐                           │
│  │  Root     │                           │
│  │  ├── P1   │                           │
│  │  │   └── [修改叶子页] ← 随机磁盘写      │
│  │  └── P2   │                           │
│  │      └── [修改叶子页] ← 随机磁盘写      │
│  └───────────┘                           │
│                                         │
│  修改已存在的页 = 随机写（seek + write）    │
│  页分裂 = 大量随机写                      │
└─────────────────────────────────────────┘

LSM-Tree（RocksDB）:
┌─────────────────────────────────────────┐
│  写入: WAL 追加 + MemTable 内存插入        │
│                                         │
│  WAL: [append][append][append][append]   │
│        ↑ 全部顺序写                      │
│                                         │
│  MemTable: 内存跳表（无磁盘 IO）           │
│                                         │
│  Flush/Compaction: 后台顺序写              │
└─────────────────────────────────────────┘
```

### 5.2 核心指标对比

| 指标 | LSM-Tree (RocksDB) | B-Tree (MySQL InnoDB) |
|------|-------------------|----------------------|
| **写入 I/O 类型** | 顺序写（WAL + SST） | 随机写（修改叶页） |
| **写入吞吐** | 高（充分利用顺序写带宽） | 中等（受限于随机写 IOPS） |
| **写放大** | 高（10-30x，数据被 compaction 反复重写） | 低（1-2x，原地修改） |
| **读放大** | 高（最多查 7 层 SST） | 低（B-Tree 深度通常 3-4 层） |
| **空间放大** | 高（多版本共存 + Tombstone） | 低（原地更新） |
| **写入延迟** | 低（WAL fsync + 内存写入） | 中等（可能触发页分裂） |
| **读取延迟** | 高（查多层 + Bloom Filter） | 低（1-2 次磁盘查找） |
| **范围扫描** | 高效（每层文件有序） | 高效（叶子页链表） |

### 5.3 适用场景

| 场景 | 推荐选择 | 原因 |
|------|---------|------|
| 高吞吐写入（日志、消息队列） | LSM-Tree | 顺序写性能优势大 |
| 读写均衡（通用数据库） | B-Tree | 读写性能均衡 |
| 读密集（缓存层、索引） | B-Tree | 读放大低 |
| 写密集（时序数据、事件流） | LSM-Tree | 写放大换来写性能 |
| 空间受限（嵌入式设备） | B-Tree | 空间放大低 |
| SSD 环境 | 两者均可 | SSD 缩小了随机/顺序写的差距 |

---

## 6. 原地更新模式（实验性）

RocksDB 提供了实验性的原地更新支持，但**不推荐在生产中使用**：

```cpp
Options options;
options.inplace_update_support = true;  // 开启原地更新
```

### 6.1 限制条件

| 限制 | 说明 |
|------|------|
| **不支持快照** | `GetSnapshot()` 返回 nullptr（`is_snapshot_supported_ = false`） |
| **仅等长值** | 原地更新要求新值长度 ≤ 旧值长度 |
| **仅 MemTable 层** | WAL 仍然是追加写，只是 MemTable 跳表节点原地替换 |
| **不支持 Merge** | Merge 操作不能原地更新 |
| **不减少写放大** | Compaction 仍然需要重写 SST 文件 |

### 6.2 源码中的检查

```cpp
// db/db_impl/db_impl.cc:4588-4615 (GetSnapshotImpl)
if (!is_snapshot_supported_) {
    delete s;
    return nullptr;  // 原地更新模式下不支持快照
}
```

### 6.3 为什么不常用

原地更新模式的收益有限：
- WAL 仍然需要追加写入（持久化需求不变）
- MemTable 内存占用略有减少（不保留旧版本），但影响微乎其微
- 丧失了快照功能（MVCC 能力）
- Compaction 的写放大没有减少

本质上，LSM-Tree 的性能优势来自顺序写，而原地更新模式既没有减少顺序写（WAL），又丧失了 MVCC 能力，因此很少被使用。

---

## 7. 为什么追加写更快

### 7.1 磁盘物理特性

| 操作 | HDD 延迟 | SSD 延迟 | 说明 |
|------|---------|---------|------|
| **顺序写** | ~0.5 MB/s → 200 MB/s | ~500 MB/s → 3 GB/s | 无需寻道，批量写入 |
| **随机写** | ~100-200 IOPS | ~10,000-100,000 IOPS | 需要寻道（HDD）或 FTL 映射（SSD） |

**HDD 上的差距尤其巨大**（100-400x），SSD 上差距缩小（10-100x）但仍然显著。

### 7.2 追加写的系统级优势

```
1. 无寻道开销（HDD）
   顺序写: 磁头一直在当前位置后面写
   随机写: 磁头需要移动到目标磁道

2. 减少 WAL/日志开销
   顺序写: WAL 就是写入本身，无需额外日志
   随机写: 还需要写 redo/undo log

3. 缓存友好
   顺序写: 操作系统预读、页缓存命中率高
   随机写: 缓存命中率低，频繁缺页

4. 减少 GC 开销（SSD）
   顺序写: SSD 固件可以高效合并写入块
   随机写: 触发更多 SSD 内部垃圾回收

5. 写放大可预测
   顺序写: 写放大由 compaction 策略决定（可配置）
   随机写: 写放大由页分裂和数据分布决定（不可预测）
```

### 7.3 LSM-Tree 的写放大来源

```
写入 1 字节数据的完整路径:

  WAL 写入:     1x（必须）
  Flush:        1x（MemTable → L0 SST）
  L0→L1:        ~10x（L0 大小 / L1 目标大小 × fanout）
  L1→L2:        ~1x（数据放大到下一层）
  L2→L3:        ~1x
  ...
  L5→L6:        ~1x

  总写放大 ≈ 10-30x（取决于配置和工作负载）

  对比 B-Tree:
  写入 1 字节: ~1-2x（修改叶页 + 可能的页分裂）
```

---

## 8. 源码索引

| 组件 | 文件 | 关键行号 |
|------|------|----------|
| `DBImpl::Write` | `db/db_impl/db_impl_write.cc` | 主写入入口 |
| `WriteThread::Enter` | `db/write_thread.h` | Writer 队列管理 |
| `WriteToWAL` | `db/db_impl/db_impl_write.cc` | WAL 追加写入 |
| `WriteBatchInternal::InsertInto` | `db/write_batch_internal.h` | MemTable 插入 |
| `WriteBatchInternal::SetSequence` | `db/write_batch_internal.h` | 161-165 |
| `PackSequenceAndType` | `db/dbformat.h` | 181-186 |
| `VersionSet::FetchAddLastAllocatedSequence` | `db/version_set.h` | 1420-1457 |
| `SetLastPublishedSequence` | `db/version_set.h` | 1438-1442 |
| `MemTable::Add` | `db/memtable.cc` | 跳表插入 |
| `FlushJob::WriteLevel0Table` | `db/flush_job.cc` | 136 |
| `SwitchMemtable` | `db/db_impl/db_impl_write.cc` | 2827-3101 |
| `Options::inplace_update_support` | `include/rocksdb/options.h` | 原地更新选项 |
| `DBImpl::GetSnapshotImpl` | `db/db_impl/db_impl.cc` | 4588-4615 |
| `CompactionJob::ProcessKeyValueCompaction` | `db/compaction/compaction_job.cc` | 1855-1936 |
| `CompactionIterator::NextFromInput` | `db/compaction/compaction_iterator.cc` | 450-1094 |
| `CompactionOutputs::AddToOutput` | `db/compaction/compaction_outputs.cc` | 365-420 |
