# NCCL Enqueue 层分析

## 1. 一句话概括

Enqueue 层是 NCCL 的「调度中枢」，位于用户 API 和 GPU Kernel 之间。它负责将用户的集合调用转化为具体任务、选择最优算法和协议、分配通道、注册内存、构建 work 描述符，最终启动 GPU Kernel 执行。

## 2. Enqueue 层在整体流程中的位置

```
用户 API:  ncclAllReduce(sendbuf, recvbuf, count, op, comm, stream)
    │
    ▼
Enqueue 层（CPU 端，本文重点）:
    ├── ncclEnqueueCheck     → 校验参数，隐式 Group 包装
    ├── taskAppend           → 路由到不同任务类型（coll/p2p/ce/rma）
    ├── collTaskAppend       → 创建 ncclTaskColl，按 trafficBytes 排序插入
    ├── ncclPrepareTasks     → 算法选择 + 协议选择 + 通道分配
    ├── ncclTasksRegAndEnqueue → 注册 buffer，构建 ncclDevWorkColl 描述符
    └── ncclLaunchKernel     → 启动 GPU Kernel
    │
    ▼
GPU Kernel:  ncclKernelMain → RunWorkBatch → Primitives send/recv
```

## 3. 入口函数：ncclEnqueueCheck

定义在 `enqueue.cc:3016`，是所有集合 API 的统一入口：

```
ncclEnqueueCheck(info):
  1. CommCheck              → 检查 comm 是否有效、是否被 revoke
  2. ncclGroupStartInternal → 隐式开启 Group（ncclGroupDepth++）
  3. ncclCommEnsureReady    → 如果 comm 还在异步初始化，等待完成
  4. cudaSetDevice          → 切换到正确的 GPU
  5. ArgsCheck              → 参数合法性校验
  6. taskAppend             → 创建任务并加入 planner
  7. ncclGroupEndInternal   → 隐式关闭 Group（ncclGroupDepth--）
                              如果 depth 归零则触发 groupLaunch 执行所有任务
```

**关键设计：** 单独调用 `ncclAllReduce()`（不在 Group 内部）会被隐式包装为 depth 0→1→0 的 Group，在 `ncclGroupEndInternal` 时立即触发执行。

## 4. 任务路由：taskAppend

定义在 `enqueue.cc:2920`，根据集合类型路由到不同的任务处理路径：

```
taskAppend(comm, info):
  │
  ├── ncclFuncSend / ncclFuncRecv
  │   → p2pTaskAppend          点对点发送/接收
  │
  ├── ncclFuncPutSignal / Signal / WaitSignal
  │   → rmaTaskAppend          单边 RMA 操作
  │
  ├── count == 0
  │   → 直接返回               空操作，不做任何事
  │
  ├── nRanks == 1
  │   → ncclLaunchOneRank      单 rank 优化（直接 cudaMemcpy）
  │
  ├── CE 可用且允许零拷贝策略
  │   → ceCollTaskAppend       CE（Compute Engine）集合任务
  │
  ├── ncclFuncAlltoAll
  │   → 分解为 nRanks 对 p2pTaskAppend（Send + Recv）
  │
  ├── ncclFuncGather / ncclFuncScatter
  │   → 分解为一对多 Send 或 多对一 Recv
  │
  ├── AllGather 在 Blackwell + 大消息
  │   → ceCollTaskAppend       Blackwell CE 优化路径
  │
  └── 其他（AllReduce/ReduceScatter/Broadcast/Reduce）
      → collTaskAppend        标准 kernel 集合任务
```

**要点：**
- **AlltoAll、Gather、Scatter 不是真正的集合操作**，NCCL 将它们分解为成对的 Send/Recv
- 只有 AllReduce、ReduceScatter、Broadcast、Reduce 走真正的 kernel 集合路径
- 单 rank 的情况直接做 cudaMemcpy，不经过任何通信

## 5. 集合任务创建：collTaskAppend

定义在 `enqueue.cc:2580`，创建 `ncclTaskColl` 并加入排序队列：

```
collTaskAppend(comm, info, opDev):
  1. ncclGroupCommJoin    → 将 comm 加入线程本地的 Group 链表
  2. 分配 ncclTaskColl     → 从内存池分配
  3. 填充任务字段:
       func         = AllReduce / ReduceScatter / ...
       sendbuff     = 用户输入 buffer 指针
       recvbuff     = 用户输出 buffer 指针
       count        = 元素个数
       datatype     = 数据类型
       opDev        = reduce 操作的设备端描述
       chunkSteps   = chunk 分步数（AllReduce=NCCL_STEPS/2, 其他=1）
       sliceSteps   = slice 分步数
  4. 计算 trafficBytes = count * elementSize * trafficPerByte(func, nRanks)
     AllReduce 的 trafficPerByte = 2（数据进出各一次）
     AllGather/ReduceScatter 的 trafficPerByte = nRanks
  5. ncclTaskCollSorterInsert → 按 trafficBytes 降序插入排序器
```

**为什么要按 trafficBytes 降序排序？**
大任务优先调度，最大化带宽利用率。NCCL 的通道并行机制会将大数据分到多个 channel 上，大任务能更好地利用所有通道。

