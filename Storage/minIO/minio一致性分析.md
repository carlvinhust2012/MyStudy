# MinIO 数据一致性机制分析

## 1. 概述

MinIO 通过**多层次机制**保证数据一致性，从单磁盘原子操作到跨磁盘 Quorum 协议。

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    MinIO 数据一致性保障层次                                │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  第 1 层: 分布式锁 (dsync)                                               │
│  ├── 防止并发写入同一对象                                                │
│  └── Quorum Lock: 多数节点持有锁才生效                                   │
│                                                                          │
│  第 2 层: Quorum 写入                                                    │
│  ├── 多数磁盘写入成功才认为提交                                          │
│  └── writeQuorum = dataBlocks (+1 if data==parity)                      │
│                                                                          │
│  第 3 层: 原子文件操作                                                   │
│  ├── 写入临时文件 + rename (崩溃安全)                                    │
│  └── 单磁盘级别的原子性                                                  │
│                                                                          │
│  第 4 层: Quorum 读取                                                    │
│  ├── 多数磁盘版本一致才认为有效                                          │
│  └── readQuorum = dataBlocks                                             │
│                                                                          │
│  第 5 层: 版本签名 + 合并                                                │
│  ├── xxhash 跨盘版本比较                                                 │
│  └── mergeXLV2Versions 多路归并                                          │
│                                                                          │
│  第 6 层: 写入回滚                                                       │
│  ├── Quorum 失败时回滚已写入磁盘                                         │
│  └── UndoWrite 机制                                                      │
│                                                                          │
│  第 7 层: 后台治愈                                                       │
│  ├── 检测不一致的磁盘                                                    │
│  └── 从 Quorum 修复数据                                                  │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 2. 并发控制: 分布式锁

### 2.1 为什么需要锁？

```
┌─────────────────────────────────────────────────────────────────────────┐
│                     并发写入冲突场景                                       │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  无锁场景:                                                               │
│                                                                          │
│  Client A: PUT object-X (100MB) ──────────────────────────▶            │
│       │                                                                  │
│       │ 同时                                                             │
│       ▼                                                                  │
│  Client B: DELETE object-X ────────▶                                    │
│                                                                          │
│  磁盘上的结果可能:                                                       │
│  ├── Disk 0: 有 A 的数据 (写入晚)                                       │
│  ├── Disk 1: 被 B 删除 (删除晚)                                         │
│  ├── Disk 2: 有 A 的数据                                                │
│  └── Disk 3: 被 B 删除                                                  │
│                                                                          │
│  → 数据不一致!                                                           │
│                                                                          │
│  加锁后:                                                                 │
│  ├── Client A 先获取锁 → 执行写入 → 释放锁                              │
│  ├── Client B 等待锁 → A 释放后执行删除                                 │
│  └── 操作串行化 → 数据一致                                              │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

### 2.2 dsync 分布式锁时序图

```mermaid
sequenceDiagram
    participant A as Client A (PUT)
    participant B as Client B (DELETE)
    participant N1 as Node 1
    participant N2 as Node 2
    participant N3 as Node 3
    participant N4 as Node 4

    Note over A,B: 两个客户端同时操作同一对象

    A->>N1: 1a. Lock(object-X) ✅
    A->>N2: 1b. Lock(object-X) ✅
    A->>N3: 1c. Lock(object-X) ✅
    A->>N4: 1d. Lock(object-X) ❌ 超时

    Note over A: 3/4 成功 ≥ Quorum → 获取锁

    B->>N1: 2a. Lock(object-X) ❌ (已被A持有)
    B->>N2: 2b. Lock(object-X) ❌
    B->>N3: 2c. Lock(object-X) ❌
    B->>N4: 2d. Lock(object-X) ❌

    Note over B: 0/4 成功 < Quorum → 等待重试

    Note over A: 执行写入操作

    A->>N1: 3. Unlock(object-X)
    A->>N2: 3. Unlock(object-X)
    A->>N3: 3. Unlock(object-X)

    Note over B: 重试获取锁

    B->>N1: 4a. Lock(object-X) ✅
    B->>N2: 4b. Lock(object-X) ✅
    B->>N3: 4c. Lock(object-X) ✅
    B->>N4: 4d. Lock(object-X) ✅

    Note over B: 4/4 成功 → 获取锁 → 执行删除
