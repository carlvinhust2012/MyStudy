# Apache Doris 存储模型分析
## 一、数据组织层级

Doris 的存储模型是一个四层树形结构：

```
Database
  └─ Table (分区 + 分桶)
       └─ Tablet (分区 × 分桶, 存储基本单元)
            └─ Rowset (一次导入或一次Compaction的产出)
                 └─ Segment (.dat 文件, 列存格式)
                      └─ Column Pages (每列独立存储, 含多种索引)
```

**磁盘路径格式**：

```
data/{shard_id}/{tablet_id}/{schema_hash}/{rowset_id}/{seg_id}.dat
```

**关键源码位置**：

| 组件 | 源码路径 | 说明 |
|------|----------|------|
| StorageEngine | `be/src/olap/storage_engine.h` | 全局单例，管理所有 Tablet、事务、Compaction |
| Tablet | `be/src/olap/tablet.h` | 存储基本单元，`_rs_version_map` 维护活跃 Rowset 版本链 |
| TabletSchema | `be/src/olap/tablet_schema.h` | Schema 定义，含 KeysType、列信息、索引标记 |
| TabletMeta | `be/src/olap/tablet_meta.h` | 持久化元数据，含 TabletState、RowsetMeta 列表 |
| TabletManager | `be/src/olap/tablet_manager.h` | Tablet 生命周期管理，分片锁 |
| Rowset | `be/src/olap/rowset/rowset.h` | Rowset 基类，版本范围 `[start_version, end_version]` |
| BetaRowset | `be/src/olap/rowset/beta_rowset.h` | 当前版本的 Rowset 实现 (segment_v2) |
| Segment | `be/src/olap/rowset/segment_v2/segment.h` | Segment 文件抽象，不可变 |
| DataDir | `be/src/olap/data_dir.h` | 单个存储根路径（磁盘）管理 |

---

## 二、三种数据模型 (KeysType)

定义在 `be/src/olap/tablet_schema.h` 中，由 `TabletSchema::_keys_type` 决定：

| 模型 | 枚举值 | 说明 | 读取策略 |
|------|--------|------|----------|
| **明细模型** | `DUP_KEYS` | 允许重复行，不做聚合 | `_direct_next_block()` — 直接读取，无需合并 |
| **唯一键模型** | `UNIQUE_KEYS` | 相同 Key 后写覆盖前写 | `_unique_key_next_block()` — 高版本到低版本查找 |
| **聚合模型** | `AGG_KEYS` | 相同 Key 按聚合函数合并 | `_agg_key_next_block()` — 归并堆排序合并 |

读取策略源码位于 `be/src/vec/olap/block_reader.h`。

---

## 三、Segment V2 文件格式

Segment 是 Doris 存储的核心单元，格式定义在 `gensrc/proto/segment_v2.proto`。

### 3.1 整体布局

```
┌──────────────────────────────────────────────┐
│  Column A Data Pages   (编码 + 压缩)         │
│  Column A Ordinal Index (B-Tree)             │
│  Column A Zone Map     (min/max per page)    │
│  Column A Bloom Filter  (per page)           │
│  Column A Bitmap Index  (Roaring Bitmap)     │
│                                              │
│  Column B Data Pages ...                     │
│  ...                                        │
│                                              │
│  Short Key Index Page (前N列的稀疏索引)       │
│                                              │
│  Footer (SegmentFooterPB, 固定尾部)          │
└──────────────────────────────────────────────┘
```

### 3.2 Page 格式

定义在 `be/src/olap/rowset/segment_v2/page_io.h`（第 69 行）：

```
PageBody | PageFooter | FooterSize(4 bytes) | Checksum(CRC32C, 4 bytes)
```

### 3.3 Page 类型

| 类型 | 说明 |
|------|------|
| `DATA_PAGE` | 列数据页 |
| `INDEX_PAGE` | B-Tree 索引页 |
| `DICTIONARY_PAGE` | DICT_ENCODING 的字典页 |
| `SHORT_KEY_PAGE` | Short Key 索引页 |

### 3.4 编码方式

