# CephFS 存储引擎：从文件到磁盘落盘

> 基于 CephFS 源码分析，解释文件如何映射为 RADOS 对象，再通过 BlueStore 落盘的全过程。

---

## 1. RADOS 是什么

**RADOS** = **R**eliable **A**utonomic **D**istributed **O**bject **S**tore

- **Reliable** — 可靠的（副本 + 数据校验 + 自愈）
- **Autonomic** — 自治的（OSD 自动发现、故障自动转移、数据自动重平衡）
- **Distributed** — 分布式的（无中心、CRUSH 算法直接定位）
- **Object Store** — 对象存储

它是 Ceph 的底层存储引擎，CephFS、RBD（块存储）、RGW（对象网关）都构建在 RADOS 之上。

---

## 2. 文件如何映射为 RADOS 对象

CephFS 的核心就是将文件映射为 RADOS 对象，存储在 OSD 上。

### 2.1 条带化映射（Striper）

通过 `Striper::file_to_extents()`（`src/osdc/Striper.cc:182`）完成，使用 `ceph_file_layout` 中的三个参数：

| 参数 | 默认值 | 含义 |
|------|--------|------|
| `stripe_unit` | 4MB | 条带单元大小 |
| `stripe_count` | 1 | 每轮循环的条带数 |
| `object_size` | 4MB | 单个 RADOS 对象大小 |

一个 16MB 文件默认映射为 4 个 RADOS 对象，命名格式为 `<inode号>.<对象序号>`。

### 2.2 完整映射链路

```
文件 "bigfile" (inode=100, 16MB)
  │
  ▼ Striper::file_to_extents()
  ├── 100.00000000  (offset 0~4MB)
  ├── 100.00000001  (offset 4MB~8MB)
  ├── 100.00000002  (offset 8MB~12MB)
  └── 100.00000003  (offset 12MB~16MB)
  │
  ▼ CRUSH 算法
  每个对象 → hash → PG → OSD Set (如 [osd.1, osd.3, osd.7])
  │
  ▼ BlueStore (各 OSD 本地)
  RocksDB (元数据/onode) + BlockDevice (数据)
```

### 2.3 与 JuiceFS 对比

| 维度 | CephFS | JuiceFS |
|------|--------|---------|
| 对象大小 | 默认 4MB（可配置） | 默认最大 64MB |
| 对象命名 | `inode.序号` | `volume/chunk-slice` |
| 写入方式 | 原地覆盖（支持 COW） | 只追加（Copy-on-Write） |
| 对象存储位置 | 本地 OSD（BlueStore） | 远程 S3/Ceph Object Gateway |
| 元数据 | MDS 内存缓存 + RADOS Journal | Redis/TiKV/SQL |

---

## 3. BlueStore 如何将对象落盘

每个 OSD 上收到的 RADOS 对象最终都通过 BlueStore 落盘。BlueStore 是 OSD 的存储引擎，负责对象的持久化。

### 3.1 存储布局

```
单个 OSD (一块物理盘)
┌──────────────────────────────────────┐
│            BlueStore                 │
│                                      │
│  ┌────────────────┐ ┌─────────────┐ │
│  │   RocksDB      │ │  BlockDev   │ │
│  │  (WAL + DB)    │ │  (数据盘)   │ │
│  │                │ │             │ │
│  │  - onode 元数据│ │  - 对象数据 │ │
│  │  - omap 键值对 │ │  - 大对象   │ │
│  │  - collections │ │  - BlueFS   │ │
│  └───────┬────────┘ └──────┬──────┘ │
│          │    BlueFS(WAL)   │        │
│          └────────┬─────────┘        │
└───────────────────┼──────────────────┘
                   │
                   ▼
              物理磁盘 (fsync)
```

- **RocksDB** — 存储 onode 元数据、omap 键值对、collection 索引
- **BlockDev** — 存储实际对象数据（大对象）
- **BlueFS** — 极简文件系统，管理 RocksDB 的 WAL 和 DB 文件，直接操作裸块设备

### 3.2 写入路径

```
对象写入请求到达 OSD
       │
       ▼
  PrimaryLogPG::execute_ctx()   — 分配 version，生成事务
       │
       ▼
  BlueStore::queue_transactions()
       │
       ├─ 小数据 → RocksDB (onode + omap)
       ├─ 大数据 → BlockDev (异步 AIO 写入)
       │
       ▼
  两阶段提交:
    1. bdev->flush()          — 等待 BlockDev 数据落盘
    2. RocksDB WAL sync       — 通过 BlueRocksEnv fsync
       │
       ▼
  事务完成 → 回复客户端 ONDISK
```

