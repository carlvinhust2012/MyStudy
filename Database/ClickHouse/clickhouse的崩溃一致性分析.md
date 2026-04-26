# ClickHouse 崩溃一致性机制深度解析

> 基于 ClickHouse 源码分析, 聚焦数据落盘前后的崩溃一致性保证: 原子 rename、checksums 校验、ZK 状态机兜底、启动恢复流程

## 一、核心设计: 无 WAL 的原子提交

ClickHouse MergeTree **没有传统 WAL (Write-Ahead Log)**，崩溃时内存中未落盘的数据会丢失，但磁盘上的数据不会被损坏。它通过以下机制保证一致性:

```mermaid
flowchart LR
    subgraph Traditional["传统数据库 (MySQL/PostgreSQL)"]
        TW1["写入 WAL (fsync)"] --> TW2["写入内存"] --> TW3["异步刷盘"]
        TW4["崩溃恢复: 回放 WAL"]
    end

    subgraph CH["ClickHouse MergeTree"]
        CW1["排序"] --> CW2["写 tmp_ 临时文件"] --> CW3["fsync"] --> CW4["原子 rename"] --> CW5["PreActive → Active"]
        CW6["崩溃恢复: 清理 tmp_ + checksums 校验"]
    end

    TW3 -.-> TW4
    CW5 -.-> CW6
```

## 二、正常写入的原子提交流程

```mermaid
sequenceDiagram
    participant Client as Client
    participant Sink as MergeTreeSink
    participant Writer as MergeTreeDataWriter
    participant Out as MergedBlockOutputStream
    participant Disk as 磁盘文件系统
    participant OS as 操作系统
    participant Parts as data_parts (内存)

    Client->>Sink: INSERT INTO table_A VALUES (...)
    Sink->>Writer: consume(Block)

    rect rgb(255, 245, 230)
        Note right of Writer: Step 1: 创建临时目录
        Writer->>Out: writeTempPart(block)
        Out->>Disk: mkdir tmp_insert_202504_1_1_0/
        Note over Disk: tmp_ 前缀: 其他线程看不到, 查询不可见
    end

    rect rgb(240, 255, 230)
        Note right of Out: Step 2: 排序 + 写列数据
        Out->>Out: stableGetPermutation(block, sort_desc)
        Out->>Disk: 写 a.bin, a.mrk2 (HashingWB → CompressedWB → WriteBufferFromFile)
        Out->>Disk: 写 b.bin, b.mrk2
        Out->>Disk: 写 c.bin, c.mrk2
        Note over Disk: 每个文件通过 HashingWriteBuffer 计算 CityHash128
    end

    rect rgb(245, 240, 255)
        Note right of Out: Step 3: 写元数据文件
        Out->>Disk: 写 primary.idx (稀疏主键索引)
        Out->>Disk: 写 columns.txt, count.txt
        Out->>Disk: 写 partition.dat, minmax_*.idx
        Out->>Disk: 写 checksums.txt (汇总所有文件的 hash + size)
        Note over Disk: checksums.txt 是完整性清单
    end

    rect rgb(255, 230, 245)
        Note right of Out: Step 4: fsync (取决于设置)
        Out->>OS: fsync 所有文件
        Note over OS: 确保数据从 OS 页缓存刷到磁盘
        Note over OS: fsync_part_directory=1 时执行
    end

    rect rgb(230, 255, 255)
        Note right of Writer: Step 5: 原子 rename
        Writer->>OS: rename("tmp_insert_202504_1_1_0", "202504_1_1_0")
        Note over OS: 操作系统保证原子性 (inode 级别)
        Note over OS: 不会出现"一半改名"的中间状态
        Note over Parts: rename 完成后目录才对其他线程可见
    end

    rect rgb(255, 255, 230)
        Note right of Writer: Step 6: 两阶段提交 (内存状态)
        Writer->>Parts: Phase 1: Part → PreActive
        Writer->>Parts: Phase 2: Part → Active
        Note over Parts: Active 后查询才能看到这个 Part
    end

    rect rgb(255, 240, 230)
        Note right of Writer: Step 7: ZK 提交 (ReplicatedMergeTree)
        Writer->>Writer: ZK multi() 原子操作
        Note over Writer: block_numbers + 1/log 新增 GET_PART/replicas/self/parts 注册
        Writer-->>Client: INSERT 成功
    end
```

