# NCCL GPU Kernel 分析

## 1. 一句话概括

NCCL 的 GPU Kernel 是跑在 GPU 上的 CUDA 函数，是所有集合通信的实际执行者。它通过 FIFO 缓冲区和原子计数器与对端 GPU 同步，利用多个 CUDA Block 并行处理不同通道的数据，实现跨 GPU 的数据交换和归约运算。

## 2. Kernel 是什么

当用户调用 `ncclAllReduce()` 等集合 API 时，NCCL 不会在 CPU 上做数据搬运，而是把工作交给 GPU Kernel 完成：

```
用户调用 ncclAllReduce(sendbuf, recvbuf, count, ...)
  ↓
CPU 端：选择算法（Ring/Tree）和协议（Simple/LL/LL128）
CPU 端：构建 work 描述符，分配 channel
  ↓
<<< cudaLaunch ncclDevKernel >>>   ← 启动 GPU Kernel
  ↓
GPU 端：每个 channel 启动一个 CUDA Block
GPU 端：Block 内所有线程协作完成数据搬运和归约
  ↓
Kernel 结束，集合操作完成
```

## 3. Kernel 入口：ncclKernelMain

所有 NCCL Kernel 共享同一个入口函数 `ncclKernelMain`，定义在 `device/common.h:346`：

```
ncclKernelMain(args):
  ├── 1. 拷贝参数到 shared memory
  │     所有线程协作，把 ncclDevKernelArgs 从 global memory 搬到 shared memory
  │     （shared memory 访问延迟极低，后续全部从 shmem 读）
  │
  ├── 2. 映射 blockIdx.x → channelId
  │     通过 channelMask 的 popcount 找到当前 block 对应的 channel 编号
  │     一个 CUDA Block = 一个 Channel
  │
  ├── 3. Warp 0：加载 ncclKernelComm
  │     从 GPU 显存拷贝通信器数据到 shared memory
  │     包含：rank, nRanks, nNodes, abortFlag, buffSizes 等
  │
  ├── 4. Warp 1：加载 ncclDevChannel
  │     从 GPU 显存拷贝通道数据到 shared memory
  │     包含：ring 拓扑（prev, next）、tree 拓扑、peer 连接信息等
  │
  ├── 5. 剩余线程：加载 work batch
  │     把本次要执行的集合任务（buffer 地址、数据量、reduce op 等）加载到 shmem
  │
  └── 6. 主循环：执行 RunWorkBatch
        while (未 abort && 还有任务) {
          调用 RunWorkColl().run(tid, nthreads, work)
          加载下一个 work batch
        }
```

### 3.1 Shared Memory 布局

每个 CUDA Block 使用一块 shared memory 存放运行时数据：

```
ncclShmemData (shared memory):
  ├── args          参数副本（避免重复从 global memory 读）
  ├── channelId     当前 block 负责的 channel 编号
  ├── comm          ncclKernelComm（rank, nRanks, buffSizes, abortFlag）
  ├── channel       ncclDevChannel（ring/tree 拓扑, peer 连接指针）
  ├── funcId        集合操作类型（AllReduce/ReduceScatter/...）
  ├── nWorks        本次 batch 中的任务数
  ├── groups[]      每组工作状态（src/dst 指针, reduce op 参数）
  └── workStorage[] 工作描述符数组（每个 work 的 sendbuff/recvbuff/nWarps 等）
```

## 4. Kernel 实例化机制

NCCL 通过宏为每种（集合类型 + 数据类型 + 算法 + 协议）组合生成一个独立的 Kernel，定义在 `device/common.h:420`：

```c
#define DEFINE_ncclDevKernel(suffix, coll, redop, ty, algo, proto, specializedFnId) \
  __global__ void ncclDevKernel_##suffix(ncclDevKernelArgs4K args4K) { \
    ncclKernelMain<specializedFnId, RunWorkBatch<coll, ty, redop<ty>, algo, proto>>(&args4K.args); \
  }
```