BlueStore 通常直接挂载裸盘，不需要 ext4/xfs，避免文件系统开销。

---

## 4. RocksDB WAL 为什么必须落盘

RocksDB WAL 必须落盘，这是 BlueStore 两阶段提交能保证崩溃一致性的关键。

### 4.1 不落盘会怎样

```
场景: 客户端写入 4MB 对象

  T1: BlueStore 将 4MB 数据写入 BlockDev → AIO 完成后数据在磁盘上
  T2: BlueStore 将 onode 元数据写入 RocksDB MemTable（内存）
  T3: RocksDB WAL 写入 BlueFS（内存缓冲）
  T4: 回复客户端 ONDISK  ← 如果这里崩了？

  崩溃后:
    BlockDev 上有 4MB 新数据 ✓
    但 RocksDB MemTable 丢失 ✗
    WAL 没落盘也丢失 ✗
    → onode 不指向新数据 → 对象"丢失" → 数据不一致
```

### 4.2 实际的两阶段提交

```
  T1: BlockDev AIO 完成
  T2: bdev->flush()          ← 确保数据在物理磁盘上
  T3: RocksDB WAL sync       ← 通过 BlueRocksEnv fsync 落盘
      │
      │  BlueRocksEnv::Sync() (BlueRocksEnv.cc:233)
      │    → BlueFS fsync() → fdatasync() → 磁盘
      │
  T4: WAL 落盘完成 → 回复客户端 ONDISK

  此时再崩溃:
    BlockDev 有数据 ✓
    RocksDB WAL 有记录 ✓
    重启后 RocksDB 从 WAL 恢复 MemTable → onode 指向新数据 ✓
    → 完全一致
```

### 4.3 关键点

WAL 落盘是 **fsync 级别**的保证，不只是写入 OS page cache。顺序是：

**BlockDev 数据先落盘 → RocksDB WAL 再落盘 → 才算写入完成。**

---

## 5. 每个 OSD 能挂多少块盘，容量有上限吗

### 5.1 一个 OSD 最多 3 块盘

BlueStore 的设计是 **1 个 OSD 绑定 1 块主盘**（`BlockDevice *bdev` 是单指针，`BlueStore.h:2352`），但可以额外挂 2 块加速盘：

```
单个 OSD 的 BlueStore 设备布局:

  ┌──────────────────────────────────────────────┐
  │              BlueStore (1个OSD)               │
  │                                              │
  │  block.wal (可选)  ← NVMe/SSD, 最快          │
  │    └── BlueFS WAL 日志                       │
  │                                              │
  │  block.db (可选)      ← SSD, 次快            │
  │    └── RocksDB (onode/omap) + BlueFS DB      │
  │                                              │
  │  block (必须)         ← HDD/SSD, 主数据盘     │
  │    └── 对象实际数据                           │
  │                                              │
  └──────────────────────────────────────────────┘
```

配置参数（`src/common/options/global.yaml.in`）：
- `bluestore_block_path` — 主数据盘（必须）
- `bluestore_block_db_path` — DB 盘（可选，放 RocksDB，默认 0）
- `bluestore_block_wal_path` — WAL 盘（可选，默认 96MB）

如果只有一块盘，WAL 和 RocksDB 都合并在主盘上。

设备枚举（`BlueFS.h:268-273`）：
```cpp
static constexpr unsigned BDEV_WAL = 0;   // WAL 设备
static constexpr unsigned BDEV_DB  = 1;   // DB 设备
static constexpr unsigned BDEV_SLOW = 2;  // 主数据设备
```

### 5.2 容量没有硬上限

OSD 启动时自动计算 CRUSH weight（`OSD.cc:4882`）：

```
weight = total_bytes / 1 TB

  例: 4TB 盘  → weight = 4.0
     16TB 盘 → weight = 16.0

  weight 越大 → CRUSH 分配的数据越多
  管理容量没有天花板，盘有多大就能用多大
```

### 5.3 使用率阈值（只读保护）

```
  0%                85%        90%      95%    97%
  ├──────────────────┼──────────┼────────┼──────┤
  │    正常           │ NEARFULL │BACKFILL│ FULL │FAILSAFE│
  │    正常读写       │ 警告     │FULL    │ 只读  │ 只读   │
  │                  │          │停止回填│      │ 安全阀 │
```

