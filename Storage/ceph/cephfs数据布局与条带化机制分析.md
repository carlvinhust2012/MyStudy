# CephFS 数据布局与条带化机制分析

## 1. 一句话概括

CephFS 中一个文件对应一个 inode（元数据由 MDS 管理），文件数据按 object_size 切分成多个 RADOS 对象，每个对象通过 PG（Placement Group）映射到不同的 OSD 上存储，副本由 CRUSH 算法决定放置位置。

## 2. 整体数据布局

```
文件 file.txt (8MB)
  │
  ├── MDS 管理的 inode（元数据）
  │     - 文件名、大小、权限、时间戳
  │     - striping 属性: stripe_unit, stripe_count, object_size
  │
  └── RADOS 对象（数据）
        ├── obj_0 (0 - 1MB)      → PG_1 → OSD 3, OSD 7, OSD 12（三副本）
        ├── obj_1 (1MB - 2MB)    → PG_5 → OSD 5, OSD 9, OSD 15（三副本）
        ├── obj_2 (2MB - 3MB)    → PG_1 → OSD 3, OSD 7, OSD 12
        ├── obj_3 (3MB - 4MB)    → PG_5 → OSD 5, OSD 9, OSD 15
        ├── obj_4 (4MB - 5MB)    → PG_1 → OSD 3, OSD 7, OSD 12
        ├── obj_5 (5MB - 6MB)    → PG_5 → OSD 5, OSD 9, OSD 15
        ├── obj_6 (6MB - 7MB)    → PG_1 → OSD 3, OSD 7, OSD 12
        └── obj_7 (7MB - 8MB)    → PG_5 → OSD 5, OSD 9, OSD 15
```

## 3. 三个关键条带化参数

| 参数 | 含义 | 默认值 | 说明 |
|---|---|---|---|
| **stripe_unit** | 每个 stripe 的大小 | 4MB | 文件按此大小切分成逻辑条带 |
| **stripe_count** | 条带跨多少个 OSD pool | 1（不条带化） | 设为 1 时所有对象在同一 pool |
| **object_size** | 每个 RADOS 对象的大小 | 4MB | stripe_unit 必须 <= object_size |

```
关系: stripe_unit <= object_size
  - stripe_unit == object_size: 一个对象对应一个条带（最常见）
  - stripe_unit < object_size:  一个对象包含多个条带（跨 pool 循环写入）
```

## 4. 条带化工作模式

### 4.1 stripe_count = 1（默认，不条带化）

```
文件 (12MB), stripe_unit=4MB, stripe_count=1, object_size=4MB

  obj_0 (0-4MB)    → Pool_A → PG → OSD 3, 7, 12
  obj_1 (4-8MB)    → Pool_A → PG → OSD 5, 9, 15
  obj_2 (8-12MB)   → Pool_A → PG → OSD 8, 11, 14

所有对象在同一 pool 中
对象顺序分配到不同 PG（由 CRUSH 决定 OSD 映射）
```

### 4.2 stripe_count = 2（跨 pool 条带化）

```
文件 (8MB), stripe_unit=1MB, stripe_count=2, object_size=1MB

  obj_0 (0-1MB)    → Pool_A → OSD 3, 7, 12
  obj_1 (1-2MB)    → Pool_B → OSD 5, 9, 15
  obj_2 (2-3MB)    → Pool_A → OSD 3, 7, 12
  obj_3 (3-4MB)    → Pool_B → OSD 5, 9, 15
  obj_4 (4-5MB)    → Pool_A → OSD 3, 7, 12
  obj_5 (5-6MB)    → Pool_B → OSD 5, 9, 15
  obj_6 (6-7MB)    → Pool_A → OSD 3, 7, 12
  obj_7 (7-8MB)    → Pool_B → OSD 5, 9, 15

数据循环分配到 2 个 pool（交替写入）
提高并行度，单个大文件可同时利用多个 pool 的带宽
```

### 4.3 对象到 OSD 的完整映射链路

```
RADOS 对象 → PG（Placement Group）→ OSD

步骤 1: 对象到 PG 的映射
  object_hash(obj_name) % pg_num = pg_id
  使用对象的名称哈希值对 PG 总数取模

步骤 2: PG 到 OSD 的映射（CRUSH 算法）
  crush(pg_id) → [osd_3, osd_7, osd_12]
  CRUSH 根据集群拓扑（机架、主机、OSD）计算 OSD 列表
  每次计算结果是确定性的（相同的 pg_id 总是映射到相同的 OSD）

步骤 3: 副本放置
  Primary OSD: osd_3（负责处理客户端请求）
  Replica 1:   osd_7
  Replica 2:   osd_12
```

