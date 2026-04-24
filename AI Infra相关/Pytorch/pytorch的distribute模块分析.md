# torch.distributed 深度解析

## 一、核心作用

`torch.distributed` 是 PyTorch 的**分布式训练框架**，解决一个关键问题：**多卡/多机协同训练一个大模型**。

```
单卡训练:
  数据: 100 万张图 → 全部过一遍
  时间: ~100 小时
  显存: 模型太大? 直接 OOM

分布式训练:
  数据: 100 万张图 → 8 卡各分 12.5 万张
  时间: ~13 小时 (8 卡并行)
  显存: 模型参数切分到 8 卡 → 单卡显存需求 ÷8

本质: 把一个大任务拆成多个子任务, 分配到多个 GPU 并行执行
```

## 二、核心概念

```
┌─────────────────────────────────────────────────────────┐
│                   分布式训练基本概念                       │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  World Size = 8    (总共 8 张卡参与训练)                 │
│  Rank = 0,1,...,7  (每张卡的编号, 全局唯一)              │
│  Local Rank         (单机内的编号, 比如 4 卡机上是 0-3)  │
│                                                         │
│  Node 0                  Node 1                         │
│  ┌────┐┌────┐           ┌────┐┌────┐                   │
│  │ R0 ││ R1 │  ──IB──   │ R4 ││ R5 │                   │
│  │GPU0││GPU1│           │GPU0││GPU1│                   │
│  └────┘└────┘           └────┘└────┘                   │
│  ┌────┐┌────┐           ┌────┐┌────┐                   │
│  │ R2 ││ R3 │  ──IB──   │ R6 ││ R7 │                   │
│  │GPU2││GPU3│           │GPU2││GPU3│                   │
│  └────┘└────┘           └────┘└────┘                   │
│   local                   local                        │
│  rank 0-3                rank 0-3                       │
│                                                         │
│  Group: 进程子集, 可以对子集做集体通信                     │
│  例如: Rank 0-3 一个 Group, Rank 4-7 另一个 Group        │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 三、三大并行策略

### 3.1 数据并行 (Data Parallel) — 最常用

```
核心思想: 每张卡持有完整模型副本, 各处理不同数据

┌─────────────────────────────────────────────────────────┐
│               Data Parallelism 时序                      │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  Step 开始                                               │
│                                                         │
│  Rank 0        Rank 1        Rank 2        Rank 3       │
│  ┌─────┐      ┌─────┐      ┌─────┐      ┌─────┐       │
│  │ 完整 │      │ 完整 │      │ 完整 │      │ 完整 │       │
│  │ 模型 │      │ 模型 │      │ 模型 │      │ 模型 │       │
│  │ 副本 │      │ 副本 │      │ 副本 │      │ 副本 │       │
│  └──┬──┘      └──┬──┘      └──┬──┘      └──┬──┘       │
│     │            │            │            │            │
│  ① 取 batch_0  取 batch_1  取 batch_2  取 batch_3       │
│  ② forward    forward     forward     forward           │
│  ③ loss_0     loss_1      loss_2      loss_3           │
│  ④ backward   backward    backward    backward          │
│     │            │            │            │            │
│     │  grad_0   grad_1      grad_2      grad_3         │
│     │     │       │           │           │             │
│     │     └───┬───┴─────┬────┘           │             │
│     │         │  ⑤ AllReduce (求梯度均值)  │             │
│     │         └──────┬────┘                │             │
│     │            avg_grad (所有卡得到相同梯度)            │
│     │                │                                │
│  ⑥ optimizer.step()  optimizer.step()  ...             │
│     │                │                                │
│     ▼                ▼                                │
│  模型参数更新 (所有卡保持同步)                           │
│                                                         │
│  关键: 每步结束后, 所有卡的模型参数完全一致                │
│  代价: 每步要通信一次 (AllReduce 梯度)                   │
│        模型越大, 通信量越大: 通信量 = 梯度大小 × 2       │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

### 3.2 模型并行 (Model Parallelism) — 超大模型

