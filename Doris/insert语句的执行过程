INSERT INTO database_01.table_A VALUES (1, 'hello', 100.5) 执行时序流程图
============================================================================

参与方:
  Client      - MySQL 客户端
  FE          - Frontend (ConnectProcessor / StmtExecutor / InsertStmt / Planner)
  TxnMgr      - GlobalTransactionMgr (FE)
  Coord       - Coordinator (FE)
  BE-C        - BE Coordinator (FragmentMgr / PlanFragmentExecutor / VOlapTableSink / NodeChannel)
  BE-T        - BE Target (LoadChannelMgr / TabletsChannel / DeltaWriter / MemTable)
  Storage     - 存储引擎 (FlushExecutor / BetaRowsetWriter / SegmentWriter / ScalarColumnWriter)

图例:
  ──────>  同步调用
  ─ ─ ─>  异步调用
  <── ──   返回


════════════════════════════════════════════════════════════════════════════════
第一阶段: SQL 解析与语义分析 (FE)
════════════════════════════════════════════════════════════════════════════════

Client          FE                TxnMgr           Coord        BE-C         BE-T       Storage
  │              │                  │               │            │            │           │
  │──COM_QUERY──>│                  │               │            │            │           │
  │  INSERT INTO │                  │               │            │            │           │
  │  table_A     │                  │               │            │            │           │
  │  VALUES(...) │                  │               │            │            │           │
  │              │                  │               │            │            │           │
  │              │ SqlParser(CUP)   │               │            │            │           │
  │              │ 解析为 InsertStmt │               │            │            │           │
  │              │                  │               │            │            │           │
  │              │ StmtExecutor     │               │            │            │           │
  │              │   .analyze()     │               │            │            │           │
  │              │                  │               │            │            │           │
  │              │ InsertStmt.analyze()            │            │            │           │
  │              │  ├ analyzeTargetTable()         │            │            │           │
  │              │  │  解析 OlapTable              │            │            │           │
  │              │  │  创建 TupleDescriptor        │            │            │           │
  │              │  ├ analyzeSubquery()            │            │            │           │
  │              │  │  分析 VALUES 子句             │            │            │           │
  │              │  └ createDataSink()             │            │            │           │
  │              │     → new OlapTableSink()       │            │            │           │
  │              │                  │               │            │            │           │
  │              │──beginTransaction──>│            │            │            │           │
  │              │                  │               │            │            │           │
  │              │                  │ DatabaseTxnMgr            │            │           │
  │              │                  │ .beginTransaction()       │            │           │
  │              │                  │               │            │            │           │
  │              │<──txnId──────────│               │            │            │           │
  │              │  (PREPARE)       │               │            │            │           │
  │              │                  │               │            │            │           │
  │              │ OlapTableSink    │               │            │            │           │
  │              │   .init(txnId)   │               │            │            │           │
  │              │                  │               │            │            │           │


════════════════════════════════════════════════════════════════════════════════
第二阶段: 查询规划 (FE)
════════════════════════════════════════════════════════════════════════════════

Client          FE                TxnMgr           Coord        BE-C         BE-T       Storage
  │              │                  │               │            │            │           │
  │              │ Planner.plan()   │               │            │            │           │
  │              │                  │               │            │            │           │
  │              │ SingleNodePlanner               │            │            │           │
  │              │  → 单节点执行计划树              │            │            │           │
  │              │                  │               │            │            │           │
  │              │ DistributedPlanner              │            │            │           │
  │              │  → 分布式 Fragment 拆分          │            │            │           │
  │              │                  │               │            │            │           │
  │              │ createInsertFragment()          │            │            │           │
  │              │  → OlapTableSink 设为           │            │            │           │
  │              │    Root Fragment 的 Sink        │            │            │           │
  │              │                  │               │            │            │           │
  │              │ OlapTableSink    │               │            │            │           │
  │              │   .complete()    │               │            │            │           │
  │              │  ├ createSchema()│               │            │            │           │
  │              │  │  列描述+Index  │               │            │            │           │
  │              │  ├ createPartition()            │            │            │           │
  │              │  │  分区+Tablet  │               │            │            │           │
  │              │  ├ createLocation()             │            │            │           │
  │              │  │  Tablet→BE   │               │            │            │           │
  │              │  └ createPaloNodesInfo()        │            │            │           │
  │              │     存活 BE 列表 │               │            │            │           │
  │              │                  │               │            │            │           │


