# ClickHouse SELECT 流程分析

> 基于 ClickHouse 源码 (ClickHouse-master) 分析, 追踪 `SELECT a, b, c FROM DB_A.table_A WHERE event_date = '2025-04-15'` 的完整执行路径, 每一步标注元数据查询和磁盘读取细节

## 一、全链路总览

```mermaid
flowchart TB
    subgraph Parse["阶段 1: SQL 解析"]
        P1["ParserSelectQuery: 词法分析"]
        P2["ASTSelectQuery: 语法树"]
    end

    subgraph Resolve["阶段 2: 语义分析"]
        R1["TreeRewriter: 列收集"]
        R2["ExpressionAnalyzer: 类型推导"]
        R3["Context.resolveStorageID: 解析 DB 名"]
        R4["DatabaseCatalog.getTable: 查找 Table"]
        R5["IStorage.getInMemoryMetadata: 获取列定义"]
        R6["checkAccess: 权限校验"]
    end

    subgraph Plan["阶段 3: 查询计划"]
        Q1["executeFetchColumns: 从存储读"]
        Q2["executeWhere: WHERE 过滤"]
        Q3["executeAggregation: GROUP BY"]
        Q4["executeProjection: 列投影"]
        Q5["executeLimit: LIMIT"]
    end

    subgraph Index["阶段 4: 多级索引过滤"]
        I1["PartitionPruner: 分区裁剪"]
        I2["MinMax 条件: Part 级过滤"]
        I3["KeyCondition: 主键索引"]
        I4["SkipIndex: 跳数索引"]
    end

    subgraph Read["阶段 5: 数据读取"]
        D1["MergeTreeReaderStream: 读 Mark"]
        D2["seek + 解压 .bin"]
        D3["Serialization: 反序列化"]
        D4["Block: 返回数据"]
    end

    Parse --> Resolve --> Plan --> Index --> Read
```

## 二、SQL 解析阶段

```mermaid
sequenceDiagram
    participant Client as Client
    participant Parser as ParserSelectQuery
    participant AST as ASTSelectQuery

    Client->>Parser: SELECT a, b, c FROM DB_A.table_A WHERE event_date = '2025-04-15'

    rect rgb(240, 245, 255)
        Note right of Parser: 词法分析
        Parser->>Parser: 匹配 SELECT 关键字
        Parser->>Parser: 解析列列表 a, b, c
        Parser->>Parser: 匹配 FROM 关键字
        Parser->>Parser: ParserIdentifier 解析 DB_A.table_A
    end

    rect rgb(255, 245, 240)
        Note right of Parser: WHERE 子句解析
        Parser->>Parser: 匹配 WHERE 关键字
        Parser->>Parser: 解析表达式 event_date = '2025-04-15'
    end

    Parser-->>AST: ASTSelectQuery

    Note over AST: select_columns = [a, b, c]<br/>table = DB_A.table_A<br/>where = event_date = '2025-04-15'
```

**此阶段不查询任何元数据**, 纯粹的语法分析, 生成 AST。

## 三、语义分析与元数据解析阶段