```

### 2.3 锁的粒度

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        锁的粒度                                           │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  锁对象: (bucket, object) 二元组                                        │
│                                                                          │
│  ┌──────────────────────────────────────────────────────────────────┐  │
│  │                                                                  │  │
│  │   bucket-A/object-1 → 锁 A (独立)                                │  │
│  │   bucket-A/object-2 → 锁 B (独立)                                │  │
│  │   bucket-B/object-1 → 锁 C (独立)                                │  │
│  │                                                                  │  │
│  │   不同对象的操作可以完全并行                                      │  │
│  │   相同对象的操作串行化                                            │  │
│  │                                                                  │  │
│  └──────────────────────────────────────────────────────────────────┘  │
│                                                                          │
│  锁类型:                                                                 │
│  ├── 写锁 (Write Lock): 互斥, PUT/DELETE 时获取                         │
│  └── 读锁 (Read Lock):  共享, GET 时获取                                │
│                                                                          │
│  锁超时:                                                                 │
│  ├── 默认: globalOperationTimeout (通常 30s)                            │
│  ├── 动态调整: 根据成功率自适应                                          │
│  └── 防止死锁: 超时后自动释放                                           │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 3. Quorum 写入机制

### 3.1 Quorum 计算

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        Quorum 计算规则                                     │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  假设: 16 盘集群, 8 Data + 8 Parity                                     │
│                                                                          │
│  dataBlocks   = 8                                                       │
│  parityBlocks = 8                                                       │
│                                                                          │
│  写 Quorum:                                                               │
│  ├── data == parity → writeQuorum = dataBlocks + 1 = 9                  │
│  └── data != parity → writeQuorum = dataBlocks                          │
│                                                                          │
│  读 Quorum:                                                               │
│  └── readQuorum = dataBlocks = 8                                        │
│                                                                          │
│  示例配置:                                                                │
│  ┌────────────┬─────┬────┬──────────┬──────────┬────────────┐           │
│  │ 配置       │ D   │ P  │ WriteQ   │ ReadQ    │ 容错        │           │
│  ├────────────┼─────┼────┼──────────┼──────────┼────────────┤           │
│  │ 4D + 2P    │ 4   │ 2  │ 4        │ 4        │ 2 盘故障   │           │
│  │ 8D + 4P    │ 8   │ 4  │ 8        │ 8        │ 4 盘故障   │           │
│  │ 8D + 8P    │ 8   │ 8  │ 9        │ 8        │ 8 盘故障   │           │
│  │ 12D + 4P   │ 12  │ 4  │ 12       │ 12       │ 4 盘故障   │           │
│  └────────────┴─────┴────┴──────────┴──────────┴────────────┘           │
│                                                                          │
│  关键: writeQuorum + readQuorum > N (写入和读取必须有交集)              │
│  → 保证读取一定能看到最新已提交的写入                                    │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

### 3.2 为什么 writeQuorum + readQuorum > N？

```
┌─────────────────────────────────────────────────────────────────────────┐
│           为什么 writeQuorum + readQuorum > N?                           │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  这是 Quorum 系统的基本不变式                                            │
│                                                                          │
│  场景: 8D + 8P, N=16                                                    │
│  ├── writeQuorum = 9                                                    │
│  ├── readQuorum = 8                                                     │
│  └── 9 + 8 = 17 > 16 ✅                                                 │
│                                                                          │
│  含义:                                                                   │
│  ├── 写入时至少 9 盘有新数据                                            │
│  ├── 读取时至少读 8 盘                                                  │
│  └── 9 + 8 - 16 = 1 → 至少有 1 盘既有新数据又被读到                    │
│                                                                          │
│  反例: 如果不满足此条件                                                  │
│  ┌───────────────────────────────────────────────────────┐              │
│  │  假设 writeQuorum = 8, readQuorum = 8                  │              │
│  │  8 + 8 = 16 = N  (不大于 N!)                          │              │
│  │                                                        │              │
│  │  写入: 写入磁盘 0-7                                   │              │
│  │  读取: 读取磁盘 8-15  ← 完全没读到新数据!              │              │
│  │  → 读到旧数据 → 不一致!                               │              │
│  └───────────────────────────────────────────────────────┘              │
│                                                                          │
│  所以当 data == parity 时, writeQuorum = data + 1                       │
│  确保 writeQuorum + readQuorum > N                                      │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

### 3.3 写入提交流程

```mermaid
sequenceDiagram
    participant Client as 客户端
    participant EO as erasureObjects
    participant Lock as 分布式锁
    participant D0 as Disk 0~7
    participant D8 as Disk 8~12
    participant D13 as Disk 13~15

    Client->>EO: PUT object

    EO->>Lock: 1. 获取写锁

    Note over EO: 数据写入临时目录 (.minio.sys/tmp/)

    par 2. 并行写入元数据 (renameData)
        EO->>D0: RenameData → 写入 part.1 + xl.meta
        D0-->>EO: ✅ Signature + OldDataDir

        EO->>D8: RenameData → 写入 part.1 + xl.meta
        D8-->>EO: ✅ Signature + OldDataDir

        EO->>D13: RenameData → 写入 part.1 + xl.meta
        D13-->>EO: ❌ 磁盘离线
    end

    EO->>EO: 3. 统计成功数量
    Note over EO: 成功 13 盘, writeQuorum = 9

    alt 成功数 >= writeQuorum (13 >= 9 ✅)
        EO->>EO: 4a. reduceCommonVersions
        Note over EO: 验证签名一致性

        EO->>EO: 4b. 提交成功
        EO->>Lock: 4c. 释放锁
        EO-->>Client: 4d. 200 OK

        Note over EO: 后台: 磁盘13-15加入治愈队列
    else 成功数 < writeQuorum
        EO->>EO: 5a. Quorum 失败
        EO->>D0: 5b. UndoWrite
        EO->>D8: 5b. UndoWrite
        Note over EO: 回滚所有已写入磁盘
        EO->>Lock: 5c. 释放锁
        EO-->>Client: 5d. 503 Quorum 失败
    end
```

