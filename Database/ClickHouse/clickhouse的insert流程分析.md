# ClickHouse INSERT 执行流程分析

> 基于 ClickHouse 源码分析, 追踪 `INSERT INTO DB_A.table_A (a, b, c) VALUES (1, 2, 3), ...` 的完整执行路径, 每一步标注了涉及的元数据查询和数据落盘细节

## 一、全链路总览

```mermaid
flowchart TB
    subgraph Parse["阶段 1: SQL 解析"]
        P1["ParserInsertQuery: 词法分析"]
        P2["ASTInsertQuery: 语法树"]
    end

    subgraph Resolve["阶段 2: 元数据解析"]
        R1["Context.resolveStorageID: 解析 DB 名"]
        R2["DatabaseCatalog.getTable: 查找 Table"]
        R3["IStorage.getInMemoryMetadata: 获取列定义"]
        R4["getSampleBlock: 匹配 (a,b,c) 列"]
        R5["checkAccess: 权限校验"]
    end

    subgraph Write["阶段 3: 数据写入"]
        W1["MergeTreeSink.consume: 接收数据"]
        W2["splitBlockByPartition: 按分区拆分"]
        W3["writeTempPart: 写临时 Part"]
        W4["排序: 按 ORDER BY"]
        W5["MergedBlockOutputStream: 写列数据"]
        W6["finalizePartOnDisk: 写元数据文件"]
    end

    subgraph Commit["阶段 4: 提交"]
        C1["renameTempPart: tmp → 正式名"]
        C2["Transaction.commit: 两阶段提交"]
        C3["background_operations.trigger: 触发合并"]
    end

    Parse --> Resolve --> Write --> Commit
```

## 二、SQL 解析阶段

```mermaid
sequenceDiagram
    participant Client as Client
    participant Parser as ParserInsertQuery
    participant AST as ASTInsertQuery

    Client->>Parser: INSERT INTO DB_A.table_A (a, b, c) VALUES (1, 'hello', 3.14), ...

    rect rgb(240, 245, 255)
        Note right of Parser: 词法分析
        Parser->>Parser: 匹配 INSERT INTO 关键字
        Parser->>Parser: 匹配可选 TABLE 关键字
        Parser->>Parser: ParserIdentifier 解析 DB_A.table_A
        Note over Parser: 分离: database=DB_A, table=table_A
    end

    rect rgb(255, 245, 240)
        Note right of Parser: 列列表解析
        Parser->>Parser: 检测左括号 (, 进入列列表模式
        Parser->>Parser: ParserList 解析 (a, b, c)
        Note over Parser: 每个元素: ParserIdentifier
        Parser->>Parser: 检测右括号 ), 退出列列表
    end

    rect rgb(245, 255, 240)
        Note right of Parser: 数据部分解析
        Parser->>Parser: 匹配 VALUES 关键字
        Parser->>Parser: 解析数据元组 (1, 'hello', 3.14), ...
        Note over Parser: data 指针 → 查询文本中数据起始位置
        Note over Parser: end 指针 → 查询文本中数据结束位置
    end

    Parser-->>AST: ASTInsertQuery
    Note over AST: database = ASTPtr(DB_A)<br/>table = ASTPtr(table_A)<br/>columns = ASTPtr([a, b, c])<br/>format = Values<br/>data = 指向查询文本的指针
```

**此阶段不查询任何元数据**, 纯粹的语法分析, 生成 AST。

## 三、元数据解析阶段 (关键步骤)

