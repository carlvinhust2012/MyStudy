# Apache Doris 压缩与去重实现原理

## 一、压缩与去重总览

Doris 的压缩和去重是两个独立但互补的机制：

- **压缩（Compression）**：减少存储空间和 I/O，在 Page 级别对编码后的数据进行 LZ4F/ZSTD 等压缩
- **去重（Deduplication）**：保证 UNIQUE_KEYS 模型下每个 Key 只保留最新值，通过 Merge-on-Read + Compaction 实现

```
写入路径:
  Row Data → MemTable(SkipList 去重) → PageBuilder(编码) → LZ4F(压缩) → Segment File

读取路径:
  Segment File → LZ4F(解压) → PageDecoder(解码) → CollectIterator(多版本去重) → Result

压缩路径:
  Compaction → 读取多版本 → Merge去重 → 重新编码压缩 → 新 Segment
```

---

## 二、存储压缩

### 2.1 压缩架构：编码 + Page 级压缩

Doris 采用**两级压缩**策略：先编码消除数据冗余，再 Page 级压缩进一步压缩。

```
原始数据 (如: ["Beijing", "Shanghai", "Beijing", "Shanghai", "Beijing"])
    │
    ▼ [第一级: 列编码]
字典编码: "Beijing"→0, "Shanghai"→1
    → 编码值: [0, 1, 0, 1, 0]  (INT 类型)
    │
    ▼ [第二级: Page 级压缩]
LZ4F 压缩编码后的字节流
    │
    ▼ [磁盘存储]
压缩后的 Page (CRC32C 校验)
```

### 2.2 支持的压缩算法

| 算法 | 压缩率 | 压缩速度 | 解压速度 | 实现库 | 说明 |
|------|--------|---------|---------|--------|------|
| **LZ4F** (默认) | 中 | 快 | **极快** | `<lz4/lz4frame.h>` | Frame 格式，256KB Block，解压速度约 2x LZ4 |
| LZ4 | 中 | 快 | 极快 | `<lz4/lz4.h>` | Raw Block 格式，最大 2GB |
| SNAPPY | 低 | 快 | 快 | `<snappy/snappy.h>` | Google Snappy，零拷贝多 Slice 压缩 |
| ZLIB | 高 | 慢 | 中 | `<zlib.h>` | Deflate 算法，默认压缩级别 |

**默认算法**：`SegmentWriter::init_column_meta()` 硬编码所有列使用 `LZ4F`。

### 2.3 支持的列编码

| 数据类型 | 默认编码 | 值查找编码 | 说明 |
|---------|---------|-----------|------|
| TINYINT ~ LARGEINT | **BIT_SHUFFLE** | FOR | 位洗牌 + LZ4，适合有序整数 |
| FLOAT / DOUBLE | **BIT_SHUFFLE** | - | 位洗牌，利用 float 的 bit 模式 |
| CHAR / VARCHAR / STRING | **DICT_ENCODING** | PREFIX | 字典编码：字符串→整数码字 |
| BOOL | **RLE** | PLAIN | 游程编码，bool 值压缩率极高 |
| DATE / DATETIME | **BIT_SHUFFLE** | FOR | 位洗牌 |
| DECIMAL | **BIT_SHUFFLE** | BIT_SHUFFLE | 位洗牌 |

### 2.4 字典编码详解（字符串列的核心优化）

```
写入阶段:
  原始数据: ["Beijing", "Shanghai", "Beijing", "Shanghai", "Beijing"]

  1. 构建 Dictionary Page:
     ┌─────────┬──────┐
     │ Beijing │  0   │
     │ Shanghai│  1   │
     └─────────┴──────┘
     → PLAIN 编码 + LZ4F 压缩 → 写入 Dictionary Page

  2. Data Page 存储码字:
     [0, 1, 0, 1, 0]  → BitShuffle(INT) 编码 → LZ4F 压缩 → Data Page

  3. 字典溢出回退:
     当唯一值超过 dict_page_size (默认 1MB) → 切换为 PLAIN 编码

读取阶段:
  Data Page → LZ4F 解压 → BitShuffle 解码 → 码字 [0, 1, 0, 1, 0]
      → Dictionary Page 查找 → ["Beijing", "Shanghai", "Beijing", "Shanghai", "Beijing"]
```

### 2.5 Page 级压缩流程

