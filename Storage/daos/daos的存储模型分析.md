# DAOS 存储模型分析

## 目录

1. [概述](#1-概述)
2. [存储层次结构](#2-存储层次结构)
3. [存储介质架构（SCM + NVMe）](#3-存储介质架构scm--nvme)
4. [持久化数据结构（PMEM Layout）](#4-持久化数据结构pmem-layout)
5. [对象数据模型（dkey + akey + value）](#5-对象数据模型dkey--akey--value)
6. [Btree 元数据组织](#6-btree-元数据组织)
7. [EVTree 数组值存储](#7-evtree-数组值存储)
8. [写流程（Client → Disk）](#8-写流程client--disk)
9. [读流程（Disk → Client）](#9-读流程disk--client)
10. [版本控制与 ILOG](#10-版本控制与-ilog)
11. [分布式事务（DTX）](#11-分布式事务dtx)
12. [NVMe 空间管理（VEA）](#12-nvme-空间管理vea)
13. [垃圾回收（GC）](#13-垃圾回收gc)
14. [聚合（Aggregation）](#14-聚合aggregation)
15. [空间预留与碎片化](#15-空间预留与碎片化)
16. [数据放置与冗余](#16-数据放置与冗余)
17. [与其他存储系统对比](#17-与其他存储系统对比)
18. [源码索引](#18-源码索引)

---

## 1. 概述

DAOS（Distributed Asynchronous Object Storage）是 Intel 开源的分布式异步对象存储系统，专为 PMEM 和 NVMe 优化设计。核心存储特点：

1. **双层存储介质**：SCM（持久化内存）存元数据和小对象，NVMe 存大对象，统一地址抽象
2. **多级 Btree 组织**：Pool → Container → Object → dkey → akey → value 五层嵌套 Btree
3. **Epoch 版本控制**：每个 I/O 操作隐式关联 epoch，实现 MVCC 非破坏性写入
4. **EVTree 数组存储**：基于 R-tree 变体的版本化区间树，存储数组类型的定长元素
5. **PMDK 事务保证**：所有 PMEM 修改在 PMDK 事务中执行，确保崩溃一致性
6. **VEA 顺序分配**：NVMe 空间按 I/O 流水线顺序分配，优化写入带宽

---

## 2. 存储层次结构

```
DAOS Pool (UUID)
  │  跨多个 Target 分布
  │  每个 Target 持有 Pool Shard
  │
  ├── Container 1 (UUID)
  │     ├── Object A (128-bit ID)
  │     │     ├── dkey_1 → akey_1 → Single Value (SV)
  │     │     ├── dkey_1 → akey_2 → Array Value (EVTree)
  │     │     ├── dkey_2 → akey_1 → Single Value (inline on SCM)
  │     │     └── dkey_2 → Flat dkey (no akey layer)
  │     ├── Object B
  │     └── Object C
  │
  ├── Container 2 (UUID)
  │     └── ...
  │
  ├── GC Bins (分层垃圾回收)
  ├── VEA Space (NVMe 空间管理)
  └── Pool Map Version (成员拓扑)
```

### 三层抽象

| 层级 | 标识 | 持久化位置 | 元数据结构 | 功能 |
|------|------|-----------|-----------|------|
| Pool | UUID | SCM (`vos_pool_df`) | Container Btree | 存储预留、Target 分布 |
| Container | UUID | SCM (`vos_cont_df`) | Object Index Btree | 对象地址空间、快照管理 |
| Object | 128-bit ID | SCM (`vos_obj_df`) | dkey Btree | 键值数据存储 |

---

## 3. 存储介质架构（SCM + NVMe）

```
┌─────────────────────────────────────────────────────────────────┐
│                       DAOS Target                            │
│                                                                 │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │  SCM (Storage Class Memory) — PMDK 管理                    │ │
│  │                                                             │ │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐    │ │
│  │  │ Pool DF     │  │ Container DF │  │ Object DF   │    │ │
│  │  │ (元数据)     │  │ (元数据)     │  │ (元数据)     │    │ │
│  │  └──────────────┘  └──────────────┘  └──────────────┘    │ │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐    │ │
│  │  │ Btree Nodes │  │ ILOG Entries │  │ Key Records  │    │ │
│  │  │ (索引树)     │  │ (版本日志)   │  │ (键记录)     │    │ │
│  │  └──────────────┘  └──────────────┘  └──────────────┘    │ │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐    │ │
│  │  │ DTX BLOB    │  │ GC Bins     │  │ Inline Value │    │ │
│  │  │ (事务记录)   │  │ (垃圾回收)   │  │ (小对象数据) │    │ │
│  │  └──────────────┘  └──────────────┘  └──────────────┘    │ │
│  │                                                             │ │
│  │  访问方式: CPU 直接 load/store (零拷贝, 纳秒级延迟)      │ │
│  └─────────────────────────────────────────────────────────────┘ │
│                                                                 │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │  NVMe SSD — SPDK 管理 (用户态 DMA)                          │ │
│  │                                                             │ │
│  │  ┌────────────────┐  ┌────────────────┐  ┌────────────────┐ │ │
│  │  │ Blob Data     │  │ Blob Data     │  │ Blob Data     │ │ │
│  │  │ (大对象数据)   │  │ (数组值数据)   │  │ (数组值数据)   │ │ │
│  │  │ Block 0       │  │ Block 256     │  │ Block 512     │ │ │
│  │  │ Block 1       │  │ Block 257     │  │ Block 513     │ │ │
│  │  └────────────────┘  └────────────────┘  └────────────────┘ │ │
│  │                                                             │ │
│  │  访问方式: DMA (微秒级延迟, 高吞吐)                       │ │
│  │  空间管理: VEA (Versioned Extent Allocator)                │ │
│  └─────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
```

### Bio Address — 统一地址抽象

```c
// include/daos_srv/bio.h:61-74
typedef struct {
    uint64_t ba_off;     // 字节偏移 (SCM: PMDK pool offset, NVMe: SPDK blob offset)
    uint8_t  ba_type;    // DAOS_MEDIA_SCM 或 DAOS_MEDIA_NVME
    uint8_t  ba_gang_nr; // gang 地址数量 (多个离散分配合并为一个地址)
    uint16_t ba_flags;   // BIO_FLAG_HOLE, BIO_FLAG_DEDUP 等
} bio_addr_t;
```

---

## 4. 持久化数据结构（PMEM Layout）

### 4.1 Pool 持久化格式

```c
// vos/vos_layout.h:136-168
struct vos_pool_df {
    uint32_t    pd_magic;           // 0x5ca1ab1e
    uint32_t    pd_version;         // VOS_POOL_DF_2_8 (28)
    uint64_t    pd_scm_sz;          // SCM 总空间
    uint64_t    pd_nvme_sz;         // NVMe 总空间
    uuid_t      pd_id;              // Pool UUID
    uint64_t    pd_cont_nr;         // 容器数量
    struct btr_root pd_cont_root;   // 容器索引 Btree 根
    struct vea_space_df pd_vea_df;  // NVMe 空闲空间追踪
    struct vos_gc_bin_df pd_gc_bins[GC_MAX]; // GC 回收桶
};
```

### 4.2 Container 持久化格式

```c
// vos/vos_layout.h:299-325
struct vos_cont_df {
    uuid_t          cd_id;                // 容器 UUID
    uint64_t        cd_nobjs;             // 对象数量
    daos_size_t     cd_used;              // 已用空间
    struct btr_root cd_obj_root;          // 对象索引 Btree 根
    umem_off_t      cd_dtx_active_head;   // 活跃 DTX 链表头
    umem_off_t      cd_dtx_committed_head; // 已提交 DTX 链表头
    struct vea_hint_df cd_hint_df[2];     // NVMe 分配提示
    struct vos_gc_bin_df cd_gc_bins[GC_CONT]; // 容器级 GC
};
```

### 4.3 Object 持久化格式

```c
// vos/vos_layout.h:415-427
struct vos_obj_df {
    daos_unit_oid_t  vo_id;              // 对象 ID (128-bit + shard)
    daos_epoch_t     vo_sync;            // 最新同步 epoch
    umem_off_t       vo_known_dkey;      // 已知 dkey 的 PMEM 偏移
    struct ilog_df   vo_ilog;            // 版本日志根
    struct btr_root  vo_tree;            // dkey Btree 根
};
```

### 4.4 Key Record 持久化格式

```c
// vos/vos_layout.h:353-377
struct vos_krec_df {
    uint8_t     kr_bmap;       // 标志位图 (KREC_BF_EVT, KREC_BF_BTR)
    uint8_t     kr_cs_type;    // 校验和类型
    uint32_t    kr_size;       // 键长度
    struct ilog_df kr_ilog;    // 版本日志
    union {
        struct { struct btr_root kr_btr; ... };  // 子树 (akey/SV)
        struct evt_root kr_evt;                      // EVTree (array)
    };
};
```

### 4.5 Value Record 持久化格式

```c
// vos_layout.h:383-409
struct vos_irec_df {
    uint32_t    ir_ver;        // pool map 版本
    uint32_t    ir_dtx;        // DTX 条目
    uint64_t    ir_size;       // 值长度
    uint64_t    ir_gsize;      // 全局长度 (EC 场景)
    bio_addr_t  ir_ex_addr;    // 外部负载地址 (SCM 或 NVMe)
    char        ir_body[0];    // 内联值数据 (小值直接存储)
};
```

---

## 5. 对象数据模型（dkey + akey + value）

### 5.1 两层键 + 两种值

```
Object
  │
  ├── dkey "sensor_data"              ← Distribution Key (分布键)
  │     ├── akey "temperature"        ← Attribute Key (属性键)
  │     │     └── Single Value: [22.5, 23.1, 24.8, ...]
  │     │         (存储在 SV Btree 中)
  │     │
  │     ├── akey "humidity"
  │     │     └── Single Value: [45, 50, 48, ...]
  │     │
  │     ├── akey "raw_samples"
  │     │     └── Array Value: [0..1023] (4KB 定长元素)
  │     │         (存储在 EVTree 中, 按 [offset, length]×epoch 索引)
  │     │
  │     └── akey "metadata"
  │           └── Single Value: {"location": "rack1", ...}
  │
  ├── dkey "config" (flat dkey, 无 akey 层)
  │     └── Single Value: {"interval": "10s", ...}
  │
  └── dkey "index"
        └── Array Value: [0..999999]
```

### 5.2 值存储决策

| 值大小 | 存储位置 | 访问延迟 |
|--------|---------|---------|
| ≤ Btree 内联阈值 | `vos_irec_df.ir_body` (SCM, Btree 节点内) | ~100ns |
| 中等大小 | SCM PMDK blob (`ba_type = DAOS_MEDIA_SCM`) | ~200ns |
| 大对象 | NVMe SPDK blob (`ba_type = DAOS_MEDIA_NVME`) | ~10μs |
| 数组值 | NVMe SPDK blob (通过 EVTree 索引) | ~10μs |

### 5.3 dkey 的分布语义

同一个 dkey 下的所有 akey 保证存储在同一个 Target 上（co-located）。这意味着对同一 dkey 的多 akey 操作不需要跨 Target 访问，提供更好的局部性。

---

## 6. Btree 元数据组织

### 6.1 四类 Btree

```c
// vos/vos_internal.h:860-868
VOS_BTR_DKEY      = 0    // 分布键树 (dkey hash → vos_krec_df)
VOS_BTR_AKEY      = 1    // 属性键树 (akey hash → vos_krec_df)
VOS_BTR_SINGV     = 2    // 单值树 (epoch → vos_irec_df)
VOS_BTR_OBJ_TABLE = 3    // 对象索引表 (unit_oid → vos_obj_df)
```

### 6.2 完整树层次

```
vos_pool_df (SCM)
  │
  └── pd_cont_root (Btree, Container Btree)
       │
       └── [container_uuid] → vos_cont_df
            │
            └── cd_obj_root (Btree, Object Index Table)
                 │
                 ├── [unit_oid_A] → vos_obj_df
                 │     │
                 │     └── vo_tree (Btree, Dkey Btree)
                 │          │
                 │          ├── [hash(dkey_1)] → vos_krec_df
                 │          │     │
                 │          │     ├── KREC_BF_BTR → kr_btr (Akey Btree)
                 │          │     │     │
                 │          │     │     ├── [hash(akey_1)] → vos_krec_df
                 │          │     │     │     │
                 │          │     │     │     ├── KREC_BF_BTR → kr_btr (SV Btree)
                 │          │     │     │     │     └── [epoch] → vos_irec_df → value
                 │          │     │     │     │
                 │          │     │     │     └── KREC_BF_EVT → kr_evt (EVTree)
                 │          │     │     │           └── [offset, len]@epoch → extent
                 │          │     │     │
                 │          │     │     └── [hash(akey_2)] → ...
                 │          │     │
                 │          │     └── KREC_BF_NO_AKEY → value (flat dkey)
                 │          │
                 │          └── [hash(dkey_2)] → vos_krec_df (flat)
                 │                └── value (直接在 krec 中)
                 │
                 ├── [unit_oid_B] → vos_obj_df
                 │     └── ...
                 │
                 └── [unit_oid_C] → vos_obj_df
                       └── ...
```

### 6.3 Btree 特性

| 特性 | 说明 |
|------|------|
| `BTR_FEAT_EMBED_FIRST` | 单条目时直接存储在根节点，省去一级树 |
| `BTR_FEAT_UINT_KEY` | 使用 uint64 哈希键做比较 |
| `BTR_FEAT_DIRECT_KEY` | 存储 PMEM 偏移用于直接键比较（避免二次查找） |
| `BTR_FEAT_DYNAMIC_ROOT` | 根节点可从少量条目开始，按需扩展 |

### 6.4 Btree 阶数

| Btree 类型 | 阶数 (Order) | 说明 |
|-----------|-------------|------|
| Container Btree | 20 | 容器数量通常不多 |
| Object Index Table | 15 | 对象数量中等 |
| Dkey / Akey Btree | 20 | 键数量可能很大 |
| Single Value Btree | 5 | 同一 akey 的版本数有限 |

---

## 7. EVTree 数组值存储

### 7.1 R-tree 变体

EVTree 是 R-tree 的变体，用于存储**版本化的定长元素数组**。每个条目表示一个 `[offset, length] × [epoch, infinity]` 的矩形区间。

### 7.2 数据布局

```
Array Object: 1M 个 4KB 元素, 版本 e1 和 e2

EVTree 中的条目:
  ┌──────────────────────┐  ┌──────────────────────┐
  │ [0, 4096)  @ epoch e1 │  │ [4096, 8192) @ e1    │  ← 版本 e1 的写入
  │ extent_A (NVMe)       │  │ extent_B (NVMe)       │
  └──────────────────────┘  └──────────────────────┘
  ┌──────────────────────┐
  │ [0, 4096)  @ epoch e2 │  ← 版本 e2 覆盖写入
  │ extent_C (NVMe)       │
  └──────────────────────┘
  ┌──────────────────────┐
  │ [4096, 8192) @ e1    │  ← 未被 e2 覆盖，保留
  └──────────────────────┘

查询 epoch=e2, range=[0, 8192):
  → 命中 [0, 4096)@e2 → extent_C
  → 未命中 [4096, 8192)@e1 → 但 e1 < e2, 仍然可见!
  → 最终: [0,4096) from extent_C + [4096,8192) from extent_B
```

### 7.3 EVTree 查询

```c
// include/daos_srv/evtree.h:114-122
struct evt_filter {
    struct evt_rect filter_rect;  // 查询范围 [offset_min, offset_max] × [epoch, MAX_EPOCH]
    uint32_t        filter_hw;     // hash window
    daos_epoch_t    filter_epc;    // epoch 参数
    uint32_t        filter_flags;
};
```

EVTree 按起始偏移排序（SSOF 策略），查询时遍历所有与查询矩形相交的条目，按 epoch 过滤可见性。

---

## 8. 写流程（Client → Disk）

```mermaid
sequenceDiagram
    participant C as Client
    participant DAOS as DAOS Server
    participant VOS as VOS Engine
    participant IO as vos_update_begin
    participant Pool as vos_pool
    member Obj as vos_object(cache)
    member Tree as Btree(dkey/akey)
    member SCM as PMDK(SCM)
    member NVMe as VEA(NVMe)

    C->>DAOS: daos_obj_update(dkey, akey, value, epoch)

    DAOS->>IO: vos_update_begin(ioc)
    IO->>Pool: vos_space_hold(scm_size, nvme_size)
    Note over Pool: 预留空间<br/>vp_space_held[NVME/SCM] += hold_size

    IO->>Obj: vos_obj_acquire(unit_oid)
    Note over Obj: 从 LRU 缓存获取/加载对象

    IO->>Tree: dkey_update_begin(ioc)
    Tree->>SCM: 查找 dkey Btree
    alt dkey 不存在
        Tree->>SCM: 分配 vos_krec_df (PMDK)
        Tree->>SCM: 创建 dkey Btree
    end

    Tree->>Tree: akey_update_begin(ioc)
    alt akey 不存在
        Tree->>SCM: 分配 vos_krec_df (PMDK)
        Tree->>SCM: 创建 akey Btree
    end

    Note over IO: 决定值存储位置

    alt 小值 (内联)
        Tree->>SCM: 分配 vos_irec_df
        Tree->>SCM: value 写入 ir_body[] (Btree 节点内)
    else 中值 (SCM)
        Tree->>SCM: umem_zalloc() 分配 SCM 空间
        Tree->>SCM: value 写入 SCM blob
        Tree->>SCM: ir_ex_addr = {off, DAOS_MEDIA_SCM}
    else 大值 (NVMe)
        IO->>NVMe: vea_reserve(size, hint)
        NVMe-->>IO: 返回 bio_addr_t (NVMe 偏移)
        IO->>NVMe: BIO DMA 写入
        Tree->>SCM: ir_ex_addr = {off, DAOS_MEDIA_NVME}
    end

    Tree->>SCM: vos_ilog_update(kr_ilog, epoch)
    Note over SCM: 记录版本信息到 incarnation log

    Note over SCM: PMDK 事务提交<br/>所有 PMDK 修改原子生效

    IO->>Pool: vos_space_release(scm_size, nvme_size)
    Note over Pool: 释放预留空间

    IO-->>DAOS: 更新完成
    DAOS-->>C: RPC 响应 (成功)
```

---

## 9. 读流程（Disk → Client）

```mermaid
sequenceDiagram
    participant C as Client
    participant DAOS as DAOS Server
    participant VOS as VOS Engine
    participant IO as vos_fetch_begin
    participant Pool as vos_pool
    member Obj as vos_object(cache)
    member Tree as Btree(dkey/akey)
    member ILOG as ILOG
    member SCM as PMDK(SCM)
    member NVMe as SPDK(NVMe)

    C->>DAOS: daos_obj_fetch(dkey, akey, epoch)

    DAOS->>IO: vos_fetch_begin(ioc)
    IO->>Obj: vos_obj_acquire(unit_oid)

    IO->>Tree: dkey_fetch(ioc)
    Tree->>SCM: 查找 dkey Btree → vos_krec_df

    alt Single Value (akey)
        IO->>Tree: akey_fetch(ioc)
        Tree->>SCM: 查找 SV Btree → vos_irec_df

        IO->>ILOG: ilog_check_visibility(kr_ilog, epoch)
        Note over ILOG: 检查 epoch 是否在可见窗口内

        alt ir_ex_addr.ba_type == SCM
            Tree->>SCM: PMDK load → 直接读取
            SCM-->>Tree: value 数据
        else ir_ex_addr.ba_type == NVME
            Tree->>NVMe: BIO DMA 读取
            NVMe-->>Tree: value 数据
        end
    else Array Value (akey)
        IO->>Tree: akey_fetch(ioc)
        Tree->>SCM: 查找 EVTree → vos_krec_df

        IO->>ILOG: ilog_check_visibility(kr_ilog, epoch)
        Tree->>Tree: evt_query(filter_rect, epoch)

        Note over Tree: EVTree 返回所有相交的 [offset,len]×epoch 条目

        Tree->>SCM: 按可见性合并条目
        loop 每个 extent
            alt extent 地址在 NVMe
                Tree->>NVMe: BIO DMA 读取
                NVMe-->>Tree: extent 数据
            else extent 地址在 SCM
                Tree->>SCM: PMDK load
                SCM-->>Tree: extent 数据
            end
        end

        Note over IO: 组装 scatter-gather response
    end

    IO-->>DAOS: 数据组装完成
    DAOS-->>C: RPC 响应 (value + sgl)
```

---

## 10. 版本控制与 ILOG

### 10.1 Epoch-based MVCC

```
时间线 ───────►

epoch e1:  write(dkey="k1", akey="a1", value="v1")
epoch e2:  write(dkey="k1", akey="a1", value="v2")  ← v1 仍可读
epoch e3:  read(dkey="k1", akey="a1", epoch=e1) → 返回 "v1" (旧版本可见)
epoch e4:  punch(dkey="k1", akey="a1")                ← 逻辑删除
epoch e5:  read(dkey="k1", akey="a1", epoch=e3) → 返回 "v2" (punch 前)
epoch e6:  read(dkey="k1", akey="a1", epoch=e5) → 不存在 (已 punch)
```

### 10.2 Incarnation Log 结构

```
ilog_df (嵌入在 vos_obj_df, vos_krec_df 中)
  │
  └── 线性日志条目:
       ├── entry 1: epoch=e1, tx_id=X, status=COMMITTED
       ├── entry 2: epoch=e3, tx_id=Y, status=COMMITTED  ← update
       ├── entry 3: epoch=e4, tx_id=Z, status=COMMITTED  ← punch
       └── ...
```

### 10.3 ILOG 可见性检查

```c
// vos/ilog.h
ilog_check_visibility(ilog_df, epoch):
  1. 找到 epoch 对应的 ilog entry
  2. 检查 entry 状态:
     - COMMITTED → 可见
     - UNCOMMITTED → 检查 DTX 是否最终提交
     - REMOVED (punch) → 不可见
     - 不存在 → 检查 create epoch 是否 ≤ 请求 epoch
```

---

## 11. 分布式事务（DTX）

### 11.1 DTX 链表

```
Container (vos_cont_df)
  │
  ├── cd_dtx_active_head ──► vos_dtx_blob_df (活跃事务)
  │                              ├── dae_xid: DTX 标识
  │                              ├── dae_oid: 修改的对象
  │                              ├── dae_epoch: 事务 epoch
  │                              └── dae_rec_inline[4]: DTX 记录
  │                              │
  │                              └── next ──► vos_dtx_blob_df ...
  │
  └── cd_dtx_committed_head ──► vos_dtx_blob_df (已提交事务)
                                 └── 下一个提交的事务...
```

### 11.2 DTX 生命周期

```
1. vos_dtx_begin()  →  创建 DTX, 分配 dtx_id
2. vos_dtx_attach() → 将修改记录关联到 DTX
3. vos_dtx_commit() → 标记为 COMMITTED
4. vos_dtx_abort()  → 标记为 ABORTED, 回滚修改
5. GC drain          → 清理已提交/已中止的 DTX blob
```

---

## 12. NVMe 空间管理（VEA）

### 12.1 VEA 架构

```c
// include/daos_srv/vea.h:85-98
struct vea_space_df {
    uint32_t        vsd_blk_sz;       // 4KB 默认
    uint64_t        vsd_tot_blks;     // 设备容量
    struct btr_root vsd_free_tree;    // 空闲区间 Btree (按偏移排序)
    struct btr_root vsd_bitmap_tree;  // 空闲位图 Btree
};
```

### 12.2 分配策略

```
每个容器维护 2 个 I/O 流水线:
  ├── hint_df[0]: 通用 I/O → 顺序分配
  └── hint_df[1]: 聚合 I/O → 顺序分配

分配请求:
  vea_reserve(size, hint)
    │
    ▼
  从 hint.vhd_off 开始查找连续空闲区间
    │
    ├── 找到合适区间 → 分配, 更新 hint
    │
    └── 未找到 → 扩展到新的空闲区域
```

**顺序分配优势**：同一 I/O 流水线的写入在 NVMe 上物理连续，最大化写带宽。

---

## 13. 垃圾回收（GC）

### 13.1 分层 GC 结构

```
GC_MAX   (Pool 级)
  └── GC_CONT  (Container 级)
       └── GC_OBJ   (Object 级)
            └── GC_DKEY  (dkey 级)
                 └── GC_AKEY  (akey 级)
```

### 13.2 GC FIFO Bag

```
vos_gc_bin_df
  │
  ├── bin_bag_first ──► vos_gc_bag_df (最旧)
  │                      ├── bag_item_first
  │                      ├── bag_item[0]: {it_addr, it_bkt_ids}
  │                      ├── bag_item[1]: {it_addr, it_bkt_ids}
  │                      └── ...
  │
  ├── ... (中间的 bags)
  │
  └── bin_bag_last ──► vos_gc_bag_df (最新)
                         └── bag_item[0..N]
```

- 每层 GC bin 使用 FIFO bag 队列
- 每行最多 `VOS_GC_DEFAULT_CAP` (250 + 3×256) 个条目
- Credit-based 节流防止 GC 占用过多资源

---

## 14. 聚合（Aggregation）

### 14.1 EVTree 合并

```
聚合前 EVTree:
  entry_1: [0, 4K) @ e1 → NVMe offset 100
  entry_2: [4K, 4K) @ e1 → NVMe offset 200
  entry_3: [0, 8K) @ e2 → NVMe offset 300  ← 覆盖 entry_1+2
  entry_4: [0, 4K) @ e3 → NVMe offset 400  ← 覆盖 entry_3
  entry_5: [8K, 4K) @ e3 → NVMe offset 500

聚合后 EVTree:
  entry_new: [0, 12K) @ e3 → NVMe offset 400+500 合并
                 ↳ 读取 entry_5 数据, 写入连续区域

释放: entry_1, entry_2, entry_3, entry_4 的 NVMe 空间
```

### 14.2 触发条件

- EVTree 中的可见逻辑条目累积超过 `VOS_MW_FLUSH_THRESH`（8MB）
- 新物理条目的 NVMe 跨度超过 `VOS_MW_NVME_THRESH`（256 blocks = 1MB）

---

## 15. 空间预留与碎片化

### 15.1 双层预留机制

```c
// vos_pool 中
vp_space_sys[DAOS_MEDIA_SCM]  // 系统预留 (GC, rebuild, 聚合)
vp_space_held[DAOS_MEDIA_SCM] // 运行中预留 (in-flight I/O)

vos_space_hold():
  1. 检查可用空间
  2. 碎片化预留: SCM 容量的 5%, 限制在 600MB ~ 6GB
  3. 系统预留: VP_SYS_SCM, VP_SYS_NVME (配置)
  4. 更新 vp_space_held
```

### 15.2 碎片化处理

```
可用空间 = 总空间 - 系统预留 - 已使用 - held - 碎片化预留

碎片化预留 = min(max(SCM总容量 × 5%, 600MB), 6GB)
```

预留的碎片化空间在空间紧张时释放，防止因碎片化导致的分配失败。

---

## 16. 数据放置与冗余

### 16.1 Object Class 与冗余

```c
// include/daos/object.h:27-52
daos_obj_id_t 的高 32 位编码 Object Class:
  - OC_RP (Replication): SC=1, RC=N (e.g., SC=1, RC=4 → 1 个条带, 4 个副本)
  - OC_EC (Erasure Code): SC=M, PC=N (e.g., SC=4, PC=2 → 4+2)
  - OC_EC_RH (EC Reed-Solomon Hybrid): 类似 EC 但使用 RS 混合编码
```

### 16.2 Placement 策略

```
Pool Map 编码存储拓扑:
  Rack → Cage → Node → Socket → Target

Placement 原则:
  1. 同一对象的冗余条带分布在不同故障域
  2. 避免同一 Node 的两个 Target 存放同一副本
  3. dkey 的 co-location 在相同 Target
  4. EC 编码的条带跨 Target 分散
```

---

## 17. 与其他存储系统对比

| 特性 | DAOS | Ceph (BlueStore) | CDS | RocksDB |
|------|------|-----------------|-----|---------|
| 存储介质 | SCM + NVMe | HDD/SSD | SSD | SSD |
| 元数据存储 | PMDK (SCM, 零拷贝) | RocksDB (WAL) | Raft Log | LSM-Tree |
| 大对象存储 | NVMe (SPDK DMA) | BlueStore blobs | BlockServer | SST files |
| 键值模型 | dkey → akey → value (二层) | Object (flat) | Volume (线性块) | Key → Value |
| 版本控制 | Epoch + ILOG (非破坏写入) | Snap/Clone | COW | MVCC (seq) |
| 数组支持 | EVTree (R-tree 变体) | 不支持 | 不支持 | 不支持 |
| 事务支持 | DTX (分布式事务) | OSD 事务 | Raft | WriteBatch |
| 空间管理 | VEA (顺序分配) | Freelist Allocator | Log offset | Compaction |
| 垃圾回收 | 分层 GC bins | BlueFS GC | COW + Rebuild | Compaction |
| 聚合 | EVTree merge window | 无 | 无 | Compaction |
| 数据布局 | 随机 (Btree 直接寻址) | 随机 (Allocator) | Stripe (哈希分布) | 顺序 (SSTable) |
| 访问延迟 | SCM ~100ns, NVMe ~10μs | ~ms | ~ms | ~ms |
| 一致性 | Epoch MVCC | Raft Paxos | Raft Paxos | 单机 ACID |

---

## 18. 源码索引

### 核心头文件

| 文件 | 核心内容 |
|------|----------|
| `src/vos/vos_layout.h` | 所有 PMEM 持久化数据结构定义 |
| `src/vos/vos_internal.h` | 内存中 Pool/Container 结构、常量、Btree 类枚举 |
| `src/include/daos_srv/bio.h` | `bio_addr_t` 统一地址、BIO Blob Header |
| `src/include/daos_srv/vea.h` | VEA 空间分配器接口、`vea_space_df`、`vea_hint_df` |
| `src/include/daos_srv/evtree.h` | EVTree 接口、`evt_rect`、`evt_desc`、`evt_filter` |
| `src/include/daos/btree.h` | Btree 核心：`btr_root`、`btr_node`、`btr_record` |
| `src/include/daos/object.h` | `daos_obj_id_t`、`daos_unit_oid_t`、Object Class 定义 |

### 核心实现文件

| 文件 | 行号 | 核心函数 |
|------|------|----------|
| `src/vos/vos_pool.c` | — | Pool 创建、PMDK/SPDK 初始化 |
| `src/vos/vos_container.c` | 71 | `vos_cont_df_rec_alloc()` — 容器分配 |
| `src/vos/vos_obj.c` | — | 对象级 fetch/update/punch/iterate |
| `src/vos/vos_obj.h` | 34-70 | `vos_obj_hold()` / `vos_obj_release()` 对象缓存 |
| `src/vos/vos_obj_index.c` | 84-100 | `oi_rec_alloc()` — 对象索引记录分配 |
| `src/vos/vos_io.c` | 1665 | `vos_fetch_begin()` — 读入口 |
| `src/vos/vos_io.c` | 2729 | `vos_update_begin()` — 写入口 |
| `src/vos/vos_tree.c` | 799-827 | Btree 属性注册 (`vos_btr_attrs[]`) |
| `src/vos/vos_tree.c` | — | `key_tree_prepare()` / `key_ilog_prepare()` |
| `src/vos/vos_gc.c` | — | 分层 GC drain、credit 节流 |
| `src/vos/vos_aggregate.c` | — | EVTree merge window、NVMe 合并 |
| `src/vos/vos_space.c` | 12-84 | `vos_space_hold()` / `vos_space_release()` |
| `src/vos/evtree.c` | 103-115 | EVTree 策略注册、实现 |
| `src/vos/ilog.h` | 1-367 | ILOG 数据结构、可见性检查 |
| `src/vos/vos_ilog.h` | 52-75 | `vos_ilog_info` 缓存信息 |
| `docs/overview/storage.md` | 1-228 | DAOS 存储模型官方文档 |
