# Apache Doris 存储层级结构详解

## 一、层级关系总览

```
Database
  └── Table (OlapTable)
        ├── Partition 1 (按 Range 分区)
        │     └── MaterializedIndex (Base Rollup / Rollup)
        │           ├── Tablet (Bucket 0)  ← 副本 1 on BE1, 副本 2 on BE2, 副本 3 on BE3
        │           ├── Tablet (Bucket 1)
        │           ├── Tablet (Bucket 2)
        │           └── ...
        ├── Partition 2
        │     └── MaterializedIndex
        │           ├── Tablet (Bucket 0)
        │           └── ...
        └── ...
```

| 层级 | 对应概念 | 划分方式 | 数据关系 | 存储位置 |
|------|---------|---------|---------|---------|
| **Database** | 数据库 | 逻辑划分 | — | FE 元数据 |
| **Table** | OLAP 表 | — | — | FE 元数据 |
| **Partition** | 数据分区 | RANGE / LIST 分区 | 数据不重叠 | FE 元数据 + BE |
| **MaterializedIndex** | 物化视图 (Rollup) | 预聚合列 | 同一 Partition 数据的不同表示 | FE 元数据 |
| **Tablet** | 存储单元 | HASH 分桶 (Bucket) | 同一分区内按 Hash 互斥 | BE 磁盘 |
| **Rowset** | 数据版本 | 事务版本 | 同一 Tablet 内版本递增 | BE 磁盘 |
| **Segment** | 数据文件 | MemTable Flush 粒度 | 一个 Rowset 内多个 Segment | BE 磁盘 (.dat) |
| **Page** | 数据页 | PageBuilder 粒度 (~64KB) | 一个 Segment 内按列存储 | Segment 文件内 |

---

## 二、Partition（分区）

### 2.1 概念

Partition 是表的**逻辑数据划分**，按 Range 或 List 条件将数据分散到不同分区。

```sql
-- Range 分区
CREATE TABLE sales (
    dt DATE,
    region VARCHAR,
    amount DECIMAL
)
PARTITION BY RANGE(dt) (
    PARTITION p202401 VALUES [("2024-01-01"), ("2024-02-01")),
    PARTITION p202402 VALUES [("2024-02-01"), ("2024-03-01"))
)
DISTRIBUTED BY HASH(region) BUCKETS 8;
```

### 2.2 FE 上的元数据

```
Partition {
    id:           long          // 分区 ID
    name:         String        // 分区名 ("p202401")
    visibleVersion: long        // 查询可见的最大版本
    nextVersion:    long        // 下一个待分配版本号
    committedVersionHash: long  // 提交时版本哈希
    distributionInfo: DistributionInfo  // 分桶信息 (buckets 数量)
    isMutable:     boolean      // 是否可写
}
```

### 2.3 Partition 与 Tablet 的映射

```
Partition p202401 (BUCKETS=8)
  ├── Bucket 0 → Tablet 10001 (3 副本: BE1, BE2, BE3)
  ├── Bucket 1 → Tablet 10002 (3 副本: BE4, BE5, BE6)
  ├── Bucket 2 → Tablet 10003
  ├── ...
  └── Bucket 7 → Tablet 10008
```

**映射公式**：`bucket_id = Hash(distribution_columns) % num_buckets`

每个 Partition 的 Bucket 数量在建表时指定，同一 Partition 内的 Bucket 按 Hash 互斥分配数据。

---

## 三、Tablet（存储单元）

### 3.1 概念

Tablet 是 Doris 的**最小物理存储单元**，对应一个 Partition 的一个 Bucket。每个 Tablet 有 3 个副本分布在不同 BE 上。

### 3.2 磁盘目录结构

```
{storage_root}/data/{shard_id}/{tablet_id}/{schema_hash}/
    ├── {rowset_id}_0.dat          ← Segment 文件
    ├── {rowset_id}_1.dat
    ├── {rowset_id}_2.dat
    └── ...
```

**路径生成规则**：

```
/storage_root/data/    ← 固定前缀 (DATA_PREFIX = "/data")
    {shard_id}/         ← 0~1023 轮询分配 (MAX_SHARD_NUM = 1024)
        {tablet_id}/    ← Tablet ID
            {schema_hash}/  ← Schema 版本 (Schema Change 时变化)
                {rowset_id}_{seg_id}.dat  ← Segment 文件
```