```mermaid
sequenceDiagram
    participant Interp as InterpreterInsertQuery
    participant Ctx as Context
    participant DB as DatabaseCatalog
    participant DBOnDisk as DatabaseOnDisk
    participant Table as IStorage (StorageMergeTree)
    participant Meta as StorageInMemoryMetadata
    participant Access as AccessChecker

    Interp->>Interp: getTable(query)

    rect rgb(255, 245, 230)
        Note right of Interp: Step 1: 解析数据库和表名
        Interp->>Ctx: resolveStorageID(DB_A, table_A)
        Note over Ctx: 如果 database 为空, 使用当前数据库
        Note over Ctx: 查询 DatabaseCatalog.getDatabase(DB_A)
        Note over Ctx: metadata 文件: 无 I/O (内存查找)
    end

    rect rgb(240, 255, 230)
        Note right of DB: Step 2: 查找表对象
        Interp->>DB: getTable(DB_A, table_A)
        DB->>DBOnDisk: getTable(table_A)
        Note over DBOnDisk: 从内存 tables map 查找 (启动时从 .sql 加载)
        Note over DBOnDisk: metadata: 读取 DB_A/table_A.sql (启动时已加载)
        DBOnDisk-->>DB: StoragePtr (IStorage)
        DB-->>Interp: StoragePtr
    end

    rect rgb(245, 240, 255)
        Note right of Interp: Step 3: 获取表结构
        Interp->>Table: getInMemoryMetadataPtr(context)
        Table-->>Interp: StorageInMemoryMetadata
        Note over Meta: 包含:<br/>columns: NamesAndTypesList [(a, UInt32), (b, String), (c, Float64), ...]<br/>partition_key: PARTITION BY 表达式<br/>sorting_key: ORDER BY 表达式<br/>primary_key: PRIMARY KEY 表达式<br/>secondary_indices: 跳数索引<br/>settings: index_granularity, compress 等
    end

    rect rgb(255, 230, 245)
        Note right of Interp: Step 4: 列匹配
        Interp->>Interp: getSampleBlock(query, table, metadata)
        Note over Interp: AST 列: [a, b, c]<br/>表列: [a UInt32, b String, c Float64, id UInt64, event_date Date, ...]<br/>匹配: a→UInt32, b→String, c→Float64<br/>不匹配: 抛 NO_SUCH_COLUMN_IN_TABLE
    end

    rect rgb(230, 255, 255)
        Note right of Interp: Step 5: 权限校验
        Interp->>Access: checkAccess(INSERT, DB_A.table_A, [a, b, c])
        Note over Access: 查询 Grant 权限表<br/>检查当前用户是否有 INSERT 权限
    end

    Interp->>Table: lockForShare()
    Note over Table: 获取共享读锁, 防止并发 ALTER/DROP
```

### 此阶段涉及的元数据查询汇总

| 步骤 | 查询内容 | 数据来源 | 是否磁盘 I/O |
|------|---------|---------|------------|
| 解析 DB 名 | database 存在性 | DatabaseCatalog 内存 map | 否 |
| 查找表 | table 对象 | DatabaseOnDisk.tables map | 否 (启动时从 .sql 加载) |
| 表结构 | 列名、类型、分区键、排序键 | StorageInMemoryMetadata | 否 (内存中) |
| 列匹配 | (a,b,c) → 类型 | metadata.getSampleBlockInsertable() | 否 |
| 权限 | INSERT 权限 | AccessRights | 否 |

## 四、数据写入阶段