════════════════════════════════════════════════════════════════════════════════
第三阶段: Fragment 调度 (FE → BE)
════════════════════════════════════════════════════════════════════════════════

Client          FE                TxnMgr           Coord        BE-C         BE-T       Storage
  │              │                  │               │            │            │           │
  │              │ handleInsertStmt()              │            │            │           │
  │              │─────────────────>│               │            │            │           │
  │              │  new Coordinator()              │            │            │           │
  │              │                  │               │            │            │           │
  │              │                  │ computeScanRangeAssignment()         │           │
  │              │                  │ → 扫描范围分配到 BE            │           │
  │              │                  │               │            │            │           │
  │              │                  │ computeFragmentExecParams()            │           │
  │              │                  │ → 分配实例 ID    │            │            │           │
  │              │                  │               │            │            │           │
  │              │                  │──sendFragment()─ ─ ─ ─ ─ ─ ─>│          │           │
  │              │                  │               │  exec_plan_fragment RPC          │
  │              │                  │               │  (Plan+Sink+TxnInfo)           │
  │              │                  │               │            │            │           │


════════════════════════════════════════════════════════════════════════════════
第四阶段: BE Fragment 执行 (BE-Coordinator)
════════════════════════════════════════════════════════════════════════════════

Client          FE                TxnMgr           Coord        BE-C         BE-T       Storage
  │              │                  │               │            │            │           │
  │              │                  │               │ FragmentMgr            │           │
  │              │                  │               │  .exec_plan_fragment()  │           │
  │              │                  │               │            │            │           │
  │              │                  │               │ PlanFragExec            │           │
  │              │                  │               │  .prepare()             │           │
  │              │                  │               │  → ExecNode::create_tree()           │
  │              │                  │               │  → DataSink::create_data_sink()      │
  │              │                  │               │            │            │           │
  │              │                  │               │ .open()                  │           │
  │              │                  │               │  → _plan->open()        │           │
  │              │                  │               │  → _sink->open()        │           │
  │              │                  │               │    VOlapTableSink.open() │           │
  │              │                  │               │            │            │           │
  │              │                  │               │          NodeChannel     │           │
  │              │                  │               │            .open()       │           │
  │              │                  │               │──tablet_writer_open── ─ ─ ─ ─>│      │
  │              │                  │               │            │            │           │
  │              │                  │               │ ┌─LOOP: 每批数据 Block──┐           │
  │              │                  │               │ │                       │           │
  │              │                  │               │ │ _plan->get_next()     │           │
  │              │                  │               │ │ → 获取 Block          │           │
  │              │                  │               │ │                       │           │
  │              │                  │               │ │ _sink->send(block)    │           │
  │              │                  │               │ │  VOlapTableSink.send()│           │
  │              │                  │               │ │  ├ apply expressions  │           │
  │              │                  │               │ │  ├ _validate_data()   │           │
  │              │                  │               │ │  ├ 分区路由+哈希分桶   │           │
  │              │                  │               │ │  └ NodeChannel.add_row()          │
  │              │                  │               │ │    累积到 RowBatch    │           │
  │              │                  │               │ └───────────────────────┘           │
  │              │                  │               │            │            │           │
  │              │                  │               │            │ 后台线程:  │           │
  │              │                  │               │            │ tablet_writer_add_batch   │
  │              │                  │               │            │ (brpc)     │           │
  │              │                  │               │            │            │           │


════════════════════════════════════════════════════════════════════════════════
第五阶段: 目标 BE 数据写入 (BE-Target)
════════════════════════════════════════════════════════════════════════════════