**举例：** AllReduce + float + Sum + Ring + Simple 组合会实例化为：

```c
__global__ void ncclDevKernel_AllReduce_Sum_float_RING_SIMPLE(args) {
  ncclKernelMain<SpecializedFnId, RunWorkBatch<ncclFuncAllReduce, float, Sum<float>, NCCL_ALGO_RING, NCCL_PROTO_SIMPLE>>(args);
}
```

**编译时特化的好处：**
- 消除运行时分支（if/else 在编译期就已确定）
- 编译器可以内联和优化具体的数据路径
- 每个独立 kernel 体积小，指令缓存命中率高

**通用 Kernel 兜底：** 如果某个组合没有预编译特化版本，则使用 `ncclDevKernel_Generic`，通过函数指针表 `ncclDevFuncTable[funcId]()` 做运行时分发。

## 5. 集合操作在 Kernel 内的执行

### 5.1 RunWorkBatch：工作批处理

`RunWorkBatch`（`device/common.h:279`）负责执行一个 batch 中的所有 work：

```
RunWorkBatch.run():
  1. 如果需要加载 reduce op 参数（如 PreMulSum 的标量），先并行加载
  2. for each work in batch:
       if (tid < work->nWarps * WARP_SIZE):
         RunWorkColl<Fn, T, RedOp, Algo, Proto>().run(tid, nthreads, work)
       如果下一个 work 的 nWarps 不同，插一个 __syncthreads()
```

支持多个 work 打包到一个 batch 中，减少 kernel launch 次数。

### 5.2 AllReduce Ring 算法在 Kernel 中的实现

以 4 个 rank 的 Ring AllReduce 为例（`device/all_reduce.h:14`）：

```
runRing(tid, nthreads, work):
  1. 创建 Primitives 对象
     Primitives<T, RedOp, FanSymmetric<1>, 1, Proto, 0> prims(
       tid, nthreads,
       &ring->prev,     // 从哪个 rank 接收
       &ring->next,     // 发给哪个 rank
       work->sendbuff,  // 用户输入 buffer
       work->recvbuff   // 用户输出 buffer
     )

  2. 计算每个 chunk 的大小（channelCount / nRanks）

  3. 循环处理数据：

     Phase 1: Reduce-Scatter
     ┌─────────────────────────────────────────────────────────────┐
     │ Step 0: prims.directSend(myChunk)                           │
     │         把自己的数据 chunk 直接写到 next 的 buffer           │
     │                                                             │
     │ Step 1~k-2: prims.directRecvReduceDirectSend(offset)        │
     │              从 prev 收一个 chunk → 与本地数据 reduce → 转发给 next │
     │                                                             │
     │ Step k-1: prims.directRecvReduceCopyDirectSend(offset)       │
     │            从 prev 收 chunk → reduce → 存到 recvbuff → 转发 │
     │            这一步产生了属于本 rank 的最终 reduce 结果         │
     └─────────────────────────────────────────────────────────────┘

     Phase 2: All-Gather
     ┌─────────────────────────────────────────────────────────────┐
     │ Step k~2k-3: prims.directRecvCopyDirectSend(offset)          │
     │              从 prev 收 chunk → 存到 recvbuff → 转发给 next  │
     │                                                             │
     │ Final: prims.directRecv(offset)                             │
     │        从 prev 接收最终结果                                  │
     └─────────────────────────────────────────────────────────────┘
```

### 5.3 4 Rank Ring AllReduce 数据流示意