```mermaid
sequenceDiagram
    participant Sink as MergeTreeSink
    participant Writer as MergeTreeDataWriter
    participant Part as MergeTreeDataPartInfo
    participant Out as MergedBlockOutputStream
    participant Disk as IDisk (DiskLocal)
    participant FS as 操作系统

    Sink->>Sink: consume(Block)

    rect rgb(255, 245, 230)
        Note right of Sink: Step 1: Schema 校验
        Sink->>Writer: splitBlockIntoParts(block)
        Writer->>Writer: metadata.check(block, true)
        Note over Writer: 校验 Block 的列数和类型<br/>是否与表的 metadata 匹配<br/>metadata: 内存中的 StorageInMemoryMetadata
    end

    rect rgb(240, 255, 230)
        Note right of Writer: Step 2: 分区键求值
        Writer->>Writer: MergeTreePartition.execute(block)
        Note over Writer: 如果 PARTITION BY toYYYYMM(event_date):<br/>  → 求值每行的 event_date 列<br/>  → 生成分区值 "202504"<br/>如果无分区键: 整个 Block 作为一个 Part
    end

    rect rgb(245, 240, 255)
        Note right of Writer: Step 3: 排序键求值
        Writer->>Writer: sortingKeyExpr.execute(block)
        Note over Writer: ORDER BY (a, b, c):<br/>  → 已有 a, b, c 列, 无需额外计算<br/>如果 ORDER BY 包含表达式:<br/>  → 求值并添加到 Block
    end

    rect rgb(255, 230, 240)
        Note right of Writer: Step 4: 计算 MinMax
        Writer->>Writer: minmax_idx.update(block, partition_columns)
        Note over Writer: 记录分区列的最小值和最大值<br/>partition_columns = metadata 中 PARTITION BY 列<br/>→ 用于后续 partition pruning
    end

    rect rgb(240, 245, 255)
        Note right of Writer: Step 5: 计算 Part 名称
        Writer->>Part: compute Part name
        Note over Part: partition_id = "202504"<br/>min_block = 当前 block number<br/>max_block = 当前 block number<br/>level = 0 (新 Part)<br/>→ Part name: "202504_1_1_0"
    end

    rect rgb(255, 255, 230)
        Note right of Writer: Step 6: 排序
        Writer->>Writer: stableGetPermutation(block, sort_desc)
        Note over Writer: 如果 ORDER BY (a, b, c):<br/>  → 按 a 升序, a 相同按 b 升序, b 相同按 c 升序<br/>生成排列数组 perm[]<br/>isAlreadySorted? → 如果已排序, 跳过
    end

    rect rgb(230, 255, 255)
        Note right of Writer: Step 7: 选择压缩算法
        Writer->>Writer: context.chooseCompressionCodec(0, 0)
        Note over Writer: 读取 config.xml 中的 compression 配置<br/>无配置 → 默认 LZ4<br/>也可以在 CREATE TABLE 中指定 CODEC(ZSTD)
    end

    rect rgb(245, 230, 255)
        Note right of Writer: Step 8: 创建输出流
        Writer->>Out: MergedBlockOutputStream(part, codec, granularity)
        Out->>Disk: createDirectories(tmp_insert_202504_1_1_0)
        Disk->>FS: mkdir (磁盘 I/O)
    end

    rect rgb(255, 240, 230)
        Note right of Out: Step 9: 写列数据
        Out->>Out: writeWithPermutation(block, perm)
        par 并行写 3 列
            Out->>Out: 写 a.bin (UInt32 数据, LZ4 压缩)
            and
            Out->>Out: 写 b.bin (String 数据, LZ4 压缩)
            and
            Out->>Out: 写 c.bin (Float64 数据, LZ4 压缩)
        end
        Note over Out: 每列的写入链:<br/>序列化 → HashingWB → CompressedWB → HashingWB → WriteBufferFromFile<br/>Granule 粒度 (默认 8192 行) 切换时生成 Mark
        Out->>Out: 写 a.mrk2, b.mrk2, c.mrk2 (Mark 文件)
    end

    rect rgb(240, 230, 255)
        Note right of Out: Step 10: 写主键索引
        Out->>Out: write primary.idx
        Note over Out: 每 Granule 第一行的 (a, b, c) 值<br/>稀疏索引, 只记录 8192 行中的第 1 行
    end
```

### 写入阶段元数据查询汇总

| 步骤 | 查询的元数据 | 用途 |
|------|------------|------|
| Schema 校验 | metadata.columns | 验证 Block 列与表定义一致 |
| 分区键求值 | metadata.partition_key | 计算 PARTITION BY 值 |
| 排序键求值 | metadata.sorting_key | 构建 SortDescription |
| MinMax | metadata.partition_key 列 | 记录分区列最值 |
| 排序 | metadata.sorting_key 列 | 生成排列顺序 |
| 主键索引 | metadata.primary_key | 选择索引列写入 primary.idx |
| 压缩算法 | context config.xml | 选择 LZ4/ZSTD/默认 |
| Granularity | metadata.settings.index_granularity | 每 8192 行生成 Mark |
| 列名+类型 | metadata.columns | 写 columns.txt |

## 五、元数据文件落盘阶段