```
核心思想: 模型太大单卡放不下, 把模型切分到多张卡

┌─────────────────────────────────────────────────────────┐
│               Model Parallelism 时序                     │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  例: 8 层 Transformer, 切成 2 段                        │
│                                                         │
│  Rank 0:  Layer 0~3 (前半部分)                          │
│  Rank 1:  Layer 4~7 (后半部分)                          │
│                                                         │
│  Forward:                                               │
│                                                         │
│  Rank 0              Rank 1                             │
│  ┌──────────┐        ┌──────────┐                      │
│  │ Layer 0  │        │ Layer 4  │                      │
│  │ Layer 1  │        │ Layer 5  │                      │
│  │ Layer 2  │        │ Layer 6  │                      │
│  │ Layer 3  │──────▶│ Layer 7  │                      │
│  └──────────┘  P2P   └──────────┘                      │
│       ↑              │                                  │
│       │    input     │    hidden_states                 │
│       │              ▼                                  │
│                    output                               │
│                                                         │
│  ① input 进入 Rank 0                                     │
│  ② Rank 0 计算 Layer 0~3, 得到 hidden_states            │
│  ③ P2P 通信: hidden_states → Rank 1 (点对点传输)        │
│  ④ Rank 1 计算 Layer 4~7, 得到 output                   │
│  ⑤ P2P 通信: output → Rank 0 (或直接在 Rank 1 算 loss) │
│                                                         │
│  Backward: 反向走一遍                                    │
│  ⑥ Rank 1 反向传播 Layer 7~4, 得到 grad                 │
│  ⑦ P2P: grad → Rank 0                                  │
│  ⑧ Rank 0 反向传播 Layer 3~0, 得到 grad                 │
│                                                         │
│  问题: 卡间串行, GPU 利用率低                             │
│  → Pipeline Parallelism 改进 (见下文)                    │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

### 3.3 流水线并行 (Pipeline Parallelism) — 提升模型并行效率

```
核心思想: 模型分阶段, 不同 stage 同时处理不同 micro-batch

┌─────────────────────────────────────────────────────────┐
│              Pipeline Parallelism 时序                    │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  模型分 4 个 Stage, 4 张卡各放一个 Stage:                │
│  micro-batch 1~4 (一个 batch 拆成 4 个小 batch)          │
│                                                         │
│  Clock:  1    2    3    4    5    6    7                │
│         ─────────────────────────────────────           │
│  Rank 0: F1   F2   F3   F4   .   B4   B3              │
│  Rank 1: .   F1   F2   F3   F4   B3   B2              │
│  Rank 2: .    .   F1   F2   F3   B4   B1              │
│  Rank 3: .    .    .   F1   F2   B3   .                │
│                                   B2                   │
│           B1   B4                  B1                   │
│                                                         │
│  F = Forward, B = Backward, . = 等待 (气泡)             │
│                                                         │
│  1F1B 调度 (One Forward One Backward):                  │
│                                                         │
│  Clock:  1    2    3    4    5    6    7                │
│  Rank 0: F1   F2   F3   F4        B1   B2              │
│  Rank 1: .   F1   F2   F3   B1   F4   B2              │
│  Rank 2: .    .   F1   F2   B1   F3   B2   B3          │
│  Rank 3: .    .    .   F1   B1   F2   B2   B3   B4     │
│                                                         │
│  优点: 气泡更少, 显存更稳定                               │
│  缺点: 调度复杂, 需要 checkpoint 管理激活值               │
│                                                         │
│  GPipe (朴素):  气泡 ~25% (4 stage)                     │
│  1F1B:         气泡 ~12.5%                              │
│  Interleaved:  气泡更少, 但实现更复杂                      │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 四、进程启动与初始化