| 编码 | 适用场景 |
|------|----------|
| PLAIN_ENCODING | 通用默认 |
| PREFIX_ENCODING | 字符串前缀编码 |
| RLE | 游程编码，适合重复值 |
| DICT_ENCODING | 字典编码，适合低基数 |
| BIT_SHUFFLE | 位混洗，适合整数序列 |
| FOR_ENCODING | Frame-of-Reference，适合有序整数 |

编码选择逻辑在 `be/src/olap/rowset/segment_v2/encoding_info.h` 中。

### 3.5 压缩方式

| 压缩算法 | 说明 |
|----------|------|
| LZ4 | **默认**，速度快 |
| SNAPPY | 高速压缩 |
| ZLIB | 高压缩比 |
| ZSTD | 新一代高压缩比 |
| LZ4F | LZ4 帧格式 |

默认 Page 大小为 **64KB**（`ColumnWriterOptions::data_page_size`），压缩最低空间节省阈值为 0.1（10%）。

### 3.6 SegmentWriter 写入顺序

源码：`be/src/olap/rowset/segment_v2/segment_writer.h`

```
append_row()  →  ScalarColumnWriter::append()
                    │
finalize()    →  _write_data()            // 写所有列的数据页
                _write_ordinal_index()    // 写序号索引
                _write_zone_map()         // 写 Zone Map
                _write_bitmap_index()     // 写 Bitmap 索引
                _write_bloom_filter_index() // 写布隆过滤器
                _write_short_key_index()  // 写 Short Key 索引
                _write_footer()           // 写 Footer（固定在文件尾部）
```

### 3.7 Segment 读取流程

```
Segment::open()
  │  从文件尾部读取 Footer (seek to end)
  ▼
_parse_footer()  →  反序列化 SegmentFooterPB
  ▼
_create_column_readers()  →  创建每列的 ColumnReader
  │  Short Key Index 延迟加载 (DorisCallOnce)
  ▼
new_iterator()  →  SegmentIterator (支持各种谓词下推)
```

---

## 四、五级索引体系

```
┌─────────────────────────────────────────────────────┐
│                    查询谓词下推                       │
│                                                      │
│  1. Short Key Index     → 段级 Key Range 过滤         │
│     (前N列稀疏索引, binary search)                    │
│                                                      │
│  2. Zone Map             → Page 级 min/max 过滤      │
│     (每列每页的统计信息)                               │
│                                                      │
│  3. Bloom Filter         → Page 级等值过滤            │
│     (BlockSplitBloomFilter, SIMD加速, 32B block)     │
│                                                      │
│  4. Bitmap Index         → Row 级精确过滤             │
│     (Roaring Bitmap + 有序字典)                       │
│                                                      │
│  5. Ordinal Index (B-Tree) → Page 定位              │
│     (ordinal → PagePointer 映射)                      │
└─────────────────────────────────────────────────────┘
```

### 4.1 Short Key Index

- **源码**：`be/src/olap/short_key_index.h`
- **原理**：每 `num_rows_per_block` 行存一条 Short Key 编码，形成有序稀疏索引
- **格式**：`KeyContent^NumEntry | KeyOffset(vint)^NumEntry`
- **查找**：`ShortKeyIndexDecoder::lower_bound()` / `upper_bound()` 通过二分查找定位行范围
- **Key 编码标记**：
  - `KEY_NULL_FIRST_MARKER (0x01)` — NULL 值
  - `KEY_NORMAL_MARKER (0x02)` — 正常值
  - `KEY_MAXIMAL_MARKER (0xFF)` — 最大值哨兵

### 4.2 Zone Map Index

- **源码**：`be/src/olap/rowset/segment_v2/zone_map_index.h`
- **原理**：每个数据页统计 min/max/has_null/has_not_null
- **结构**：Page 级 Zone Map + Segment 级 Zone Map
- **用途**：范围谓词（`<`, `>`, `BETWEEN`）裁剪不满足条件的 Page
- **API**：`ColumnReader::get_row_ranges_by_zone_map()`

### 4.3 Bloom Filter Index

- **源码**：`be/src/olap/rowset/segment_v2/block_split_bloom_filter.h`
- **算法**：Block Split Bloom Filter (Putze et al.)
- **特性**：
  - 32 字节分块（Cache-Line 对齐，SIMD 友好）
  - 每块 8 bits set，使用 SIMD 加速的 salt 值
  - Hash 策略：MURMUR3_X64_64，默认种子 1575457558
  - 支持 NULL 值（最后一个字节为 NULL 标记）