| 参数 | 默认值 | 行为 |
|------|--------|------|
| `mon_osd_nearfull_ratio` | 0.85 | 健康警告 |
| `mon_osd_backfillfull_ratio` | 0.90 | 停止数据回填 |
| `mon_osd_full_ratio` | 0.95 | 只读，拒绝写入 |
| `osd_failsafe_full_ratio` | 0.97 | 最终安全阀 |

达到 95% 后 OSD 拒绝一切写入，防止磁盘写满导致不可恢复。

### 5.4 其他限制

| 维度 | 软限制 | 硬限制 |
|------|--------|--------|
| PG 数/OSD | 250（`mon_max_pg_per_osd`，超过报警） | 750（`osd_max_pg_per_osd_hard_ratio * 250`，拒绝） |
| 自动调优目标 | 100（`mon_target_pg_per_osd`） | — |
| 单对象大小 | — | 128MB（`osd_max_object_size`） |
| 单对象名长度 | — | 2KB（`osd_max_object_name_len`） |
| 集群 OSD 总数 | — | 无编译期上限（受 Monitor 内存限制） |

### 5.5 总结

**一个 OSD = 一块主盘（最多加 2 块加速盘），容量无硬上限，通过多 OSD 横向扩展。** 扩容方式是加盘 → 加 OSD → CRUSH 自动重平衡数据。

---

## 6. librados 是独立进程吗

**不是，librados 是一个库（library），不是独立进程。** 它以动态链接库（`.so`）的形式加载到应用程序的进程空间中。

```
┌─────────────────────────────────────────────┐
│           应用程序进程                        │
│                                             │
│  ┌─────────┐                                │
│  │ App 代码 │                                │
│  └────┬────┘                                │
│       │ 调用客户端库 API                     │
│       ▼                                     │
│  ┌──────────┐  ┌──────────┐  ┌───────────┐  │
│  │ librados  │  │ librbd   │  │ libcephfs │  │
│  │ (lib库)   │  │ (lib库)  │  │ (lib库)   │  │
│  └─────┬────┘  └────┬─────┘  └─────┬─────┘  │
│        └────────────┼──────────────┘         │
│                     ▼                        │
│              网络通信 (TCP/RDMA)              │
└─────────────────────┼───────────────────────┘
                      │
                      ▼
         ┌────────────────────────┐
         │    Ceph 集群 (独立进程) │
         │                        │
         │  Monitor (ceph-mon)    │
         │  OSD      (ceph-osd)   │
         │  MDS      (ceph-mds)   │
         └────────────────────────┘
```

### 6.1 Ceph 客户端库的层次关系

| 库 | 功能 | 通信对象 |
|----|------|---------|
| **librados** | 底层 RADOS 客户端库，直接操作对象 | Monitor + OSD |
| **libcephfs** | CephFS 文件系统客户端，基于 librados | Monitor + MDS + OSD |
| **librbd** | RBD 块存储客户端，基于 librados | Monitor + OSD |

它们都是 `.so` 动态库，链接到应用进程内。例如 `ceph-fuse` 进程加载了 `libcephfs.so`，后者依赖 `librados.so`。

### 6.2 Ceph 集群侧的独立进程

Ceph 集群侧运行的是 **独立守护进程**，每个都是单独的操作系统进程：

| 守护进程 | 职责 | 是否独立进程 |
|---------|------|------------|
| `ceph-mon` | 集群状态管理、OSD Map、CRUSH Map | 是 |
| `ceph-osd` | 对象存储、副本管理、数据读写 | 是 |
| `ceph-mds` | CephFS 元数据管理 | 是 |
| `ceph-mgr` | 集群监控、插件管理 | 是 |

### 6.3 关键区别

```
客户端侧（库，共享进程）:
  librados.so / libcephfs.so / librbd.so
  → 嵌入到应用进程中，通过 TCP 与集群通信
  → 多个应用可以各自加载独立的客户端库实例

服务端侧（独立进程）:
  ceph-mon / ceph-osd / ceph-mds / ceph-mgr
  → 每个是独立的操作系统进程
  → 监听端口，接收客户端连接
```

---

## 7. 文件条带化逻辑在哪执行

**条带化逻辑完全在 CephFS 客户端上完成，OSD 端不感知文件概念。**