---

## 4. 单磁盘原子操作

### 4.1 崩溃安全写入

```
┌─────────────────────────────────────────────────────────────────────────┐
│                  单磁盘原子写入 (崩溃安全)                                 │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  问题: 写入 xl.meta 时崩溃怎么办?                                        │
│                                                                          │
│  错误方式 (直接覆写):                                                    │
│  ┌───────────────────────────────────────────────────────┐              │
│  │  open(xl.meta, O_TRUNC | O_WRONLY)                     │              │
│  │  write(xl.meta, new_data)   ← 崩溃在这中间             │              │
│  │  close()                                               │              │
│  │                                                        │              │
│  │  结果: xl.meta 半新半旧 → 损坏!                        │              │
│  └───────────────────────────────────────────────────────┘              │
│                                                                          │
│  正确方式 (临时文件 + rename):                                           │
│  ┌───────────────────────────────────────────────────────┐              │
│  │  1. write(xl.meta.tmp, new_data)   ← 写临时文件        │              │
│  │  2. fsync(xl.meta.tmp)             ← 刷盘              │              │
│  │  3. rename("xl.meta.tmp", "xl.meta") ← 原子替换        │              │
│  │                                                        │              │
│  │  崩溃在任何时候都安全:                                  │              │
│  │  ├── 崩溃在步骤1前: xl.meta 保持旧版本 (未修改)        │              │
│  │  ├── 崩溃在步骤1中: xl.meta 保持旧版本 + 残留 tmp     │              │
│  │  ├── 崩溃在步骤2后: xl.meta.tmp 完整 → 下次 rename    │              │
│  │  └── 崩溃在步骤3后: xl.meta 已更新为新版本 (成功)     │              │
│  └───────────────────────────────────────────────────────┘              │
│                                                                          │
│  rename() 的原子性保证:                                                  │
│  ├── POSIX 文件系统保证 rename 是原子的                                  │
│  ├── 要么旧文件存在, 要么新文件存在                                      │
│  └── 不会出现两个都不存在或都损坏                                        │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

### 4.2 RenameData 操作详解

```
┌─────────────────────────────────────────────────────────────────────────┐
│                  disk.RenameData() 完整操作流程                           │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  输入:                                                                   │
│  ├── srcBucket: .minio.sys/tmp                                          │
│  ├── srcEntry:  {upload-uuid}/{DataDir}                                 │
│  ├── dstBucket: {bucket}                                                │
│  ├── dstEntry:  {object}                                                │
│  └── FileInfo:   新版本元数据                                           │
│                                                                          │
│  操作步骤:                                                                │
│                                                                          │
│  Step 1: 读取目标 xl.meta (如存在)                                      │
│  ├── 路径: {bucket}/{object}/xl.meta                                    │
│  ├── 存在 → 加载版本列表, 保留 LegacyType 条目                          │
│  └── 不存在 → 新建空列表                                                │
│                                                                          │
│  Step 2: 合并版本                                                        │
│  ├── 检查 VersionID 是否已存在                                          │
│  ├── 添加新版本到列表头部                                                │
│  └── 按时间排序 (最新在前)                                                │
│                                                                          │
│  Step 3: 移动数据 (原子)                                                 │
│  ├── src: .minio.sys/tmp/{uuid}/{DataDir}/part.1                        │
│  ├── dst: {bucket}/{object}/{DataDir}/part.1                            │
│  ├── rename(src, dst) → 原子操作                                        │
│  └── 如果 dst 目录不存在, 先创建                                        │
│                                                                          │
│  Step 4: 写入 xl.meta (原子)                                            │
│  ├── 序列化版本列表 (MessagePack)                                        │
│  ├── 写入临时文件 + fsync                                                │
│  ├── 计算 CRC32 并附加                                                  │
│  ├── rename("xl.meta.tmp", "xl.meta")                                  │
│  └── 原子替换                                                            │
│                                                                          │
│  Step 5: 返回                                                            │
│  ├── Signature: 新版本签名 (用于跨盘验证)                                │
│  └── OldDataDir: 旧数据目录 (用于 GC 清理)                              │
│                                                                          │
│  崩溃恢复:                                                                │
│  ├── 临时文件残留 → 启动时清理 .minio.sys/tmp/                          │
│  ├── 数据已移动但 xl.meta 未更新 → 治愈修复                              │
│  └── 整体保证: Quorum 机制兜底                                           │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 5. Quorum 读取与版本合并