- **结构**：每个数据页一个 Bloom Filter
- **用途**：等值谓词（`=`）裁剪

### 4.4 Bitmap Index

- **源码**：`be/src/olap/rowset/segment_v2/bitmap_index_reader.h`
- **结构**：有序字典列 + Roaring Bitmap 列
- **API**：
  - `seek_dictionary()` — 在字典中二分查找
  - `read_bitmap()` — 读取对应值的 Bitmap
  - `read_null_bitmap()` — NULL 专用 Bitmap（最后一个条目）
  - `read_union_bitmap()` — 合并范围内 Bitmap
- **用途**：IN 列表和等值谓词的行级精确过滤

### 4.5 Ordinal Index

- **源码**：`be/src/olap/rowset/segment_v2/ordinal_page_index.h`
- **结构**：B-Tree，映射 `(ordinal → PagePointer)`
- **优化**：仅一个数据页时直接存储指针，无需索引页
- **用途**：按行号精确定位到数据页

---

## 五、写入路径 (Write Path)

```
VOlapTableSink                        be/src/vec/sink/vtablet_sink.h
  │  接收 vectorized::Block，按分区/分桶路由
  ▼
DeltaWriter (每个 tablet 一个)         be/src/olap/delta_writer.h
  │  数据校验 + 写入 MemTable
  ▼
MemTable (SkipList 排序结构)          be/src/olap/memtable.h
  │  内存排序，支持 AGG/UNIQUE 聚合
  │  满阈值触发 flush
  ▼
MemTableFlushExecutor (异步线程池)     be/src/olap/memtable_flush_executor.h
  │  后台异步执行 flush
  ▼
BetaRowsetWriter                      be/src/olap/rowset/beta_rowset_writer.h
  │  管理多个 Segment，满阈值创建新 Segment
  ▼
SegmentWriter                         be/src/olap/rowset/segment_v2/segment_writer.h
  │  append_row() → ScalarColumnWriter → PageBuilder → 编码/压缩
  │  finalize()   → 数据页 + 索引 + Short Key + Footer
  ▼
ColumnWriter                          be/src/olap/rowset/segment_v2/column_writer.h
  │  ScalarColumnWriter: 单值列写入
  │  ArrayColumnWriter: 数组列写入 (offset + null + item 子写入器)
  ▼
  Segment .dat 文件落盘
```

**WriteType 类型**（`delta_writer.h`）：
- `LOAD = 1` — 普通数据导入
- `LOAD_DELETE = 2` — 带删除条件的导入
- `DELETE = 3` — 纯删除操作

---

## 六、读取路径 (Read Path)

```
VOlapScanNode                         be/src/vec/exec/volap_scan_node.h
  │  生成 VOlapScanner (每个 Tablet+Version 一个)
  ▼
VOlapScanner                          be/src/vec/exec/volap_scanner.h
  │  调用 get_block() 获取数据
  ▼
BlockReader (vectorized::TabletReader) be/src/vec/olap/block_reader.h
  │  根据 KeysType 选择读取策略：
  │  ├─ _direct_next_block()           DUP_KEYS，直接读
  │  ├─ _direct_agg_key_next_block()   AGG_KEYS 单版本优化
  │  ├─ _agg_key_next_block()          AGG_KEYS 多版本归并
  │  └─ _unique_key_next_block()       UNIQUE_KEYS 高版本优先
  ▼
BetaRowsetReader                      be/src/olap/rowset/beta_rowset_reader.h
  │  创建 Segment 实例
  ▼
SegmentIterator                       be/src/olap/rowset/segment_v2/segment_iterator.h
  │  ★ 核心：多级索引裁剪
  │  ① _get_row_ranges_by_keys()              Short Key 过滤
  │  ② _get_row_ranges_by_column_conditions() ZoneMap + BF + Bitmap 过滤
  │  ③ _apply_bitmap_index()                  Bitmap 交集
  │  → 构建 Roaring _row_bitmap 候选行集
  │  → 惰性物化 (Lazy Materialization)
  │  → 向量化谓词求值 _evaluate_vectorization_predicate()
  │  → 短路求值 _evaluate_short_circuit_predicate()
  ▼
FileColumnIterator                    be/src/olap/rowset/segment_v2/column_reader.h
  │  OrdinalIndex 定位 Page → 读取 → 解压 → 解码
  ▼
DefaultValueColumnIterator            be/src/olap/rowset/segment_v2/column_reader.h
  │  处理 Schema Evolution：返回新增列的默认值
  ▼
  返回 vectorized::Block
```