```mermaid
sequenceDiagram
    participant Interp as InterpreterSelectQuery
    participant TW as TreeRewriter
    participant EA as ExpressionAnalyzer
    participant Ctx as Context
    participant DB as DatabaseCatalog
    participant Table as IStorage (StorageMergeTree)
    participant Meta as StorageInMemoryMetadata
    participant Access as AccessChecker

    Interp->>Interp: InterpreterSelectQuery(query, context)

    rect rgb(255, 245, 230)
        Note right of Interp: Step 1: TreeRewriter 语法重写
        Interp->>TW: analyzeSelectQuery(ast)
        TW->>TW: 收集 requiredSourceColumns
        Note over TW: 遍历 AST:<br/>SELECT 列: a, b, c<br/>WHERE 列: event_date<br/>合计 required = [a, b, c, event_date]
        Note over TW: 额外检查: PARTITION BY, ORDER BY, PRIMARY KEY 列
    end

    rect rgb(240, 255, 230)
        Note right of Interp: Step 2: ExpressionAnalyzer
        Interp->>EA: analyze(ast, required_columns)
        EA->>EA: 类型推导
        Note over EA: event_date 类型 = Date<br/>'2025-04-15' 类型 = String<br/>自动 cast 为 Date 比较
    end

    rect rgb(245, 240, 255)
        Note right of Interp: Step 3: 解析数据库和表名
        Interp->>Ctx: resolveStorageID(DB_A, table_A)
        Note over Ctx: 查询 DatabaseCatalog.getDatabase(DB_A)<br/>metadata: 内存 map 查找, 无 I/O
    end

    rect rgb(255, 230, 245)
        Note right of Interp: Step 4: 查找表对象
        Interp->>DB: getTable(DB_A, table_A)
        DB->>DB: DatabaseOnDisk.getTable(table_A)
        Note over DB: 从内存 tables map 查找<br/>metadata: 启动时从 .sql 文件加载
        DB-->>Interp: StoragePtr
    end

    rect rgb(230, 255, 255)
        Note right of Interp: Step 5: 获取表结构
        Interp->>Table: getInMemoryMetadataPtr(context)
        Table-->>Interp: StorageInMemoryMetadata
        Note over Meta: columns: [(a, UInt32), (b, String), (c, Float64),<br/>         (event_date, Date), (id, UInt64), ...]<br/>partition_key: PARTITION BY toYYYYMM(event_date)<br/>sorting_key: ORDER BY (event_date, a)<br/>primary_key: PRIMARY KEY (event_date, a)<br/>secondary_indices: [bf_a, minmax_b]
    end

    rect rgb(255, 240, 230)
        Note right of Interp: Step 6: 创建 StorageSnapshot
        Interp->>Table: getStorageSnapshot(required_columns)
        Note over Table: 快照包含:<br/>- 列定义 (仅 required columns)<br/>- partition key, sorting key<br/>- 所有 Active Part 列表<br/>metadata: data_parts (内存中)
    end

    rect rgb(240, 230, 255)
        Note right of Interp: Step 7: 权限校验
        Interp->>Access: checkAccess(SELECT, DB_A.table_A, [a, b, c, event_date])
        Note over Access: 查询 Grant 权限表<br/>metadata: AccessRights (内存中)
    end

    Interp->>Table: lockForShare()
    Note over Table: 获取共享读锁, 防止并发 ALTER/DROP
```

### 此阶段涉及的元数据查询汇总

| 步骤 | 查询内容 | 数据来源 | 是否磁盘 I/O |
|------|---------|---------|------------|
| 列收集 | requiredSourceColumns | AST 遍历 | 否 |
| 类型推导 | 列类型 | StorageInMemoryMetadata | 否 (内存中) |
| 解析 DB 名 | database 存在性 | DatabaseCatalog 内存 map | 否 |
| 查找表 | table 对象 | DatabaseOnDisk.tables map | 否 |
| 表结构 | 列名、类型、分区键、排序键、主键、跳数索引 | StorageInMemoryMetadata | 否 |
| StorageSnapshot | Active Part 列表 | data_parts (内存中) | 否 |
| 权限 | SELECT 权限 | AccessRights | 否 |

## 四、查询计划构建阶段

