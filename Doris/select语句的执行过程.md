# Apache Doris SELECT 语句执行流程分析

## 一、总体架构概览

```
                           +-------------------+
                           |     MySQL Client  |
                           +--------+----------+
                                    | MySQL Protocol
                                    v
+----------------------------------------------------------------------+
|                         FE (Frontend)                                |
|                                                                      |
|  QeService -> MysqlServer -> ConnectScheduler -> ConnectProcessor    |
|       -> StmtExecutor -> Analyzer -> Planner -> Coordinator          |
|                                                                      |
+----------------------------------------------------------------------+
         | Thrift RPC (exec_plan_fragment)        | Thrift RPC (fetch_data)
         v                                        v
+-------------------+                    +-------------------+
|   BE Node 1       |   Data Exchange    |   BE Node 2       |
|   FragmentMgr     |  (brpc/Channel)    |   FragmentMgr     |
|   PlanFragment    |<------------------>|   PlanFragment    |
|   Executor        |                    |   Executor        |
|                   |                    |                   |
|  OlapScanNode     |                    |  OlapScanNode     |
|  -> VOlapScanner  |                    |  -> VOlapScanner  |
|  -> BlockReader   |                    |  -> BlockReader   |
+-------------------+                    +-------------------+
```

## 二、完整执行流程时序图（Mermaid）

