# ClickHouse 存储模型分析

> 基于 ClickHouse 源码 (ClickHouse-master) 分析，聚焦 MergeTree 存储引擎家族

## 一、整体架构

```mermaid
flowchart TB
    SQL["SQL 查询 (SELECT/INSERT)"]

    subgraph IStorage["IStorage (抽象基类)"]
        Read["virtual Pipe read()"]
        Write["virtual SinkToStorage write()"]
        Mutate["virtual void mutate()"]
        Optimize["virtual void optimize()"]
        LockShare["lockForShare() 共享读锁"]
        LockAlter["lockForAlter() 排他改锁"]
        LockExclusive["lockExclusively() 完全排他锁"]
    end

    subgraph MT["MergeTree 系列 (90%+ 使用场景)"]
        MTD["MergeTreeData"]
        SMT["StorageMergeTree"]
        Writer["MergeTreeDataWriter"]
        Merger["MergeTreeDataMergerMutator"]
        Executor["MergeTreeDataSelectExecutor"]
        Cleanup["MergeTreeCleanupThread"]
        BGPool["MergeTreeBackgroundTaskPool"]
    end

    subgraph Other["其他引擎家族"]
        Memory["Memory / Log / Null"]
        External["Kafka / JDBC / MySQL"]
    end

    subgraph Variants["MergeTree 变体"]
        MergeTree["MergeTree: 基础版"]
        Replacing["ReplacingMergeTree: 保留最新版本"]
        Collapsing["CollapsingMergeTree: 折叠行"]
        Summing["SummingMergeTree: 数值求和"]
        Aggregating["AggregatingMergeTree: 聚合合并"]
        VCM["VersionedCollapsingMergeTree: 带版本折叠"]
        Graphite["GraphiteMergeTree: Graphite 预聚合"]
        Replicated["Replicated*: 副本版 (ZooKeeper)"]
    end

    SQL --> IStorage
    IStorage --> MT
    IStorage --> Other
    MTD --> SMT
    SMT --> Writer
    SMT --> Merger
    SMT --> Executor
    SMT --> Cleanup
    SMT --> BGPool
```

## 二、数据 Part 结构

### 磁盘目录结构

```mermaid
flowchart LR
    subgraph Part["Part: 202504_1_3_1"]
        direction TB
        Check["checksums.txt: 所有文件 CityHash 校验和"]
        Cols["columns.txt: 列名和类型列表"]
        Count["count.txt: 总行数"]
        PIdx["primary.idx: 稀疏主键索引"]
        PData["partition.dat: 分区表达式值"]
        MinMax["minmax_event_date.idx: 分区列最值"]

        subgraph ColumnFiles["列数据文件"]
            IDBin["id.bin: 压缩数据"]
            IDMrk["id.mrk2: mark 文件"]
            EDBin["event_date.bin: 压缩数据"]
            EDMrk["event_date.mrk2: mark 文件"]
            VBin["value.bin: 压缩数据"]
            VMrk["value.mrk2: mark 文件"]
        end

        subgraph SkipIdx["跳数索引 (可选)"]
            SIIdx["skip_idx.idx"]
            SIMrk["skip_idx.mrk"]
        end
    end

    Check --> Cols --> Count --> PIdx --> PData --> MinMax --> ColumnFiles --> SkipIdx
```

### Part 命名规则

`partition_min_block_max_block_level`
- 示例: `202504_1_3_1` → 分区=202504, 最小块=1, 最大块=3, 合并层级=1

### Part 状态机

```mermaid
stateDiagram-v2
    [*] --> Temporary: INSERT 写入
    Temporary --> PreActive: commit()
    PreActive --> Active: commit()
    Active --> Outdated: 被新 Part 覆盖 (合并/变异完成)
    Outdated --> Deleting: 清理线程
    Deleting --> [*]: 已删除

    note right of Temporary: tmp_xxx
    note right of Active: 可查询
    note right of Outdated: 不可查询
    note right of Deleting: 删除文件
```

