# 为什么选择 RocksDB 作为嵌入式元数据存储

---

## 目录

1. [核心需求分析](#1-核心需求分析)
2. [RocksDB 的匹配优势](#2-rocksdb-的匹配优势)
3. [为什么不用其他方案](#3-为什么不用其他方案)
4. [实际选择案例](#4-实际选择案例)
5. [RocksDB Env 可扩展性](#5-rocksdb-env-可扩展性)
6. [WAL/MANIFEST 文件丢失的影响](#6-walmanifest-文件丢失的影响)
7. [总结](#7-总结)

---

## 1. 核心需求分析

BlueFS（Ceph）和 Doris BE 选择嵌入式 KV 引擎存储元数据，核心需求如下：

```
嵌入式元数据存储的 7 大需求:

  ┌────────────────────────────────────────────────────────┐
  │ 1. 嵌入式运行                                        │
  │    与主程序共享进程空间，无独立进程，无网络开销           │
  ├────────────────────────────────────────────────────────┤
  │ 2. 高频小写入                                        │
  │    onode/元数据更新频繁，需要 O(1) 级别的写入性能       │
  ├────────────────────────────────────────────────────────┤
  │ 3. 低延迟读取                                        │
  │    每次对象/数据读写都要查元数据，必须在微秒级返回       │
  ├────────────────────────────────────────────────────────┤
  │ 4. 可靠持久化                                        │
  │    fsync 保证崩溃不丢数据，双日志（WAL + MANIFEST）     │
  ├────────────────────────────────────────────────────────┤
  │ 5. 跨平台零依赖                                      │
  │    不依赖外部服务，部署简单，无 DBA，无连接池管理        │
  ├────────────────────────────────────────────────────────┤
  │ 6. 可定制适配                                        │
  │    支持自定义 Env（I/O 层适配）、Compaction 策略等      │
  ├────────────────────────────────────────────────────────┤
  │ 7. 生产级稳定性                                      │
  │    经过大规模生产验证，社区活跃，持续维护               │
  └────────────────────────────────────────────────────────┘
```

---

## 2. RocksDB 的匹配优势

### 2.1 需求匹配矩阵

| 需求 | RocksDB 能力 | 匹配度 |
|------|-------------|--------|
| 嵌入式运行 | 纯 C++ 库，链接到进程（`librocksdb.so`），无独立进程 | **完美** |
| 高频小写入 | LSM-Tree 架构，写入 = 内存 SkipList 插入 O(log N)，后台异步刷盘 | **完美** |
| 低延迟读取 | Block Cache（LRU）+ Bloom Filter + 前缀压缩，热点数据微秒级 | **优秀** |
| 可靠持久化 | WAL（fsync）+ MANIFEST（fsync）双日志保证 | **完美** |
| 跨平台零依赖 | 无外部依赖，支持 Linux/macOS/Windows/嵌入式 | **完美** |
| 可定制适配 | Env 抽象层可替换 I/O；支持 Column Family、Compaction 策略选择 | **优秀** |
| 生产级稳定性 | Meta/Meta 内部大规模使用（数十 PB 级），社区活跃 | **完美** |

### 2.2 LSM-Tree vs B-Tree 在此场景的关键差异

```
场景: BlueStore 存储对象元数据（onode）

  写入模式:
    每次 OSD 写入对象 → 更新 onode (大小、extent map、属性)
    → 高频、小量、随机 key 的写入

  B-Tree (如 SQLite):
    Put(key, value)
    → 找到叶子节点 → 原地更新 → 可能触发页分裂
    → 随机磁盘 IO（即使有 page cache 也有写放大）
    → 写入延迟: ~毫秒级

  LSM-Tree (RocksDB):
    Put(key, value)
    → 插入内存 SkipList → 完成
    → 后台 Flush 到 SST（异步，不影响写入延迟）
    → 写入延迟: ~微秒级

  结论: 元数据更新场景中，LSM-Tree 的写入性能远优于 B-Tree
```

### 2.3 RocksDB 的关键特性

```
对 BlueFS/Doris BE 重要的特性:

  Column Family:
    ├── 不同类型数据隔离存储（如 onode、omap、pg_log 各自一个 CF）
    ├── 独立的 MemTable、Compaction 策略
    └── 独立的 Bloom Filter 和 Block Cache

  Compaction 策略选择:
    ├── Leveled — 通用，读放大低
    ├── Universal — 写优化，写放大小
    └── FIFO — 时序数据

  Bloom Filter:
    ├── SST 级别过滤，大幅减少不必要的磁盘读
    └── 全键 / 前缀两种模式

  Block Cache:
    ├── 热点 onode 常驻内存
    └── 直接避免磁盘 IO

  Env 抽象:
    ├── BlueRocksEnv — RocksDB I/O 路由到 BlueFS
    └── 可替换存储后端（如远程存储）
```

---

## 3. 为什么不用其他方案

### 3.1 独立数据库服务

| 方案 | 延迟 | 部署 | 可靠性 | 不选的原因 |
|------|------|------|--------|-----------|
| **MySQL** | ~1ms (网络+查询) | 每个 OSD/Doris 节点都装一个 MySQL 实例 | 高，但运维成本高 | 独立进程、网络开销、部署复杂，OSD 节点数量大时运维灾难 |
| **PostgreSQL** | ~1ms | 同上 | 高 | 同上 |
| **Redis** | ~0.1ms | 需要 Redis Cluster | 需要额外配置 AOF/RDB | 独立服务，内存成本高，数据持久化不如嵌入式方案可靠 |
| **TiKV** | ~5ms | 需要 PD + TiKV 集群 | 分布式一致 | 网络延迟太高，不适合微秒级元数据查询 |
| **etcd** | ~1ms | 需要 etcd 集群 | 分布式一致 | 设计目标是配置管理，不是高频 KV 存储 |

**核心问题: BlueFS 和 Doris BE 是存储引擎，不是应用服务器。让存储引擎依赖外部数据库服务会引入单点故障、网络延迟、部署复杂度，违背了存储系统"自包含"的设计原则。**

### 3.2 其他嵌入式 KV 引擎

| 方案 | 语言 | 数据模型 | 写性能 | 读性能 | 不选的原因 |
|------|------|---------|--------|--------|-----------|
| **LevelDB** | C++ | LSM-Tree | 好 | 一般 | RocksDB 的前身，缺少 Column Family、Bloom Filter、丰富 Compaction 策略等关键特性 |
| **LMDB** | C | B+Tree (mmap) | 差（写放大大） | 优秀 | mmap 方案写放大严重，不适合高吞吐写入场景 |
| **SQLite** | C | B-Tree | 差 | 好 | B-Tree 写放大大，不适合高并发写入 |
| **Badger** | Go | LSM-Tree | 好 | 好 | Go 实现，Ceph/Doris 都是 C++ 项目，跨语言集成成本高 |
| **bbolt** | Go | B+Tree | 差 | 好 | 纯 Go，B+Tree，不适合写密集型 |
| **WiredTiger** | C | B+Tree + LSM | 好 | 好 | MongoDB 引擎，有独立版本但生态不如 RocksDB，LSM 模式不如 RocksDB 成熟 |
| **KyotoCabinet** | C++ | Hash + B+Tree | 好 | 好 | 已停止维护（2012 年），RocksDB 功能完全覆盖 |

### 3.3 关键淘汰因素

```
LevelDB → 被 RocksDB 替代（RocksDB 就是 LevelDB 的增强版）
  RocksDB 在 LevelDB 基础上增加了:
    ├── Column Family (多数据流隔离)
    ├── 事务支持
    ├── Merge Operator
    ├── 丰富的 Compaction 策略 (Leveled/Universal/FIFO)
    ├── Bloom Filter (SST 级别)
    ├── Block Cache (LRU)
    ├── 备份/检查点
    ├── WAL 压缩和回收
    └── 多种压缩算法 (Snappy/ZSTD/LZ4 等)

LMDB → 写放大问题
  LMDB 使用 mmap + B+Tree:
    每次写入可能触发页分裂 → 随机磁盘写入
    对于 BlueFS 这种"每个对象 IO 都要更新 onode"的场景，写放大不可接受

SQLite → B-Tree 写放大
  同理，B-Tree 的原地更新模式不适合高并发写入

Go 系列 (Badger/bbolt) → 语言不匹配
  Ceph 和 Doris 都是 C++ 项目
  跨语言集成需要 CGO 或 Thrift/GRPC bridge
  引入额外复杂度和性能损耗
```

---

## 4. 实际选择案例

### 4.1 Ceph BlueStore

```
Ceph 的演进:

  FileStore (已废弃):
    ├── 使用本地文件系统 (ext4/xfs)
    ├── 元数据操作通过文件系统调用
    ├── 小文件问题严重（每个对象一个文件或 xattr）
    └── 性能瓶颈明显

  BlueStore (当前):
    ├── 绕过文件系统，直接管理裸块设备
    ├── 使用 RocksDB 存储:
    │   ├── onode (对象元数据: 大小、extent map、属性)
    │   ├── omap (对象的键值对扩展属性)
    │   ├── pg_log (PG 操作日志, 存为 pgmeta 对象的 omap)
    │   ├── collection (PG 的对象集合)
    │   └── 其他内部状态
    ├── 通过 BlueRocksEnv 适配:
    │   RocksDB WAL → BlueFS db.wal/ → BDEV_WAL
    │   RocksDB SST → BlueFS db/ → BDEV_DB
    │   RocksDB MANIFEST → BlueFS db/ → BDEV_DB
    └── 性能远超 FileStore

  BlueStore 的 RocksDB 配置:
    ├── Column Families: default, meta, wal
    ├── write_buffer_size: 64MB~256MB
    ├── max_write_buffer_number: 4
    ├── compression: kZSTD / kLZ4
    ├── Bloom Filter: 全局开启
    └── Block Cache: 自定义大小 (默认 128MB+)
```

### 4.2 Doris BE

```
Doris BE 使用 RocksDB:

  存储内容:
    ├── Tablet 元数据 (schema、版本、分区信息)
    ├── 数据版本信息 (rowset 元数据、compaction 状态)
    ├── Global 信息 (cluster 元数据、统计信息)
    └── 部分索引数据 (bloom filter 索引)

  配置特点:
    ├── 多个 Column Family 隔离不同类型数据
    ├── 使用 Merge Operator 处理版本合并
    └── 定制 Compaction 策略优化元数据管理
```

### 4.3 其他使用 RocksDB 的知名项目

```
大规模使用 RocksDB 的系统:

  ├── Meta/Facebook — Instagram、Messenger、WhatsApp 后端
  ├── Ceph — BlueStore 元数据引擎
  ├── Apache Doris — BE 元数据引擎
  ├── Apache Flink — 状态后端 (RocksDBStateBackend)
  ├── Apache Kafka — Streams 状态存储
  ├── TiKV — 底层使用 RocksDB 作为本地存储
  ├── CockroachDB — 本地存储引擎
  ├── MySQL — MyRocks 存储引擎
  └── AI/ML — 许多训练框架的 checkpoint 存储
```

---

## 5. RocksDB Env 可扩展性

RocksDB 之所以能被各种系统深度集成，关键在于 **Env 抽象层**：

```
RocksDB 架构:

  ┌─────────────────────────────────────────┐
  │              RocksDB                     │
  │  ┌─────────┐ ┌──────────┐ ┌──────────┐ │
  │  │MemTable │ │ SST Files│ │Manifest  │ │
  │  └────┬────┘ └────┬─────┘ └────┬─────┘ │
  │       │           │            │        │
  │  ─────┴───────────┴────────────┴─────  │
  │              Env 接口层                │
  │  ┌──────────────────────────────────┐  │
  │  │ Env (抽象接口)                    │  │
  │  │  ├── NewWritableFile()            │  │
  │  │  ├── NewRandomAccessFile()        │  │
  │  │  ├── NewDirectory()               │  │
  │  │  ├── FileExists()                 │  │
  │  │  ├── GetFileSize()                │  │
  │  │  └── DeleteFile()                 │  │
  │  └──────────────┬───────────────────┘  │
  └─────────────────┼───────────────────────┘
                    │
          ┌─────────┼─────────┐
          ▼         ▼         ▼
    ┌──────────┐ ┌────────┐ ┌─────────┐
    │ POSIX Env│ │BlueFS  │ │ 内存 Env │
    │ (默认)   │ │ Env    │ │ (测试)  │
    │ 文件系统 │ │(Ceph)  │ │         │
    └──────────┘ └────────┘ └─────────┘

  通过替换 Env 实现，RocksDB 的数据可以存储在任何地方:
    ├── 本地文件系统 (默认)
    ├── BlueFS (Ceph BlueStore)
    ├── 远程对象存储 (自定义 Env)
    ├── 内存 (测试)
    └── NVM/持久化内存 (PMEM Env)
```

---

## 6. WAL/MANIFEST 文件丢失的影响

### 6.1 三种文件丢失的影响对比

| 文件 | 丢失后能否恢复 | 数据丢失风险 | 恢复复杂度 |
|------|-------------|-----------|-----------|
| **SST 文件** | 不可恢复 | 严重（直接丢数据） | 不可恢复 |
| **MANIFEST** | 能容错恢复 | 无 | 低（自动 TryRecover） |
| **WAL** | 部分可恢复 | 低（只丢未 Flush 的数据） | 低 |

**核心原则：SST 文件才是真正存数据的，WAL 和 MANIFEST 都是辅助结构。丢失 SST = 丢数据；丢失 WAL/MANIFEST = 丢"索引"，数据还在但可能找不到。**

### 6.2 MANIFEST 丢失

```
影响: 严重，但不丢数据

  MANIFEST 记录的是 SST 文件的"目录":
    MANIFEST → VersionEdit 记录列表 → 重建 Version → 每层的 SST 文件列表

  丢失后:
    → RocksDB 不知道有哪些 SST 文件 → 无法打开数据库
    → 但 SST 文件本身还在磁盘上，数据没丢

  恢复方式:
    1. RocksDB 自带容错: TryRecover() (version_set.cc:6849)
       → 自动寻找备选 MANIFEST 文件 (MANIFEST-000002, MANIFEST-000001...)
       → 如果 CURRENT 指向的 MANIFEST 损坏，会尝试旧版本
       → 逐一尝试直到找到一个可用的 MANIFEST

    2. 手动修复: 修改 CURRENT 文件指向可用的 MANIFEST
       echo "MANIFEST-000002" > CURRENT

    3. 极端情况: 所有 MANIFEST 都丢失
       → .sst 文件还在磁盘上
       → 但无法自动重建 SST 映射关系
       → 需要手动重建（非常复杂，通常不推荐）

  类比: MANIFEST 像数据库的"数据字典"
    字典丢了，数据文件还在，但系统不知道哪个文件存什么
```

### 6.3 WAL 丢失

```
影响: 取决于 MemTable 是否已 Flush 为 SST

  场景 1: WAL 丢失，数据已 Flush 为 SST
    → 完全无影响
    → SST 文件中有完整数据，WAL 本来就可以删了

  场景 2: WAL 丢失，数据未 Flush（仍在 MemTable 中）
    → 丢失 MemTable 中尚未持久化的写入
    → 如果 sync=false: 可能丢失最后一个 WAL record
    → 如果 sync=true: WAL 数据已 fsync，通常不会出现"丢失"情况

  场景 3: WAL 部分丢失（最后几条记录损坏）
    → Reader 遇到损坏记录 → 报告 corruption → 跳过
    → 可能丢失损坏点之后的部分写入

  类比: WAL 像数据库的"redo log"
    log 丢了，已经写入数据文件的数据不丢
    只有还在内存中、没来得及刷盘的数据会丢
```

### 6.4 BlueStore 中的多层保护

```
Ceph BlueStore 对 RocksDB 有额外保护层:

  RocksDB 文件存储路径:
    RocksDB WAL      → BlueFS db.wal/  → BDEV_WAL 裸设备
    RocksDB SST      → BlueFS db/      → BDEV_DB 裸设备
    RocksDB MANIFEST  → BlueFS db/      → BDEV_DB 裸设备

  多层日志保护:

    Layer 1: BlueFS Log (ino=1, BDEV_WAL)
      → 保护 BlueFS 的文件系统元数据（文件分配、目录结构）
      → 崩溃后重放 BlueFS Log → 重建文件系统

    Layer 2: RocksDB WAL
      → 保护 MemTable 数据
      → 崩溃后重放 WAL → 重建 MemTable

    Layer 3: RocksDB MANIFEST
      → 保护 SST 文件映射
      → 崩溃后重放 MANIFEST → 重建 Version

  正常断电恢复过程:
    1. BlueFS Log 重放 → RocksDB 的 .log/.sst/.MANIFEST 文件都可访问
    2. MANIFEST 重放 → 重建 SST 文件列表
    3. WAL 重放 → 重建 MemTable
    4. 恢复完成，所有数据完整

  只有裸盘物理损坏才会导致真正意义上的数据丢失
```

---

## 7. 总结

```
选择 RocksDB 的本质原因:

  1. 它是嵌入式库，不是独立服务
     → 零部署成本，零运维成本，零网络开销

  2. LSM-Tree 架构天然适合写密集的元数据场景
     → 写入 O(log N) 内存操作，不阻塞前台请求

  3. 功能丰富度在嵌入式 KV 领域无出其右
     → Column Family / Bloom Filter / Block Cache / 多 Compaction 策略
     → 完全覆盖了 LevelDB、LMDB、SQLite 等替代品的所有功能

  4. Env 抽象允许深度定制
     → BlueRocksEnv 让 Ceph 的 RocksDB 数据存储在裸块设备上
     → 这是其他嵌入式 KV 引擎不具备的灵活性

  5. 大规模生产验证
     → Meta/Meta 全球部署，数十 PB 级数据
     → Ceph 每个全球部署中有数万个 OSD 节点运行 RocksDB

  一句话: 选择 RocksDB 不是因为它"最好"，而是因为它在"嵌入式 KV"这个赛道上
  综合能力最强、生态最成熟、可定制性最好，没有明显的替代品。
```
