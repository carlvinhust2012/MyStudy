# Lustre 数据一致性保障机制深度分析

## 1. 一致性保障全景

Lustre 从多个层面保障数据一致性：

```
┌─────────────────────────────────────────────────────────────────┐
│                   一致性保障层次                                 │
│                                                                 │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │ Layer 1: 传输一致性                                          │ │
│  │ CRC/Adler/T10 校验 + PTLRPC 重试 + RPC 幂等重放           │ │
│  ├─────────────────────────────────────────────────────────────┤ │
│  │ Layer 2: 锁一致性                                           │ │
│  │ LDLM 分布式锁 + Intent Lock + AST 回调 + LVB              │ │
│  ├─────────────────────────────────────────────────────────────┤ │
│  │ Layer 3: 元数据一致性                                       │ │
│  │ ldiskfs jbd2 事务 + 跨 MDT 两阶段提交 + FID 原子性         │
│  ├─────────────────────────────────────────────────────────────┤ │
│  │ Layer 4: 数据一致性                                         │
│  │ OSC 脏页缓存 + Grant 空间预约 + fsync/close 持久化        │ │
│  ├─────────────────────────────────────────────────────────────┤ │
│  │ Layer 5: 冗余一致性                                         │
│  │ FLR 镜像同步 + EC 纠删码 + LFSCK 自修复                   │
│  ├─────────────────────────────────────────────────────────────┤ │
│  │ Layer 6: 恢复一致性                                         │
│  │ last_rcvd 重放 + Changelog 审计 + Orphan 跟踪              │ │
│  └─────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
```

---

## 2. Layer 1: 传输一致性

### 2.1 校验和机制

Lustre 对所有 RPC 和数据传输使用校验和保护：

```c
// lustre/include/obd_cksum.h
enum cksum_types {
    OBD_CKSUM_ADLER,       // 默认: Adler-32
    OBD_CKSUM_CRC32,       // CRC-32
    OBD_CKSUM_CRC32C,      // CRC-32C (硬件加速)
    OBD_CKSUM_T10IP512,    // T10 DIF IP-512
    OBD_CKSUM_T10IP4K,     // T10 DIF IP-4K
    OBD_CKSUM_T10CRC512,   // T10 DIF CRC-512
    OBD_CKSUM_T10CRC4K,    // T10 DIF CRC-4K
};
```

校验覆盖范围：
- **RPC 头部**：ptlrpc 消息头自带校验
- **BRW 数据页**：每个 bulk transfer 页面独立校验
- **OSC 缓存页**：缓存页数据写入时计算并存储校验值

### 2.2 RPC 幂等重放（故障恢复一致性）

当客户端重连 MDT 后，可能重发已提交的 RPC。MDT 通过以下机制保证幂等：

```mermaid
sequenceDiagram
    participant Client as Client
    participant MDT as MDT (故障恢复后)
    participant LAST_RCVD as last_rcvd 文件
    participant REPLY as reply_data 文件

    Note over Client,MDT: 正常运行阶段

    Client->>MDT: RPC (xid=10043, CREATE file)
    MDT->>MDT: 执行操作 + 提交事务
    MDT->>REPLY: 缓存 reply_data (xid=10043, transno, result=0)
    MDT->>LAST_RCVD: 更新 last_xid=10043

    Note over MDT: MDT 故障重启

    Client->>MDT: 重连 + 重发 RPC (xid=10043)
    MDT->>LAST_RCVD: 读取 last_xid=10043
    Note over MDT: 发现 xid=10043 <= last_xid, 已提交

    MDT->>REPLY: 读取 reply_data (xid=10043)
    REPLY-->>MDT: 返回缓存的原始回复
    MDT-->>Client: 返回原始结果 (不重复执行)
    Note over Client: 幂等性保证: 操作不会重复执行
```

**VBR（Version-Based Replay）**：每个对象维护版本号。重放时对比版本号，不匹配则拒绝重放并重建客户端缓存。

### 2.3 PTLRPC 重试机制