**两阶段提交**: 新 part 先进 PreActive，commit 时 PreActive → Active，被覆盖的 part → Outdated，保证查询始终看到完整一致的数据视图。

## 三、列式存储格式

### Wide 格式 (每列独立文件)

```mermaid
flowchart TB
    subgraph Table["表数据 (逻辑视图)"]
        direction LR
        H["id | name | value"]
        R1["1 | alice | 10.5"]
        R2["3 | bob | 20.3"]
        R3["5 | carol | 15.7"]
        RN["8191 | zzz | 99.9"]
        RB["8192 | aaa | 11.0"]
    end

    subgraph Disk["磁盘存储 (Wide 格式, 列独立)"]
        direction TB
        ID["id.bin (LZ4)"]
        Name["name.bin (LZ4+ZSTD)"]
        Value["value.bin (LZ4)"]
    end

    subgraph Granules["Granule 划分"]
        G0["Granule 0: 行 0~8191"]
        G1["Granule 1: 行 8192~..."]
    end

    Table --> Granules
    Granules --> Disk
```

### Mark 文件与压缩块关系

```mermaid
flowchart LR
    subgraph MarkFile["id.mrk2 (自适应粒度)"]
        M0["Mark 0: (0, 0, 8192)"]
        M1["Mark 1: (12345, 678, 4096)"]
        M2["Mark 2: (...)"]
    end

    subgraph BinFile["id.bin (压缩文件)"]
        CB0["压缩块 0: 1MB"]
        CB1["压缩块 1: 2MB"]
        CB2["压缩块 2: ..."]
    end

    M0 --> |"offset_in_compressed=0"| CB0
    M1 --> |"offset_in_compressed=12345"| CB1
    M2 --> |"..."| CB2

    Note["每个 Mark = 压缩文件偏移 + 解压块偏移"]
```

### 索引粒度对比

```mermaid
flowchart TB
    subgraph Fixed["固定粒度 (默认 8192 行/Mark)"]
        direction LR
        F1["8192行: 1MB"]
        F2["8192行: 50MB"]
        F3["8192行: 2MB"]
        F4["8192行: 100MB"]
        Note1["压缩块大小不均匀"]
    end

    subgraph Adaptive["自适应粒度 (index_granularity_bytes)"]
        direction LR
        A1["4096行: ~10MB"]
        A2["8192行: ~10MB"]
        A3["16384行: ~10MB"]
        A4["8192行: ~10MB"]
        Note2["压缩块大小均匀"]
    end
```

## 四、稀疏主键索引

### 索引查找流程

```mermaid
sequenceDiagram
    participant Q as SQL 查询
    participant KC as KeyCondition
    participant IDX as primary.idx
    participant MRK as .mrk2
    participant BIN as .bin

    Q->>KC: WHERE event_date = '2025-01-02'
    KC->>IDX: 二分查找
    Note over IDX: 找到 Mark 2 (2025-01-02) 和 Mark 3 (2025-01-03)
    IDX-->>KC: Mark 范围 [2, 3)
    KC-->>Q: 只需读 Mark 2

    Q->>MRK: 读取 Mark 2 的偏移
    MRK-->>Q: (offset_compressed=12345, offset_decompressed=678)
    Q->>BIN: seek + 解压 Granule 2
    BIN-->>Q: Granule 数据
    Q->>Q: WHERE 过滤 (跳过 75% 数据)
```

### 多级索引过滤 (层层裁剪)