```mermaid
sequenceDiagram
    participant Interp as InterpreterSelectQuery
    participant Plan as QueryPlan
    participant QI as QueryInfo
    participant Storage as StorageMergeTree

    Interp->>Interp: buildQueryPlan()

    rect rgb(255, 245, 230)
        Note right of Interp: Step 1: 执行 executeFetchColumns
        Interp->>Interp: executeFetchColumns(query_plan)
        Note over Interp: 将 required_columns 传给存储引擎
        Note over Interp: required_columns = [a, b, c, event_date]
        Note over Interp: event_date 是 WHERE 条件需要的, a,b,c 是 SELECT 需要的
    end

    rect rgb(240, 255, 230)
        Note right of Interp: Step 2: 构建 QueryInfo
        Interp->>QI: 构建 QueryInfo 结构体
        Note over QI: query_info.query = ASTSelectQuery
        Note over QI: query_info.set_ast = ASTSet (SETTINGS)
        Note over QI: query_info.prewhere_info = 分析 PREWHERE 优化
        Note over QI: query_info.filter_actions_dag = WHERE 表达式 DAG
        Note over QI: query_info.order_optimizer = 排序优化信息
    end

    rect rgb(245, 240, 255)
        Note right of Interp: Step 3: 调用 storage->read
        Interp->>Storage: read(query_plan, required_columns, snapshot, query_info)
        Note over Storage: 参数:<br/>columns_to_read = [a, b, c, event_date]<br/>storage_snapshot = 快照 (Part 列表 + 列定义)<br/>query_info = WHERE 条件 + 排序信息
    end

    rect rgb(255, 230, 245)
        Note right of Storage: Step 4: 创建 ReadFromMergeTree
        Storage->>Storage: MergeTreeDataSelectExecutor::read()
        Storage->>Storage: readFromParts()
        Note over Storage: 创建 ReadFromMergeTree QueryPlan Step<br/>注意: 此时仅创建 Step 对象<br/>索引分析是懒加载, 在 pipeline 初始化时执行
        Storage-->>Plan: 添加 ReadFromMergeTree 步骤
    end

    rect rgb(230, 255, 255)
        Note right of Interp: Step 5: 后续查询步骤
        Interp->>Plan: executeWhere (WHERE 过滤)
        Interp->>Plan: executeAggregation (GROUP BY, 如有)
        Interp->>Plan: executeProjection (SELECT a, b, c 投影)
        Interp->>Plan: executeLimit (LIMIT, 如有)
        Note over Plan: 完整 Pipeline:<br/>ReadFromMergeTree → Filter → Projection → Limit
    end
```

## 五、多级索引过滤阶段 (核心优化)

```mermaid
sequenceDiagram
    participant RFM as ReadFromMergeTree
    participant BI as buildIndexes
    participant PP as PartitionPruner
    participant MM as MinMaxCondition
    participant KC as KeyCondition
    participant SI as SkipIndex

    RFM->>RFM: selectRangesToRead() [pipeline 初始化时触发]

    rect rgb(255, 245, 230)
        Note right of RFM: Step 1: 构建索引分析器
        RFM->>BI: buildIndexes()
        Note over BI: 从 metadata 构建:<br/>1. KeyCondition (主键条件)<br/>2. PartitionPruner (分区裁剪器)<br/>3. MinMaxCondition (分区列最值)<br/>4. SkipIndex 条件列表
        BI-->>RFM: 索引分析器集合
    end

    rect rgb(240, 255, 230)
        Note right of RFM: Step 2: 分区裁剪
        RFM->>PP: filterPartsByPartition(all_parts)
        Note over PP: WHERE event_date = '2025-04-15'<br/>PARTITION BY toYYYYMM(event_date)<br/>求值: toYYYYMM('2025-04-15') = 202504
        Note over PP: 遍历所有 Part:<br/>Part 202501_xxx: canBePruned=true<br/>Part 202502_xxx: canBePruned=true<br/>Part 202503_xxx: canBePruned=true<br/>Part 202504_xxx: canBePruned=false<br/>Part 202505_xxx: canBePruned=true
        Note over PP: metadata: partition.dat (每个 Part 的分区值, 内存中)
        PP-->>RFM: 保留 202504 分区的 Part (假设 5 个)
    end

    rect rgb(245, 240, 255)
        Note right of RFM: Step 3: MinMax 条件过滤
        RFM->>MM: checkInHyperrectangle(part.minmax_idx)
        Note over MM: Part 202504_1_1_0:<br/>minmax_event_date = [2025-04-01, 2025-04-30]<br/>WHERE event_date = 2025-04-15<br/>intersect? Yes
        Note over MM: Part 202504_2_2_0:<br/>minmax_event_date = [2025-04-01, 2025-04-15]<br/>intersect? Yes
        Note over MM: metadata: minmax_event_date.idx (内存中)
        MM-->>RFM: 保留 5/5 Part
    end

    rect rgb(255, 230, 240)
        Note right of RFM: Step 4: 主键索引过滤
        RFM->>KC: markRangesFromPKRange(part, key_condition)
        Note over KC: PRIMARY KEY = (event_date, a)<br/>WHERE event_date = '2025-04-15'<br/>对主键的第一列有等值条件
        KC->>KC: 加载 primary.idx (稀疏索引)
        Note over KC: Part 202504_1_1_0, 共 100 个 Granule:<br/>Granule 0: event_date=2025-04-01, a=1<br/>Granule 10: event_date=2025-04-10, a=1<br/>Granule 15: event_date=2025-04-15, a=1<br/>Granule 20: event_date=2025-04-20, a=1<br/>...
        KC->>KC: 二分查找: event_date = 2025-04-15<br/>找到 Granule 范围 [15, 20)
        Note over KC: metadata: primary.idx (内存中)
        KC-->>RFM: Mark Range [15, 20), 即 5 个 Granule
    end

    rect rgb(230, 255, 255)
        Note right of RFM: Step 5: 跳数索引过滤
        RFM->>SI: filterMarksUsingIndex(bf_a_index)
        Note over SI: 假设有 BloomFilter 跳数索引 bf_a<br/>对 Mark Range [15, 20) 逐 Granule 检查:<br/>Granule 15: bf_a.contains(a=5)? true<br/>Granule 16: bf_a.contains(a=5)? false<br/>Granule 17: bf_a.contains(a=5)? true<br/>Granule 18: bf_a.contains(a=5)? true<br/>Granule 19: bf_a.contains(a=5)? false
        Note over SI: metadata: skip_idx.idx + skip_idx.mrk (按需从磁盘加载)
        SI-->>RFM: 最终 Mark Range: [15,16), [17,19)
    end
```