```
┌─────────────────────────────────────────────────────────┐
│                进程启动时序                                │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  方式一: torchrun (推荐)                                  │
│                                                         │
│  用户执行:                                               │
│  $ torchrun --nproc_per_node=4 train.py                 │
│                                                         │
│  torchrun 内部:                                         │
│                                                         │
│  torchrun                    train.py × 4               │
│  ┌──────────┐               ┌──────────────────┐       │
│  │ 创建 4 个 │── fork ─────▶│ Rank 0 (GPU 0)   │       │
│  │ Python   │── fork ─────▶│ Rank 1 (GPU 1)   │       │
│  │ 子进程   │── fork ─────▶│ Rank 2 (GPU 2)   │       │
│  │          │── fork ─────▶│ Rank 3 (GPU 3)   │       │
│  │          │               └──────────────────┘       │
│  │ 设置环境变量:              │                        │
│  │  RANK=0,1,2,3            │                        │
│  │  WORLD_SIZE=4             │                        │
│  │  MASTER_ADDR=127.0.0.1   │                        │
│  │  MASTER_PORT=29500        │                        │
│  │  LOCAL_RANK=0,1,2,3       │                        │
│  └──────────┘               └──────────────────┘       │
│                                                         │
│  train.py 内部:                                         │
│                                                         │
│  ① import torch.distributed as dist                    │
│                                                         │
│  ② dist.init_process_group(                             │
│         backend="nccl",        # 通信后端               │
│         init_method="env://",  # 从环境变量读配置        │
│  )                                                      │
│                                                         │
│  init_process_group 内部时序:                            │
│                                                         │
│  Rank 0              Rank 1              Rank 2         │
│    │                   │                   │            │
│    ├── TCP connect ──▶ Master (c10d Store)              │
│    │                   │                   │            │
│    │              TCP connect ──────▶│                   │
│    │                   │        TCP connect ────▶│      │
│    │                   │                   │            │
│    ├── barrier ────────┤──── barrier ────────┤───       │
│    │   (等所有 rank 都连上)                 │            │
│    │                   │                   │            │
│    ▼                   ▼                   ▼            │
│  NCCL 通信环建立完成, 可以开始训练                       │
│                                                         │
│  ③ torch.cuda.set_device(local_rank)                   │
│  ④ model = model.to(local_rank)                        │
│  ⑤ 开始训练循环                                         │
│                                                         │
│  方式二: multiprocessing.spawn                           │
│  方式三: 手动 mpirun / slurm srun                       │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 五、通信原语详解

```
┌─────────────────────────────────────────────────────────┐
│                  通信原语一览                              │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  1. Broadcast (广播)                                    │
│                                                         │
│     Rank 0:  [A B C]                                    │
│     Rank 1:  [? ? ?]     ◀─── Broadcast from Rank 0     │
│     Rank 2:  [? ? ?]                                    │
│     Rank 3:  [? ? ?]                                    │
│     结果:   所有卡都得到 [A B C]                         │
│     用途: 同步初始化参数、同步随机种子                     │
│                                                         │
│  2. AllReduce (归约 + 广播)                              │
│                                                         │
│     Rank 0:  [1]  ─┐                                    │
│     Rank 1:  [2]  ─┼─ Sum ─▶ 所有卡得到 [10]            │
│     Rank 2:  [3]  ─┤       (也可以是 Max, Min, Prod)    │
│     Rank 3:  [4]  ─┘                                    │
│     用途: 梯度同步 (DP 的核心操作)                       │
│                                                         │
│  3. Reduce (归约, 结果只到一张卡)                        │
│                                                         │
│     Rank 0:  [1]  ─┐                                    │
│     Rank 1:  [2]  ─┼─ Sum ─▶ Rank 0 得到 [10]          │
│     Rank 2:  [3]  ─┤       其他卡不更新                 │
│     Rank 3:  [4]  ─┘                                    │
│     用途: 汇总损失、汇总指标                             │
│                                                         │
│  4. AllGather (收集所有卡的数据, 所有卡都拿到)            │
│                                                         │
│     Rank 0:  [a]  ─┐                                    │
│     Rank 1:  [b]  ─┼─▶ 所有卡得到 [a b c d]            │
│     Rank 2:  [c]  ─┤                                    │
│     Rank 3:  [d]  ─┘                                    │
│     用途: ZeRO-3 收集参数、Gather 输出结果               │
│                                                         │
│  5. ReduceScatter (分散归约, 每张卡得到一块归约结果)      │
│                                                         │
│     Rank 0:  [a0 a1 a2 a3]  ─┐                         │
│     Rank 1:  [b0 b1 b2 b3]  ─┼─▶ Rank 0: [a0+b0+c0+d0]│
│     Rank 2:  [c0 c1 c2 c3]  ─┤   Rank 1: [a1+b1+c1+d1]│
│     Rank 3:  [d0 d1 d2 d3]  ─┘   Rank 2: [a2+b2+c2+d2]│
│                                   Rank 3: [a3+b3+c3+d3]│
│     用途: ZeRO-2 梯度分片归约                            │
│                                                         │
│  6. Send/Recv (点对点)                                  │
│                                                         │
│     Rank 0 ─── Send(tensor) ───▶ Rank 1                │
│     用途: Pipeline Parallelism 的 stage 间传输           │
│                                                         │
│  7. Barrier (同步栅栏)                                  │
│                                                         │
│     所有卡都到达 barrier 后才继续                         │
│     用途: 确保所有卡完成某个阶段后再进入下一阶段          │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