```mermaid
sequenceDiagram
    participant Client as MySQL Client
    participant CP as ConnectProcessor
    participant SE as StmtExecutor
    participant Parser as SqlParser (CUP/JFlex)
    participant Analyzer as Analyzer
    participant Planner as Planner
    participant SNP as SingleNodePlanner
    participant DP as DistributedPlanner
    participant Coord as Coordinator
    participant QEP as QeProcessorImpl
    participant BE as BE Node(s)
    participant FM as FragmentMgr
    participant PFE as PlanFragmentExecutor
    participant EN as ExecNode Tree
    participant Scan as OlapScanNode
    participant BR as BlockReader

    Note over Client,BR: Phase 1: 连接与SQL解析
    Client->>CP: MySQL COM_QUERY (SELECT ...)
    CP->>Parser: SqlScanner + SqlParser.parse()
    Parser-->>CP: List<StatementBase> (SelectStmt AST)

    Note over Client,BR: Phase 2: 语义分析
    CP->>SE: new StmtExecutor(ctx, parsedStmt)
    CP->>SE: execute(queryId)
    SE->>SE: analyze(tQueryOptions)
    SE->>Analyzer: new Analyzer(catalog, context)
    SE->>Analyzer: parsedStmt.analyze(analyzer)
    Analyzer-->>SE: 表/列解析完成, TupleDescriptor 创建

    Note over Client,BR: Phase 3: 表达式重写 (RBO)
    SE->>Analyzer: ExprRewriter.apply()
    Note right of Analyzer: 常量折叠、谓词标准化、IN推导等
    SE->>SE: StmtRewriter.rewrite() (子查询展开)
    SE->>Analyzer: parsedStmt.analyze() (重新分析)

    Note over Client,BR: Phase 4: 查询计划生成
    SE->>Planner: new Planner()
    SE->>Planner: planner.plan(parsedStmt, analyzer)

    Note over Client,BR: Phase 4a: 单节点逻辑计划
    Planner->>SNP: createSingleNodePlan(queryStmt, ctx)
    SNP-->>Planner: PlanNode 树 (OlapScanNode, HashJoinNode, AggregationNode, SortNode...)

    Note over Client,BR: Phase 4b: 物化视图选择
    Planner->>Planner: MaterializedViewSelector.rewrite()

    Note over Client,BR: Phase 4c: 分布式计划生成
    Planner->>DP: createPlanFragments(root, ...)
    DP-->>Planner: List<PlanFragment> + ExchangeNode 插入

    Note over Client,BR: Phase 4d: 运行时过滤生成
    Planner->>Planner: RuntimeFilterGenerator.generateRuntimeFilters()
    Planner->>Planner: PlanFragment.finalize() (DataSink分配)

    Note over Client,BR: Phase 5: 分布式执行调度
    SE->>SE: handleQueryStmt()
    SE->>Coord: new Coordinator(ctx, analyzer, planner)
    SE->>QEP: registerQuery(queryId, coord)
    SE->>Coord: exec()

    Note over Client,BR: Phase 5a: Coordinator 准备
    Coord->>Coord: prepare()
    Coord->>Coord: computeScanRangeAssignment()
    Note right of Coord: 将 Tablet 分配到 BE 节点
    Coord->>Coord: computeFragmentExecParams()
    Coord->>Coord: sendFragment()

    Note over Client,BR: Phase 5b: Fragment 分发到 BE
    loop 对每个 Fragment (从叶子到根)
        Coord->>BE: exec_plan_fragment RPC (TExecPlanFragmentParams)
        BE->>FM: FragmentMgr.exec_plan_fragment()
        FM->>FM: 创建/复用 QueryFragmentsCtx
        FM->>PFE: FragmentExecState.prepare()
        PFE->>EN: ExecNode.create_tree() (TPlan -> ExecNode树)
        PFE->>EN: _plan->prepare(state)
        PFE->>EN: 分配 ScanRange 到 ScanNode
        FM->>PFE: 线程池提交 -> execute()
    end

    Note over Client,BR: Phase 6: BE 端执行
    PFE->>PFE: open()
    PFE->>EN: _plan->open(state)
    EN->>Scan: OlapScanNode.open() -> 启动 Scanner 线程
    Scan->>BR: VOlapScanner -> BlockReader.init()
    Scan->>BR: BlockReader.next_block() (读取 Segment)

    alt 中间 Fragment (有 DataSink)
        loop 拉取数据块
            PFE->>EN: _plan->get_next(state, block)
            EN-->>PFE: Block
            PFE->>PFE: _sink->send(state, block)
            PFE->>BE: VDataStreamSender -> brpc transmit_block()
        end
        BE->>FM: VDataStreamMgr.transmit_block() -> DataStreamRecvr
    end

    Note over Client,BR: Phase 7: 结果返回
    SE->>Coord: coord.getNext() (循环获取结果)
    Coord->>BE: fetch_data RPC
    BE->>PFE: ResultBufferMgr.fetch_data() -> BufferControlBlock
    PFE-->>BE: RowBatch/Block
    BE-->>Coord: 结果批次
    Coord-->>SE: RowBatch
    SE->>Client: channel.sendOnePacket() (MySQL 结果包)
    Note right of SE: 直到 batch.isEos() 返回 EOF

    Note over Client,BR: Phase 8: 执行状态汇报
    BE->>FM: coordinator_callback() (定期)
    FM->>Coord: reportExecStatus RPC
```

## 三、各阶段详细说明

### 3.1 连接接收与 SQL 解析

**入口类**: `PaloFe.main()` -> `QeService` -> `MysqlServer`

| 步骤 | 类 | 方法 | 说明 |
|------|-----|------|------|
| 1 | `PaloFe` | `start()` | 启动 FE 进程，创建 QeService(9030)、FeServer(9020)、HttpServer(8030) |
| 2 | `MysqlServer` | `Listener.run()` | 接受 MySQL 连接，创建 ConnectContext，提交到 ConnectScheduler |
| 3 | `ConnectScheduler` | `LoopHandler.run()` | MySQL 握手认证，创建 ConnectProcessor |
| 4 | `ConnectProcessor` | `dispatch()` | 读取 MySQL 包，COM_QUERY 分发到 handleQuery() |
| 5 | `SqlParser` | `parse()` | CUP/JFlex 生成的语法解析器，SQL 字符串 -> SelectStmt AST |