```mermaid
sequenceDiagram
    participant Client
    participant PTLRPC as PTLRPC 层
    participant MDT as MDT

    Client->>PTLRPC: 发送请求 (xid, timeout)
    PTLRPC->>MDT: 转发 RPC

    alt 正常返回
        MDT-->>PTLRPC: Response
        PTLRPC-->>Client: 返回结果
    else 超时无响应
        PTLRPC->>PTLRPC: 等待超时
        PTLRPC->>MDT: 连接断开
        Note over PTLRPC: 自动重试 (最大次数)
        PTLRPC->>MDT: 重新建立连接
        PTLRPC->>MDT: 重发请求 (同一 xid)
        MDT-->>PTLRPC: 返回结果
        PTLRPC-->>Client: 返回结果
    else 服务端返回 EAGAIN
        MDT-->>PTLRPC: EAGAIN (服务端繁忙)
        PTLRPC->>PTLRPC: 指数退避后重试
        PTLRPC->>MDT: 重发请求
        MDT-->>PTLRPC: 返回结果
    end
```

---

## 3. Layer 2: 锁一致性（LDLM）

### 3.1 LDLM 分布式锁概述

```c
// lustre/include/lustre_dlm.h:14-23
// 基于 VAX DLM，两大职责:
// 1. 提供锁机制保证所有 Lustre 节点间数据一致性
// 2. 允许客户端通过持锁来缓存受锁保护的状态
```

### 3.2 Intent Lock（意图锁）— 元数据一致性核心

Intent Lock 是 Lustre 最重要的锁一致性机制：**将锁获取和元数据操作原子化**。客户端发送 Intent 请求时，MDT 先处理锁请求，在同一个 RPC 中返回锁 + 操作结果。

| Intent 类型 | 操作 | 锁模式 | 作用 |
|------------|------|--------|------|
| `IT_CREAT` | 创建文件 | INODE(PW) + IBITS | 创建前检查重名 |
| `IT_OPEN` | 打开文件 | INODE + EXTENT | 返回最新属性 |
| `IT_GETATTR` | 获取属性 | IBITS | 返回最新 size/mtime |
| `IT_LAYOUT` | 获取布局 | IBITS | 返回最新条带布局 |
| `IT_READDIR` | 读目录 | INODE(PR) | 返回目录内容 |
| `IT_GETXATTR` | 获取 xattr | IBITS | 返回扩展属性 |

### 3.3 Intent Lock 流程时序

```mermaid
sequenceDiagram
    participant Client as Client
    participant MDC as MDC (MDT Client)
    participant MDT as MDT
    participant LDLM as LDLM Server

    Client->>MDC: open("/file", O_RDWR)
    MDC->>MDT: MDS_REINT RPC<br/>携带 Intent: IT_OPEN

    Note over MDT: 步骤1: 处理 Intent Lock 请求

    MDT->>LDLM: 请求: INODE(PW) + IBITS(lock_size)
    LDLM->>LDLM: 检查是否有冲突锁

    alt 无冲突
        LDLM-->>MDT: 授予锁
    else 有其他客户端持有 PR 锁
        LDLM->>Client: 发送 Blocking AST (锁回撤请求)
        Client->>Client: 刷写脏页到 OST
        Client->>LDLM: 释放锁
        LDLM-->>MDT: 锁已释放
        LDLM-->>MDT: 授予 PW 锁
    end

    Note over MDT: 步骤2: 执行实际操作 (在锁保护下)

    MDT->>MDT: mdd_lookup() 查找文件
    MDT->>MDT: mdd_attr_get() 读取属性
    MDT->>MDT: mdd_la_get() 读取 SOM 缓存大小
    MDT->>LDLM: 将最新属性填入 LVB (lock value block)

    Note over MDT: 步骤3: 原子返回 锁 + 属性

    MDT-->>MDC: REINT_REPLY<br/>包含: 锁句柄 + FID + 最新属性 + LVB(size/mtime)
    MDC-->>Client: open 成功

    Note over Client: 客户端持有 PW 锁<br/>锁内缓存的属性保证一致性<br/>其他客户端无法写入
```

### 3.4 AST 回调（锁回收一致性）

当其他客户端需要访问已被锁保护的资源时，LDLM 通过 AST（Asynchronous Trap）回调通知当前客户端释放锁：