## 6. 算法选择：ncclPrepareTasks

定义在 `enqueue.cc:362`，是 Enqueue 层最核心的函数，在 `ncclGroupEnd` 触发时执行：

```
ncclPrepareTasks(comm):
  │
  ├── 1. 处理 Broadcast 优化
  │     如果只有一个 bcast peer，将其转为 AllGatherV 任务
  │
  ├── 2. 从排序器取出所有任务（大优先）
  │     task = ncclTaskCollSorterDequeueAll()
  │
  ├── 3. 按 (func, op, datatype) 分桶聚合
  │     三个维度相同的任务合并为一个聚合任务（agg）
  │     合并条件：trafficBytes 在 4 倍以内
  │     目的：同一类任务共享同一套算法/协议参数
  │
  ├── 4. 对每个聚合桶调用 ncclGetAlgoInfo
  │     输入：comm 拓扑信息 + 聚合任务的 count/datatype
  │     输出：
  │       ├── algorithm = RING / TREE / COLLNET_DIRECT / COLLNET_CHAIN / NVLS / NVLS_TREE / PAT
  │       ├── protocol  = SIMPLE / LL / LL128
  │       ├── nMaxChannels = 可用最大通道数
  │       ├── nWarps    = 每个 CTA 使用的 warp 数
  │       └── devFuncId = 对应的 GPU Kernel 函数 ID
  │
  ├── 5. 计算 devFuncId
  │     devFuncId = ncclDevFuncId(func, op, datatype, algorithm, protocol)
  │     用于运行时或编译时选择正确的特化 kernel
  │
  ├── 6. 按调度约束重新分桶
  │     collBins[isCollnet][isNvls]
  │     四种类型：标准 / CollNet / NVLS / CollNet+NVLS
  │     因为 CollNet 和 NVLS 需要特殊的通道分配
  │
  ├── 7. 注册 buffer
  │     ncclRegisterCollBuffers / ncclRegisterCollNvlsBuffers
  │     为 DMA-BUF / IPC / NVLS 注册用户 buffer，启用零拷贝
  │
  └── 8. 构建 ncclDevWorkColl 描述符
        填充：sendbuff, recvbuff, root, nWarps, redOpArg, oneNode, netRegUsed
        如果是 NVLS：使用 ncclDevWorkCollReg（包含注册后的 buffer handle）
        加入 collWorkQueue 等待 kernel launch
```

## 7. 算法选择详解：ncclGetAlgoInfo

这是决定性能的关键函数，根据消息大小和拓扑选择最优算法和协议：

### 7.1 算法选择依据

```
消息大小            优先选择算法
─────────────────────────────────
极小 (< 4KB)      Tree（延迟低，步骤少）
小 (4KB ~ 128KB)  Tree 或 Ring
中 (128KB ~ 1MB)   Ring（带宽最优）
大 (> 1MB)        Ring 或 Tree（通道并行充分）
```

### 7.2 协议选择依据

```
消息大小 + 传输类型         优先选择协议
───────────────────────────────────
同节点 NVLink (< 128KB)    SIMPLE + Direct 模式（零拷贝直写）
同节点 NVLink (任意大小)    SIMPLE
跨节点网络 (< 2KB)         LL（flag-word 开销可忽略）
跨节点网络 (2KB ~ 128KB)   LL128（128-bit 打包效率高）
跨节点网络 (> 128KB)       LL
```

### 7.3 特殊路径

| 条件 | 算法 | 说明 |
|------|------|------|
| 节点内全部 GPU + NVLink | NVLS | 通过 NVLink 互联做单节点集合，无需经过 CPU |
| 节点内 NVLS + 节点间网络 | NVLS_TREE | 节点内 NVLS reduce，节点间 Tree broadcast |
| 有 SHARP 硬件 | COLLNET_DIRECT / CHAIN | 利用交换机内聚合能力卸载 |
| 自适应拓扑 | PAT | 递归倍增/减半，适合异构网络 |

## 8. Work 描述符构建：ncclTasksRegAndEnqueue

定义在 `enqueue.cc:299`，将 `ncclTaskColl` 转化为 GPU Kernel 可消费的 `ncclDevWorkColl`：

```
ncclTasksRegAndEnqueue(comm):
  对 collTaskQueue 中的每个 task:

  1. ncclRegisterCollBuffers
     检查用户 buffer 是否需要注册:
     - 跨节点 NET 传输 → DMA-BUF 注册（GPU 直连 NIC，零拷贝）
     - 同节点 P2P IPC  → CUDA IPC 注册
     - NVLS            → NVLS 专用注册

  2. 填充 ncclDevWorkColl（GPU 端结构体）:
     sendbuff       用户输入 buffer
     recvbuff       用户输出 buffer
     root           root rank（Broadcast/Reduce 用）
     nWarps         CTA 使用的 warp 数
     redOpArg       reduce 操作参数（Sum 的标量值等）
     oneNode        是否单节点
     netRegUsed     是否使用了 DMA-BUF 注册
     regUsed        是否使用了 IPC/NVLS 注册

  3. 如果是 NVLS 且有注册 buffer → 使用 ncclDevWorkCollReg（多了 dnInputs/dnOutputs）

  4. 加入 collWorkQueue → 等待 ncclLaunchKernel 消费
```