**关键优化 — 惰性物化 (Lazy Materialization)**：

`SegmentIterator` 先只读取谓词涉及的列，完成过滤后，再按需读取非谓词列，大幅减少 I/O 量。

---

## 七、Compaction 机制

Doris 采用 **Base + Cumulative** 两层 Compaction 策略，本质上是 LSM-Tree 的合并优化。

### 7.1 版本空间划分

```
Version:  [2] [3] [4] [5] [6] [7] [8] [9] [10]
                      ↑
              cumulative_point
    ─────────────────┤────────────────────────
    Base Rowsets     Cumulative Rowsets
    (已排序, 不重叠)   (可能重叠)
```

`Tablet::_cumulative_point`（`std::atomic<int64_t>`）将版本空间一分为二。

### 7.2 Compaction 类型

| 类型 | 范围 | 策略 | 源码 |
|------|------|------|------|
| **Base Compaction** | `[2, cumulative_point]` | 全量合并为一个 Rowset，要求输入不重叠 | `be/src/olap/base_compaction.h` |
| **Cumulative Compaction** | `[cumulative_point, latest]` | 逐层合并，满足条件后提升为 Base | `be/src/olap/cumulative_compaction.h` |

### 7.3 Cumulative Compaction 策略

源码：`be/src/olap/cumulative_compaction_policy.h`

| 策略 | 说明 |
|------|------|
| **SizeBasedPolicy**（默认） | 按 2^n 大小层级分组，输出超过 Base Size 的 promotion ratio 时提升 |
| **NumBasedPolicy** | 线性合并，每个 Rowset 累积一次即提升到 Base |

### 7.4 Compaction 流程

```
Compaction::compact()                 be/src/olap/compaction.h
  │
  ├─ prepare_compact()                子类准备，选择输入 Rowset
  │
  ├─ execute_compact_impl()
  │     │
  │     ▼
  │   Merger::merge_rowsets()         be/src/olap/merger.h
  │     │  读取输入 RowsetReader → 排序归并 → 写入输出 RowsetWriter
  │     ▼
  │   新 Rowset 生成
  │
  ├─ modify_rowsets()                 原子替换 Tablet 中的 Rowset 版本
  │     └─ Tablet::_rs_version_map 更新
  │
  └─ gc_output_rowset()               失败时清理
```

### 7.5 调度机制

```
StorageEngine::_compaction_tasks_producer_thread
  │  后台线程持续运行
  ▼
_generate_compaction_tasks()
  │  对所有 DataDir 下的 Tablet 打分
  │  提交 Top 候选到线程池
  ▼
_push_tablet_into_submitted_compaction()
  │  记录已提交 Tablet，防止重复
  ▼
线程池执行 Compaction Task
```

---

## 八、事务与可见性机制

### 8.1 事务生命周期

```
FE 分配 txn_id
       │
       ▼
TxnManager::prepare_txn()             be/src/olap/txn_manager.h
       │  注册 (partition_id, txn_id) → TabletTxnInfo
       │  数据结构: 分片 HashMap<txn_id, map<TabletInfo, TabletTxnInfo>>
       ▼
DeltaWriter 写数据 → MemTable → Segment
       │
       ▼
TxnManager::commit_txn()
       │  关联 RowsetSharedPtr，RowsetState → COMMITTED
       ▼
FE 发送 publish_txn(version)
       │
       ▼
TxnManager::publish_txn()
       │  Tablet::add_inc_rowset()
       │  Rowset 加入 _rs_version_map
       │  更新 VersionGraph
       │  RowsetState → VISIBLE
       ▼
  数据对查询可见
```

### 8.2 TxnManager 数据结构

源码：`be/src/olap/txn_manager.h`

```cpp
// 分片哈希: (partition_id, txn_id) → map<TabletInfo, TabletTxnInfo>
txn_tablet_map_t

// 事务 → 涉及的分区集合
txn_partition_map_t  // txn_id → set<partition_id>
```