```mermaid
sequenceDiagram
    participant A as Client A<br/>(持有 PW 锁)
    participant LDLM as LDLM Server
    participant B as Client B<br/>(请求同一区域)

    B->>LDLM: 请求 EXTENT [0,1MB) PW
    LDLM->>LDLM: 发现与 A 的 PW 锁冲突
    LDLM->>A: 发送 Blocking AST
    Note over A: AST: 请释放锁 [0,1MB) 的 PW 锁

    A->>A: 检查 dirty pages
    alt 有脏页需要刷写
        A->>A: osc_flush() 将脏页刷写到 OST
    end
    A->>LDLM: 释放锁 [0,1MB)
    LDLM->>LDLM: 重新评估 B 的请求
    LDLM-->>B: 授予 PW 锁 [0,1MB)
    B->>B: 安全地读写该区域
```

### 3.5 LVB（Lock Value Block）— 属性缓存一致性

LVB 携带在锁中返回，保证客户端看到**锁授予时刻**的最新属性：

```c
// lustre/include/uapi/linux/lustre/lustre_idl.h:1563
struct ost_lvb {
    __u64 lvb_size;       // 文件大小 (bytes)
    __s64 lvb_mtime;      // 修改时间
    __s64 lvb_atime;      // 访问时间
    __s64 lvb_ctime;      // 变更时间
    __u64 lvb_blocks;      // 分配的 512B 块数
    __u32 lvb_mtime_ns;   // 纳秒精度
    __u32 lvb_atime_ns;
    __u32 lvb_ctime_ns;
};
```

**关键点**：LVB 值在锁授予时从服务端获取，客户端持有锁期间属性不会过期，从而保证 `stat()` 返回一致的结果。

---

## 4. Layer 3: 元数据一致性

### 4.1 单 MDT 事务（jbd2）

MDT 使用 ldiskfs 的 jbd2 日志系统保证元数据操作的原子性：

```mermaid
sequenceDiagram
    participant MDT as MDT (mdd)
    participant JBD2 as jbd2 (ldiskfs)
    participant DISK as 磁盘

    MDT->>MDT: mdd_trans_create()<br/>barrier_entry() 检查写入屏障

    Note over MDT: 声明阶段 (无需锁)

    MDT->>JBD2: mdd_declare_create()<br/>预分配日志空间
    MDT->JBD2: mdd_declare_index_insert()
    MDT->JBD2: mdd_declare_xattr_set()

    Note over MDT: 执行阶段 (持有锁)

    MDT->>JBD2: mdd_trans_start()<br/>开始 jbd2 事务

    MDT->>JBD2: dt_create() 创建 inode
    MDT->JBD2: dt_insert() 插入目录项
    MDT->JBD2: mdo_xattr_set("trusted.lma") 设置 FID
    MDT->JBD2: mdo_xattr_set("trusted.lov") 设置条带布局
    MDT->JBD2: mdo_xattr_set("trusted.link") 更新硬链接

    MDT->JBD2: mdd_trans_stop()<br/>jbd2: 原子提交所有变更

    JBD2->>DISK: journal commit<br/>所有操作原子写入或全部回滚

    MDT->>MDT: barrier_exit()<br/>释放写入屏障
```

### 4.2 跨 MDT 事务（两阶段提交）

DNE 环境下的跨 MDT 操作（如跨 MDT rename）使用两阶段提交：

```mermaid
sequenceDiagram
    participant Client as Client
    participant MDT_A as MDT_A (源目录)
    participant MDT_B as MDT_B (目标目录)

    Client->>MDT_A: rename("/dir_A/file", "/dir_B/file")

    Note over MDT_A,MDT_B: Phase 1: 准备阶段

    MDT_A->>MDT_A: mdd_trans_start()
    MDT_A->>MDT_A: dt_delete("dir_A/file")

    MDT_A->>MDT_B: CROSS_REF RPC (准备)
    Note over MDT_B: 在 B 上锁定目标并检查冲突
    MDT_B->>MDT_B: mdd_declare_insert("dir_B/file")
    MDT_B-->>MDT_A: 准备就绪 (无冲突)

    Note over MDT_A,MDT_B: Phase 2: 提交阶段

    MDT_A->>MDT_B: CROSS_REF_COMMIT RPC
    MDT_B->>MDT_B: mdd_trans_start()
    MDT_B->MDT_B: dt_insert("dir_B/file")
    MDT_B->>MDT_B: mdd_trans_stop() (本地提交)
    MDT_B-->>MDT_A: 提交成功

    MDT_A->>MDT_A: 更新 link EA
    MDT_A->>MDT_A: mdd_trans_stop() (本地提交)

    MDT_A-->>Client: rename 完成

    Note over MDT_A,MDT_B: 任一阶段失败 → 双方回滚<br/>保证原子性
```