### 5.1 读取一致性时序图

```mermaid
sequenceDiagram
    participant Client as 客户端
    participant EO as erasureObjects
    participant D0 as Disk 0~4 (新版本)
    participant D5 as Disk 5~9 (旧版本)
    participant D10 as Disk 10~15 (离线)

    Client->>EO: GET object

    Note over EO: writeQuorum=9, readQuorum=8

    par 1. 并行读取 xl.meta
        EO->>D0: ReadXL() → xl.meta (V2 新版本)
        D0-->>EO: ✅ 5 个磁盘返回 V2

        EO->>D5: ReadXL() → xl.meta (V1 旧版本)
        D5-->>EO: ✅ 5 个磁盘返回 V1

        EO->>D10: ReadXL()
        D10-->>EO: ❌ 6 个磁盘离线
    end

    Note over EO: 成功读取 10 盘 >= readQuorum(8)

    EO->>EO: 2. 解析版本
    Note over EO: 5盘: [V2, V1]
    Note over EO: 5盘: [V1]

    EO->>EO: 3. mergeXLV2Versions
    Note over EO: V2: 5盘有 (>= quorum)
    Note over EO: V1: 10盘有 (>= quorum)
    Note over EO: 合并: [V2, V1]

    EO->>EO: 4. pickValidFileInfo
    Note over EO: 选择最新版本 V2
    Note over EO: SHA-256 验证结构

    EO->>EO: 5. objectQuorumFromMeta
    Note over EO: readQuorum=8, writeQuorum=9

    Note over EO: 读取 V2 的数据分片

    par 6. 读取数据
        EO->>D0: 读取 V2 分片 ✅
        EO->>D5: 读取 V2 分片 ❌ (只有V1)
    end

    Note over EO: 5个数据分片 < readQuorum(8)

    EO->>EO: 7. 需要从更多磁盘读取
    EO->>D10: 尝试读取离线盘的替代分片

    Note over EO: 解码: 8个分片恢复数据

    EO-->>Client: 8. 返回 V2 数据

    Note over EO: 9. 后台治愈: Disk 5~9 需要 V2 数据
```

### 5.2 版本冲突解决

```
┌─────────────────────────────────────────────────────────────────────────┐
│                     版本冲突解决机制                                       │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  场景: 网络分区导致部分磁盘有新版本, 部分有旧版本                        │
│                                                                          │
│  磁盘状态:                                                               │
│  ├── Disk 0-6:   xl.meta 包含 V2 (t=200), V1 (t=100)                   │
│  ├── Disk 7-12:  xl.meta 只有 V1 (t=100)                                │
│  └── Disk 13-15: 离线                                                    │
│                                                                          │
│  读取时 mergeXLV2Versions:                                               │
│                                                                          │
│  Round 1: 比较各盘最新版本                                               │
│  ├── Disk 0-6:  V2 (Sig=0xABCD, t=200)                                  │
│  ├── Disk 7-12: V1 (Sig=0x1234, t=100)                                  │
│  │                                                                       │
│  ├── V2 出现 7 次 (7/13 可用磁盘)                                       │
│  ├── V1 出现 6 次 (6/13)                                                │
│  └── readQuorum = 8                                                     │
│                                                                          │
│  问题: V2 只有 7 次 < readQuorum(8)                                     │
│  → V2 不满足 Quorum!                                                     │
│                                                                          │
│  两种结果:                                                                │
│                                                                          │
│  结果 A (strict=false, 非严格模式):                                      │
│  ├── V2 虽然不满足 Quorum                                                │
│  ├── 但如果按 VersionID+Type 匹配                                       │
│  ├── 7次 > N/2 → 可能接受 (取决于实现)                                   │
│  └── 标记为需要治愈                                                     │
│                                                                          │
│  结果 B (strict=true, 严格模式):                                         │
│  ├── V2 不满足 Quorum → 丢弃                                             │
│  ├── V1 满足 Quorum (6次, 但加上Disk0-6也有V1 = 13次)                   │
│  └── 返回 V1 (旧版本)                                                   │
│                                                                          │
│  后台治愈:                                                                │
│  ├── Disk 7-12 缺少 V2 → 从 Disk 0-6 复制                               │
│  └── 恢复后所有磁盘一致                                                  │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 6. 写入回滚机制

### 6.1 回滚时序图

```mermaid
sequenceDiagram
    participant Client as 客户端
    participant EO as erasureObjects
    participant D0 as Disk 0~8 (成功)
    participant D9 as Disk 9~15 (失败)

    Client->>EO: PUT object

    Note over EO: writeQuorum = 9

    par 并行写入
        EO->>D0: RenameData ✅ (9盘成功)
        D0-->>EO: Signature 返回

        EO->>D9: RenameData ❌ (7盘失败)
        Note over D9: 磁盘故障/网络分区
    end

    EO->>EO: 成功 9 盘 >= writeQuorum 9
    Note over EO: Quorum 通过! (刚好满足)

    EO->>EO: reduceCommonVersions
    Note over EO: 9 个签名一致 → 验证通过

    EO-->>Client: 返回成功

    Note over D9: 7 盘没有新数据

    Note over EO: 后台治愈: 修复 Disk 9~15

    Note over EO,Client: === 对比: Quorum 失败的场景 ===

    Client->>EO: PUT object2

    par 并行写入
        EO->>D0: RenameData ✅ (8盘成功)
        EO->>D9: RenameData ❌ (8盘失败)
    end

    EO->>EO: 成功 8 盘 < writeQuorum 9
    Note over EO: Quorum 失败!

    par 回滚
        EO->>D0: DeleteVersion(UndoWrite: true)
        Note over D0: 删除刚写入的版本
        Note over D0: 恢复到写入前状态
    end

    EO-->>Client: 503 Quorum 失败

    Note over EO: 保证: 不会出现部分提交