**Shard 机制**：将 Tablet 分散到 1024 个子目录中，避免单个目录下文件过多。

### 3.3 Tablet 元数据

```
TabletMetaPB {
    table_id:            int64
    partition_id:        int64
    tablet_id:           int64
    schema_hash:         int32
    shard_id:            int32          // 磁盘目录分片 (0~1023)
    tablet_uid:          PUniqueId      // 唯一实例标识 (防 clone 冲突)
    cumulative_layer_point: int64      // 累积层分界点
    preferred_rowset_type: BetaRowset  // 优先使用 segment v2
    storage_medium:      HDD/SSD       // 存储介质

    schema:              TabletSchemaPB  // 完整的表 Schema
    rs_metas:            [RowsetMetaPB]  // 活跃 Rowset 列表
    stale_rs_metas:      [RowsetMetaPB]  // Compaction 后待清理的 Rowset
}
```

### 3.4 Tablet 的版本链

```
Tablet 内的 Rowset 版本链示例:

活跃 (_rs_version_map):
  [1,1] → Rowset (初始数据)
  [2,2] → Rowset (第一次导入)
  [3,3] → Rowset (第二次导入)
  [4,4] → Rowset (第三次导入)

Stale (_stale_rs_version_map):
  [1,2] → Rowset (Compaction 合并 [1,1]+[2,2] 后的旧版本)

查询 snapshot=4 时的版本路径: [[1,2], [3,3], [4,4]]
```

---

## 四、Rowset（数据版本）

### 4.1 概念

Rowset 是一次**事务提交**产生的一批数据的集合。一个 Tablet 包含多个 Rowset，每个 Rowset 关联一个版本区间 `[start_version, end_version]`。

### 4.2 Rowset 的生命周期

```
创建: Stream Load → MemTable Flush → 生成 Rowset (version=-1, PENDING)
发布: FE Commit + Publish → version=N, VISIBLE
合并: Compaction → 多个 Rowset → 一个新 Rowset (version=[first, last])
清理: Stale Rowset 过期 → 文件删除 + 空间回收
```

### 4.3 Rowset 元数据

```
RowsetMetaPB {
    rowset_id:        int64          // Rowset ID
    partition_id:     int64
    tablet_id:        int64
    txn_id:           int64          // 产生该 Rowset 的事务
    tablet_schema_hash: int32
    start_version:    int64          // [2
    end_version:      int64          //  2] 单版本为 delta
    version_hash:     int64
    num_rows:         int64          // 行数
    total_disk_size:  int64          // 总磁盘大小
    data_disk_size:   int64          // 数据页大小
    index_disk_size:  int64          // 索引页大小
    num_segments:     int64          // Segment 文件数量
    segments_overlap: OVERLAPPING / NONOVERLAPPING
    rowset_state:     PREPARED / COMMITTED / VISIBLE
    rowset_type:      ALPHA / BETA   // segment 格式
    empty:            bool           // 空 Rowset
    delete_flag:      bool           // 是否为 Delete 操作生成
    load_id:          PUniqueId      // 负载标识
    tablet_uid:       PUniqueId
    creation_time:    int64
}
```

### 4.4 Rowset 与 Segment 的关系

```
Rowset (version=[2,2], num_segments=3):
  ├── {rowset_id}_0.dat  ← 第 1 次 MemTable Flush
  ├── {rowset_id}_1.dat  ← 第 2 次 MemTable Flush
  └── {rowset_id}_2.dat  ← 第 3 次 MemTable Flush

Compaction 后的 Rowset (version=[2,5], num_segments=1):
  └── {rowset_id}_0.dat  ← 合并后的单个 Segment
```

- **写入时**：每次 MemTable Flush 生成一个 Segment，一个 Rowset 可能有多个 Segment
- **Compaction 后**：输出 Rowset 通常只有一个 Segment，数据有序且无重叠

---

## 五、Segment（数据文件）

### 5.1 概念

Segment 是 Rowset 中的**物理文件**（`.dat`），采用列式存储格式。一个 Segment 包含所有列的数据页和索引页。

### 5.2 Segment 文件的磁盘布局