## 三、崩溃场景逐一分析

### 场景总览

```mermaid
flowchart TD
    Crash["ClickHouse 进程崩溃"]

    Crash --> S1{"崩溃时在哪个阶段?"}

    S1 -->|"A: 写 tmp_ 文件中"| A["tmp_insert_xxx/ 不完整"]
    S1 -->|"B: 文件写完, 未 rename"| B["tmp_insert_xxx/ 完整"]
    S1 -->|"C: rename 后, 内存未更新"| C["202504_1_1_0/ 完整, 内存无记录"]
    S1 -->|"D: Active 后, ZK 未提交"| D["本地正常, ZK 无记录"]
    S1 -->|"E: ZK 已提交"| E["本地 + ZK 都正常"]

    A --> RA["启动恢复: 删除 tmp_数据丢失, 客户端重试"]
    B --> RB["启动恢复: 删除 tmp_数据丢失, 客户端重试"]
    C --> RC["启动恢复: checksums 校验通过重新加载到 Active数据保留"]
    D --> RD["启动恢复 (副本表): ZK 无记录清理本地 Part, 从其他副本拉取"]
    E --> RE["正常恢复, 数据完整保留"]

    style A fill:#FFCDD2
    style B fill:#FFCDD2
    style C fill:#C8E6C9
    style D fill:#FFF9C4
    style E fill:#C8E6C9
    style RA fill:#FFCDD2
    style RB fill:#FFCDD2
    style RC fill:#C8E6C9
    style RD fill:#FFF9C4
    style RE fill:#C8E6C9
```

### 场景 A: 写 tmp_ 文件中崩溃

```mermaid
sequenceDiagram
    participant Server as ClickHouse
    participant Disk as 磁盘

    Note over Server: 崩溃前状态
    Note over Disk: tmp_insert_202504_1_1_0/  a.bin (不完整, 只写了 50%)  a.mrk2 (不存在)  b.bin (不存在)  ...

    rect rgb(255, 245, 230)
        Note right of Server: 启动恢复
        Server->>Disk: 扫描数据目录
        Server->>Disk: 发现 tmp_insert_202504_1_1_0/
        Note over Server: tmp_ 前缀 = 未完成的写入
        Server->>Disk: 删除 tmp_insert_202504_1_1_0/
        Note over Server: 清理完毕, 不影响现有数据
    end
```

**结果**: 数据丢失, 客户端收到超时/连接错误, 可以重试 INSERT。

### 场景 B: 文件写完, 未 rename 崩溃

```mermaid
sequenceDiagram
    participant Server as ClickHouse
    participant Disk as 磁盘

    Note over Server: 崩溃前状态
    Note over Disk: tmp_insert_202504_1_1_0/  a.bin (完整)  a.mrk2 (完整)  b.bin (完整)  ...  checksums.txt (完整)202504_1_1_0/ 不存在 (rename 未执行)

    rect rgb(255, 245, 230)
        Note right of Server: 启动恢复
        Server->>Disk: 扫描数据目录
        Server->>Disk: 发现 tmp_insert_202504_1_1_0/
        Note over Server: 虽然文件完整, 但 tmp_ 前缀说明 rename 未完成
        Server->>Disk: 删除 tmp_insert_202504_1_1_0/
        Note over Server: 保守策略: 统一清理所有 tmp_
    end
```

**结果**: 数据丢失, 但磁盘上没有残留的脏数据。

### 场景 C: rename 后, 内存未更新崩溃

```mermaid
sequenceDiagram
    participant Server as ClickHouse
    participant Disk as 磁盘

    Note over Server: 崩溃前状态
    Note over Disk: 202504_1_1_0/  a.bin (完整)  a.mrk2 (完整)  ...  checksums.txt (完整)tmp_insert_* 不存在 (rename 已完成)但 data_parts 内存中没有此 Part 的记录

    rect rgb(255, 245, 230)
        Note right of Server: 启动恢复
        Server->>Disk: 扫描数据目录
        Server->>Disk: 发现 202504_1_1_0/ (无 tmp_ 前缀)
        Server->>Disk: 读取 checksums.txt
        Server->>Server: 逐文件校验 CityHash128
        Note over Server: a.bin: hash ✓a.mrk2: hash ✓primary.idx: hash ✓所有文件校验通过
        Server->>Server: 加载 Part 到 data_parts (Active)
        Note over Server: 数据恢复成功, 可查询
    end
```