### 4.3 FID 唯一性

FID 的唯一性由 seq 分配器保证：

- 每个 seq 范围只分配给一个 MDT
- 客户端从已分配的 seq 范围内本地分配 OID，不会冲突
- seq 范围通过 LAST_ID 文件 + jbd2 事务持久化
- FID 永不重用

---

## 5. Layer 4: 数据一致性

### 5.1 OSC 客户端缓存一致性

客户端的 OSC 层缓存脏页，通过 LDLM 锁机制保证缓存一致性：

```mermaid
sequenceDiagram
    participant App as 应用程序
    participant OSC as OSC<br/>(客户端缓存)
    participant LDLM as LDLM
    participant OST as OST

    App->>OSC: write(fd, buf, 1MB, offset=0)
    OSC->>OSC: 写入 Page Cache (dirty page)
    OSC->>OSC: osc_extent 记录脏页范围

    Note over OSC: 脏页暂不刷写，等 LDLM 通知

    LDLM->>OSC: Blocking AST: 释放 [0, 1MB) 锁
    OSC->>OSC: 检测到 AST 回调

    OSC->>OST: BRW_WRITE RPC (刷写脏页)
    OST->>OST: osd-ldiskfs: 写入磁盘
    OST-->>OSC: ACK (确认持久化)

    OSC->>LDLM: 释放锁
    Note over OSC: 脏页已刷写，锁安全释放
```

### 5.2 Grant 空间预约

Lustre 使用 Grant 机制预分配 OST 上的磁盘空间：

```mermaid
sequenceDiagram
    participant OSC as OSC
    participant OST as OST (ofd)

    OSC->>OST: OST_FIRST_RPC (初始连接)
    OST->>OST: ofd_grant_check()
    Note over OST: 检查可用空间

    OST-->>OSC: Grant: 可写入 N MB
    Note over OSC: 客户端缓存此 Grant<br/>写入时无需每次 RPC

    loop 后续写入 (在 Grant 范围内)
        App->>OSC: write()
        OSC->>OSC: 使用 Grant 空间，不请求新 Grant
    end

    OSC->>OST: Grant 即将用尽
    OSC->>OST: OST_SYNC RPC (刷写脏页 + 释放 Grant)
    OST->>OST: osd-ldiskfs: fsync
    OST-->>OSC: 新 Grant (N MB)
```

### 5.3 fsync/close 持久化保证

```mermaid
sequenceDiagram
    participant App
    participant OSC
    participant OST
    participant LDLM

    App->>OSC: fsync(fd) 或 close(fd)

    alt 有脏页
        OSC->>OST: BRW_WRITE (刷写所有脏页)
        OST->>OST: osd-ldiskfs: 数据落盘
    end

    OSC->>OST: OST_SYNC RPC
    OST->>OST: osd-ldiskfs: fsync()
    OST-->>OSC: SYNC ACK (已持久化)

    Note over OSC: 释放写锁，通知 MDT 更新 SOM

    OSC->>LDLM: 释放 EXTENT PW 锁
    LDLM->>LDLM: 检查是否有等待的 Blocking AST
    alt 有等待者
        LDLM->>LDLM: 授予等待者的锁
    end
```

---

## 6. Layer 5: 冗余一致性

### 6.1 FLR（File Level Redundancy）镜像

FLR 通过多副本镜像提供数据冗余：

```mermaid
sequenceDiagram
    participant Client
    participant LOV as LOV<br/>(条带聚合)
    participant OSC0 as OSC → Mirror 0
    participant OSC1 as OSC → Mirror 1
    participant OST0 as OST_0
    participant OST1 as OST_1

    Client->>LOV: write(fd, buf, 1MB, offset=0)
    LOV->>LOV: 选择主镜像 (LCME_FL_PREF_WR)

    par 写入两个镜像
        LOV->>OSC0: sub-io: 写 Mirror 0
        OSC0->>OST0: BRW_WRITE
    and
        LOV->>OSC1: sub-io: 写 Mirror 1
        OSC1->>OST1: BRW_WRITE
    end

    OSC0->>LOV: Mirror 0 写入完成
    OSC1->>LOV: Mirror 1 写入完成
    Note over LOV: 两个镜像都成功写入<br/>数据冗余保证

    Note over Client: 读取时选择 LCME_FL_PREF_RD 镜像
```