```
+================================================================+
|                    SEGMENT FILE (.dat)                            |
+================================================================+
|                                                                  |
|  ┌─ DATA SECTION (按列写入，列内按页顺序) ──────────────────────┐  |
|  │                                                              │  |
|  │  Column 0 (id, INT):                                        │  |
|  │    [DataPage0][DataPage1]...[DataPageN]                      │  |
|  │                                                              │  |
|  │  Column 1 (name, VARCHAR):                                   │  |
|  │    [DataPage0][DataPage1]...[DataPageN][DictPage]            │  |
|  │    ^^^^^^^^^^^^                                             │  |
|  │    DICT_ENCODING 列有独立的字典页                             │  |
|  │                                                              │  |
|  │  Column 2 (amount, DECIMAL):                                │  |
|  │    [DataPage0][DataPage1]...[DataPageN]                      │  |
|  │                                                              │  |
|  │  ...                                                        │  |
|  │  Column N:                                                  │  |
|  │    [DataPage0]...[DataPageN][DictPage]                       │  |
|  └──────────────────────────────────────────────────────────────┘  |
|                                                                  |
|  ┌─ INDEX SECTION (按列写入) ─────────────────────────────────┐  |
|  │                                                              │  |
|  │  Per-Column Ordinal Index Pages                             │  |
|  │  Per-Column Zone Map Index Pages                            │  |
|  │  Per-Column Bitmap Index Pages (if enabled)                 │  |
|  │  Per-Column Bloom Filter Index Pages (if enabled)           │  |
|  │                                                              │  |
|  └──────────────────────────────────────────────────────────────┘  |
|                                                                  |
|  ┌─ SHORT KEY INDEX (单页) ───────────────────────────────────┐  |
|  │  [ShortKeyPage]                                             │  |
|  └──────────────────────────────────────────────────────────────┘  |
|                                                                  |
|  ┌─ FOOTER (文件末尾 12 字节固定尾 + 可变 protobuf) ──────────┐  |
|  │  [SegmentFooterPB protobuf]                                │  |
|  │  [FooterSize: 4B uint32 LE]                                │  |
|  │  [FooterChecksum: 4B CRC32C]                               │  |
|  │  [MagicNumber: 4B "D0R1"]                                  │  |
|  └──────────────────────────────────────────────────────────────┘  |
+================================================================+
```

### 5.3 Segment Footer (Protobuf)

```
SegmentFooterPB {
    version:           uint32       // Segment 格式版本
    columns:           [ColumnMetaPB]  // 每列的元数据 (编码、压缩类型、索引位置)
    num_rows:          uint32       // 总行数
    index_footprint:   uint64       // 索引区总大小
    data_footprint:    uint64       // 数据区总大小
    raw_data_footprint: uint64      // 压缩前原始大小
    compress_type:     LZ4F         // 默认压缩算法
    short_key_index_page: PagePointerPB  // Short Key 索引页的位置
}
```

### 5.4 文件读取流程

```
读取一个 Segment:
  1. 读最后 4 字节 → 验证 Magic "D0R1"
  2. 读倒数 5~8 字节 → FooterChecksum (CRC32C)
  3. 读倒数 9~12 字节 → FooterSize
  4. 读取 [FooterSize] 字节的 SegmentFooterPB → 解析列元数据
  5. 根据查询条件选择需要的列
  6. 从 SegmentFooter 获取各列的 OrdinalIndex 位置 → 精确定位 Data Page
```

---

## 六、Page（数据页）

### 6.1 Page 的磁盘格式

```
+====================================================+
|                      PAGE                           |
+====================================================+
|                                                     |
|  [PageBody]                                         │
|    编码后的数据 (可能经过 LZ4F 压缩)                │
|    DATA_PAGE: encoded_values + [nullmap]            │
|    DICT_PAGE: dictionary values (PLAIN 编码)        │
|    INDEX_PAGE: index entries                        │
|                                                     |
|  [PageFooterPB]                                     │
|    type:          DATA_PAGE / INDEX_PAGE /           │
|                   DICTIONARY_PAGE / SHORT_KEY_PAGE  │
|    uncompressed_size: 压缩前的原始大小               │
|    type-specific fields (num_values, ordinal 等)    │
|                                                     |
|  [FooterSize: 4B uint32 LE]                        │
|                                                     |
|  [CRC32C: 4B] ← 校验 body + footer + size          │
|                                                     |
+====================================================+
最小 8 字节 (footer_size + checksum)
典型 ~64KB (data_page_size)
```

**判断是否压缩**：`body_size != footer.uncompressed_size` → 已压缩

### 6.2 不同 Page 类型