```mermaid
sequenceDiagram
    participant Writer as ScalarColumnWriter
    participant Builder as PageBuilder
    participant PageIO as PageIO
    participant Codec as LZ4F Codec
    participant Disk

    Writer->>Builder: append_data() 写入行数据
    Note over Builder: 编码 (DictEncoding/BitShuffle/RLE)

    Writer->>Writer: finish_current_page()
    Writer->>Builder: finish() 获取编码后的字节
    Writer->>Writer: 构建 DataPageFooterPB (ordinal, num_values, nullmap)

    Writer->>PageIO: compress_page_body(encoded_bytes, codec=LZ4F)
    PageIO->>Codec: compress(input, output)
    Codec-->>PageIO: compressed_bytes
    PageIO->>PageIO: space_saving = 1 - compressed/raw
    alt space_saving >= 10%
        Note over PageIO: 使用压缩版本
    else space_saving < 10%
        Note over PageIO: 丢弃压缩版本，存储原始数据
    end

    Writer->>PageIO: write_page(body, footer, wblock)
    PageIO->>Disk: [body] + [PageFooterPB] + [footer_size: 4B] + [CRC32C: 4B]
    Note over Disk: 单个 Page 约 64KB (data_page_size)
```

### 2.6 Page 缓存（避免重复解压）

```
StoragePageCache (LRU, 默认占系统内存 20%):
┌─────────────────────────────────────────┐
│ _data_page_cache (90%)                  │
│   key=(filepath, offset) → 解压后的 Page │
│   避免重复磁盘读取和解压                  │
├─────────────────────────────────────────┤
│ _index_page_cache (10%)                 │
│   key=(filepath, offset) → 解压后的索引  │
│   包括 Dictionary Page、Ordinal Index 等 │
└─────────────────────────────────────────┘
```

读取路径：先查 Page Cache → 命中直接返回 → 未命中则读磁盘解压后写入缓存。

### 2.7 Bloom Filter（减少无效 I/O）

```
Segment 文件结构:
┌──────┐ ┌──────┐ ┌──────┐ ┌────────┐ ┌───────────┐ ┌──────┐
│Data 1│ │Data 2│ │Data 3│ │Dict Page│ │Ordinal Idx│ │Footer│
│Page  │ │Page  │ │Page  │ │         │ │           │ │+Magic│
└──┬───┘ └──┬───┘ └──┬───┘ └────┬────┘ └───────────┘ └──────┘
   │        │        │          │
   ▼        ▼        ▼          ▼
 ┌─────────────────────────────────────────┐
 │ Zone Map (per-page min/max 统计)       │ ← 第一级过滤
 │ Bloom Filter (per-page 布隆过滤器)      │ ← 第二级过滤
 └─────────────────────────────────────────┘
```

- **BlockSplitBloomFilter**：32 字节 Block（一个 Cache Line），SIMD 友好
- **FPP = 0.05**（5% 误判率）
- **过滤流程**：Zone Map (min/max) → Bloom Filter → 剩余 Page 才需读取和解压

### 2.8 压缩写入完整流程

```mermaid
sequenceDiagram
    participant Load as Stream Load
    participant Writer as DeltaWriter
    participant MemTbl as MemTable
    participant SegWriter as SegmentWriter
    participant ColWriter as ScalarColumnWriter
    participant PageIO as PageIO
    participant Disk

    Load->>Writer: write(tuple)
    Writer->>MemTbl: insert(tuple)
    Note over MemTbl: MemTable 满 → flush

    MemTbl->>SegWriter: add_row()
    SegWriter->>ColWriter: append_data()
    Note over ColWriter: PageBuilder 编码

    Note over ColWriter: Page 满 (64KB)
    ColWriter->>ColWriter: finish_current_page()
    ColWriter->>PageIO: compress_page_body(LZ4F)
    Note over PageIO: 编码 → LZ4F 压缩 (节省 >=10%)

    Note over SegWriter: 所有列写完 → finalize()
    SegWriter->>PageIO: write_page() × N
    Note over PageIO: CRC32C 校验

    SegWriter->>Disk: 写入 Segment 文件
    Note over Disk: [Data Pages][Dict Page][Zone Maps][Bloom Filters][Footer]
```

---

## 三、去重

### 3.1 三种数据模型的去重策略

| 数据模型 | 写入去重 | 读取去重 | Compaction 去重 | 说明 |
|---------|---------|---------|---------------|------|
| **DUP_KEYS** | 无 | 无 | 无 | 允许完全重复，不去做重 |
| **UNIQUE_KEYS** | SkipList (单批次) | Merge Heap (跨版本) | Merge + 物理去重 | 保证每 Key 仅有最新值 |
| **AGG_KEYS** | SkipList + 聚合 | Merge Heap + 聚合 | Merge + 聚合 | 相同 Key 按聚合函数合并 |