```mermaid
flowchart TB
    All["全部 Parts: 3 个分区 × 100 Granule"]
    All --> L1
    All --> L2
    All --> L3
    All --> L4

    subgraph L1["Level 1: 分区裁剪"]
        P1["Part 202501: minmax=[01-01, 01-31]"]
        P2["Part 202502: minmax=[02-01, 02-28]"]
        P3["Part 202503: minmax=[03-01, 03-31]"]
        Hit1["保留 1/3 分区"]
        P1 -.->|"命中"| Hit1
        P2 -.->|"淘汰"| X2["X"]
        P3 -.->|"淘汰"| X3["X"]
    end

    subgraph L2["Level 2: 主键索引"]
        M0["Mark 0: 2025-01-01"]
        M1["Mark 1: 2025-01-10"]
        M2["Mark 2: 2025-01-15"]
        M3["Mark 3: 2025-01-20"]
        Hit2["保留 Mark [2,5), 淘汰 50%"]
    end

    subgraph L3["Level 3: 跳数索引 (BloomFilter)"]
        B1["Granule [2,3): false"]
        B2["Granule [3,4): true"]
        B3["Granule [4,5): false"]
        Hit3["只保留 1 个 Granule"]
    end

    subgraph L4["Level 4: 数据读取 + SIMD 过滤"]
        Result["实际扫描 8192 行"]
        Final["最终只读原始数据的 ~0.3%"]
    end

    L1 --> L2 --> L3 --> L4
```

## 五、数据写入流程

```mermaid
sequenceDiagram
    participant C as Client
    participant Sink as MergeTreeSink
    participant Writer as MergeTreeDataWriter
    participant Output as MergedBlockOutputStream

    C->>Sink: INSERT SQL
    Sink->>Writer: consume(Chunk)

    rect rgb(230, 255, 230)
        Note right of Writer: Step 1: 按分区键拆分
        Writer->>Writer: splitBlockByPartition()
        Note over Writer: Block A → 分区1, Block B → 分区2
    end

    rect rgb(255, 245, 230)
        Note right of Writer: Step 2: 写临时 Part
        Writer->>Output: writeTempPart()
        Output->>Output: 按排序键排序
        Output->>Output: 创建 tmp_xxx
    end

    rect rgb(240, 240, 255)
        Note right of Output: Step 3: 逐列写入
        Output->>Output: write(sorted_block)
        par 并行写入列
            Output->>Output: id.bin + id.mrk2
            and
            Output->>Output: name.bin + name.mrk2
            and
            Output->>Output: value.bin + value.mrk2
        end
    end

    rect rgb(255, 230, 255)
        Note right of Output: Step 4: 写元数据
        Output->>Output: finalizePart()
        par 元数据文件
            Output->>Output: primary.idx
            and
            Output->>Output: columns.txt
            and
            Output->>Output: count.txt
            and
            Output->>Output: checksums.txt
        end
    end

    rect rgb(255, 255, 230)
        Note right of Writer: Step 5: 两阶段提交
        Writer->>Writer: Phase 1: tmp → PreActive
        Writer->>Writer: Phase 2: PreActive → Active
    end

    Sink-->>C: OK, N rows
```

**关键特点**: 每个 INSERT 批次生成独立 Part; 数据先排序再写入; 两阶段提交保证原子性; 写完立即可查; 不需要 WAL。

## 六、数据读取流程

```mermaid
sequenceDiagram
    participant Q as SQL 查询
    participant Exec as MergeTreeDataSelectExecutor
    participant Parts as DataParts
    participant KC as KeyCondition
    participant Reader as MergeTreeReaderWide
    participant Vec as 向量化引擎

    Q->>Exec: SELECT name, value FROM t WHERE event_date='2025-01-15' AND user_id=12345

    rect rgb(255, 240, 230)
        Note right of Exec: Step 1: Part 级过滤
        Exec->>Parts: filterPartsByPartition()
        Parts-->>Exec: 3 个分区 → 保留 1 个 (202501)
    end

    rect rgb(240, 255, 230)
        Note right of Exec: Step 2: Granule 级过滤
        Exec->>KC: KeyCondition 检查 primary.idx
        KC-->>Exec: 100 个 Granule → 保留 3 个 Mark Range
    end

    rect rgb(230, 240, 255)
        Note right of Exec: Step 3: 跳数索引过滤
        Exec->>Exec: BloomFilter 检查
        Note over Exec: 3 个 Mark Range → 保留 1 个
    end

    rect rgb(255, 255, 230)
        Note right of Exec: Step 4: 构建读取 Pipeline
        Exec->>Reader: 读取 Mark [3,4)
        par 多列并行读取
            Reader->>Reader: name.mrk2 → seek → 解压 → 反序列化
            and
            Reader->>Reader: value.mrk2 → seek → 解压 → 反序列化
        end
        Note over Reader: 列裁剪: id 列不读
    end

    rect rgb(255, 230, 240)
        Note right of Reader: Step 5: 向量化过滤
        Reader->>Vec: Block (8192 行)
        Vec->>Vec: SIMD 批量比较 user_id = 12345
        Vec-->>Exec: 过滤掩码 [1,0,0,1,0,...]
        Exec-->>Q: 结果 (name, value)
    end
```