### 索引过滤效果汇总

```mermaid
flowchart TB
    All["全部数据: 12 个分区, 1200 个 Part, 120000 个 Granule"]

    L1["Level 1: 分区裁剪"]
    All --> L1
    L1 --> P1["淘汰 11 个分区<br/>保留: 202504 分区 100 个 Part, 10000 个 Granule"]

    L2["Level 2: MinMax 过滤"]
    P1 --> L2
    L2 --> P2["保留 100/100 Part (分区内无法再裁剪)"]

    L3["Level 3: 主键索引"]
    P2 --> L3
    L3 --> P3["淘汰 9995 个 Granule<br/>保留 5 个 Granule (Mark Range)"]

    L4["Level 4: 跳数索引"]
    P3 --> L4
    L4 --> P4["淘汰 2 个 Granule<br/>保留 3 个 Granule"]

    L5["Level 5: 数据读取 + WHERE 过滤"]
    P4 --> L5
    L5 --> Final["实际读取 3 x 8192 = 24576 行<br/>WHERE 再过滤掉 ~95%<br/>最终返回 ~1200 行"]

    style Final fill:#90EE90
```

## 六、数据读取阶段 (从磁盘到内存)

```mermaid
sequenceDiagram
    participant Pool as MergeTreeReadPool
    participant Task as MergeTreeReadTask
    participant Chain as MergeTreeReadersChain
    participant Range as MergeTreeRangeReader
    participant Reader as MergeTreeReaderWide
    participant Stream as MergeTreeReaderStream
    participant MarkCache as MarkCache
    participant UncompCache as UncompressedCache
    participant Disk as DiskLocal

    Pool->>Task: getTask() [分配读取任务]
    Task->>Chain: read(rows_to_read, mark_ranges)

    rect rgb(255, 245, 230)
        Note right of Chain: Step 1: PREWHERE 链 (如果有)
        Chain->>Chain: 分析 PREWHERE 列 vs 普通列
        Note over Chain: WHERE event_date = '2025-04-15'<br/>event_date 列需要先读<br/>然后过滤行, 再读 a, b, c
        Note over Chain: 实际上 PREWHERE 优化可能将<br/>event_date 的过滤提前到读取阶段
    end

    rect rgb(240, 255, 230)
        Note right of Chain: Step 2: 启动读取链
        Chain->>Range: startReadingChain()
        Range->>Reader: readRows(from_mark=15, max_rows=8192)
    end

    rect rgb(245, 240, 255)
        Note right of Reader: Step 3: 按列逐列读取
        Reader->>Reader: prefetchForAllColumns()
        Note over Reader: 预取所有列的 read buffer

        Reader->>Reader: readData(event_date列)
        Reader->>Stream: getStream(false, substream, event_date)

        rect rgb(255, 240, 240)
            Note right of Stream: Step 3.1: 加载 Mark 文件
            Stream->>MarkCache: getMark(part_name, "event_date.mrk2")
            alt MarkCache 命中
                MarkCache-->>Stream: Mark 数据 (内存中, 无 I/O)
                Note over MarkCache: Mark 大小 = 2 x 8 bytes x marks数<br/>远小于缓存容量, 命中率高
            else MarkCache 未命中
                Stream->>Disk: read("event_date.mrk2")
                Disk-->>Stream: Mark 数据 (磁盘 I/O)
                Stream->>MarkCache: putMark(part_name, "event_date.mrk2", marks)
            end
            Note over Stream: Mark 15: offset_in_compressed=123456, offset_in_decompressed=0
        end

        rect rgb(240, 255, 255)
            Note right of Stream: Step 3.2: Seek 到目标位置
            Stream->>Stream: seekToMark(mark_15)
            Note over Stream: compressed_buffer->seek(123456, 0)
            Note over Stream: 定位到 .bin 文件中 Granule 15 的起始位置
        end

        rect rgb(255, 255, 230)
            Note right of Stream: Step 3.3: 读取压缩数据
            Stream->>UncompCache: get(data_hash)
            alt UncompressedCache 命中
                UncompCache-->>Stream: 解压后的数据块 (内存中)
                Note over UncompCache: 避免重复解压, 高命中
            else UncompressedCache 未命中
                Stream->>Disk: read("event_date.bin", compressed_block)
                Note over Stream: 压缩块包含多个 Granule 的数据
                Stream->>Stream: LZ4 解压 (CPU 密集)
                Stream->>UncompCache: put(data_hash, decompressed_block)
            end
            Note over Stream: 从解压块中按 offset 取出 Granule 15 的数据
        end

        rect rgb(255, 230, 255)
            Note right of Stream: Step 3.4: 反序列化
            Stream->>Stream: Serialization::deserializeBinaryBulk()
            Note over Stream: Date 类型: 从 2 字节 (day num) 转换为 Date 对象<br/>读取 8192 个 Date 值
            Stream-->>Reader: Column with 8192 rows (event_date)
        end
    end

    rect rgb(255, 245, 230)
        Note right of Reader: Step 4: WHERE 过滤 (event_date)
        Reader->>Reader: SIMD 批量比较 event_date = 2025-04-15
        Note over Reader: 生成过滤掩码: [0,0,1,0,...]<br/>假设 8192 行中 82 行匹配
    end

    rect rgb(240, 255, 230)
        Note right of Reader: Step 5: 读取剩余列 (仅匹配行)
        Reader->>Reader: readData(a 列)
        Note over Reader: 同样的流程: Mark → Seek → 解压 → 反序列化<br/>但 a 列不需要 WHERE 过滤
        Reader->>Reader: readData(b 列)
        Reader->>Reader: readData(c 列)
        Note over Reader: b 列和 c 列读取流程相同<br/>3 列可以并行读取 (prefetch + read)
    end

    Reader-->>Range: Block (event_date, a, b, c)
    Range-->>Chain: Block + 过滤掩码
    Chain-->>Task: Block (仅 WHERE 匹配的行)
```