### 3.2 UNIQUE_KEYS 去重：Merge-on-Read (MoR)

本版本 Doris 采用**纯 Merge-on-Read** 策略：写入时不查磁盘去重，读取时多版本合并去重。

```
                          去重时机
                    ┌──────────┬──────────┐
                    │ 写入时   │ 读取时   │
               ┌────┤          │          │
               │Mem │ SkipList │          │
               │Tbl │ 去重     │          │
               └────┤          │          │
                    │          │          │
               ┌────┤          ├──────────┤
               │跨  │          │ Merge    │
               │Row │          │ Heap     │
               │set │          │ 去重     │
               └────┤          │          │
                    │          │          │
               ┌────┤          │          │
               │跨  │          │          │
               │Txn │          │          │
               └────┤          │          │
                    │          │          │
               ┌────┤          ├──────────┤
               │Comp│          │ Merge +  │
               │act │          │ 物理     │
               │ion │          │ 去重     │
               └────┴──────────┴──────────┘
```

### 3.3 写入去重：MemTable SkipList

```mermaid
sequenceDiagram
    participant Load as Stream Load
    participant Writer as DeltaWriter
    participant MemTbl as MemTable
    participant SList as SkipList

    Load->>Writer: write(tuple1: key=A, val=100)
    Writer->>MemTbl: insert(tuple1)
    MemTbl->>SList: Find(key=A)
    SList-->>MemTbl: not found
    MemTbl->>SList: Insert(A → {val=100})

    Load->>Writer: write(tuple2: key=B, val=200)
    Writer->>MemTbl: insert(tuple2)
    MemTbl->>SList: Find(key=B)
    SList-->>MemTbl: not found
    MemTbl->>SList: Insert(B → {val=200})

    Load->>Writer: write(tuple3: key=A, val=150)
    Writer->>MemTbl: insert(tuple3)
    MemTbl->>SList: Find(key=A)
    SList-->>MemTbl: found! val=100
    MemTbl->>MemTbl: _aggregate_two_row(existing, new)
    Note over MemTbl: UNIQUE_KEYS: agg_update → 覆盖
    Note over MemTbl: SkipList 中 A → {val=150}

    Note over MemTbl: MemTable 满 → flush
    MemTbl->>MemTbl: flush → Segment File
    Note over MemTbl: 输出: (A, 150), (B, 200) — 重复 key 已消除
```

**SkipList 关键特性**：
- `can_dup = false`（UNIQUE_KEYS 模式下不允许重复）
- O(log n) 的 Find + InsertWithHint
- 单次 flush 内保证 Key 唯一

### 3.4 读取去重：Merge Heap

```mermaid
sequenceDiagram
    participant Q as 查询
    participant Reader as TupleReader
    participant Collect as CollectIterator
    participant R1 as Rowset [2,2]
    participant R2 as Rowset [3,3]
    participant R3 as Rowset [4,4]

    Q->>Reader: SELECT * WHERE key = 'A'
    Reader->>Collect: build_heap()

    Note over Collect: UNIQUE_KEYS: reverse=true<br/>高版本优先排列

    Collect->>R1: key=A, val=100, version=2
    Collect->>R2: key=A, val=120, version=3
    Collect->>R3: key=A, val=150, version=4

    Note over Collect: Merge Heap 按 (key, version DESC) 排序<br/>R3(version=4) 优先

    Reader->>Collect: _unique_key_next_row()
    Collect-->>Reader: A, val=150 (version=4, 最高版本)
    Note over Reader: 直接取最高版本行，跳过低版本
    Note over Collect: version=3, version=2 标记 need_skip=true

    Reader-->>Q: 返回 (A, 150)
    Note over Q: 只看到最新值 ✓
```

**Merge Heap 排序规则**（`LevelIteratorComparator`）：

```
比较两个 Row:
  1. 先比 Key → Key 不同则正常排序
  2. 再比 Sequence 列 (如果有) → Sequence 大的保留
  3. 最后比 Version → Version 高的保留，低版本标记 need_skip
```

### 3.5 Compaction 物理去重