**关键源文件**:
- `fe/fe-core/src/main/java/org/apache/doris/PaloFe.java`
- `fe/fe-core/src/main/java/org/apache/doris/qe/QeService.java`
- `fe/fe-core/src/main/java/org/apache/doris/mysql/MysqlServer.java`
- `fe/fe-core/src/main/java/org/apache/doris/qe/ConnectScheduler.java`
- `fe/fe-core/src/main/java/org/apache/doris/qe/ConnectProcessor.java`
- `fe/fe-core/src/main/cup/sql_parser.cup` (CUP 语法)
- `fe/fe-core/src/main/jflex/sql_scanner.flex` (JFlex 词法)

### 3.2 语义分析

**核心类**: `Analyzer` + `SelectStmt`

`StmtExecutor.analyzeAndGenerateQueryPlan()` 方法执行以下操作：

1. **`parsedStmt.analyze(analyzer)`** - 对 SelectStmt 进行语义分析：
   - 解析 FROM 子句中的表引用（TableRef）
   - 解析 SELECT 列表中的列引用和表达式
   - 解析 JOIN ON 条件
   - 解析 WHERE / GROUP BY / HAVING / ORDER BY / LIMIT
   - 创建 TupleDescriptor（行内存布局）和 SlotDescriptor（列内存布局）
   - 收集等价类（Equivalence Classes）用于后续优化

2. **`ExprRewriter.apply()`** - 规则驱动的表达式重写：
   - 常量折叠（FoldConstantsRule）
   - 谓词标准化（NormalizePredicateRule）
   - BETWEEN 展开（BetweenPredicateRule）
   - 公因提取（CommonFactorRule）
   - 过滤推导（InferFiltersRule）

3. **`StmtRewriter.rewrite()`** - 子查询重写（如果存在子查询）

**关键源文件**:
- `fe/fe-core/src/main/java/org/apache/doris/analysis/Analyzer.java`
- `fe/fe-core/src/main/java/org/apache/doris/analysis/SelectStmt.java`

### 3.3 查询计划生成

**核心类**: `Planner` -> `SingleNodePlanner` -> `DistributedPlanner`

#### 3.3.1 单节点计划 (SingleNodePlanner)

将分析后的 AST 转换为 PlanNode 树：

```
SelectStmt
    |
    v
SingleNodePlanner.createQueryPlan()
    |
    +-- createScanNode()       -> OlapScanNode
    +-- createJoinNode()       -> HashJoinNode / CrossJoinNode
    +-- createAggregationPlan()-> AggregationNode
    +-- createSortPlan()       -> SortNode
    +-- createSelectNode()     -> SelectNode
    +-- createAnalyticPlan()   -> AnalyticEvalNode
```

**优化点**:
- **Join 排序**: `createCheapestJoinPlan()` 在启用 `enable_join_reorder_based_cost` 时，尝试多种 Join 顺序，选择基数最小的
- **谓词下推**: `PredicatePushDown` 将过滤条件推到 Join 下方
- **分区裁剪**: OlapScanNode 中根据 WHERE 条件裁剪分区/列
- **预聚合检查**: `turnOffPreAgg()` 判断能否使用预聚合数据

#### 3.3.2 分布式计划 (DistributedPlanner)

将单节点计划拆分为分布式 Fragment：

```
PlanNode Tree (single-node)
        |
        v
DistributedPlanner.createPlanFragments()
        |
        +-- 递归遍历 PlanNode 树
        +-- 在数据交换边界处切割
        +-- 插入 ExchangeNode
        +-- 创建 PlanFragment 列表
        +-- 选择数据分布策略
```

**数据分布策略优先级** (以 HashJoin 为例):

| 策略 | 说明 | 条件 |
|------|------|------|
| Colocate Join | 无需数据移动 | 分桶匹配，无 Exchange |
| Bucket Shuffle Join | 仅重分布右表 | 左表分桶信息可用 |
| Broadcast Join | 广播右表 | `broadcastCost < shuffleCost` 且内存允许 |
| Shuffle Join | 两表都重分布 | 默认回退策略 |

**聚合分布策略**:
- 单实例: 本地聚合
- Colocate: 分区匹配时无需重分布
- 两阶段: 本地预聚合 + Exchange + 合并聚合

#### 3.3.3 运行时过滤