```

### 6.2 回滚与未回滚的对比

```
┌─────────────────────────────────────────────────────────────────────────┐
│                  回滚 vs 不回滚的影响                                      │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  场景: 覆盖写, 旧版本 V1 → 新版本 V2                                    │
│                                                                          │
│  ┌─────────────────────────────────────────────────────────────────┐    │
│  │  有回滚机制 (MinIO 实际行为):                                    │    │
│  │                                                                  │    │
│  │  Quorum 失败:                                                    │    │
│  │  ├── Disk 0-7: 有 V2 (已写入)                                   │    │
│  │  ├── Disk 8-15: 只有 V1                                         │    │
│  │  │                                                               │    │
│  │  回滚后:                                                         │    │
│  │  ├── Disk 0-7: 恢复为 V1 (UndoWrite 删除 V2)                   │    │
│  │  ├── Disk 8-15: 仍是 V1                                         │    │
│  │  └── 所有磁盘一致: V1                                            │    │
│  │                                                                  │    │
│  │  读取结果: V1 (一致)                                             │    │
│  │                                                                  │    │
│  └─────────────────────────────────────────────────────────────────┘    │
│                                                                          │
│  ┌─────────────────────────────────────────────────────────────────┐    │
│  │  无回滚机制 (假设的错误行为):                                    │    │
│  │                                                                  │    │
│  │  Quorum 失败:                                                    │    │
│  │  ├── Disk 0-7: 有 V2 (已写入, 未回滚)                           │    │
│  │  ├── Disk 8-15: 只有 V1                                         │    │
│  │  │                                                               │    │
│  │  下次读取:                                                       │    │
│  │  ├── V2 出现 8 次                                                │    │
│  │  ├── V1 出现 16 次                                               │    │
│  │  ├── V1 满足 Quorum → 返回 V1                                   │    │
│  │  └── 但 Disk 0-7 实际存储了 V2 的数据!                          │    │
│  │                                                                  │    │
│  │  → 数据不一致 (元数据说V1, 数据是V2)                             │    │
│  │                                                                  │    │
│  └─────────────────────────────────────────────────────────────────┘    │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 7. Bitrot 保护 (静默数据损坏)

### 7.1 Bitrot 检测