**结果**: 数据保留, 启动后重新加载到内存。

### 场景 D: Active 后, ZK 未提交崩溃 (副本表)

```mermaid
sequenceDiagram
    participant Server as ClickHouse
    participant ZK as ZooKeeper/Keeper
    participant Other as 其他副本

    Note over Server: 崩溃前状态
    Note over Server: 本地 202504_1_1_0/ 完整, data_parts 有记录
    Note over ZK: /replicas/self/parts 中无 202504_1_1_0
    Note over ZK: /log 中无 GET_PART 条目

    rect rgb(255, 245, 230)
        Note right of Server: 启动恢复
        Server->>ZK: 读取 /replicas/self/parts
        ZK-->>Server: ZK 中注册的 Part 列表
        Note over Server: 对比本地 vs ZK:本地有 202504_1_1_0ZK 无 202504_1_1_0
        Server->>Server: 本地有但 ZK 无 → 孤立 Part
        Server->>Disk: 清理本地 202504_1_1_0/
        Note over Server: 如果是重复 INSERT 的数据, 其他副本可能已有如果只有这个副本有, 数据丢失 (INSERT 未达成 quorum)
    end
```

**结果**: ZK 是真相源, 本地与 ZK 不一致的 Part 被清理。

### 场景 E: ZK 已提交, 本地完整 (最安全)

```mermaid
sequenceDiagram
    participant Server as ClickHouse
    participant ZK as ZooKeeper/Keeper
    participant Other as 其他副本

    Note over Server: 崩溃前状态
    Note over Server: 本地 202504_1_1_0/ 完整, Active
    Note over ZK: /replicas/self/parts 有 202504_1_1_0
    Note over ZK: /log 有 GET_PART 条目

    rect rgb(255, 245, 230)
        Note right of Server: 启动恢复
        Server->>ZK: 读取 /replicas/self/parts
        ZK-->>Server: ZK 中有 202504_1_1_0
        Server->>Disk: checksums 校验通过
        Server->>Server: 正常加载 Part
        Note over Server: 数据完整保留
    end

    rect rgb(240, 255, 230)
        Note right of ZK: 其他副本同步
        Other->>ZK: watch /log, 发现 GET_PART 条目
        Other->>Other: 如果本地缺失, 从源副本 fetchPart
    end
```

**结果**: 数据完整保留, 其他副本从 ZK 发现并拉取。

## 四、启动恢复完整流程

```mermaid
sequenceDiagram
    participant Server as ClickHouse Server
    participant Disk as 磁盘
    participant Parts as data_parts
    participant ZK as ZooKeeper/Keeper
    participant Other as 其他副本

    rect rgb(255, 245, 230)
        Note right of Server: Phase 1: 扫描与清理
        Server->>Disk: 遍历数据目录
        Disk-->>Server: 目录列表
        Note over Server: 发现:202504_1_1_0/ (正常 Part)202504_2_2_0/ (正常 Part)202504_1_3_1/ (合并 Part)tmp_insert_202504_xxx/ (临时, 不完整)tmp_merge_202504_yyy/ (临时合并, 不完整)
        Server->>Disk: 删除所有 tmp_ 和 delete_tmp_ 目录
        Note over Server: 清理崩溃残留的不完整写入
    end

    rect rgb(240, 255, 230)
        Note right of Server: Phase 2: checksums 校验
        loop 每个 Part 目录
            Server->>Disk: 读取 Part/checksums.txt
            loop 每个文件
                Server->>Disk: 计算文件 CityHash128
                alt hash 匹配
                    Note over Server: 文件完整 ✓
                else hash 不匹配
                    Note over Server: 文件损坏! ✗
                end
            end
        end
    end

    rect rgb(245, 240, 255)
        Note right of Server: Phase 3: 加载完整 Part
        Server->>Parts: 加载所有 checksums 通过的 Part
        Note over Parts: 202504_1_1_0 → Active ✓202504_2_2_0 → Active ✓202504_1_3_1 → Active ✓
    end

    rect rgb(255, 230, 245)
        Note right of Server: Phase 4: 与 ZK 对比 (ReplicatedMergeTree)
        Server->>ZK: 读取 /replicas/self/parts
        ZK-->>Server: ZK 注册的 Part 列表
        Note over Server: 对比本地 vs ZK
    end

    rect rgb(230, 255, 255)
        Note right of Server: Phase 5: 修复不一致
        Server->>Server: 本地有 + ZK 无 → 清理孤立 Part
        Server->>Server: ZK 有 + 本地无 → 需要补齐
        loop 每个缺失的 Part
            Server->>ZK: 查找拥有该 Part 的健康副本
            Server->>Other: HTTP fetch Part 数据
            Other-->>Server: Part 文件 + checksums
            Server->>Server: 校验 checksums
            Server->>Disk: 写入本地 Part 目录
        end
    end

    rect rgb(255, 255, 230)
        Note right of Server: Phase 6: 恢复完成
        Server->>Server: 所有 Part 与 ZK 一致
        Server->>ZK: 更新 is_active 临时节点
        Note over Server: 开始接受读写请求
    end
```