```mermaid
sequenceDiagram
    participant BG as 后台线程
    participant CumComp as Cumulative Compaction
    participant BaseComp as Base Compaction
    participant Merger as Merger
    participant Reader as TupleReader
    participant Tablet as Tablet

    Note over BG: Rowset 状态:
    Note over BG: Base: [1,1]
    Note over BG: Cumulative: [2,2], [3,3], [4,4]

    BG->>CumComp: 触发 Cumulative Compaction
    CumComp->>Merger: merge_rowsets([2,2], [3,3])
    Merger->>Reader: init(reader_type=CUMULATIVE)
    Note over Reader: _unique_key_next_row() 去重
    Merger->>Merger: 输出 Rowset [2,3] (NONOVERLAPPING)
    Merger->>Tablet: modify_rowsets(add=[2,3], remove=[2,2],[3,3])
    Note over Tablet: Base: [1,1], Cum: [2,3], [4,4]

    BG->>BaseComp: 触发 Base Compaction
    BaseComp->>Merger: merge_rowsets([1,1], [2,3], [4,4])
    Note over Merger: _unique_key_next_row() 去重
    Note over Merger: _filter_delete=true (物理删除)
    Merger->>Merger: 输出 Rowset [1,4] (NONOVERLAPPING)
    Merger->>Tablet: modify_rowsets(add=[1,4], remove=[1,1],[2,3],[4,4])
    Note over Tablet: Base: [1,4] ← 每个 Key 只出现一次
```

### 3.6 Compaction 类型对比

| 维度 | Cumulative Compaction | Base Compaction |
|------|----------------------|-----------------|
| **触发条件** | 累积 rowset 数 > 阈值 | 累积/基线比例 > 阈值 |
| **处理范围** | 最近几个 delta rowset | 所有 rowset (base + cumulative) |
| **Delete 处理** | **跳过** (保留 delete predicate) | **物理删除**匹配行 |
| **输出** | NONOVERLAPPING rowset | 单个 NONOVERLAPPING rowset |
| **去重效果** | 合并近期版本 | 完全去重，每 Key 一行 |
| **频率** | 高频 | 低频 |

---

## 四、Delete 语句的实现

### 4.1 DELETE FROM 工作原理

```mermaid
sequenceDiagram
    participant User
    participant FE as FE (DeleteHandler)
    participant BE as BE

    User->>FE: DELETE FROM t1 WHERE dt < '2024-01-01'
    FE->>FE: 验证条件仅涉及 Key 列
    FE->>FE: 创建事务 + PushTask(DELETE)

    FE->>BE: PushTask(delete_predicate={dt < '2024-01-01'})
    BE->>BE: 存储 DeletePredicatePB 到 Tablet Meta

    FE->>FE: commit() → 分配 version=5
    FE->>BE: PublishVersion(version=5)
    BE->>BE: rowset.make_visible(5)

    Note over BE: Delete 生效版本 = 5

    Note over FE,BE: 查询 version=4 (发布前)
    User->>FE: SELECT * FROM t1
    FE->>BE: 扫描 (version=4)
    Note over BE: DeletePredicate.filter_version=5 > 4 → 不应用删除
    BE-->>User: dt < '2024-01-01' 的行仍然可见 ✓

    Note over FE,BE: 查询 version=5 (发布后)
    User->>FE: SELECT * FROM t1
    FE->>BE: 扫描 (version=5)
    Note over BE: DeletePredicate.filter_version=5 <= 5 → 应用删除
    Note over BE: DeleteHandler::is_filter_data() 过滤匹配行
    BE-->>User: dt < '2024-01-01' 的行已被过滤 ✓

    Note over FE,BE: Base Compaction (最终物理删除)
    BE->>BE: 读取所有 rowset (version <= latest)
    Note over BE: _filter_delete = true
    BE->>BE: 匹配 delete 的行不写入新 rowset
    Note over BE: 删除行被永久移除，空间回收 ✓
```

### 4.2 Delete 在不同阶段的行为

| 阶段 | Delete 行处理 | 说明 |
|------|-------------|------|
| 写入 | 生成 DeletePredicatePB，存入 Tablet Meta | 不实际删除数据 |
| 查询 | `is_filter_data()` 逻辑过滤 | 仅过滤 `version <= 查询版本` 的 predicate |
| Cumulative Compaction | **跳过** delete predicate | 保留给 Base 处理 |
| Base Compaction | **物理移除**匹配行 | `_filter_delete = true`，空间回收 |

---

## 五、完整的写入→去重→压缩→查询流程