| Page 类型 | 内容 | 编码方式 | 压缩 |
|-----------|------|---------|------|
| **DATA_PAGE** | 编码后的列值 + null bitmap | BitShuffle / DictEncoding / RLE / FOR | LZ4F |
| **DICTIONARY_PAGE** | 字符串字典 (唯一值列表) | PLAIN | LZ4F |
| **INDEX_PAGE** | Ordinal Index / Zone Map / Bitmap / Bloom Filter | 按 Index 类型 | LZ4F |
| **SHORT_KEY_PAGE** | 前 N 列的排序键 + 行号偏移 | 专有格式 | LZ4F |

### 6.3 Page 与 Block 的区别

在 Doris 中，**Page** 和 **Block** 是两个不同的概念：

| 维度 | Page (存储层) | Block (执行层) |
|------|-------------|---------------|
| **定义** | Segment 文件中的最小 I/O 单位 | 执行引擎中的数据批处理单元 |
| **大小** | ~64KB (data_page_size) | ~4096 行 (batch_size) |
| **存储** | 磁盘上列式存储 | 内存中行式存储 |
| **结构** | 单列数据 | 多列数据 (一行多列) |
| **粒度** | 按列存储，每列独立 Page | 按行存储，一个 Block 含多列 |
| **生命周期** | 持久化到磁盘 | 查询执行时存在于内存 |

```
Page → Block 的转换过程:

Segment 文件 (列式 Pages):
  Col0: [Page0(rows 0~1023)] [Page1(rows 1024~2047)]
  Col1: [Page0(rows 0~1023)] [Page1(rows 1024~2047)]
  Col2: [Page0(rows 0~1023)] [Page1(rows 1024~2047)]

    ↓ 查询执行时按需读取和解码

内存中的 Block (行式):
  Block 0 (rows 0~1023):
    Col0: [val0, val1, ..., val1023]
    Col1: [val0, val1, ..., val1023]
    Col2: [val0, val1, ..., val1023]
```

---

## 七、完整的数据定位路径

### 7.1 从 SQL 查询到磁盘 Page

```
SELECT region, SUM(amount) FROM sales WHERE dt = '2024-01-15'

  Step 1: FE 路由
    Table → Partition (p202401, dt range 匹配)
    → MaterializedIndex (Base Rollup)
    → Tablet (Bucket 0~7, 需要扫描所有 Bucket)

  Step 2: BE 收到扫描请求
    Tablet → Rowset (visibleVersion 下的所有 Rowset)
    → VersionGraph (capture_consistent_versions)
    → Segment (.dat 文件)

  Step 3: Segment 内定位
    SegmentFooter → ColumnMetaPB (获取 region 列的 OrdinalIndex 位置)
    → OrdinalIndex (定位 Data Page)
    → Zone Map (过滤不满足条件的 Page)
    → Bloom Filter (进一步过滤)
    → Data Page (磁盘读取)

  Step 4: Page 解码
    CRC32C 校验 → LZ4F 解压 (如已压缩)
    → DictEncoding 解码 (码字→字符串)
    → BitShuffle 解码 (整数)
    → 构造 Block (列→行转换)

  Step 5: 执行计算
    Block → SUM(amount) 聚合 → 返回结果
```

### 7.2 存储层级的大小参考

| 层级 | 典型数量 | 典型大小 | 说明 |
|------|---------|---------|------|
| Database | 10~100 | — | 逻辑划分 |
| Table | 100~1000 | — | 含所有分区的总数据 |
| Partition | 10~3650 | 10GB~1TB | 按天分区，一年 365 个 |
| Tablet | 8~数百/分区 | 100MB~10GB | Bucket 数量决定 |
| Rowset | 1~100/查询 | 10MB~1GB | 版本数取决于 Compaction 速度 |
| Segment | 1~10/Rowset | 64MB~256MB | 每次 Flush 约 256MB |
| Page | 数千/Segment | ~64KB | data_page_size = 64KB |
| Block (内存) | 数千/查询 | ~4096 行 | batch_size = 4096 |

---

## 八、层级关系图