### 磁盘 I/O 细节: ReadBuffer 链

```mermaid
flowchart TB
    subgraph Chain["event_date.bin 读取链路"]
        File["ReadBufferFromFile<br/>操作系统 read() 系统调用"]
        Hash1["HashingReadBuffer<br/>校验 CityHash128"]
        Comp["CompressedReadBufferFromFile<br/>LZ4 解压"]
        Hash2["HashingReadBuffer<br/>校验解压数据"]

        File --> Hash1 --> Comp --> Hash2
    end

    Cache["UncompressedCache<br/>缓存解压后的数据块"]

    Comp -.->|"缓存查询"| Cache
    Cache -.->|"缓存命中, 跳过解压"| Hash2

    MarkFile["event_date.mrk2<br/>Mark 文件读取"]
    MarkHash["HashingReadBuffer"]
    MarkCacheHit["MarkCache<br/>缓存 Mark 数据"]

    MarkFile --> MarkHash
    MarkHash -.->|"缓存查询"| MarkCacheHit
    MarkCacheHit -.->|"缓存命中"| MarkHash

    subgraph ParallelCols["并行列读取 (3 列)"]
        A_Chain["a.bin 读取链"]
        B_Chain["b.bin 读取链"]
        C_Chain["c.bin 读取链"]
    end

    style Cache fill:#E8F5E9
    style MarkCacheHit fill:#E8F5E9
```