### 6.2 EC（Erasure Coding）纠删码

EC 通过 k+p 编码提供数据保护：

```
示例: 4+2 EC 配置 (k=4, p=2)
┌────────┬────────┬────────┬────────┬─────────┬─────────┐
│Data_0 │Data_1 │Data_2 │Data_3 │Parity_0│Parity_1 │
│OST_0  │OST_1  │OST_2  │OST_3  │OST_4   │OST_5   │
└────────┴────────┴────────┴────────┴─────────┴─────────┘
│←──── stripe_width = 6 × stripe_size ────→│

任意 2 个 OST 故障，数据仍可恢复
```

### 6.3 LFSCK 在线自修复

LFSCK（Lustre File System Check）在运行时检测和修复不一致：

| 检查类型 | 检测内容 | 修复操作 |
|----------|----------|----------|
| **Namespace LFSCK** | MDT inode 与 link EA 不一致 | 修复孤立 inode、重建 link EA |
| **Layout LFSCK** | LOV EA 与 OST 对象不匹配 | 修复 LOV EA hole、重建丢失的 OST 对象 |
| **DNE LFSCK** | LMV 条目与实际 MDT 不一致 | 修复分布式目录映射 |

---

## 7. Layer 6: 恢复一致性

### 7.1 MDT 故障恢复完整流程

```mermaid
sequenceDiagram
    participant Client as Client
    participant OLD as 旧 MDT<br/>(已崩溃)
    participant NEW as 新 MDT<br/>(故障转移)
    participant LAST_RCVD as last_rcvd
    participant REPLY as reply_data
    participant CLOG as Changelog

    Note over OLD: MDT 故障

    Client->>NEW: 重连请求<br/>(携带: client_gen, last_transno)
    NEW->>LAST_RCVD: 读取每个客户端的 last_transno

    loop 为每个未完成的 RPC 检查
        NEW->>REPLY: 读取 reply_data (xid, transno, result)
        alt transno > last_transno (未提交)
            NEW->>NEW: 丢弃 (操作未完成，无需重放)
        else transno <= last_transno (已提交)
            NEW->>NEW: 使用缓存回复 (幂等重放)
        end
    end

    NEW->>CLOG: 获取故障前的 changelog 记录
    NEW->>NEW: 重放 changelog 中尚未应用的条目

    NEW-->>Client: 恢复完成
    Note over Client: 未完成的操作由应用层重试
```

### 7.2 Orphan 机制

当文件被 unlink 但仍有客户端打开时，MDT 将其移入 PENDING 目录（orphan）：

```mermaid
sequenceDiagram
    participant Client as Client
    participant MDT as MDT

    Note over Client: 文件仍被打开 (fd 有效)

    Client->>MDT: unlink("/dir/file")
    MDT->>MDT: mdd_unlink()
    MDT->>MDT: dt_delete(目录项)
    MDT->>MDT: nlink == 0 但 mod_count > 0

    MDT->>MDT: mdd_orphan_insert()<br/>移入 PENDING 目录
    Note over MDT: 对象保留在 PENDING 目录中<br/>等待所有客户端 close

    MDT-->>Client: unlink 成功 (文件对象仍在磁盘上)

    Client->>MDT: close(fd)
    MDT->>MDT: mod_count == 0
    MDT->>MDT: mdd_orphan_destroy()<br/>从 PENDING 删除
    MDT->>MDT: dt_destroy() 释放对象
```

**崩溃恢复场景**：MDT 重启后扫描 PENDING 目录，对 `mod_count == 0` 的对象执行 `orphan_destroy()`。

---

## 8. 一致性保障策略总结

### 8.1 按操作类型的一致性保障

| 操作 | 锁机制 | 事务 | 持久化 | 冗余 |
|------|--------|------|--------|------|
| **create** | INODE(PW) + IBITS | jbd2 | sync | — |
| **open** | INODE + EXTENT + LVB | — | — | — |
| **read** | EXTENT(PR) | — | — | FLR/EC |
| **write** | EXTENT(PW) | — | fsync | FLR/EC |
| **stat** | IBITS + LVB | — | — | — |
| **unlink** | INODE(PW) + IBITS | jbd2 | sync | — |
| **rename** | INODE(PW) + PDO | jbd2 | sync | — |
| **setattr** | IBITS(UPDATE) | jbd2 | sync | — |
| **mkdir** | INODE(PW) + IBITS | jbd2 | sync | — |
| ** readdir** | INODE(PR) | — | — | — |