```
┌─────────────────────────────────────────────────────────────┐
│                        Table (OLAP)                          │
│  columns: (dt, region, amount, ...)                          │
│  distributed by: HASH(region) BUCKETS 8                      │
│  partitioned by: RANGE(dt)                                   │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌─── Partition p202401 ─────────────────────────────────┐  │
│  │  range: ['2024-01-01', '2024-02-01')                  │  │
│  │  visibleVersion=10                                     │  │
│  │  nextVersion=11                                        │  │
│  │                                                        │  │
│  │  ┌── MaterializedIndex (Base) ─────────────────────┐  │  │
│  │  │                                                  │  │  │
│  │  │  ┌── Tablet 10001 (Bucket 0) ────────────────┐  │  │  │
│  │  │  │  3 replicas: BE1, BE2, BE3                │  │  │  │
│  │  │  │                                           │  │  │  │
│  │  │  │  Rowset [1,1] → 1 Segment (base data)    │  │  │  │
│  │  │  │  Rowset [2,3] → 2 Segments (cumulative)  │  │  │  │
│  │  │  │  Rowset [4,4] → 1 Segment (delta)        │  │  │  │
│  │  │  │                                           │  │  │  │
│  │  │  │  Rowset [1,1]:                            │  │  │  │
│  │  │  │    Segment 0.dat:                         │  │  │  │
│  │  │  │      Col0 [Page0][Page1]...              │  │  │  │
│  │  │  │      Col1 [Page0][Page1]...[DictPage]    │  │  │  │
│  │  │  │      Col2 [Page0][Page1]...              │  │  │  │
│  │  │  │      [OrdinalIdx][ZoneMap][BloomFilter]  │  │  │  │
│  │  │  │      [ShortKeyIdx]                       │  │  │  │
│  │  │  │      [Footer][Size][CRC32C]["D0R1"]      │  │  │  │
│  │  │  │                                           │  │  │  │
│  │  │  └───────────────────────────────────────────┘  │  │  │
│  │  │                                                  │  │  │
│  │  │  ┌── Tablet 10002 (Bucket 1) ────────────────┐  │  │  │
│  │  │  │  3 replicas: BE4, BE5, BE6                │  │  │  │
│  │  │  │  Rowset [1,1] → 1 Segment                 │  │  │  │
│  │  │  │  Rowset [2,3] → 2 Segments                 │  │  │  │
│  │  │  │  Rowset [4,4] → 1 Segment                 │  │  │  │
│  │  │  └───────────────────────────────────────────┘  │  │  │
│  │  │                                                  │  │  │
│  │  │  ... Tablet 10003 ~ 10008 ...                    │  │  │
│  │  └──────────────────────────────────────────────────┘  │  │
│  └────────────────────────────────────────────────────────┘  │
│                                                              │
│  ┌─── Partition p202402 ─────────────────────────────────┐  │
│  │  range: ['2024-02-01', '2024-03-01')                  │  │
│  │  ... (same structure)                                  │  │
│  └────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

---

## 九、各层级的核心操作

| 操作 | 涉及层级 | 说明 |
|------|---------|------|
| CREATE TABLE | Table → Partition → Tablet | FE 创建元数据，BE 创建 Tablet 目录 |
| INSERT / LOAD | Tablet → Rowset → Segment → Page | 数据写入，生成新版本 Rowset |
| SELECT | Partition → Tablet → Rowset → Segment → Page | 版本快照 + 索引过滤 + Page 读取 |
| DELETE | Tablet → Rowset (DeletePredicate) | 逻辑删除，Compaction 时物理删除 |
| ALTER TABLE | Tablet (Schema Change) | 新 schema_hash 下的新 Tablet |
| COMPACTION | Tablet → Rowset → Segment → Page | 合并 Rowset，去重，重新压缩 |
| CLONE | Tablet → Rowset → Segment | 整 Tablet 复制修复副本 |
| DROP PARTITION | Partition → Tablet → Rowset → Segment | 递归删除磁盘文件 |

---

## 十、与 3FS 存储层级的对比

| 维度 | Doris | 3FS |
|------|-------|-----|
| **数据划分** | Database → Partition → Tablet → Rowset → Segment | Volume → Inode → Chunk |
| **最小存储单元** | Page (~64KB) | Chunk (512KB~64MB) |
| **物理文件** | Segment (.dat) | Chunk File |
| **副本粒度** | Tablet 级 3 副本 | Chain 级多副本 |
| **列存 vs 行存** | 列式存储 (按列 Page) | 行式存储 (按 Chunk) |
| **压缩** | Page 级 LZ4F + 列编码 | 无内置压缩 |
| **索引** | Zone Map, Bloom Filter, Bitmap Index, Short Key | 无内置索引 |
| **版本管理** | Rowset 版本链 [start, end] | Chunk updateVer |

---