### AllReduce 底层实现：Ring Algorithm

```
┌─────────────────────────────────────────────────────────┐
│               Ring AllReduce 时序                         │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  数据: 每卡有 buffer [A B C D] (4 块)                   │
│  目标: 所有卡得到 [A+B+C+D] 的每块 sum                  │
│                                                         │
│  Phase 1: Reduce-Scatter (N-1 步, 每步传一块)            │
│                                                         │
│  Step 1: Rank 0 发 B → Rank 1, Rank 3 发 D → Rank 0    │
│  Step 2: Rank 1 发 C → Rank 2, Rank 0 发 B+D → Rank 3  │
│  Step 3: Rank 2 发 D+C → Rank 3, Rank 1 发 C+B → Rank 0│
│                                                         │
│  Phase 2: AllGather (N-1 步, 每步传一块)                │
│                                                         │
│  Step 4: Rank 3 发完整结果 → Rank 0                     │
│  Step 5: Rank 0 发完整结果 → Rank 1                     │
│  Step 6: Rank 1 发完整结果 → Rank 2                     │
│                                                         │
│  通信量: 2 × (N-1) × (数据大小 / N)                    │
│         = 2 × 3 × (buffer/4) = 1.5 × buffer            │
│         对比朴素 Broadcast: N × buffer                   │
│         → Ring 算法通信量是朴素的 2(N-1)/N ≈ 2x 更优    │
│                                                         │
│  NVLink 环境下 (8 卡全互联):                              │
│    实际用 Tree Algorithm 而非 Ring (利用全互联拓扑)      │
│    带宽更高, 延迟更低                                    │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 六、通信后端对比

```
┌─────────────────────────────────────────────────────────┐
│                  通信后端对比                              │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  后端        适用场景          通信介质      特点          │
│  ─────────────────────────────────────────────────────   │
│  NCCL       GPU-GPU          NVLink/IB    最快,推荐     │
│             通信                                          │
│                                                         │
│  Gloo       CPU-CPU           Ethernet    CPU 通信备选   │
│             CPU-GPU 混合                                   │
│                                                         │
│  MPI       跨框架整合          IB/ETH     需要安装       │
│            (TF+PyTorch)                    OpenMPI       │
│                                                         │
│  TPU       Google TPU        ICI        仅 GCP          │
│                                                         │
│  NCCL 通信路径 (同机):                                   │
│                                                         │
│  GPU 0 ──NVLink── GPU 1 ──NVLink── GPU 2               │
│    │              │              │                       │
│    └──NVLink──────┘──NVLink──────┘                       │
│                                                         │
│  带宽: NVLink 4.0 = 900 GB/s (双向) per link             │
│  同机 8 卡全互联: 聚合带宽 ~3.6 TB/s                     │
│  延迟: ~1-2 μs                                          │
│                                                         │
│  NCCL 通信路径 (跨机):                                   │
│                                                         │
│  GPU 0 ─ NVLink ─ NVSwitch ─ PCIe ─ NIC ──IB── NIC     │
│  ─ PCIe ─ NVSwitch ─ NVLink ─ GPU 1                   │
│                                                         │
│  带宽: HDR IB = 200 Gbps (25 GB/s) per link             │
│  延迟: ~5-10 μs (跨机)                                  │
│                                                         │
│  → 跨机通信是同机的 1/10~1/30 带宽, 这是分布式瓶颈       │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 七、DistributedSampler — 数据分片