### 8.2 故障场景分析

| 故障场景 | 检测机制 | 恢复方式 |
|----------|----------|----------|
| **客户端崩溃** | MDT 检测断连 | 客户端重连，orphan 清理打开文件 |
| **MDT 崩溃** | OST 心跳超时 | last_rcvd + reply_data 重放 + changelog |
| **MDT 崩溃 + 数据丢失** | LFSCK 检测 | LFSCK 重建丢失的 MDT inode |
| **OST 崩溃** | OSS 心跳超时 | OST 重启后 OST 对象仍在 ldiskfs 中 |
| **OST 崩溃 + 数据丢失** | LFSCK Layout 检测 | LFSCK 修复 LOV EA hole |
| **网络分区** | PTLRPC 超时 | 锁超时释放 + 客户端重连重放 |
| **元数据损坏** | jbd2 journal replay | journal 回放修复 |
| **数据损坏** | T10/CRC 校验 | 检测到校验错误返回 EIO |

### 8.3 与其他系统的一致性机制对比

| 维度 | Lustre | 3FS | Doris |
|------|--------|-----|-------|
| **分布式锁** | LDLM (VAX DLM) | CoLockManager (per-chunk) | FE 全局锁 (无细粒度数据锁) |
| **事务** | jbd2 (本地) + 2PC (跨 MDT) | FDB SSI | FE BDB JE 两阶段提交 |
| **元数据持久化** | ldiskfs jbd2 journal | FDB 事务 | EditLog + Image 快照 |
| **数据持久化** | fsync + write barrier | Chain Replication 5步 ACK | BE 本地磁盘 + FE 路由 |
| **故障恢复** | RPC 重放 + Changelog | RPC 重试 + 幂等 (IDEM) | FE EditLog 回放 |
| **校验** | CRC32/Adler/T10 | MD5 per chunk | 无端到端校验 |
| **冗余** | FLR 镜像 + EC 纠删码 | Chain Replication (3副本) | Tablet 级 3 副本 |
| **自修复** | LFSCK 在线检查 | 无 | 无 (需人工介入) |

---

## 9. 关键源码索引

| 一致性机制 | 关键文件 | 核心函数/结构 |
|------------|----------|---------------|
| LDLM 锁 | `lustre/ldlm/ldlm_lock.c` | `ldlm_grant()`, `ldlm_enqueue()` |
| AST 回调 | `lustre/ldlm/ldlm_request.c` | `ldlm_blocking_ast()` |
| Intent Lock | `lustre/mdc/mdc_locks.c` | IT_OPEN, IT_CREAT, IT_GETATTR 处理 |
| jbd2 事务 | `lustre/mdd/mdd_trans.c` | `mdd_trans_create/start/stop()` |
| Orphan | `lustre/mdd/mdd_orphans.c` | `mdd_orphan_insert/delete/destroy()` |
| RPC 重放 | `lustre/mdt/mdt_recovery.c` | `mdt_req_from_lrd()`, VBR |
| SOM | `lustre/mdt/mdt_som.c` | `mdt_get_som()`, `mdt_lsom_update()` |
| 校验和 | `lustre/include/obd_cksum.h` | CRC32, CRC32C, T10 系列 |
| OSC 刷写 | `lustre/osc/osc_cache.c` | AST 回调触发 `osc_flush()` |
| LVB | `lustre/mdt/mdt_lvb.c` | `mdt_lvbo_fill()`, `mdt_dom_disk_lvbo_update()` |
| Changelog | `lustre/mdd/mdd_dir.c` | `mdd_changelog_store()`, `mdd_changelog_ns_store()` |
| LFSCK | `lustre/lfsck/` | Namespace, Layout, DNE 检查 |
| 写屏障 | `lustre/mdd/mdd_trans.c` | `barrier_entry()`, `barrier_exit()` |
| 跨 MDT | `lustre/mdt/mdt_handler.c` | CROSS_REF, CROSS_REF_COMMIT |