### 8.3 Rowset 状态机

```
PREPARED → COMMITTED → VISIBLE
```

- `PREPARED`：正在写入
- `COMMITTED`：写入完成但未可见（BE 不可删除）
- `VISIBLE`：对查询可见

### 8.4 Version Graph（一致性读）

源码：`be/src/olap/version_graph.h`

- **结构**：邻接表有向图，顶点为版本号，边为 Rowset 的版本范围 `[start, end+1)`
- **用途**：查询时通过 `capture_consistent_rowsets()` 找到最短一致版本路径，保证**快照隔离读**（Snapshot Isolation）
- **TimestampedVersionTracker**：扩展 VersionGraph，带时间戳追踪过期 Rowset，用于安全 GC

### 8.5 Tablet 并发控制

| 锁 | 类型 | 保护对象 |
|----|------|----------|
| `_meta_lock` | RWLock | 版本映射 (`_rs_version_map`)、Tablet 元数据 |
| `_ingest_lock` | Mutex | 数据写入序列化 |
| `_base_lock` | Mutex | Base Compaction 串行化 |
| `_cumulative_lock` | Mutex | Cumulative Compaction 串行化 |
| `_migration_lock` | RWLock | 数据迁移/克隆串行化 |

---

## 九、总结架构图

```
┌───────────────────────────────────────────────────────────┐
│                    StorageEngine (单例)                     │
│  ┌──────────┐  ┌───────────┐  ┌────────────────┐          │
│  │TabletMgr │  │ TxnManager│  │CompactionThread│          │
│  └────┬─────┘  └─────┬─────┘  └───────┬────────┘          │
│       │               │                │                    │
│  ┌────▼───────────────▼────────────────▼────────┐          │
│  │                 Tablet                       │          │
│  │  _rs_version_map: Version → Rowset           │          │
│  │  _cumulative_point: base/cumulative 分界      │          │
│  │  _timestamped_version_tracker: 一致性快照     │          │
│  │  5种锁保证并发安全                            │          │
│  └────┬──────────────────────────────────────────┘          │
│       │ Rowset (版本区间 [start_ver, end_ver])               │
│  ┌────▼──────────────────────────────────────────┐          │
│  │                 Segment (.dat)                 │          │
│  │  ┌─────────┐ ┌─────────┐ ┌─────────────────┐ │          │
│  │  │ Col A   │ │ Col B   │ │ Col C ...       │ │          │
│  │  │ Pages   │ │ Pages   │ │ Pages           │ │          │
│  │  │ +5索引  │ │ +5索引  │ │ +5索引          │ │          │
│  │  └─────────┘ └─────────┘ └─────────────────┘ │          │
│  │  [Short Key Index]  [Footer]                  │          │
│  └───────────────────────────────────────────────┘          │
└───────────────────────────────────────────────────────────┘
```

---

## 十、核心设计总结

Doris 的存储模型本质上是一个 **LSM-Tree 变体**，兼具写优化和读优化：

1. **写优化**：数据以 Rowset 为单位追加写入，无需原地更新，写入路径经过 MemTable → Segment 的异步流水线，高吞吐
2. **读优化**：Segment V2 的**列式存储 + 五级索引（Short Key → Zone Map → Bloom Filter → Bitmap Index → Ordinal Index）+ 惰性物化**，实现高效的 OLAP 分析查询
3. **数据一致性**：通过 Version Graph 保证快照隔离读，Base/Cumulative 两层 Compaction 平衡写放大和读放大
4. **灵活的数据模型**：DUP_KEYS / UNIQUE_KEYS / AGG_KEYS 三种模型满足明细、去重、预聚合等不同分析场景
5. **Schema Evolution**：`DefaultValueColumnIterator` 支持新增列的默认值回填，实现无缝 Schema 变更

### 关键 Protobuf 定义

| 文件 | 内容 |
|------|------|
| `gensrc/proto/segment_v2.proto` | Segment 文件格式（SegmentFooterPB、ColumnMetaPB、PagePointerPB、ZoneMapPB、索引类型、编码/压缩枚举） |
| `gensrc/proto/olap_file.proto` | Rowset/Tablet 元数据（RowsetMetaPB、TabletMetaPB、KeysType、DeletePredicatePB） |