```
┌─────────────────────────────────────────────────────────┐
│              DistributedSampler 数据分片                   │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  全量数据: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]     │
│  World Size: 4                                          │
│                                                         │
│  Epoch 0 (shuffle=True):                                │
│                                                         │
│  全局打乱:  [7, 2, 9, 0, 5, 11, 3, 8, 1, 10, 6, 4]   │
│                                                         │
│  Rank 0: [7, 2, 9, 0]      ← indices[0:4]              │
│  Rank 1: [5, 11, 3, 8]     ← indices[4:8]              │
│  Rank 2: [1, 10, 6, 4]    ← indices[8:12]             │
│                                                         │
│  Epoch 1 (不同 shuffle):                                │
│                                                         │
│  全局打乱:  [4, 10, 1, 6, 8, 3, 11, 5, 0, 9, 2, 7]    │
│                                                         │
│  Rank 0: [4, 10, 1, 6]                                  │
│  Rank 1: [8, 3, 11, 5]                                  │
│  Rank 2: [0, 9, 2, 7]                                   │
│                                                         │
│  保证:                                                   │
│    1. 每个 Epoch 内, 各 Rank 数据不重叠                  │
│    2. 跨 Epoch, 每个 Rank 看到的数据不同 (shuffle)        │
│    3. 多个 Epoch 覆盖全部数据                             │
│                                                         │
│  注意: set_epoch(epoch) 必须每个 Epoch 调用一次!         │
│    sampler.set_epoch(epoch)                              │
│    → 否则每个 Epoch 的 shuffle 种子相同, 数据划分一样    │
│                                                         │
│  DataLoader 配合:                                       │
│    sampler = DistributedSampler(dataset)                │
│    loader = DataLoader(dataset, sampler=sampler)        │
│    # 不要设 shuffle=True, sampler 自己管打乱             │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 八、DDP vs DP vs FSDP

```
┌─────────────────────────────────────────────────────────┐
│            PyTorch 分布式 API 对比                         │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  1. DP (DataParallel) — 旧 API, 不推荐                  │
│                                                         │
│     用户代码                                             │
│        │                                                 │
│        ▼                                                 │
│     DP Wrapper (单进程, 多线程)                           │
│        │                                                 │
│        ├── GPU 0: forward (主卡)                         │
│        ├── GPU 1: forward (scatter 输入)                 │
│        ├── GPU 2: forward                               │
│        │        │                                        │
│        │  AllReduce (到 GPU 0)                           │
│        │        │                                        │
│        ▼        ▼                                        │
│     GPU 0: backward + update (GIL 锁, 串行!)            │
│                                                         │
│     缺点:                                               │
│       单进程多线程 → Python GIL 瓶颈                    │
│       主卡 (GPU 0) 负载不均 (forward + backward + update)│
│       显存: 主卡额外存所有卡梯度副本                      │
│       效率: 4 卡 DP ~ 2.5x 加速 (不是线性)              │
│                                                         │
│  2. DDP (DistributedDataParallel) — 推荐               │
│                                                         │
│     每张卡一个独立进程                                    │
│                                                         │
│     Rank 0          Rank 1          Rank 2              │
│     ┌──────┐       ┌──────┐       ┌──────┐             │
│     │进程 0│       │进程 1│       │进程 2│             │
│     │GPU 0 │       │GPU 1 │       │GPU 2 │             │
│     │模型   │       │模型   │       │模型   │             │
│     └──┬───┘       └──┬───┘       └──┬───┘             │
│        │              │              │                   │
│     forward        forward        forward                │
│     backward       backward       backward              │
│        │              │              │                   │
│        └── AllReduce ──┘──────────────┘                 │
│               (异步, 梯度计算完就开始传)                   │
│        │              │              │                   │
│     optimizer     optimizer     optimizer               │
│     .step()       .step()       .step()                 │
│                                                         │
│     优点:                                               │
│       多进程, 无 GIL 限制                                │
│       各卡负载均衡                                       │
│       AllReduce 与 backward 重叠 (通信计算重叠)          │
│       效率: 4 卡 DDP ~ 3.8x 加速 (接近线性)             │
│                                                         │
│  3. FSDP (Fully Sharded Data Parallel) — 大模型         │
│                                                         │
│     模型参数 + 梯度 + 优化器状态 全部分片                 │
│                                                         │
│     Rank 0        Rank 1        Rank 2                  │
│     ┌────────┐   ┌────────┐   ┌────────┐               │
│     │Param   │   │Param   │   │Param   │               │
│     │分片 0  │   │分片 1  │   │分片 2  │               │
│     └───┬────┘   └───┬────┘   └───┬────┘               │
│         │            │            │                      │
│     ① AllGather 收集完整参数 (仅 forward 需要)           │
│         │            │            │                      │
│     ② forward (用完整参数计算)                           │
│         │            │            │                      │
│     ③ 丢弃非本卡参数, 只保留本卡参数的梯度                │
│         │            │            │                      │
│     ④ ReduceScatter 梯度分片                             │
│         │            │            │                      │
│     ⑤ optimizer.step() (用本地梯度更新本地参数)           │
│                                                         │
│     优点:                                               │
│       单卡显存: O(model/N + optimizer/N + activations)  │
│       70B 模型 + FSDP: 8×80GB A100 可训练              │
│       (DDP 需要: 70B×2字节 = 140GB → 单卡放不下)         │
│                                                         │
│     缺点:                                               │
│       通信量更大 (每层 forward/backward 都要 AllGather)  │
│       适合节点内 NVLink 高带宽, 跨机效率下降明显         │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 九、端到端 DDP 训练完整时序