**性能优化要点**: 列裁剪 (只读需要的列); Mark 命中 (稀疏索引); 压缩缓存 (UncompressedCache); Mark 缓存 (MarkCache); 多 Part 并行; 向量化执行 (SIMD)。

## 七、后台合并流程

### 合并选择策略

```mermaid
flowchart LR
    subgraph Before["合并前"]
        direction LR
        A["10MB"] --> B["10MB"] --> C["10MB"] --> D["50MB"] --> E["50MB"] --> F["200MB"]
    end

    subgraph Select["SimpleMergeSelector"]
        Note1["base=5: 合并后/最大 ≈ base"]
        Note2["小 Part 优先, 老优先, 大小对齐"]
        Selected["选中: [10MB, 10MB, 10MB]"]
    end

    subgraph After["合并后"]
        direction LR
        G["30MB"] --> D2["50MB"] --> E2["50MB"] --> F2["200MB"]
    end

    Before --> Select --> After
```

### 合并执行流程

```mermaid
sequenceDiagram
    participant Pool as BackgroundPool
    participant Selector as SimpleMergeSelector
    participant Merger as MergeTreeDataMergerMutator
    participant SrcA as Part A (10MB)
    participant SrcB as Part B (10MB)
    participant SrcC as Part C (10MB)
    participant Output as MergedBlockOutputStream
    participant Tmp as tmp_xxx (30MB)

    Pool->>Selector: 选择合并候选
    Selector-->>Pool: [Part A, B, C]

    Pool->>Merger: 执行合并

    rect rgb(240, 255, 240)
        Note right of Merger: 多路归并
        Merger->>SrcA: 读取有序数据
        Merger->>SrcB: 读取有序数据
        Merger->>SrcC: 读取有序数据
        Merger->>Output: 归并排序写入
        Output->>Tmp: 逐列写入 .bin + .mrk2
    end

    rect rgb(255, 245, 230)
        Note right of Merger: 特殊语义 (MergingParams)
        Note over Merger: ReplacingMergeTree: 保留最新 version
        Note over Merger: CollapsingMergeTree: sign 相反则删除
        Note over Merger: SummingMergeTree: 数值列求和
        Note over Merger: AggregatingMergeTree: 聚合状态合并
    end

    rect rgb(230, 240, 255)
        Note right of Merger: 两阶段提交
        Merger->>Merger: tmp → PreActive
        Merger->>Merger: PreActive → Active (level+1)
        Note over SrcA: Part A → Outdated
        Note over SrcB: Part B → Outdated
        Note over SrcC: Part C → Outdated
    end
```

### 垂直合并 (内存不足时)

```mermaid
flowchart TB
    subgraph VM["垂直合并 (分阶段)"]
        direction TB

        subgraph Phase1["阶段 1: 只合并排序列"]
            PA1["Part A: id"] --> Merge1["归并排序"]
            PB1["Part B: id"] --> Merge1
            PC1["Part C: id"] --> Merge1
            Merge1 --> Tmp1["tmp: id"]
        end

        subgraph Phase2["阶段 2: 逐列收集非排序列"]
            PA2["Part A: name"] --> Collect["按新 id 顺序收集"]
            PB2["Part B: name"] --> Collect
            PC2["Part C: name"] --> Collect
            Collect --> Tmp2["tmp: name"]
        end

        Phase1 --> Phase2
    end

    Note["内存 = 排序列 + 1 列 (而非全部列)"]
```