Client          FE                TxnMgr           Coord        BE-C         BE-T       Storage
  │              │                  │               │            │            │           │
  │              │                  │               │            │──add_batch──>│           │
  │              │                  │               │            │  (brpc)     │           │
  │              │                  │               │            │            │           │
  │              │                  │               │            │ LoadChannelMgr            │
  │              │                  │               │            │  → TabletsChannel         │
  │              │                  │               │            │            │           │
  │              │                  │               │            │            │ DeltaWriter          │
  │              │                  │               │            │            │  ::write()           │
  │              │                  │               │            │            │           │
  │              │                  │               │            │            │ 首次写入:           │
  │              │                  │               │            │            │  ├ init()           │
  │              │                  │               │            │            │  │  TxnManager       │
  │              │                  │               │            │            │  │   .prepare_txn() │
  │              │                  │               │            │            │  └ 创建 RowsetWriter│
  │              │                  │               │            │            │           │
  │              │                  │               │            │            │ MemTable  │           │
  │              │                  │               │            │            │  .insert()│           │
  │              │                  │               │            │            │           │
  │              │                  │               │            │            │ ┌─按数据模型分支──┐  │
  │              │                  │               │            │            │ │                 │  │
  │              │                  │               │            │            │ │ DUP_KEYS:       │  │
  │              │                  │               │            │            │ │  SkipList.Insert()│ │
  │              │                  │               │            │            │ │                 │  │
  │              │                  │               │            │            │ │ UNIQUE/AGG_KEYS:│  │
  │              │                  │               │            │            │ │  SkipList.Find() │  │
  │              │                  │               │            │            │ │  ├ Key存在→聚合  │  │
  │              │                  │               │            │            │ │  └ Key不存在→插入│  │
  │              │                  │               │            │            │ └─────────────────┘  │
  │              │                  │               │            │            │           │
  │              │                  │               │            │            │ ┌─MemTable满时────┐  │
  │              │                  │               │            │            │ │ >= write_buffer  │  │
  │              │                  │               │            │            │ │ → 异步刷盘 ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─>│
  │              │                  │               │            │            │ │ → 新建 MemTable │  │
  │              │                  │               │            │            │ └─────────────────┘  │
  │              │                  │               │            │            │           │


════════════════════════════════════════════════════════════════════════════════
第六阶段: MemTable 异步刷盘 (BE-Target → Storage)
════════════════════════════════════════════════════════════════════════════════

Client          FE                TxnMgr           Coord        BE-C         BE-T       Storage
  │              │                  │               │            │            │           │
  │              │                  │               │            │            │──flush──>│
  │              │                  │               │            │            │  (async) │
  │              │                  │               │            │            │           │
  │              │                  │               │            │            │ FlushToken│
  │              │                  │               │            │            │  ._flush_ │
  │              │                  │               │            │            │   memtable│
  │              │                  │               │            │            │           │
  │              │                  │               │            │            │      MemTable::flush()
  │              │                  │               │            │            │      → BetaRowsetWriter
  │              │                  │               │            │            │        .flush_single_memtable()
  │              │                  │               │            │            │           │
  │              │                  │               │            │            │      ┌─SkipList 迭代每行─┐
  │              │                  │               │            │            │      │                   │
  │              │                  │               │            │            │      │ SegmentWriter      │
  │              │                  │               │            │            │      │  .append_row()     │
  │              │                  │               │            │            │      │                   │
  │              │                  │               │            │            │      │  ┌─每列循环───────┐│
  │              │                  │               │            │            │      │  │ ScalarColWriter ││
  │              │                  │               │            │            │      │  │  .append(cell)  ││
  │              │                  │               │            │            │      │  │  ├ PageBuilder  ││
  │              │                  │               │            │            │      │  │  │  .add()      ││
  │              │                  │               │            │            │      │  │  │  (DICT/PLAIN/ ││
  │              │                  │               │            │            │      │  │  │   BIT_SHUFFLE)││
  │              │                  │               │            │            │      │  │  ├ 更新 ZoneMap ││
  │              │                  │               │            │            │      │  │  │  BloomFilter ││
  │              │                  │               │            │            │      │  │  │  BitmapIndex ││
  │              │                  │               │            │            │      │  │  └ Page满时:    ││
  │              │                  │               │            │            │      │  │    finish_page()││
  │              │                  │               │            │            │      │  │    → LZ4压缩   ││
  │              │                  │               │            │            │      │  │    → Page链表  ││
  │              │                  │               │            │            │      │  └────────────────┘│
  │              │                  │               │            │            │      │                   │
  │              │                  │               │            │            │      │  每N行添加Short Key│
  │              │                  │               │            │            │      └───────────────────┘
  │              │                  │               │            │            │           │
  │              │                  │               │            │            │    SegmentWriter::finalize()
  │              │                  │               │            │            │    ┌──────────────────────┐
  │              │                  │               │            │            │    │1 _write_data()       │
  │              │                  │               │            │            │    │  → 所有列数据页      │
  │              │                  │               │            │            │    │2 _write_ordinal_index│
  │              │                  │               │            │            │    │  → B-Tree 页偏移     │
  │              │                  │               │            │            │    │3 _write_zone_map()   │
  │              │                  │               │            │            │    │  → min/max 统计      │
  │              │                  │               │            │            │    │4 _write_bitmap_index│
  │              │                  │               │            │            │    │  → Roaring Bitmap   │
  │              │                  │               │            │            │    │5 _write_bloom_filter │
  │              │                  │               │            │            │    │  → BlockSplit BF    │
  │              │                  │               │            │            │    │6 _write_short_key    │
  │              │                  │               │            │            │    │  → Short Key稀疏索引│
  │              │                  │               │            │            │    │7 _write_footer()     │
  │              │                  │               │            │            │    │  → FooterPB+CRC32C │
  │              │                  │               │            │            │    │    +"D0R1" 魔数     │
  │              │                  │               │            │            │    └──────────────────────┘
  │              │                  │               │            │            │           │
  │              │                  │               │            │            │     Segment .dat 落盘
  │              │                  │               │            │            │     data/{shard}/{tablet}/
  │              │                  │               │            │            │      {schema}/{rowset}/
  │              │                  │               │            │            │      00000000.dat
  │              │                  │               │            │            │           │