```
┌──────────────────────────────────────────────────────────────────┐
│              DDP 单步训练完整时序                                   │
├──────────────────────────────────────────────────────────────────┤
│                                                                  │
│  每个 Rank 独立进程, 以下以 Rank 0 为例:                          │
│                                                                  │
│  Rank 0 (进程)                                                    │
│  │                                                               │
│  │  ① sampler.set_epoch(epoch)                                   │
│  │  ② for batch in loader:    # DistributedSampler 自动分片       │
│  │      │                                                      │
│  │  ③   input, label = batch                                     │
│  │      input = input.cuda()       # 数据到 GPU                   │
│  │      label = label.cuda()                                     │
│  │      │                                                      │
│  │  ④   optimizer.zero_grad()    # 清零梯度                       │
│  │      │                                                      │
│  │  ⑤   output = model(input)    # 前向计算 (GPU)                 │
│  │      │                                                      │
│  │  ⑥   loss = criterion(output, label)                         │
│  │      │                                                      │
│  │  ⑦   loss.backward()           # 反向传播                      │
│  │      │   ┌─────────────────────────────────────┐             │
│  │      │   │ 反向过程中 DDP 自动做:                │             │
│  │      │   │                                      │             │
│  │      │   │ backward 计算梯度                     │             │
│  │      │   │     │                                │             │
│  │      │   │     ▼ (梯度 ready 的桶立即启动)      │             │
│  │      │   │ AllReduce (bucket 1: 梯度 0~99)     │             │
│  │      │   │ AllReduce (bucket 2: 梯度 100~199)  │             │
│  │      │   │ ...                                 │             │
│  │      │   │                                      │             │
│  │      │   │ 通信与反向计算重叠!                   │             │
│  │      │   │ → 不等所有梯度算完就开始传            │             │
│  │      │   └─────────────────────────────────────┘             │
│  │      │                                                      │
│  │  ⑧   optimizer.step()        # 用平均梯度更新参数              │
│  │      │                                                      │
│  │  ⑨   (可选) logging, checkpoint, etc.                        │
│  │      │                                                      │
│  │  ⑩  dist.barrier()           # (可选) 等所有 Rank 同步        │
│  │      │                                                      │
│  ▼                                                               │
│                                                                  │
│  DDP 关键优化 (Gradient Bucketing):                              │
│                                                                  │
│    梯度不等到全部算完再通信                                       │
│    而是分桶 (bucket), 每个桶填满就立即 AllReduce                  │
│                                                                  │
│    Bucket 大小默认: 25MB                                         │
│    backward 中:                                                  │
│      layer 5 grad ready → bucket 0 满 → AllReduce(bucket 0)     │
│      layer 4 grad ready → bucket 0 满 → AllReduce(bucket 0)     │
│      ...                                                         │
│      layer 1 grad ready → bucket N 满 → AllReduce(bucket N)     │
│                                                                  │
│    效果: 通信与计算并行, 减少 GPU 空等时间                        │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
```