### 每个 .bin 文件的读取路径

```mermaid
flowchart LR
    subgraph Read["读取一个 Granule 的流程"]
        R1["1. 查 MarkCache<br/>获取 (offset_compressed, offset_decompressed)"]
        R2["2. seek 到 .bin 文件偏移位置"]
        R3["3. 查 UncompressedCache<br/>是否有已解压的数据块"]
        R4["4a. 缓存命中: 直接读取"]
        R5["4b. 缓存未命中: 读取压缩块 + LZ4 解压"]
        R6["5. 从解压块中按 offset 取数据"]
        R7["6. 反序列化为 Column"]
    end

    R1 --> R2 --> R3
    R3 -->|"命中"| R4 --> R7
    R3 -->|"未命中"| R5 --> R6 --> R7
```

## 七、查询结果返回阶段

```mermaid
sequenceDiagram
    participant Reader as MergeTreeReadersChain
    participant Filter as ExpressionStep (WHERE)
    participant Proj as ExpressionStep (Projection)
    participant Limit as LimitStep
    participant Client as Client

    rect rgb(255, 245, 230)
        Note right of Reader: 所有 Part 读取完成
        Reader-->>Filter: Block (event_date, a, b, c)
        Note over Reader: 3 个 Part x 3 个 Granule = 9 个 Block<br/>每个 Block 最多 8192 行
    end

    rect rgb(240, 255, 230)
        Note right of Filter: Step 1: WHERE 过滤
        Filter->>Filter: event_date = '2025-04-15'
        Note over Filter: SIMD 向量化比较<br/>8192 行并行处理
        Filter-->>Proj: Block (a, b, c) 过滤后
    end

    rect rgb(245, 240, 255)
        Note right of Proj: Step 2: 列投影
        Proj->>Proj: 保留 a, b, c
        Note over Proj: 丢弃 event_date 列<br/>减少内存占用
        Proj-->>Limit: Block (a, b, c)
    end

    rect rgb(255, 230, 245)
        Note right of Limit: Step 3: LIMIT (如果有)
        Limit->>Limit: 截取前 N 行
        Limit-->>Client: 最终结果
    end
```

## 八、全程元数据访问热力图

```mermaid
flowchart LR
    subgraph Hot["热路径 (每次 SELECT 都访问, 内存中)"]
        H1["data_parts (Part 集合)"]
        H2["StorageInMemoryMetadata (表结构)"]
        H3["partition_key 表达式"]
        H4["sorting_key / primary_key 表达式"]
        H5["columns (NamesAndTypesList)"]
        H6["secondary_indices 定义"]
        H7["index_granularity 设置"]
    end

    subgraph Warm["温路径 (按需从磁盘加载, 有缓存)"]
        W1["primary.idx (稀疏主键索引, 内存中)"]
        W2["partition.dat (分区值, 内存中)"]
        W3["minmax_*.idx (最值索引, 内存中)"]
        W4[".mrk2 Mark 文件 (MarkCache)"]
        W5["skip_idx.idx (跳数索引, 按需加载)"]
        W6["UncompressedCache (解压缓存)"]
    end

    subgraph Cold["冷路径 (磁盘 I/O)"]
        C1["读取 .bin 压缩列数据"]
        C2["LZ4 解压 (CPU)"]
        C3["MarkCache 未命中时读 .mrk2"]
        C4["SkipIndex 未加载时读 .idx/.mrk"]
    end
```

## 九、查询涉及的磁盘 I/O 完整清单

### 每个 Part 的读取文件