```
CephFS 客户端进程 (libcephfs.so + librados.so)
  │
  │  Client::_write() / Client::_read()
  │    → Filer::write_trunc() / Filer::read_trunc()
  │      → Striper::file_to_extents()    ← 条带化在这里！
  │        将 (文件offset, len) 映射为 ObjectExtent 列表
  │
  │  例: write(offset=10MB, len=8MB)
  │    → ObjectExtent { obj="100.00000002", off=2MB, len=4MB }
  │    → ObjectExtent { obj="100.00000003", off=0MB,  len=4MB }
  │
  │  Objecter::sg_write_trunc()  ← 直接发 OSD 操作
  │
  ▼ TCP
  ┌──────────────────────────────────────────┐
  │  OSD 端 — 只看到一个个独立对象操作        │
  │                                          │
  │  收到: WRITE "100.00000002" off=2MB      │
  │  收到: WRITE "100.00000003" off=0MB      │
  │                                          │
  │  OSD 不知道这些对象属于同一个文件          │
  │  OSD 也不知道文件条带化的概念              │
  │  OSD 只负责: 对象存储 + 副本复制           │
  └──────────────────────────────────────────┘
```

### 7.1 条带参数的来源

```
文件创建时:
  Client::_create() → 发送 CEPH_MDS_OP_CREATE 到 MDS
    → 可指定 stripe_unit, stripe_count, object_size (Client.cc:16012-16014)

  如果没指定:
    → MDS 分配默认 layout (stripe_unit=4MB, stripe_count=1, object_size=4MB)
    → 通过 cap grant 消息下发给客户端

  文件打开时:
    Client 拿到 inode 的 layout（存在 Inode::layout 中）
    → 后续所有 read/write 都用这个 layout 做 Striper 映射
```

### 7.2 条带化的好处

| 特性 | 说明 |
|------|------|
| 并行 I/O | 一个大文件的多个条带可以并行发往不同 OSD |
| 分散负载 | 避免单个 OSD 成为热点 |
| 客户端控制 | 客户端知道 layout 参数，可以按需调整 |

---

## 8. OSD 磁盘上存的是文件还是裸数据

**OSD 磁盘上存的是原始二进制数据块（blob），不是文件。** BlueStore 绕过操作系统文件系统，直接管理裸块设备。

### 8.1 BlueStore 对象存储结构

```
对象 "100.00000000" (inode=100, 第0个对象)
  │
  ├── onode (存在 RocksDB 中)
  │     ├── 对象大小、属性
  │     ├── extent map: [(blob_offset, disk_offset, length)]
  │     └── omap: 键值对
  │
  └── blob (存在 BlockDev 裸设备上)
        └── 原始二进制数据，如:
           00 01 02 03 ... (文件的实际字节内容)

  onode 通过 extent map 定位 blob 在磁盘上的物理偏移，
  类似于 inode 的 block pointer，但直接指向裸设备的偏移量。
```

### 8.2 磁盘上的数据分布

```
传统文件系统 (ext4/xfs):
  磁盘 → VFS → 文件系统 → 文件（ls 能看到）
  ├── /data/file1.txt
  ├── /data/file2.txt
  └── inode table + data blocks

BlueStore:
  磁盘 → BlueStore → 裸块设备上的数据区域（ls 看不到）
  ┌────────────────────────────────┐
  │ offset 0x000000: blob (对象A) │  ← 没有文件名
  │ offset 0x100000: blob (对象B) │  ← 没有目录结构
  │ offset 0x230000: blob (对象C) │  ← 连续的二进制块
  │ ...                           │
  └────────────────────────────────┘
  元数据由 RocksDB 的 onode 索引
```

### 8.3 与传统文件系统的区别

| 维度 | 传统文件系统 (ext4/xfs) | BlueStore |
|------|----------------------|-----------|
| 磁盘管理 | VFS → 文件系统 → 块设备 | BlueStore → 直接操作块设备 |
| 最小管理单元 | 文件 | RADOS 对象 |
| 元数据位置 | inode table（磁盘固定区域） | RocksDB |
| 数据定位 | block pointer | extent map（onode 中） |
| 能 `ls` 看到吗 | 能 | 不能（裸盘） |
| 能 `mount` 浏览吗 | 能 | 不能 |

### 8.4 总结

**BlueStore 绕过操作系统文件系统，直接管理裸盘上的二进制块。对象数据就是一段 buf，元数据由 RocksDB 的 onode 索引，通过 extent map 关联数据在磁盘上的物理位置。** 这也是 BlueStore 性能优于 FileStore（旧版，基于 ext4/xfs）的根本原因——减少了一层文件系统开销。