```
Rank 0 持有 [A0|A1|A2|A3]
Rank 1 持有 [B0|B1|B2|B3]
Rank 2 持有 [C0|C1|C2|C3]
Rank 3 持有 [D0|D1|D2|D3]
环方向: 0→1→2→3→0

Phase 1 Reduce-Scatter（每个 rank 最终得到一个完整的 reduce chunk）:
  Step 0: 0 发 A2 给 1, 1 发 B3 给 2, 2 发 C0 给 3, 3 发 D1 给 0
  Step 1: 0 发 D1+A1 给 1, 1 发 A2+B2 给 2, 2 发 B3+C3 给 3, 3 发 C0+D0 给 0
  Step 2: 0 得到 C0+D0+A0（完整 chunk 0）, 1 得到 D1+A1+B1（完整 chunk 1）, ...

Phase 2 All-Gather（扩散完整结果）:
  Step 3: 0 发 chunk0 给 1
  Step 4: 1 转发 chunk0 给 2
  Step 5: 2 转发 chunk0 给 3
  ... 最终所有 rank 持有完整结果
```

## 6. Primitives：底层数据搬运原语

Primitives 是 kernel 中最核心的模板类，封装了对端通信的同步逻辑。

### 6.1 三种协议

```
╔══════════════════╦════════════════════════════════════════════════════════════╗
║ ProtoSimple      ║ FIFO 缓冲区 + head/tail 计数器同步                      ║
║                  ║ 发送方写数据到 FIFO 后更新 tail                          ║
║                  ║ 接收方轮询 tail 到达后读数据再更新 head                  ║
║                  ║ 支持 Direct 模式跳过 FIFO 直接写对端 output buffer       ║
╠══════════════════╬════════════════════════════════════════════════════════════╣
║ ProtoLL          ║ 每 16B = 8B data + 8B flag                              ║
║                  ║ flag 是单调递增计数器（step+1）                           ║
║                  ║ 接收方自旋轮询 flag 直到匹配期望值                       ║
║                  ║ 写入方用 __threadfence_system 保证全局可见                ║
║                  ║ 适合高延迟网络传输                                       ║
╠══════════════════╬════════════════════════════════════════════════════════════╣
║ ProtoLL128       ║ 128-bit line 打包 + 末尾 flag                            ║
║                  ║ 专用 flag 线程（tid % WARP_SIZE == FLAGTHREAD）           ║
║                  ║ 两阶段加载/存储（Begin/Finish）隐藏 shuffle 延迟         ║
║                  ║ 适合中等延迟（PCIe/NVLink）                              ║
╚══════════════════╩════════════════════════════════════════════════════════════╝
```

### 6.2 ProtoSimple 的 genericOp 流程

这是 kernel 中最核心的数据搬运路径（`device/prims_simple.h:184`）：

```
genericOp(srcIx, dstIx, nelem):
  按 slice 循环处理数据:

  1. waitPeer()
     - 轮询对端的 conn->tail 计数器
     - 如果是 DirectRecv 模式：等对端写入完成
     - 如果是普通模式：等对端把数据写入 FIFO 完成

  2. subBarrier()
     - CTA 内部参与搬运的线程同步

  3. reduceCopyMulti()
     - 从 src（可能是 FIFO 或 Direct buffer）读取数据
     - 如果需要 reduce：与本地数据做 element-wise 归约
     - 写入 dst（可能是 FIFO 或 Direct buffer）
     - 这是真正的数据搬运和计算

  4. barrier()
     - CTA 内全部线程同步

  5. postPeer()
     - 更新自己的 conn->head 计数器
     - 通知对端可以读取数据
```

### 6.3 Direct 模式 vs 缓冲模式

```
普通模式:
  发送方: reduceCopy(userBuffer → FIFO) → postPeer()
  接收方: waitPeer() → reduceCopy(FIFO → userBuffer)
  数据经过 FIFO 中转

Direct 模式（NVLink 同节点）:
  发送方: reduceCopy(userBuffer → 对端 userBuffer) → postPeer()
  接收方: waitPeer() → 数据已在自己的 output buffer 中
  跳过 FIFO，零拷贝直写对端
```

## 7. FIFO 缓冲区与对端同步

### 7.1 数据结构

每个 peer 连接共享一对 `ncclConnInfo`：