════════════════════════════════════════════════════════════════════════════════
第七阶段: 事务提交与发布 (BE → FE → Client)
════════════════════════════════════════════════════════════════════════════════

Client          FE                TxnMgr           Coord        BE-C         BE-T       Storage
  │              │                  │               │            │            │           │
  │              │                  │               │            │ mark_close │           │
  │              │                  │               │            │  (eos=true)│           │
  │              │                  │               │            │            │           │
  │              │                  │               │            │            │ TabletsChannel
  │              │                  │               │            │            │  ::close() │
  │              │                  │               │            │            │   DeltaWriter
  │              │                  │               │            │            │   ::close() │
  │              │                  │               │            │            │           │
  │              │                  │               │            │            │ 刷最后一个MemTable
  │              │                  │               │            │            │──flush──>│
  │              │                  │               │            │            │           │
  │              │                  │               │            │            │ FlushToken│
  │              │                  │               │            │            │  .wait()  │
  │              │                  │               │            │            │ 等待刷盘完成│
  │              │                  │               │            │            │           │
  │              │                  │               │            │            │ _rowset_  │
  │              │                  │               │            │            │ writer    │
  │              │                  │               │            │            │  .build() │
  │              │                  │               │            │            │ →BetaRowset│
  │              │                  │               │            │            │           │
  │              │                  │               │            │            │ TxnManager│
  │              │                  │               │            │            │ .commit_  │
  │              │                  │               │            │            │  txn()    │
  │              │                  │               │            │            │ →COMMITTED│
  │              │                  │               │            │            │           │
  │              │                  │               │            │<──CommitInfo─│           │
  │              │                  │               │            │  (tablet_id,│           │
  │              │                  │               │            │   num_rows) │           │
  │              │                  │               │            │            │           │
  │              │                  │               │  reportExecStatus         │           │
  │              │                  │               │<────────────────────────│           │
  │              │                  │               │  (RPC callback)          │           │
  │              │                  │               │            │            │           │
  │              │                  │               │ 收集所有 Fragment         │           │
  │              │                  │               │ 的 TTabletCommitInfo      │           │
  │              │                  │               │            │            │           │
  │              │<──coord.join()────│               │            │            │           │
  │              │  所有Fragment完成  │               │            │            │           │
  │              │                  │               │            │            │           │
  │              │──commitAndPublish──>│            │            │            │           │
  │              │                  │               │            │            │           │
  │              │                  │ commitTransaction()         │            │           │
  │              │                  │  ├ 校验 CommitInfo          │            │           │
  │              │                  │  │ (tablet行数等)            │            │           │
  │              │                  │  ├ PREPARE → COMMITTED      │            │           │
  │              │                  │  └ 写 EditLog 持久化         │            │           │
  │              │                  │               │            │            │           │
  │              │                  │ publishTransaction()        │            │           │
  │              │                  │  └ COMMITTED → VISIBLE      │            │           │
  │              │                  │    (等待PublishVersionDaemon)│            │           │
  │              │                  │               │            │            │           │
  │<──返回结果────│                  │               │            │            │           │
  │  {"label":"..│                  │               │            │            │           │
  │   status":   │                  │               │            │            │           │
  │   "VISIBLE", │                  │               │            │            │           │
  │   "txnId":   │                  │               │            │            │           │
  │   "..."}      │                  │               │            │            │           │
  │              │                  │               │            │            │           │
  ▼              ▼                  ▼               ▼            ▼            ▼           ▼