## 五、checksums.txt 校验机制

### 写入时的校验和计算

```mermaid
flowchart TB
    subgraph Write["写入链 (每个文件)"]
        Serial["Serialization 序列化数据"]
        HWB1["HashingWriteBuffer累加 CityHash128"]
        CWB["CompressedWriteBufferLZ4 压缩"]
        HWB2["HashingWriteBuffer校验压缩后数据"]
        File["WriteBufferFromFile写入磁盘"]
        Serial --> HWB1 --> CWB --> HWB2 --> File
    end

    subgraph Checksum["checksums.txt 汇总"]
        H1["a.bin: size=1048576, hash=0xABCDEF"]
        H2["a.mrk2: size=256, hash=0x123456"]
        H3["primary.idx: size=128, hash=0x789ABC"]
        H4["columns.txt: size=64, hash=0xDEF012"]
    end

    Write --> Checksum
```

### 启动时的校验流程

```mermaid
flowchart TB
    Start["读取 checksums.txt"] --> Loop{"遍历每个文件条目"}

    Loop --> Read["从磁盘读取文件内容"]
    Read --> Calc["计算 CityHash128"]
    Calc --> Match{"hash 匹配?"}

    Match -->|"是"| Next["文件完整 ✓"]
    Match -->|"否"| Corrupt["文件损坏 ✗"]

    Next --> Loop
    Corrupt --> Action{"副本表?"}

    Action -->|"是"| Fetch["从其他副本 fetchPart替换损坏的 Part"]
    Action -->|"否"| Error["Part 标记为不可用需手动修复"]

    Loop -->|"全部完成"| Done["Part 校验通过, 加载到 Active"]

    style Corrupt fill:#FFCDD2
    style Error fill:#FFCDD2
    style Fetch fill:#FFF9C4
    style Done fill:#C8E6C9
```

### 读取时的在线校验

```
正常查询时的校验链:
  ReadBufferFromFile → HashingReadBuffer (校验) → CompressedReadBuffer (解压) → HashingReadBuffer (校验) → 返回数据

如果校验失败:
  抛出异常: "Checksum doesn't match: expected ..., got ..."
  副本表: 自动尝试从其他副本读取
  非副本表: 查询失败, 报错
```

## 六、fsync 策略与数据安全权衡

```mermaid
flowchart TB
    subgraph NoFSync["fsync_after_insert=0 (默认)"]
        N1["INSERT → 写 tmp_ → rename → Active → 返回"]
        N2["数据在 OS 页缓存中"]
        N3["OS 异步刷盘 (最多丢失 ~30s 数据)"]
        N4["性能最高, 吞吐量最大"]
        N5["适合 ReplicatedMergeTree (ZK 可恢复)"]
        N1 --> N2 --> N3 --> N4 --> N5
    end

    subgraph WithFSync["fsync_after_insert=1"]
        W1["INSERT → 写 tmp_ → rename → fsync → Active → 返回"]
        W2["数据已落盘"]
        W3["断电不丢失"]
        W4["fsync 延迟 ~1-10ms, 吞吐量下降"]
        W5["适合非副本 MergeTree (无可恢复机制)"]
        W1 --> W2 --> W3 --> W4 --> W5
    end
```