```
发送方持有:
  conn->buffs[proto]    中间缓冲区（GPU 显存）
  conn->head            发送方已完成的 step 数
  conn->tail            接收方已完成的 step 数

接收方持有（同一块内存的不同视角）:
  conn->buffs[proto]    同一块缓冲区
  conn->head            发送方已完成的 step 数（对端写入）
  conn->tail            接收方已完成的 step 数（本地写入）
```

### 7.2 同步协议（以 SIMPLE 为例）

```
发送方:
  1. reduceCopy(data → buffs[step % NCCL_STEPS])
  2. __threadfence_system()     // 确保写入对其他 GPU 可见
  3. conn->tail = step + 1      // 通知接收方数据就绪

接收方:
  1. spin_wait(conn->tail == step)  // 等待发送方写完
  2. reduceCopy(buffs[step % NCCL_STEPS] → output)
  3. __threadfence_system()
  4. conn->head = step + 1      // 通知发送方可以复用 FIFO slot
```

FIFO 有 `NCCL_STEPS` 个 slot（通常 4~8 个），环形复用，形成流水线。

## 8. Kernel 与 Proxy 的协作

GPU Kernel 只负责「同节点 GPU 间通过 NVLink 直接搬运」的场景。跨节点或需要 CPU 中转的场景由 Proxy 线程处理：

```
GPU Kernel 写入 FIFO 并更新 tail
  ↓
Proxy 线程检测到 tail 变化
  ↓
┌──────────────────────────────────────────────────┐
│ Proxy 根据传输类型做不同处理:                       │
│                                                   │
│ P2P Direct (NVLink): 不需要 Proxy，对端 GPU 直读   │
│ P2P IPC:               不需要 Proxy，通过 IPC 直读  │
│ SHM:     Proxy 做 cudaMemcpyAsync D2H/H2D        │
│ NET GDR: Proxy 调用 ncclNet isend/irecv (RDMA)   │
│ NET:     Proxy 做 GPU→Host→NIC 拷贝后发送          │
└──────────────────────────────────────────────────┘
```

GPU Kernel 和 Proxy 通过 FIFO 的 head/tail 原子计数器进行无锁协作。

## 9. 启动参数

Kernel 启动时传入 `ncclDevKernelArgs` 结构：

```
ncclDevKernelArgs:
  ├── channelMask     哪些 channel 参与本次执行（bitmask）
  ├── comm            ncclKernelComm* GPU 端通信器地址
  ├── workFifoIdx     work FIFO 索引
  ├── workType        任务类型（Coll/P2p/CollReg/Rma）
  ├── nWorks          work 数量
  ├── funcId          集合函数 ID
  └── ...             其他参数
```

启动配置：`<<<nChannels, nThreads, shmemSize, stream>>>`
- Grid 维度：`nChannels` 个 block（每个 channel 一个）
- Block 维度：`nThreads` 个线程（由算法和协议决定，通常是 128 或 256）
- Shared memory：`shmemSize` 字节（存放 ncclShmemData）
- CUDA Stream：用户指定的 stream

## 10. 源码索引

| 文件 | 内容 |
|------|------|
| `device/common.h:346` | ncclKernelMain 入口函数 |
| `device/common.h:420` | DEFINE_ncclDevKernel 宏定义 |
| `device/common.h:279` | RunWorkBatch 工作批处理 |
| `device/common.h:260` | RunWorkColl 单个集合执行 |
| `device/all_reduce.h:14` | AllReduce Ring 算法实现 |
| `device/reduce_scatter.h` | ReduceScatter 算法实现 |
| `device/all_gather.h` | AllGather 算法实现 |
| `device/sendrecv.h` | Send/Recv P2P 实现 |
| `device/prims_simple.h` | SIMPLE 协议 + genericOp |
| `device/prims_ll.h` | LL 协议（flag-word） |
| `device/prims_ll128.h` | LL128 协议（128-bit line） |
| `device/primitives.h` | Primitives 模板基类 |
| `include/device.h` | ncclDevKernelArgs 等设备端结构定义 |
| `include/nccl_device/comm.h` | ncclKernelComm 设备端通信器 |