## 9. Kernel 启动：ncclLaunchKernel

Enqueue 层的最终输出就是启动 GPU Kernel：

```
ncclLaunchKernel(comm, plan):
  1. 从 collWorkQueue 取出所有 ncclDevWorkColl
  2. 打包成 ncclDevKernelArgs 结构
  3. 设置 channelMask（哪些 channel 参与本次执行）
  4. 计算启动参数:
     Grid:  nChannels 个 block
     Block: nWarps * 32 个线程
     Shared memory: ncclShmemDynamicSize 字节
     Stream: 用户指定的 CUDA stream
  5. 选择正确的 kernel 函数:
     如果有编译时特化 → 直接调用 ncclDevKernel_XXX
     否则 → 调用 ncclDevKernel_Generic（运行时通过函数指针分发）
  6. <<< 启动 kernel >>>
```

## 10. Enqueue 层的完整数据流

以 `ncclAllReduce(sendbuf, recvbuf, 1MB float, Sum, comm, stream)` 为例：

```
ncclAllReduce(sendbuf, recvbuf, 1MB float, Sum, comm, stream)
  │
  ▼ ncclEnqueueCheck
  │ 校验 comm 有效、buffer 非空、datatype 合法
  │ ncclCommEnsureReady 等待初始化完成
  │
  ▼ taskAppend → collTaskAppend
  │ 创建 ncclTaskColl:
  │   func = AllReduce
  │   sendbuff = sendbuf
  │   recvbuff = recvbuf
  │   count = 262144 (1MB / 4B)
  │   datatype = float
  │   op = Sum
  │   trafficBytes = 262144 * 4 * 2 = 2MB
  │ 插入 collSorter（降序）
  │
  ▼ ncclGroupEndInternal → groupLaunch
  │
  ▼ ncclPrepareTasks
  │ 从排序器取出任务
  │ ncclGetAlgoInfo:
  │   1MB float → algorithm = RING
  │   1MB + 同节点 NVLink → protocol = SIMPLE
  │   nMaxChannels = 8（假设拓扑分析结果）
  │   nWarps = 16
  │ ncclRegisterCollBuffers: 检查 buffer，可能注册 DMA-BUF
  │ 构建 ncclDevWorkColl:
  │   sendbuff = sendbuf
  │   recvbuff = recvbuf
  │   nWarps = 16
  │   redOpArg = 1.0f (Sum 的标量)
  │ 加入 collWorkQueue
  │
  ▼ ncclTasksRegAndEnqueue
  │ 对每个 work node 做最终处理
  │
  ▼ ncclLaunchKernel
  │ 选择 ncclDevKernel_AllReduce_Sum_float_RING_SIMPLE
  │ <<< 8 blocks, 512 threads, shmem, stream >>>
  │
  ▼ GPU Kernel 执行
    每个 block 处理 128KB（1MB / 8 channels）
    Ring AllReduce: reduce-scatter + all-gather
    通过 FIFO + head/tail 与对端 GPU 同步
```

## 11. Enqueue 层的关键设计思想

| 设计点 | 说明 |
|------|------|
| **延迟执行** | API 调用不立即执行，而是创建 task 攒到 GroupEnd 时统一执行。好处：多个操作可以合并优化、通道分配更全局 |
| **隐式 Group** | 单个 API 调用自动包装为 Group，用户无感知 |
| **大任务优先** | 按 trafficBytes 降序排列，最大化通道并行利用率 |
| **同类聚合** | 相同 (func,op,datatype) 的任务聚合后共享算法/协议参数，减少重复计算 |
| **运行时连接** | runtimeConn 模式下，算法首次使用时才建立传输连接（lazy connect） |
| **buffer 注册** | 支持运行时检测用户 buffer 是否可注册（DMA-BUF/IPC/NVLS），启用零拷贝直传 |
| **多路径分发** | AlltoAll/Gather/Scatter 降级为 P2P，单 rank 降级为 cudaMemcpy，CE 可用时走 CE 路径 |

## 12. 源码索引

| 文件 | 行号 | 内容 |
|------|------|------|
| `enqueue.cc` | 3016 | ncclEnqueueCheck — 所有 API 的统一入口 |
| `enqueue.cc` | 2920 | taskAppend — 任务路由（coll/p2p/ce/rma） |
| `enqueue.cc` | 2580 | collTaskAppend — 创建集合任务并排序插入 |
| `enqueue.cc` | 362 | ncclPrepareTasks — 算法选择 + 通道分配 + buffer 注册 |
| `enqueue.cc` | 299 | ncclTasksRegAndEnqueue — 构建 ncclDevWorkColl |
| `group.cc` | 753 | ncclGroupEndInternal — 触发 groupLaunch |
| `group.cc` | 587 | groupLaunch — 启动异步任务 + 同步准备 |
| `group.cc` | 307 | doLaunches — 遍历 clique 启动 kernel |