```mermaid
flowchart TB
    subgraph Part["Part: 202504_1_1_0"]
        direction TB

        subgraph MemFiles["内存中 (Part 加载时读入)"]
            PIdx["primary.idx: 已在内存"]
            PData["partition.dat: 已在内存"]
            MMIdx["minmax_event_date.idx: 已在内存"]
        end

        subgraph MarkFiles["Mark 文件 (MarkCache 缓存)"]
            EDMrk["event_date.mrk2: MarkCache"]
            AMrk["a.mrk2: MarkCache"]
            BMrk["b.mrk2: MarkCache"]
            CMrk["c.mrk2: MarkCache"]
        end

        subgraph SkipFiles["跳数索引 (按需加载)"]
            SIIdx["skip_idx_bf_a.idx: 按需"]
            SIMrk["skip_idx_bf_a.mrk2: 按需"]
        end

        subgraph DataFiles["列数据 (磁盘 I/O)"]
            EDBin["event_date.bin: 磁盘读取 + 解压"]
            ABin["a.bin: 磁盘读取 + 解压"]
            BBin["b.bin: 磁盘读取 + 解压"]
            CBin["c.bin: 磁盘读取 + 解压"]
        end
    end

    MemFiles --> MarkFiles --> SkipFiles --> DataFiles

    style DataFiles fill:#FFCDD2
    style MarkFiles fill:#FFF9C4
    style SkipFiles fill:#F0F4C3
```

### 每个文件的读取方式

| 文件 | 读取方式 | 缓存策略 | 磁盘 I/O |
|------|---------|---------|---------|
| `primary.idx` | 整个文件 (Part 加载时) | 内存常驻 | 启动时加载一次 |
| `partition.dat` | 整个文件 (Part 加载时) | 内存常驻 | 启动时加载一次 |
| `minmax_*.idx` | 整个文件 (Part 加载时) | 内存常驻 | 启动时加载一次 |
| `event_date.mrk2` | 按需 seek | MarkCache | 未命中时 read |
| `a.mrk2` | 按需 seek | MarkCache | 未命中时 read |
| `b.mrk2` | 按需 seek | MarkCache | 未命中时 read |
| `c.mrk2` | 按需 seek | MarkCache | 未命中时 read |
| `event_date.bin` | seek + read + decompress | UncompressedCache | read + LZ4 解压 |
| `a.bin` | seek + read + decompress | UncompressedCache | read + LZ4 解压 |
| `b.bin` | seek + read + decompress | UncompressedCache | read + LZ4 解压 |
| `c.bin` | seek + read + decompress | UncompressedCache | read + LZ4 解压 |
| `skip_idx_bf_a.idx` | 按需加载 | 无缓存 | read |
| `skip_idx_bf_a.mrk2` | 按需加载 | 无缓存 | read |
| `columns.txt` | 不读取 (metadata 中已有) | - | 无 |
| `count.txt` | 不读取 | - | 无 |
| `checksums.txt` | 不读取 (查询时无需校验) | - | 无 |

## 十、PREWHERE 列裁剪优化

```mermaid
sequenceDiagram
    participant Interp as InterpreterSelectQuery
    participant RW as TreeRewriter
    participant RFM as ReadFromMergeTree

    rect rgb(255, 245, 230)
        Note right of Interp: PREWHERE 自动优化
        Note over Interp: 原始查询:<br/>SELECT a, b, c FROM t<br/>WHERE event_date = '2025-04-15' AND a = 5
        Interp->>RW: 分析 WHERE 子句
        RW->>RW: 选择最优 PREWHERE 列
        Note over RW: 评估每列的过滤能力:<br/>event_date 等值: 选择性 ~0.08% (1/1200天)<br/>a 等值: 选择性未知
        Note over RW: event_date 有更高的过滤选择性<br/>同时 event_date 是主键第一列, 已被索引过滤
    end

    rect rgb(240, 255, 230)
        Note right of RFM: 两阶段读取
        Note over RFM: 阶段 1 (PREWHERE):<br/>只读 event_date 列<br/>8192 行 → 过滤后 82 行
        RFM->>RFM: 读取 event_date.bin
        RFM->>RFM: WHERE event_date = '2025-04-15'
        Note over RFM: 阶段 2 (普通列):<br/>只读 a, b, c 的 82 行数据<br/>而非全部 8192 行
        RFM->>RFM: 读取 a.bin, b.bin, c.bin (仅 82 行)
    end

    Note over RFM: 优势: b 和 c 列的数据量大幅减少<br/>尤其是 String 类型的 b 列<br/>节省大量磁盘 I/O 和内存
```

## 十一、多线程并行读取