## 十、踩坑与最佳实践

```
1. set_epoch 忘记调
   ❌ 每个 Epoch 数据划分完全一样 → 模型只学子集
   ✅ sampler.set_epoch(epoch)

2. Batch Size 问题
   总有效 batch_size = per_gpu_batch_size × world_size
   ❌ world_size 变了但没调学习率 → 训练不稳定
   ✅ lr = base_lr × world_size (线性缩放规则)

3. 保存/加载 Checkpoint
   ❌ 所有 Rank 都保存 → 文件冲突 + IO 争抢
   ✅ if rank == 0: torch.save(...)
   ✅ 或每个 Rank 保存自己的分片 (FSDP/sharding)

4. NCCL 超时
   ❌ 一个 Rank 挂了, 其他 Rank 永远等 → 死锁
   ✅ dist.init_process_group(timeout=timedelta(minutes=30))
   ✅ 设置 NCCL_DEBUG=INFO 环境变量排查

5. 梯度累积 vs 大 Batch
   梯度累积: 多个 micro-batch 算完梯度再更新
   loss = loss / accumulation_steps
   if step % accumulation_steps == 0:
       optimizer.step()
   → 等效大 batch, 但显存不需要放大

6. 混合精度 + DDP
   ✅ DDP 和 AMP 完全兼容
   ✅ GradScaler 在 DDP 外层即可
   ✅ 每个进程独立维护自己的 scaler

7. 性能排查命令
   NCCL_DEBUG=INFO torchrun ...          # 查看通信详情
   NCCL_DEBUG_SUBSYS=ALL                 # 更详细
   NCCL_P2P_DISABLE=1                    # 禁用 P2P, 用 fallback
   NCCL_IB_DISABLE=1                     # 禁用 IB, 用以太网

8. DDP 模型初始化顺序
   ✅ 正确: model → DDP → cuda
     model = MyModel()
     model = DDP(model, device_ids=[local_rank])
   ✅ 或: model.to(device) → DDP
   ❌ 错误: 先 DDP 再 .cuda() (可能导致 device 不匹配)
```

## 十一、3D 并行 (大规模训练)

```
训练 175B+ 参数模型, 单一并行策略不够:

┌─────────────────────────────────────────────────────────┐
│               3D 并行 (TP + PP + DP)                     │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  例: GPT-3 175B, 1024 GPU                               │
│  TP=8, PP=16, DP=8                                      │
│                                                         │
│  Node 0-1 (TP=2 per node, 2 nodes)                      │
│  ┌────────────────────────┐                             │
│  │ PP Stage 0: TP Split  │                             │
│  │  GPU 0: Layer 0 left  │                             │
│  │  GPU 1: Layer 0 right │← TP: 同层切分到 2 卡        │
│  └──────────┬─────────────┘                             │
│             │ P2P                                       │
│  ┌──────────▼─────────────┐                             │
│  │ PP Stage 1: TP Split  │                             │
│  │  GPU 2: Layer 1 left  │                             │
│  │  GPU 3: Layer 1 right │                             │
│  └────────────────────────┘                             │
│  ... (更多 PP stage)                                     │
│                                                         │
│  DP 组 0: Stage 0-15, 各 1 份 → 处理不同数据             │
│  DP 组 1: Stage 0-15, 各 1 份 → 处理不同数据             │
│  ... (共 8 个 DP 组)                                    │
│                                                         │
│  每步通信:                                               │
│    TP: 每层 forward/backward 通信 (AllReduce)            │
│    PP: stage 间 P2P 通信 (Send/Recv)                     │
│    DP: 梯度 AllReduce (跨 DP 组)                         │
│                                                         │
│  框架选择:                                               │
│    Megatron-LM: TP + PP + DP (NVIDIA, 大模型标准)       │
│    DeepSpeed:  ZeRO (DP 变体) + 支持接入 PP              │
│    FSDP: 纯 PyTorch 原生, ZeRO 思路                      │
│                                                         │
└─────────────────────────────────────────────────────────┘
```