`RuntimeFilterGenerator` 在 Join 节点生成运行时过滤：
- IN 过滤（Bloom Filter）
- MIN/MAX 过滤
- 分配到 Probe 侧的 ScanNode

**关键源文件**:
- `fe/fe-core/src/main/java/org/apache/doris/planner/Planner.java`
- `fe/fe-core/src/main/java/org/apache/doris/planner/SingleNodePlanner.java`
- `fe/fe-core/src/main/java/org/apache/doris/planner/DistributedPlanner.java`
- `fe/fe-core/src/main/java/org/apache/doris/planner/PlanFragment.java`

### 3.4 分布式执行调度

**核心类**: `Coordinator`

```
Coordinator.exec()
    |
    +-- prepare()                    # 初始化 Fragment 参数
    +-- computeScanRangeAssignment()  # Tablet -> BE 节点分配
    +-- computeFragmentExecParams()   # 构建每个 Fragment 的执行参数
    +-- sendFragment()                # 从叶子到根分发 Fragment 到 BE
```

**Coordinator -> BE 通信**:
- Fragment 分发: Thrift RPC `exec_plan_fragment`
- 结果获取: Thrift RPC `fetch_data`
- 状态汇报: Thrift RPC `reportExecStatus` (BE 定期汇报)

**关键源文件**:
- `fe/fe-core/src/main/java/org/apache/doris/qe/Coordinator.java`
- `fe/fe-core/src/main/java/org/apache/doris/qe/QeProcessorImpl.java`

### 3.5 BE 端执行引擎

**执行模型**: Volcano (Pull-Based) 模型 + 向量化 (Block) 执行

#### 3.5.1 Fragment 接收与准备

```
PInternalServiceImpl::exec_plan_fragment()   # brpc RPC 入口
    |
    v
FragmentMgr::exec_plan_fragment()
    |
    +-- 去重检查 (_fragment_map)
    +-- 创建/复用 QueryFragmentsCtx        # 共享 DescriptorTbl 等资源
    +-- 创建 FragmentExecState             # 包装 PlanFragmentExecutor
    +-- FragmentExecState::prepare()
    |       +-- PlanFragmentExecutor::prepare()
    |       |     +-- 创建 RuntimeState
    |       |     +-- ExecNode::create_tree()   # TPlan -> ExecNode 树
    |       |     +-- 设置 ScanRange
    |       |     +-- 创建 DataSink (中间 Fragment)
    +-- 线程池提交 -> FragmentExecState::execute()
            |
            +-- PlanFragmentExecutor::open()
            +-- PlanFragmentExecutor::close()
```

#### 3.5.2 数据读取 (OlapScanNode)

```
VOlapScanNode
    |
    +-- open() -> 启动多个 VOlapScanner 线程
    |       |
    |       +-- BlockReader.init()   # 打开 RowsetReader
    |       +-- BlockReader.next_block_with_aggregation()
    |             |
    |             +-- _direct_next_block()              # DUPLICATE KEY 表
    |             +-- _direct_agg_key_next_block()      # AGGREGATE KEY (单 rowset)
    |             +-- _agg_key_next_block()             # AGGREGATE KEY (多 rowset 合并)
    |             +-- _unique_key_next_block()          # UNIQUE KEY (版本合并)
    |                   |
    |                   v
    |             VCollectIterator (合并多个 Rowset/Segment)
    |
    +-- get_next() <- 从 _materialized_blocks 获取 Block
```

**生产者-消费者模式**: Scanner 线程生产 Block 到 `_scan_blocks`，transfer_thread 转移到 `_materialized_blocks`，主线程消费。

#### 3.5.3 Fragment 间数据交换

```
Fragment A (发送方)                    Fragment B (接收方)
====================                   ====================
PlanFragmentExecutor                   PlanFragmentExecutor
  |-- ExecNode tree                        |-- ExchangeNode
       |                                       |
       +-- DataSink                             +-- VDataStreamRecvr
            |                                      |
            +-- VDataStreamSender                 +-- SenderQueue(s)
                 |                                   |
                 +-- Channel (per dest)
                      |
                      +-- brpc transmit_block() --------> VDataStreamMgr::transmit_block()
                      |
                      +-- [本地优化: 直接 add] ----------> VDataStreamRecvr::add_block()
```