## 5. PG（Placement Group）的作用

PG 是 Ceph 架构中最核心的抽象之一，在对象和 OSD 之间增加了一层间接映射:

### 5.1 为什么需要 PG

```
没有 PG 的情况（对象直接映射 OSD）:
  对象_1 → OSD_3
  对象_2 → OSD_7
  对象_3 → OSD_3
  ...

  问题: 新增一个 OSD 时，需要重新计算所有对象的映射关系
        需要迁移大量对象，集群抖动严重

有 PG 的情况:
  对象_1, 对象_2, 对象_3 → PG_5 → OSD_3
  对象_4, 对象_5, 对象_6 → PG_8 → OSD_7

  新增 OSD 时: 只需迁移少量 PG 到新 OSD
              每个 PG 内的所有对象一起迁移
              减少元数据更新量，集群更稳定
```

### 5.2 PG 的关键特性

```
1. PG 是副本管理的基本单位
   - 每个 PG 维护自己的副本状态机（Peering → Active → Clean）
   - PG 内的所有对象使用相同的副本策略

2. PG 是数据迁移的基本单位
   - OSD 增减时，以 PG 为粒度迁移数据
   - 减少数据迁移的粒度和元数据更新量

3. PG 是故障恢复的基本单位
   - OSD 故障时，该 OSD 上的 PG 由其他副本重建
   - PG 的 Peering 过程确保所有副本达成一致

4. PG 数量是固定的
   - 创建 pool 时指定 pg_num（默认 32，生产通常 256-1024）
   - 不随对象数量变化
   - 但 pg_num 可以手动扩展（会触发 PG 分裂）
```

### 5.3 PG 到 OSD 的映射: CRUSH 算法

```
CRUSH（Controlled Replication Under Scalable Hashing）:

  输入: pg_id + 副本数 + CRUSH map（集群拓扑）
  输出: OSD 列表 [primary, replica1, replica2, ...]

  CRUSH Map 定义了物理拓扑:
    Root
     ├── Rack_1
     │    ├── Host_1
     │    │    ├── OSD_0
     │    │    └── OSD_1
     │    └── Host_2
     │         ├── OSD_2
     │         └── OSD_3
     └── Rack_2
          ├── Host_3
          │    ├── OSD_4
          │    └── OSD_5
          └── Host_4
               ├── OSD_6
               └── OSD_7

  CRUSH 规则示例:
    rule replicated_rule {
      step take default         // 从 root 开始
      step chooseleaf firstn 2 type host  // 选 2 个不同 host
      step emit                 // 输出 OSD 列表
    }

  结果: 同一个 PG 的副本分布在不同 host 上
        单个 host 故障不会导致数据丢失
```

## 6. CephFS 写入完整流程

```
客户端写文件 (8MB):
  │
  ├── 1. 元数据操作（MDS）
  │     客户端 → MDS: open/lookup inode
  │     MDS 返回: inode 属性 + striping 参数
  │
  ├── 2. 数据切分
  │     客户端根据 object_size=4MB 切分:
  │     → [obj_0(0-4MB), obj_1(4-8MB)]
  │
  ├── 3. 对象映射
  │     obj_0 → hash → PG_5  → CRUSH → OSD_3(primary), OSD_7, OSD_12
  │     obj_1 → hash → PG_8  → CRUSH → OSD_5(primary), OSD_9, OSD_15
  │
  ├── 4. 数据写入（并发）
  │     obj_0: 客户端 → OSD_3 → OSD_7 → OSD_12 → ACK
  │     obj_1: 客户端 → OSD_5 → OSD_9 → OSD_15 → ACK（并行）
  │
  └── 5. 元数据更新（MDS）
        客户端 → MDS: 更新文件大小、mtime
        MDS 持久化到 RADOS（MDS 自身的元数据也以对象形式存入 RADOS）
```

## 7. CephFS vs 3FS 架构对比