```mermaid
sequenceDiagram
    participant Out as MergedBlockOutputStream
    participant Part as IMergeTreeDataPart
    participant Disk as DiskLocal
    participant FS as 操作系统

    Out->>Out: finalizePartOnDisk()

    rect rgb(255, 245, 230)
        Note right of Out: Step 1: 刷列数据到磁盘
        Out->>Out: preFinalize() 所有列写入流
        Note over Out: compressed_hashing.finalize()<br/>compressor.finalize() (刷新剩余数据)<br/>plain_hashing.finalize()<br/>→ a.bin, b.bin, c.bin 写入完成
        Out->>Disk: fsync (可选)
        Disk->>FS: fsync 系统调用 (确保数据落盘)
    end

    rect rgb(240, 255, 230)
        Note right of Out: Step 2: 写控制文件
        par 控制文件写入 (每次: writeFile → HashingWriteBuffer → 写内容 → 记录 checksum)
            Out->>Disk: writeFile("partition.dat")
            Note over FS: 存储分区键值 (如 "202504")
            Out->>Disk: writeFile("minmax_a.idx")
            Note over FS: 分区列 a 的最小最大值
            Out->>Disk: writeFile("minmax_c.idx")
            Note over FS: 分区列 c 的最小最大值
            Out->Disk: writeFile("columns.txt")
            Note over FS: 列名和类型列表
            Out->Disk: writeFile("count.txt")
            Note over FS: 总行数
            Out->Disk: writeFile("checksums.txt")
            Note over FS: 所有文件的 CityHash128 + 大小
        end
    end

    rect rgb(245, 240, 255)
        Note right of Out: Step 3: fsync
        Out->Disk: sync()
        Disk->>FS: fsync 所有文件 (确保元数据落盘)
    end

    Out-->>Part: Part 已 finalize, 文件写入完成

    Note over Part: tmp_insert_202504_1_1_0/ 目录下:<br/>a.bin, a.mrk2, b.bin, b.mrk2, c.bin, c.mrk2<br/>primary.idx<br/>partition.dat, minmax_*.idx<br/>columns.txt, count.txt, checksums.txt
```

## 六、两阶段提交阶段

```mermaid
sequenceDiagram
    participant Sink as MergeTreeSink
    participant Data as MergeTreeData
    participant Lock as PartsLock
    participant FS as 操作系统

    Sink->>Sink: finishDelayedChunk()

    rect rgb(255, 245, 230)
        Note right of Sink: Step 1: 开始事务
        Sink->>Data: Transaction(storage)
        Note over Data: 创建两阶段提交事务对象
    end

    rect rgb(240, 255, 230)
        Note right of Sink: Step 2: 获取 Parts 锁
        Sink->>Lock: lockParts()
        Note over Lock: 防止并发修改 data_parts 集合
    end

    rect rgb(245, 240, 255)
        Note right of Sink: Step 3: 填充最终 Part 名称
        Sink->>Data: fillNewPartName(part)
        Note over Data: 查询 data_parts 中的 max_block number<br/>metadata: data_parts (内存中)<br/>新 Part: "202504_1_1_0" → "202504_1_1_0" (首次)
        Note over Data: 后续 INSERT: "202504_2_2_0", "202504_3_3_0" ...
    end

    rect rgb(255, 230, 240)
        Note right of Sink: Step 4: 原子重命名
        Sink->>FS: rename("tmp_insert_202504_1_1_0", "202504_1_1_0")
        Note over FS: 这是一个原子操作<br/>metadata: 无 (直接磁盘操作)
    end

    rect rgb(230, 255, 255)
        Note right of Sink: Step 5: 两阶段提交
        Sink->>Data: Phase 1: tmp → PreActive
        Sink->>Data: Phase 2: PreActive → Active
        Note over Data: Active: data_parts 集合新增 Part<br/>metadata: data_parts (内存中更新)<br/>→ 其他查询现在可以看到这个 Part
    end

    rect rgb(255, 255, 230)
        Note right of Sink: Step 6: 触发后台任务
        Sink->>Data: background_operations.trigger()
        Note over Data: 检查 Part 数量是否超过阈值<br/>metadata: data_parts.size() (内存中)<br/>如果超过 → 触发后台合并
    end
```

## 七、数据落盘完整文件列表