### fsync 相关设置

| 设置 | 默认值 | 说明 |
|------|--------|------|
| `fsync_after_insert` | `0` | INSERT 后是否 fsync |
| `fsync_part_directory` | `1` | Part 写完后是否 fsync 目录 |
| `write_buffer_size` | `1048576` | 写入缓冲区大小 |
| `compress_block_size` | `1048576` | 压缩块大小 |

### 生产环境建议

| 场景 | fsync 策略 | 原因 |
|------|-----------|------|
| ReplicatedMergeTree | `fsync_after_insert=0` | ZK 状态机保证可恢复, 无需每次 fsync |
| MergeTree (非副本) | `fsync_after_insert=1` | 没有副本兜底, fsync 防止断电丢数据 |
| 金融/审计数据 | `fsync_after_insert=1` | 即使有副本, 也保证本地落盘 |

## 七、两阶段提交的状态机保证

```mermaid
stateDiagram-v2
    [*] --> Temporary: 写入 tmp_ 目录
    Temporary --> PreActive: rename 完成
    PreActive --> Active: 内存状态更新

    Active --> Outdated: 被新 Part 覆盖 (合并/变异)
    Outdated --> Deleting: 清理线程
    Deleting --> [*]

    note right of Temporary: tmp_insert_xxx崩溃时直接删除
    note right of PreActive: rename 完成checksums 校验后加载
    note right of Active: 可查询
    note right of Outdated: 不可查询, 等待物理删除
```

**两阶段提交的作用**: PreActive → Active 的切换是在持有 PartsLock 的情况下原子完成的。查询线程要么看到完整的 Part (Active)，要么看不到 (不存在)，不会看到中间状态。

## 八、崩溃一致性保证总结

```mermaid
mindmap
    root((ClickHouse 崩溃一致性))
        原子 rename
            tmp_ 前缀不可见
            rename 是 OS 原子操作
            查询只看到完整 Part
        tmp_ 清理
            启动时删除所有 tmp_ 目录
            不残留不完整的写入
            保守策略, 完整的 tmp_ 也删除
        checksums 校验
            写入时 HashingWriteBuffer 计算
            启动时逐文件验证 CityHash128
            读取时在线校验
            损坏检测 + 自动修复
        ZK 状态机
            ZK 是最终真相源
            本地与 ZK 不一致时修复
            孤立 Part 清理
            缺失 Part 从副本拉取
        两阶段提交
            PreActive → Active 原子切换
            PartsLock 保护
            查询无中间状态
        无 WAL 代价
            内存数据崩溃丢失
            适合批量写入
            配合 ZK 可实现可恢复
```

## 九、关键源码文件索引

| 文件 | 职责 |
|------|------|
| `Storages/MergeTree/MergeTreeData.h` | data_parts 容器, Transaction 两阶段提交, Part 状态管理 |
| `Storages/MergeTree/IMergeTreeDataPart.h` | Part 状态枚举 (Temporary/PreActive/Active/Outdated/Deleting) |
| `Storages/MergeTree/MergeTreeDataWriter.cpp` | writeTempPart, finalizePart, rename, 两阶段提交 |
| `Storages/MergeTree/MergedBlockOutputStream.cpp` | 列数据写入, finalizePartOnDisk, checksums 生成 |
| `Storages/MergeTree/MergeTreeDataPartWriterWide.cpp` | Wide 格式列写入, HashingWriteBuffer 链 |
| `IO/HashingWriteBuffer.h` | 写入时 CityHash128 校验 |
| `IO/HashingReadBuffer.h` | 读取时 CityHash128 校验 |
| `Storages/MergeTree/MergeTreeDataMergerMutator.cpp` | 合并时的原子提交 |
| `Storages/StorageReplicatedMergeTree.cpp` | 副本恢复, 与 ZK 对比, fetchPart |
| `Storages/MergeTree/ReplicatedMergeTreeRestartingThread.cpp` | ZK session 恢复 + 元数据修复 |