```
┌─────────────────────────────────────────────────────────────────────────┐
│                     Bitrot (静默数据损坏) 保护                             │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  问题: 磁盘上的数据可能被静默损坏                                        │
│  ├── 磁盘坏道                                                            │
│  ├── 宇宙射线                                                            │
│  ├── 控制器固件 Bug                                                      │
│  └── 数据位翻转 (bit flip)                                               │
│                                                                          │
│  这些损坏操作系统检测不到, 但数据已经损坏                                 │
│                                                                          │
│  MinIO 解决方案: HighwayHash 校验                                         │
│                                                                          │
│  写入时:                                                                 │
│  ┌───────────────────────────────────────────────────────┐              │
│  │  每个 Block 的每个分片:                                 │              │
│  │  ├── 计算原始分片数据的 HighwayHash                     │              │
│  │  ├── 将 Hash 存储在分片数据之后                        │              │
│  │  └── 连同分片一起写入磁盘                               │              │
│  │                                                        │              │
│  │  part.1 文件结构:                                      │              │
│  │  [Block0_Shard][Block0_Hash][Block1_Shard][Block1_Hash]│              │
│  │  ...                                                   │              │
│  └───────────────────────────────────────────────────────┘              │
│                                                                          │
│  读取时:                                                                 │
│  ┌───────────────────────────────────────────────────────┐              │
│  │  读取分片数据 + Hash                                    │              │
│  │  重新计算 HighwayHash                                   │              │
│  │  比较两个 Hash                                          │              │
│  │                                                        │              │
│  │  ├── 匹配 → 数据完整, 使用此分片                       │              │
│  │  └── 不匹配 → 数据损坏, 跳过此盘, 读其他盘             │              │
│  │                                                        │              │
│  │  只要有 readQuorum 个完好分片 → 可恢复数据             │              │
│  │  损坏的分片 → 后台治愈修复                              │              │
│  └───────────────────────────────────────────────────────┘              │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

### 7.2 Bitrot 验证时序图

```mermaid
sequenceDiagram
    participant EO as erasureObjects
    participant D0 as Disk 0
    participant D1 as Disk 1
    participant DN as Disk N

    Note over EO: 读取对象, 需要 8 个数据分片

    par 优先读取 8 个数据盘
        EO->>D0: ReadShard(part.1, block=0)
        D0->>D0: 读取数据 + Hash
        D0->>D0: 重新计算 HighwayHash
        D0-->>EO: ✅ Hash 匹配

        EO->>D1: ReadShard(part.1, block=0)
        D1->>D1: 读取数据 + Hash
        D1->>D1: 重新计算 HighwayHash
        D1-->>EO: ❌ Hash 不匹配 (Bitrot!)

        EO->>DN: ReadShard(part.1, block=0)
        DN-->>EO: ✅ Hash 匹配
    end

    Note over EO: Disk 0: ✅, Disk 1: ❌, ...
    Note over EO: 收集到 7 个有效分片 < readQuorum(8)

    EO->>EO: 需要读取校验盘

    par 读取校验盘
        EO->>D8: ReadShard(part.1, block=0)
        D8-->>EO: ✅ Hash 匹配 (Parity Shard)
    end

    Note over EO: 8 个有效分片 (7 Data + 1 Parity)

    EO->>EO: ReconstructData(8 shards)
    Note over EO: Reed-Solomon 解码重建

    EO-->>EO: 返回完整数据
    Note over EO: 触发治愈: 修复 Disk 1
```

---

## 8. 后台治愈机制

### 8.1 不一致检测

```
┌─────────────────────────────────────────────────────────────────────────┐
│                     不一致检测与治愈                                       │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  检测时机:                                                               │
│                                                                          │
│  1. 读取时检测 (即时治愈)                                                │
│  ├── 读取发现某些磁盘数据不一致                                          │
│  ├── 但仍能从 Quorum 读取                                                │
│  └── 加入 PartialOperation 治愈队列                                      │
│                                                                          │
│  2. MRF 队列 (Missing Resource Feedback)                                 │
│  ├── 写入时有磁盘离线                                                    │
│  ├── 对象加入 MRF 队列                                                   │
│  └── 磁盘恢复后从队列治愈                                                │
│                                                                          │
│  3. 后台扫描 (定期治愈)                                                  │
│  ├── Scanner 周期性扫描所有对象                                          │
│  ├── 检查 xl.meta 一致性                                                 │
│  └── 修复不一致的磁盘                                                    │
│                                                                          │
│  4. 磁盘上线触发                                                         │
│  ├── 磁盘恢复连接                                                        │
│  ├── 需要同步缺失数据                                                    │
│  └── 触发该磁盘的治愈                                                    │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

### 8.2 治愈时序图

```mermaid
sequenceDiagram
    participant Scan as 后台扫描器
    participant EO as erasureObjects
    participant D0 as Disk 0~6 (正常)
    participant D7 as Disk 7~9 (不一致)
    participant D10 as Disk 10~15 (正常)

    Scan->>EO: 1. 检查对象一致性

    par 2. 读取所有磁盘的 xl.meta
        EO->>D0: ReadXL() → V2 ✅
        EO->>D7: ReadXL() → V1 ❌ (版本不一致!)
        EO->>D10: ReadXL() → V2 ✅
    end

    EO->>EO: 3. mergeXLV2Versions
    Note over EO: V2: 13盘 → Quorum 有效
    Note over EO: V1: 3盘 → Quorum 不足, 丢弃

    EO->>EO: 4. 识别需要治愈的磁盘
    Note over EO: Disk 7~9: 缺少 V2

    par 5. 从正常盘读取数据
        EO->>D0: ReadShard(D0 的 V2 分片) ✅
        EO->>D10: ReadShard(D10 的 V2 分片) ✅
    end

    EO->>EO: 6. 重建缺失分片
    Note over EO: Reed-Solomon 重新编码
    Note over EO: 为 Disk 7~9 生成对应的分片

    par 7. 写入修复数据
        EO->>D7: RenameData(V2 数据 + xl.meta)
        D7-->>EO: ✅ 修复成功
        EO->>D8: RenameData(V2 数据 + xl.meta)
        D8-->>EO: ✅ 修复成功
        EO->>D9: RenameData(V2 数据 + xl.meta)
        D9-->>EO: ✅ 修复成功
    end

    Scan->>Scan: 8. 记录治愈结果
    Note over Scan: 所有磁盘现在一致: V2
```