```mermaid
flowchart TB
    subgraph PartDir["202504_1_1_0/ (最终 Part 目录)"]
        direction TB

        subgraph DataFiles["列数据文件 (磁盘 I/O: 压缩写入)"]
            ABin["a.bin: UInt32 列压缩数据"]
            AMrk["a.mrk2: Mark 文件 (Granule 偏移)"]
            BBin["b.bin: String 列压缩数据"]
            BMrk["b.mrk2: Mark 文件"]
            CBin["c.bin: Float64 列压缩数据"]
            CMrk["c.mrk2: Mark 文件"]
        end

        subgraph IndexFiles["索引文件"]
            PIdx["primary.idx: 稀疏主键索引"]
        end

        subgraph MetaFiles["元数据文件"]
            PData["partition.dat: 分区键值"]
            MM["minmax_a.idx: 列 a 最小最大值"]
            MMB["minmax_c.idx: 列 c 最小最大值"]
            Cols["columns.txt: 列名 + 类型"]
            Cnt["count.txt: 总行数"]
            Chk["checksums.txt: 所有文件校验和"]
        end
    end

    ABin --> AMrk
    BBin --> BMrk
    CBin --> CMrk
```

### 每个文件的落盘细节

| 文件 | 写入方式 | Buffer 链 | 磁盘 I/O |
|------|---------|---------|---------|
| `a.bin` | 逐 Granule | Serialize → HashingWB → **CompressedWB** → HashingWB → WriteBufferFromFile | write() + fsync |
| `a.mrk2` | 逐 Granule | 计算偏移 → HashingWB → CompressedWB → HashingWB → WriteBufferFromFile | write() |
| `b.bin` | 逐 Granule | 同 a.bin, 但 String 列数据更大 | write() |
| `primary.idx` | 每 Granule 第一行 | Serialize → (可选) CompressedWB → HashingWB → WriteBufferFromFile | write() |
| `partition.dat` | 一次性 | Serialize → HashingWB → WriteBufferFromFile | write() |
| `minmax_*.idx` | 一次性 | Serialize → HashingWB → WriteBufferFromFile | write() |
| `columns.txt` | 一次性 | NamesAndTypesList.writeText → HashingWB → WriteBufferFromFile | write() |
| `count.txt` | 一次性 | toString → HashingWB → WriteBufferFromFile | write() |
| `checksums.txt` | 所有文件写完后 | 遍历 checksum map → HashingWB → WriteBufferFromFile | write() + fsync |

## 八、全程元数据访问热力图

```mermaid
flowchart LR
    subgraph Hot["热路径 (每次 INSERT 都访问, 内存中)"]
        H1["data_parts (Part 集合)"]
        H2["StorageInMemoryMetadata (表结构)"]
        H3["partition_key 表达式"]
        H4["sorting_key 表达式"]
        H5["primary_key 表达式"]
        H6["columns (NamesAndTypesList)"]
        H7["index_granularity 设置"]
        H8["compression codec 配置"]
    end

    subgraph Warm["温路径 (间接或条件访问)"]
        W1[".sql 文件 (启动时加载一次)"]
        W2["DatabaseCatalog (DB 查找)"]
        W3["AccessRights (权限检查)"]
        W4["data_parts.size() (合并阈值)"]
        W5["max_block_number (Part 命名)"]
    end

    subgraph Cold["冷路径 (磁盘 I/O, 尽量少)"]
        C1["创建 tmp 目录 (mkdir)"]
        C2["写 .bin 文件 (write + fsync)"]
        C3["写 .mrk2 文件 (write)"]
        C4["写 primary.idx (write)"]
        C5["写元数据文件 (write)"]
        C6["rename (原子重命名)"]
        C7["checksums.txt fsync"]
    end
```

## 九、性能优化要点

```
1. 零元数据 I/O 设计
   所有元数据 (表结构, Part 列表, 设置) 在内存中
   INSERT 过程中不读取任何磁盘元数据文件
   .sql 文件仅在启动时加载一次

2. 批量写入效率
   一次 INSERT 批量写入, 不是逐行
   排序后顺序写入, 利用磁盘顺序写优势
   压缩减少磁盘 I/O 量

3. 延迟提交
   Part 先写为 tmp_, 数据就绪后再原子 rename
   rename 是原子操作, 保证查询不会看到不完整 Part

4. 后台合并异步化
   INSERT 路径完全不等待合并
   合并由后台线程独立触发
   INSERT 延迟 = 排序 + 写磁盘 + rename (不含合并)
```