**流控机制**: `DataStreamRecvr` 有 `_total_buffer_limit`，缓冲超过限制时停止 ACK，提供反压。

#### 3.5.4 结果返回

**Coordinator Fragment** (顶层 Fragment，无 DataSink):
```
FE Coordinator
    |
    +-- coord.getNext()
    +-- fetch_data RPC
          |
          v
    BE: ResultBufferMgr::fetch_data()
          |
          v
    BE: BufferControlBlock::get_batch()
          |
          v
    BE: PlanFragmentExecutor::get_next()
          |
          v
    BE: _plan->get_next() (ExecNode 树拉取)
```

**关键源文件**:
- `be/src/service/internal_service.cpp` - RPC 入口
- `be/src/runtime/fragment_mgr.h/.cpp` - Fragment 管理
- `be/src/runtime/plan_fragment_executor.h/.cpp` - Fragment 执行
- `be/src/exec/exec_node.h` - ExecNode 基类
- `be/src/vec/exec/volap_scan_node.h` - 向量化扫描节点
- `be/src/vec/exec/volap_scanner.h` - 向量化扫描器
- `be/src/vec/olap/block_reader.h` - Block 读取器
- `be/src/vec/sink/vdata_stream_sender.h` - 数据发送器
- `be/src/vec/runtime/vdata_stream_mgr.h` - 数据流管理
- `be/src/runtime/result_buffer_mgr.h` - 结果缓冲管理
- `be/src/runtime/buffer_control_block.h` - 缓冲控制块

## 四、PlanNode 层次结构

```
PlanNode (abstract)
  |-- ScanNode (abstract)
  |     |-- OlapScanNode          # Doris 表扫描
  |     |-- MysqlScanNode         # MySQL 外表
  |     |-- OdbcScanNode          # ODBC 外表
  |     |-- EsScanNode            # ES 外表
  |     |-- BrokerScanNode        # Broker 外表
  |     |-- SchemaScanNode        # Schema 扫描
  |-- SelectNode                  # 投影/谓词计算
  |-- HashJoinNode                # Hash Join
  |-- CrossJoinNode               # 笛卡尔积
  |-- AggregationNode             # 聚合
  |-- SortNode                    # 排序
  |-- AnalyticEvalNode            # 窗口函数
  |-- ExchangeNode                # Fragment 间数据交换
  |-- MergeNode                   # 合并
  |-- UnionNode                   # UNION
  |-- IntersectNode               # INTERSECT
  |-- ExceptNode                  # EXCEPT
  |-- EmptySetNode                # 空集
  |-- AssertNumRowsNode           # 行数断言
  |-- TableFunctionNode           # 表函数
```

## 五、优化策略汇总

### 规则优化 (RBO)
| 优化 | 阶段 | 说明 |
|------|------|------|
| 表达式重写 | 分析后 | 常量折叠、谓词标准化、BETWEEN 展开 |
| 谓词下推 | 规划中 | 通过等值 Join 下推过滤条件 |
| 分区裁剪 | 扫描规划 | WHERE 条件匹配裁剪分区/列 |
| 预聚合检查 | 扫描规划 | 判断能否使用预聚合的 Rollup 数据 |
| 物化视图重写 | 计划生成 | MaterializedViewSelector 重写为 MV 查询 |
| 运行时过滤 | 计划生成 | Join 谓词生成 Bloom/MIN_MAX 过滤推到扫描 |

### 代价优化 (CBO)
| 优化 | 说明 |
|------|------|
| Join 排序 | `createCheapestJoinPlan()` 多顺序枚举，选最小基数 |
| Join 分布策略 | `JoinCostEvaluation` 评估 Broadcast vs Shuffle 成本 |
| 基数估算 | `PlanNode.computeStats()` + `computeSelectivity()` + 指数退避 |