---

## 9. 端到端一致性场景分析

### 9.1 场景 1: 正常写入

```mermaid
sequenceDiagram
    participant C as 客户端
    participant API as S3 API
    participant EO as erasureObjects
    participant Lock as dsync
    participant Disks as 16 个磁盘

    C->>API: PUT object (100MB)
    API->>EO: PutObject()

    EO->>Lock: 1. 获取写锁 (Quorum)

    Note over EO: 2. 接收数据流
    Note over EO: 按 1 MiB Block 编码
    Note over EO: 写入临时目录

    Note over Disks: 3. 所有 16 盘写入成功

    par 4. 原子提交
        EO->>Disks: renameData (16 盘并行)
    end

    Note over Disks: 16/16 成功 >= writeQuorum(9)

    EO->>EO: 5. 签名验证一致
    EO->>Lock: 6. 释放锁
    EO-->>API: 7. 成功
    API-->>C: 8. 200 OK

    Note over C: 数据一致 ✅
```

### 9.2 场景 2: 写入时部分磁盘故障

```mermaid
sequenceDiagram
    participant C as 客户端
    participant EO as erasureObjects
    participant Lock as dsync
    participant OK as Disk 0~12 (正常)
    participant Bad as Disk 13~15 (故障)

    C->>EO: PUT object

    EO->>Lock: 获取写锁

    Note over EO: 数据写入临时目录

    par 并行提交
        EO->>OK: renameData ✅ (13盘)
        EO->>Bad: renameData ❌ (3盘故障)
    end

    EO->>EO: 13 成功 >= writeQuorum(9) ✅

    Note over EO: 动态升级 parity
    Note over EO: 原本 8D+8P, 现在 5D+11P
    Note over EO: 记录: minIOErasureUpgraded: "8->11"

    EO-->>C: 200 OK

    Note over Bad: 3 盘缺少数据

    EO->>EO: 加入 MRF 治愈队列
    Note over EO: Disk 13~15 恢复后治愈
```

### 9.3 场景 3: 写入时 Quorum 失败

```mermaid
sequenceDiagram
    participant C as 客户端
    participant EO as erasureObjects
    participant Lock as dsync
    participant OK as Disk 0~7 (成功)
    participant Bad as Disk 8~15 (失败)

    C->>EO: PUT object

    EO->>Lock: 获取写锁

    par 并行提交
        EO->>OK: renameData ✅ (8盘)
        EO->>Bad: renameData ❌ (8盘故障)
    end

    EO->>EO: 8 成功 < writeQuorum(9) ❌

    Note over EO: Quorum 失败! 必须回滚

    par 回滚已写入磁盘
        EO->>OK: DeleteVersion(UndoWrite=true)
        Note over OK: 删除新写入的版本
        Note over OK: 恢复到操作前状态
    end

    EO->>Lock: 释放锁
    EO-->>C: 503 Quorum 失败

    Note over C: 客户端重试或报错
    Note over EO: 数据一致: 所有磁盘无残留
```

### 9.4 场景 4: 读取时磁盘损坏

```mermaid
sequenceDiagram
    participant C as 客户端
    participant EO as erasureObjects
    participant Good as Disk 0~9 (正常)
    participant Rot as Disk 10 (Bitrot)
    participant Off as Disk 11~15 (离线)

    C->>EO: GET object

    EO->>EO: 获取读锁

    par 读取 xl.meta
        EO->>Good: ReadXL() ✅
        EO->>Rot: ReadXL() ✅ (元数据完好)
        EO->>Off: ReadXL() ❌
    end

    EO->>EO: 版本合并 → V2 有效

    par 读取数据分片 (Block 0)
        EO->>Good: ReadShard + Bitrot 检查
        Good-->>EO: 8个数据分片 ✅

        EO->>Rot: ReadShard + Bitrot 检查
        Rot-->>EO: ❌ Hash 不匹配 (Bitrot!)
    end

    Note over EO: 8 个有效数据分片 = readQuorum ✅

    EO->>EO: 解码数据 (无需校验分片)

    EO-->>C: 返回完整数据

    Note over EO: 触发治愈: 修复 Disk 10
    Note over EO: MRF: 恢复 Disk 11~15
```

---

## 10. 一致性保证总结

### 10.1 各层级机制总结