## 八、Part 演化全景

```mermaid
timeline
    title Part 生命周期
    t0 : INSERT 1000行 → Part 1 (10MB, level=0)
    t1 : INSERT 1000行 → Part 2 (10MB, level=0)
    t2 : INSERT 1000行 → Part 3 (10MB, level=0)
    t3 : 后台合并 → Part 4 (30MB, level=1, blocks=[1,3])
    t4 : 3x INSERT → Part 5~7
    t5 : 再次合并 → Part 8 (30MB, level=1)
    t6 : 大合并 → Part 9 (60MB, level=2)
```

**写放大公式**: 总写入 = 原始数据 × (1 + 1/base + 1/base^2 + ...)
- base=5: 写放大 ≈ 1.25x
- base=2: 写放大 ≈ 2.0x
- base 越大 → 写放大小, Part 多, 查询慢; base 越小 → 写放大大, Part 少, 查询快

## 九、关键源码文件索引

```mermaid
flowchart TB
    subgraph Core["核心抽象"]
        IStorage["IStorage.h: 引擎基类"]
        IStorageRead["IStorage.h:386: read()"]
        IStorageWrite["IStorage.h:430: write()"]
    end

    subgraph MTC["MergeTree 核心"]
        MTD["MergeTreeData.h: Part 集合"]
        MTDTxn["MergeTreeData.h:353: Transaction"]
        MTDPrimary["MergeTreeData.h:481: 主键表达式"]
        SMT["StorageMergeTree.h: 具体实现"]
    end

    subgraph DP["Data Part"]
        IMTDP["IMergeTreeDataPart.h: Part 基类"]
        State["IMergeTreeDataPart.h:323: 状态机"]
        DPWide["MergeTreeDataPartWide.h"]
        DPCompact["MergeTreeDataPartCompact.h"]
    end

    subgraph Write["写入链路"]
        Sink["MergeTreeSink.h: INSERT 入口"]
        DataWriter["MergeTreeDataWriter.h: 分区+临时Part"]
        MBOS["MergedBlockOutputStream.h: 排序+写列"]
    end

    subgraph Read["读取链路"]
        Executor["MergeTreeDataSelectExecutor.h"]
        KC["KeyCondition.h: WHERE→索引过滤"]
        RWide["MergeTreeReaderWide.h"]
        RStream["MergeTreeReaderStream.h: Mark+解压+Seek"]
    end

    subgraph Merge["合并链路"]
        Task["MergeTask.h: 合并协程"]
        MMM["MergeTreeDataMergerMutator.h"]
        SMS["SimpleMergeSelector.h: 对数合并树"]
        MA["MergeAlgorithm.h: 水平/垂直选择"]
    end

    IStorage --> MTC --> DP
    MTC --> Write
    MTC --> Read
    MTC --> Merge
```

## 十、设计精髓总结

```mermaid
mindmap
    root((ClickHouse 存储设计精髓))
        LSM-Tree 变体
            像 LSM: Append-Only 写入, 后台合并
            像 B+Tree: 全局有序, 范围查询
            区别: 无 MemTable, 直接写有序文件
        稀疏索引
            每 8192 行一个索引条目
            索引极小可全装入内存
            代价: 找到 Granule 后还需内部扫描
        列式存储
            只读需要的列, IO 放大极小
            同列连续存储, 压缩比极高
            同列连续存储, SIMD 向量化执行
        零 WAL
            直接写有序文件
            写入延迟 = 排序 + 写磁盘
            代价: 单行插入效率低, 适合批量
        多级过滤
            分区 → Part minmax → 主键索引
            → 跳数索引 → 数据过滤
            每层淘汰大量无关数据
        合并权衡
            base 大: 写放大小, Part 多, 查询慢
            base 小: 写放大大, Part 少, 查询快
            默认 base=5 是平衡点
```