```mermaid
sequenceDiagram
    participant Client
    participant FE as Master FE
    participant BE as Backend
    participant MemTbl as MemTable
    participant SList as SkipList
    participant SegWriter as SegmentWriter
    participant Tablet as Tablet

    Note over Client,Tablet: Phase 1: 数据写入 (去重 + 编码 + 压缩)

    Client->>BE: Stream Load (CSV data)
    BE->>BE: 逐行解析
    BE->>MemTbl: insert(tuple: key=A, val=100)
    MemTbl->>SList: Find(A) → not found → Insert
    BE->>MemTbl: insert(tuple: key=A, val=150)
    MemTbl->>SList: Find(A) → found → agg_update(val=150)

    Note over MemTbl: MemTable 满 → flush
    MemTbl->>SegWriter: add_row()
    Note over SegWriter: DictEncoding (字符串) / BitShuffle (整数)
    Note over SegWriter: LZ4F Page 压缩 (节省 >=10%)
    SegWriter->>BE: Segment File 写入磁盘

    BE->>FE: commit(txnId)
    FE->>FE: 分配 version=5, commit
    FE->>BE: PublishVersion(5)
    BE->>Tablet: add_inc_rowset([5,5])

    Note over Client,Tablet: Phase 2: 查询 (解压 + 解码 + 版本去重)

    Client->>FE: SELECT * FROM t1
    FE->>BE: 扫描 (version=5)
    BE->>BE: capture_rs_readers(0, 5)
    Note over BE: Rowsets: [1,2], [3,3], [4,4], [5,5]

    BE->>BE: Zone Map + Bloom Filter 过滤 Page
    BE->>BE: Page Cache 查找 → 未命中 → 磁盘读取
    BE->>BE: LZ4F 解压 → DictEncoding 解码

    Note over BE: TupleReader._unique_key_next_row()
    Note over BE: Merge Heap: 高版本 Key 优先
    Note over BE: 同一 Key 只返回最高版本

    BE-->>Client: 去重后的结果 ✓

    Note over Client,Tablet: Phase 3: Compaction (物理去重 + 重新压缩)

    BE->>BE: Cumulative Compaction: [3,3]+[4,4] → [3,4]
    BE->>BE: Base Compaction: [1,2]+[3,4]+[5,5] → [1,5]
    Note over BE: 合并后每 Key 仅一行
    Note over BE: 重新编码 + LZ4F 压缩
    Note over BE: 压缩率更高 (数据更有序)
```

---

## 六、压缩率优化分析

### 6.1 为什么 Compaction 后压缩率更高

```
Compaction 前 (3 个 Rowset):
  Rowset [3,3]: Segment 内 Key 无序 → 字典编码效率低 → BitShuffle 效果差
  Rowset [4,4]: 同上
  Rowset [5,5]: 同上

Compaction 后 (1 个 Rowset):
  Rowset [3,5]: Key 有序 → 字典编码效率高 → BitShuffle 充分利用排序局部性
  → 压缩率显著提升

实际效果:
  3 个小 Rowset 总计 300MB
  → Compaction 后 1 个 Rowset 约 150~200MB (去重 + 有序编码 + 更高压缩率)
```

### 6.2 各编码的适用场景

| 场景 | 最佳编码 | 压缩效果 | 原因 |
|------|---------|---------|------|
| 低基数字符串 (如城市名) | DICT_ENCODING + LZ4F | **极高** | 字典将字符串映射为小整数，压缩率极高 |
| 有序整数 (自增 ID) | BIT_SHUFFLE + LZ4F | **高** | BitShuffle 将高位集中，LZ4 利用重复模式 |
| 日期列 | BIT_SHUFFLE + LZ4F | **高** | 日期连续，相邻行差异小 |
| Boolean 列 | RLE | **高** | 连续相同值，RLE 效果极佳 |
| 高基数字符串 (如 UUID) | PLAIN + LZ4F | **低** | 字典溢出回退，压缩效果有限 |
| 浮点数 | BIT_SHUFFLE + LZ4F | **中** | Bit 模式局部性不如整数 |

---

## 七、总结

| 维度 | 压缩 | 去重 |
|------|------|------|
| **目标** | 减少存储空间和 I/O | 保证数据一致性 |
| **时机** | 写入时 Page 级、Compaction 后重新压缩 | 写入(SkipList)、读取(Merge Heap)、Compaction(物理) |
| **粒度** | Page 级 (64KB) | Row 级 (按 Key) |
| **默认算法** | LZ4F | UNIQUE_KEYS: Last-Write-Wins |
| **回退策略** | 节省 <10% 则不压缩 | AGG_KEYS: 按聚合函数合并 |
| **缓存优化** | StoragePageCache (解压后缓存) | 无缓存 (每次查询重新合并) |
| **对查询的影响** | 减少磁盘 I/O 和内存占用 | Rowset 多时读性能下降，依赖 Compaction |

**核心设计理念**：压缩通过编码 + Page 级压缩在存储层消除冗余；去重通过 MoR 模型在读/写/Compaction 三个阶段分层消除重复。两者结合使得 Doris 在 AI 训练数据仓库场景下能以较低存储成本支持高并发查询。

---