```
┌─────────────────────────────────────────────────────────────────────────┐
│                  一致性保证机制全景                                        │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │                    并发隔离层                                     │   │
│  │                                                                   │   │
│  │   dsync 分布式锁                                                  │   │
│  │   ├── 同一对象操作串行化                                          │   │
│  │   └── Quorum Lock (多数派)                                        │   │
│  │                                                                   │   │
│  └──────────────────────────────┬──────────────────────────────────┘   │
│                                  │                                       │
│  ┌──────────────────────────────▼──────────────────────────────────┐   │
│  │                    写入原子层                                     │   │
│  │                                                                   │   │
│  │   临时文件 + rename (单磁盘原子)                                  │   │
│  │   ├── 崩溃安全 (crash-safe)                                       │   │
│  │   └── 不会出现半写入状态                                          │   │
│  │                                                                   │   │
│  └──────────────────────────────┬──────────────────────────────────┘   │
│                                  │                                       │
│  ┌──────────────────────────────▼──────────────────────────────────┐   │
│  │                    Quorum 协议层                                  │   │
│  │                                                                   │   │
│  │   writeQuorum + readQuorum > N                                   │   │
│  │   ├── 写入: 多数成功才提交                                        │   │
│  │   ├── 读取: 多数一致才有效                                        │   │
│  │   └── 保证读写有交集                                              │   │
│  │                                                                   │   │
│  └──────────────────────────────┬──────────────────────────────────┘   │
│                                  │                                       │
│  ┌──────────────────────────────▼──────────────────────────────────┐   │
│  │                    版本一致性层                                   │   │
│  │                                                                   │   │
│  │   版本签名 (xxhash) + 多路归并                                    │   │
│  │   ├── 跨盘版本比较                                                │   │
│  │   ├── 冲突版本过滤 (不满足 Quorum 的丢弃)                         │   │
│  │   └── 确定性的版本排序 (sortsBefore)                              │   │
│  │                                                                   │   │
│  └──────────────────────────────┬──────────────────────────────────┘   │
│                                  │                                       │
│  ┌──────────────────────────────▼──────────────────────────────────┐   │
│  │                    回滚机制层                                     │   │
│  │                                                                   │   │
│  │   UndoWrite                                                      │   │
│  │   ├── Quorum 失败时回滚已写入磁盘                                │   │
│  │   └── 保证不会出现部分提交                                        │   │
│  │                                                                   │   │
│  └──────────────────────────────┬──────────────────────────────────┘   │
│                                  │                                       │
│  ┌──────────────────────────────▼──────────────────────────────────┐   │
│  │                    数据完整性层                                   │   │
│  │                                                                   │   │
│  │   Bitrot (HighwayHash)                                           │   │
│  │   ├── 写入时附加校验                                              │   │
│  │   ├── 读取时验证校验                                              │   │
│  │   └── 损坏分片自动跳过                                            │   │
│  │                                                                   │   │
│  └──────────────────────────────┬──────────────────────────────────┘   │
│                                  │                                       │
│  ┌──────────────────────────────▼──────────────────────────────────┐   │
│  │                    自愈合层                                       │   │
│  │                                                                   │   │
│  │   MRF + 后台扫描 + 治愈                                          │   │
│  │   ├── 检测不一致磁盘                                              │   │
│  │   ├── 从 Quorum 修复数据                                          │   │
│  │   └── 最终一致性恢复                                              │   │
│  │                                                                   │   │
│  └─────────────────────────────────────────────────────────────────┘   │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

### 10.2 CAP 理论下的 MinIO

| CAP 维度 | MinIO 选择 | 说明 |
|----------|-----------|------|
| **C (一致性)** | ✅ 强一致 | Quorum 写入保证 |
| **A (可用性)** | ⚠️ 条件可用 | 需要 Quorum 个磁盘在线 |
| **P (分区容忍)** | ✅ 支持 | 网络分区时少数派不可写 |

```
可用性分析:
├── 16盘集群, 8D+8P, writeQuorum=9
│   ├── 最多 7 盘故障 → 仍可读写
│   ├── 8 盘故障 → 可读不可写 (readQuorum=8, writeQuorum=9)
│   └── 9+ 盘故障 → 不可读写
│
└── MinIO 选择 CP 系统: 一致性优先于可用性
```

---

## 11. 总结

| 问题 | 答案 |
|------|------|
| **如何防止并发冲突** | dsync 分布式锁 (Quorum Lock) |
| **如何保证写入原子** | 临时文件 + rename (崩溃安全) |
| **如何保证多数一致** | Quorum 协议 (writeQ + readQ > N) |
| **如何比较跨盘版本** | xxhash 版本签名 |
| **如何解决版本冲突** | mergeXLV2Versions 多路归并 |
| **写入失败怎么办** | UndoWrite 回滚 |
| **如何检测数据损坏** | HighwayHash Bitrot 校验 |
| **如何修复不一致** | 后台治愈 (MRF + Scanner) |
| **CAP 定位** | CP 系统 (强一致性) |
| **最终一致性** | 治愈机制保证最终所有磁盘一致 |

---