════════════════════════════════════════════════════════════════════════════════
完整数据流架构图
════════════════════════════════════════════════════════════════════════════════

┌──────────────────────────────────────────────────────────────────────┐
│                           FE (Frontend)                              │
│                                                                      │
│  ┌─────────────┐    ┌──────────────┐    ┌──────────────────────┐    │
│  │ConnectProc.  │───>│StmtExecutor  │───>│ Planner              │    │
│  │SQL 解析      │    │语义分析+执行  │    │ 生成 Fragment        │    │
│  └─────────────┘    └──────┬───────┘    └──────────┬───────────┘    │
│                            │                        │                │
│                            ▼                        ▼                │
│                    ┌──────────────┐        ┌─────────────────┐      │
│                    │InsertStmt    │        │Coordinator      │      │
│                    │事务开始      │        │Fragment 调度    │      │
│                    │OlapTableSink │        │结果收集          │      │
│                    └──────┬───────┘        └────────┬────────┘      │
│                           │                         │                │
│                           ▼                         │                │
│                    ┌──────────────────────┐         │                │
│                    │GlobalTransactionMgr  │<────────┘                │
│                    │ beginTxn / commitTxn │                          │
│                    │ / publishTxn         │                          │
│                    └──────────────────────┘                          │
└───────────────────────────┬──────────────────────────────────────────┘
                            │
              ┌─────────────┼─────────────┐
              │ Thrift/brpc │ Thrift/brpc │
              ▼             │             ▼
┌─────────────────────┐     │   ┌─────────────────────┐
│  BE (Coordinator)   │     │   │  BE (Target Node)   │
│                     │     │   │                     │
│ ┌─────────────────┐ │     │   │ ┌─────────────────┐ │
│ │PlanFragmentExec │ │     │   │ │LoadChannelMgr   │ │
│ │执行计划树       │ │     │   │ └───────┬─────────┘ │
│ └───────┬─────────┘ │     │   │         │           │
│         │           │     │   │ ┌───────▼─────────┐ │
│ ┌───────▼─────────┐ │     │   │ │TabletsChannel   │ │
│ │VOlapTableSink   │─┼─────┼──>│ │按 tablet 分发   │ │
│ │分区/分桶路由    │ │     │   │ └───────┬─────────┘ │
│ │Batch 累积+发送  │ │     │   │         │           │
│ └─────────────────┘ │     │   │ ┌───────▼─────────┐ │
│ ┌─────────────────┐ │     │   │ │DeltaWriter x N   │ │
│ │NodeChannel x N  │─┼─────┘   │ │(每 tablet 一个) │ │
│ │brpc 异步发送    │ │         │ └───────┬─────────┘ │
│ └─────────────────┘ │         │         │           │
└─────────────────────┘         │ ┌───────▼─────────┐ │
                                │ │MemTable         │ │
                                │ │SkipList 排序    │ │
                                │ │Key 去重/聚合    │ │
                                │ └───────┬─────────┘ │
                                │         │ async     │
                                │ ┌───────▼─────────┐ │
                                │ │BetaRowsetWriter │ │
                                │ └───────┬─────────┘ │
                                │         │           │
                                │ ┌───────▼─────────┐ │
                                │ │SegmentWriter    │ │
                                │ │┌─────┐┌─────┐  │ │
                                │ ││Col A││Col B│.. │ │
                                │ ││Pages││Pages│   │ │
                                │ │└─────┘└─────┘  │ │
                                │ │+ 5种索引        │ │
                                │ │+ Footer         │ │
                                │ └───────┬─────────┘ │
                                │         │           │
                                │         ▼           │
                                │   Segment .dat      │
                                │   落盘              │
                                └─────────────────────┘