| 对比项 | CephFS | 3FS |
|---|---|---|
| 定位 | 通用分布式文件系统 | AI 训练专用高性能文件系统 |
| 元数据服务 | MDS（活跃-standby，单主） | Meta Server（无状态，多实例） |
| 元数据存储 | RADOS（对象形式） | FoundationDB（分布式 KV） |
| 数据切分 | object_size（默认 4MB） | chunkSize（64KB-64MB） |
| 数据分散 | 条带化到多个 pool | 条带化到多个 chain |
| 副本管理 | PG + CRUSH 算法 | Chain 表（Mgmtd 集中管理） |
| 写路径 | Client → Primary OSD → Replica → Client | Client → Head → Member → Tail → Client |
| 副本放置 | CRUSH 规则（机架/主机感知） | Chain 表定义（Mgmtd 管理） |
| 读路径 | 可从任意副本读 | 可从任意 chain 成员读 |
| 数据映射层 | 对象 → PG → OSD（两层映射） | Chunk → Chain → Target（直接映射） |
| 恢复粒度 | PG 级别 | Chunk 级别 |
| 网络协议 | TCP（Messenger v2） | RDMA（零拷贝） |
| 故障检测 | OSD 心跳 + MDS 心跳 | Mgmtd 心跳 |
| 一致性 | PG 日志 + Peering | Chain 版本号 + CRAQ |
| 扩展性 | PG 自动分裂 | Chain 表手动/自动扩展 |

## 8. 两种架构的取舍

### 8.1 CephFS 的优势

```
1. 去中心化的数据放置（CRUSH）
   - 不依赖中心节点计算 OSD 映射
   - 客户端本地即可计算对象到 OSD 的映射
   - 扩展时无需中心协调

2. PG 提供了天然的故障恢复和迁移粒度
   - OSD 增减只影响部分 PG
   - PG 内部自动完成 Peering 和数据恢复

3. 通用的通用文件系统
   - 支持 POSIX 锁、配额、快照、ACL
   - 适合各种工作负载（不只是大文件顺序读写）

4. 成熟的生态
   - 10+ 年的发展，社区活跃
   - 支持 RBD（块）、RGW（对象）、CephFS（文件）多种接口
```

### 8.2 3FS 的优势

```
1. 极致的 I/O 性能（RDMA 零拷贝）
   - 服务器端 RDMA WRITE 直接推送到客户端内存
   - 无内核参与，零 CPU 拷贝
   - 吞吐量: 6.6 TiB/s（180 节点集群）

2. 更简单的数据路径
   - 对象 → Chain → Target（两层映射，无 PG 中间层）
   - 写路径更短，延迟更低

3. 无状态元数据服务
   - Meta Server 无状态，可水平扩展
   - 不像 MDS 有单主瓶颈

4. 针对 AI 训练优化
   - 无客户端读缓存（避免缓存一致性问题）
   - 大文件顺序读写优化
   - 支持极高并发客户端（500+ 节点）
```

### 8.3 核心架构差异图

```
CephFS 架构:
  ┌──────┐    ┌──────┐    ┌────────┐    ┌──────┐    ┌─────┐
  │ 客户端│ →  │ MDS  │    │ RADOS  │ →  │  PG  │ →  │ OSD │
  └──────┘    └──────┘    │ 对象池  │    └──────┘    └─────┘
              元数据       └────────┘       CRUSH
              活跃standby   数据存储       去中心化映射

3FS 架构:
  ┌──────┐    ┌────────┐    ┌────────┐    ┌────────┐
  │ 客户端│ →  │ Meta   │    │ Chain  │ →  │ Target │
  └──────┘    │ Server │    │ 表     │    │ (OSD)  │
              └────────┘    └────────┘    └────────┘
              无状态FDB      Mgmtd管理     链式复制

关键差异:
  CephFS: CRUSH 去中心化 → 数据映射不需要中心节点
  3FS:    Chain 表集中化 → Mgmtd 是映射关系的中心（但有缓存）
  CephFS: PG 中间层 → OSD 变化只影响部分 PG
  3FS:    直接映射 → 更简单但 Mgmtd 是潜在瓶颈
```

## 9. 总结

CephFS 采用"对象 → PG → OSD"三层映射架构，通过 CRUSH 算法实现去中心化的数据放置，PG 提供了故障恢复和数据迁移的自然粒度。3FS 采用"Chunk → Chain → Target"两层映射，通过 Mgmtd 集中管理 chain 表，以更简单的数据路径换取更高的 I/O 性能。两种方案各有取舍: CephFS 更通用、更去中心化，3FS 更极致、更简单。

CephFS 多一层 PG 抽象的代价是更复杂的数据路径（需要 Peering、PG 状态机），但换来的是更灵活的故障恢复和更小的迁移粒度。3FS 省去 PG 层的代价是 Mgmtd 成为映射关系的中心，但换来了更短的 I/O 路径和更简单的实现。