```mermaid
flowchart TB
    subgraph Pool["MergeTreeReadPool"]
        direction TB
        Task1["Thread 1: Part 202504_1_1_0, Mark [15,16)"]
        Task2["Thread 2: Part 202504_1_1_0, Mark [17,19)"]
        Task3["Thread 3: Part 202504_2_2_0, Mark [15,16)"]
        Task4["Thread 4: Part 202504_3_3_0, Mark [15,16)"]
    end

    subgraph T1["Thread 1 执行"]
        T1R["MergeTreeReaderWide.readRows()"]
        T1P["prefetch: a.mrk2, b.mrk2, c.mrk2"]
        T1D["并行读: event_date.bin, a.bin, b.bin, c.bin"]
        T1F["WHERE 过滤 + SIMD"]
        T1R --> T1P --> T1D --> T1F
    end

    Task1 --> T1
    Task2 --> T2["Thread 2 执行 (同上)"]
    Task3 --> T3["Thread 3 执行 (同上)"]
    Task4 --> T4["Thread 4 执行 (同上)"]

    Note["线程数由 max_threads 设置控制<br/>默认 = CPU 核心数<br/>每个线程独立处理一个 Part + Mark Range 组合"]
```

## 十二、性能优化要点

```
1. 多级索引过滤
   分区裁剪 → Part 级 MinMax → 主键稀疏索引 → 跳数索引
   逐层淘汰无关数据, 最终只读极少量 Granule
   本例: 120000 Granule → 3 Granule, 过滤率 99.997%

2. 列裁剪
   只读 SELECT + WHERE 涉及的列, 不读其他列
   本例: 表可能有 20 列, 但只读 4 列 (a,b,c,event_date)
   I/O 量减少 ~80%

3. PREWHERE 优化
   先读过滤列 (event_date), 过滤后再读数据列 (a,b,c)
   避免读取注定被过滤掉的数据

4. 缓存体系
   MarkCache: 缓存 .mrk2 文件, 避免重复 seek
   UncompressedCache: 缓存解压后的数据块, 避免重复解压
   两者共同减少磁盘 I/O 和 CPU 解压开销

5. 多线程并行
   不同 Part / Mark Range 分配到不同线程
   列级别并行预取 (prefetch)
   充分利用多核 CPU 和磁盘 I/O 带宽

6. 稀疏索引 + 范围扫描
   主键索引极小 (每 8192 行仅 1 条记录)
   二分查找定位 Mark Range, 然后顺序扫描
   索引完全在内存中, 无磁盘 I/O
```

## 十三、关键源码文件索引

| 文件 | 职责 |
|------|------|
| `Interpreters/InterpreterSelectQuery.cpp` | SELECT 解释器主入口, buildQueryPlan, executeFetchColumns |
| `Interpreters/TreeRewriter.cpp` | AST 语法重写, requiredSourceColumns 收集 |
| `Core/QueryProcessingStage.h` | 查询处理阶段枚举 |
| `Processors/QueryPlan/ReadFromMergeTree.cpp` | 核心索引分析, selectRangesToRead |
| `Storages/MergeTree/MergeTreeDataSelectExecutor.cpp` | Part/Mark 选择, filterPartsByPartition, markRangesFromPKRange |
| `Storages/MergeTree/KeyCondition.cpp` | 主键条件 RPN 构建, checkInRange |
| `Storages/MergeTree/PartitionPruner.cpp` | 分区裁剪器 |
| `Storages/MergeTree/MergeTreeIndices.h` | 跳数索引基类 |
| `Storages/MergeTree/MergeTreeIndexBloomFilter.h` | BloomFilter 跳数索引 |
| `Storages/MergeTree/MergeTreeReadPool.cpp` | 多线程任务分发 |
| `Storages/MergeTree/MergeTreeReadTask.cpp` | 单线程读取任务 |
| `Storages/MergeTree/MergeTreeReadersChain.cpp` | PREWHERE 读取链 |
| `Storages/MergeTree/MergeTreeReaderWide.cpp` | Wide 格式列读取, readRows |
| `Storages/MergeTree/MergeTreeReaderStream.cpp` | Mark 文件 + 压缩数据读取 |
| `Storages/MergeTree/MergeTreeRangeReader.h` | Granule 范围读取器 |
