# torch.utils.data 分析

## 一、核心作用

`torch.utils.data` 是 PyTorch 数据加载的核心模块，解决一个关键问题：**让 GPU 不等数据**。

```
没有 DataLoader:
  GPU: [计算] ────── 等数据 ────── [计算] ────── 等数据 ──────
  CPU: ── 读磁盘 ── 解码 ── Transform ── 组batch ── 读磁盘 ──
                                                    ↑ GPU 空等!

有 DataLoader:
  GPU: [计算][计算][计算][计算][计算][计算][计算][计算]
  CPU: ─ 预取 ─ 预取 ─ 预取 ─ 预取 ─ 预取 ─ 预取 ─ 预取 ─
         Worker0  Worker1  Worker2  Worker3
  → 多进程并行 + 预取队列, GPU 几乎无等待
```

## 二、核心组件

```
torch.utils.data
│
├── Dataset              # 数据源抽象
│   ├── MapDataset       # 按 index 访问: __getitem__(idx), __len__()
│   └── IterableDataset  # 按迭代访问: __iter__()
│
├── Sampler              # 决定数据访问顺序
│   ├── SequentialSampler   # 顺序 0,1,2,...
│   ├── RandomSampler       # 随机打乱
│   ├── WeightedRandomSampler  # 加权采样
│   └── DistributedSampler # 分布式训练, 每卡分不同数据
│
├── DataLoader            # 总调度器
│   ├── 多进程 Worker 管理
│   ├── 自动 Batching (collate_fn)
│   ├── 数据预取 (prefetch_factor)
│   ├── Pin Memory (加速 CPU→GPU 传输)
│   └── 异常 Worker 自动重启
│
├── DataLoader2           # 新一代 DataLoader (实验性)
│
└── 可组合的 Transform 工具链
    ├── functional    # 纯函数式 transform
    ├── v2 transforms # 新版 transform 框架
    └── autoaugment   # 数据增强策略
```

## 三、DataLoader 配置参数速查

```python
DataLoader(
    dataset,              # 数据源
    batch_size=32,        # 每批样本数
    shuffle=False,        # 是否打乱 (内部创建 RandomSampler)
    sampler=None,         # 自定义采样器 (与 shuffle 互斥)
    num_workers=4,        # 子进程数 (0 = 主进程加载)
    collate_fn=None,      # 自定义 batch 组装函数
    pin_memory=False,     # 锁页内存, 加速 CPU→GPU DMA
    drop_last=False,      # 丢弃最后不足 batch_size 的批次
    prefetch_factor=2,    # 每个 worker 预取的 batch 数
    persistent_workers=False,  # 是否在 epoch 间保留 worker 进程
    multiprocess_context='fork',  # 多进程启动方式
)
```

## 四、完整时序流程图

### 4.1 初始化阶段

```
时间 ──────────────────────────────────────────────────────────────────────▶

主进程:
  │
  ├── 1. 创建 DataLoader(dataset, num_workers=4, pin_memory=True, prefetch_factor=2)
  │      │
  │      ├── 创建 Sampler (决定 index 顺序)
  │      ├── 创建 _DataLoaderIter (迭代器)
  │      └── 设置 prefetch 队列: max = num_workers × prefetch_factor = 8 个 batch
  │
  ├── 2. 第一次调用 iter(dataloader).__next__()
  │      │
  │      ├── fork 4 个 Worker 进程
  │      │     Worker0 ──── fork ────▶ 独立进程, 复制 Dataset 引用
  │      │     Worker1 ──── fork ────▶ 独立进程, 复制 Dataset 引用
  │      │     Worker2 ──── fork ────▶ 独立进程, 复制 Dataset 引用
  │      │     Worker3 ──── fork ────▶ 独立进程, 复制 Dataset 引用
  │      │
  │      ├── 启动 pin_memory_thread (如果 pin_memory=True)
  │      │
  │      └── 每个 Worker 立即开始预取
  │
  ▼
```

### 4.2 稳态运行阶段 (每个 Worker 的内部流程)

```
时间 ──────────────────────────────────────────────────────────────────────▶

Worker0:    [取index][读数据][Transform][Collate][放队列][取index][读数据]...
                  ▲                                          │
                  │ 共享 Index Queue (主进程 Sampler 产出)     │ 共享 Result Queue
                  │                                          ▼
Worker1:    [取index][读数据][Transform][Collate][放队列][取index][读数据]...
                                     │
                                     ▼
Worker2:    [取index][读数据][Transform][Collate][放队列]...
                                                        │
                                                        ▼
Worker3:    [取index][读数据][Transform][Collate][放队列]...
                                                           │
                                                           ▼
pin_memory_thread:  [从 Result Queue 取 batch]─[pin_memory]─[放入 Pinned Queue]
                                                               │
主进程:     ◀──────────────────── [从 Pinned Queue 取 batch] ─┘
              │
              ▼
          batch.to(device='cuda')   ← cudaMemcpy 异步, 几乎零开销
              │
              ▼
          model(batch) → GPU 计算
```

### 4.3 单个 Worker 的详细时序

```
Worker 进程内部, 处理 1 个 batch 的详细流程:

Clock:  0        1         2          3          4         5
        │        │         │          │          │         │
Step1:  ├─ 从 Index Queue 取出一组 index ──────┤
        │  (例如: [idx_32, idx_57, idx_91, ...])  │
        │        │                                    │
Step2:  │        ├─ dataset[idx] 逐个读取 ───────────┤
        │        │  ┌──────────────────────────┐      │
        │        │  │ idx_32 → 读文件 → 解码    │      │
        │        │  │ idx_57 → 读文件 → 解码    │      │
        │        │  │ idx_91 → 读文件 → 解码    │      │
        │        │  └──────────────────────────┘      │
        │        │  ↑ 这步最慢! 磁盘 I/O 是瓶颈       │
        │        │                                    │
Step3:  │        │                                    ├─ Transform ─┤
        │        │                                    │ (裁剪/翻转/  │
        │        │                                    │  归一化等)   │
        │        │                                    │              │
Step4:  │        │                                    │              ├─ Collate ─┤
        │        │                                    │              │ (组装成     │
        │        │                                    │              │  Tensor)   │
        │        │                                    │              │            │
Step5:  │        │                                    │              │            ├─ 放入 Queue ─┤
        │        │                                    │              │            │              │
        ▼        ▼                                    ▼              ▼            ▼              ▼

实际耗时分布 (以 ImageNet 为例):
  Step2 (读文件+解码): ~60-80%   ← 瓶颈, 取决于磁盘速度
  Step3 (Transform):   ~10-20%
  Step4 (Collate):     ~5%
  Step5 (IPC 传输):    ~5-10%   ← 共享内存, 开销不大
```

## 五、并发模型详解

### 5.1 进程间通信机制

```
┌─────────────────── 主进程 ───────────────────┐
│                                              │
│  Sampler ──(index_queue)──▶ 所有 Worker 共享   │
│                                              │
│  pin_memory_thread                            │
│    ├── 从 worker_result_queue 取 batch        │
│    ├── 调用 cuda.pin_memory(batch)            │
│    └── 放入 pinned_memory_queue               │
│                                              │
│  __next__() ◀── 从 pinned_memory_queue 取     │
│                                              │
├────────────────── 进程间共享内存 ─────────────┤
│                                              │
│  index_queue       → 每个 batch 的 index 列表  │
│  worker_result_queue → 每个 Worker 的输出 batch│
│  pinned_memory_queue → pin 后的 batch         │
│  (都用 multiprocessing.Queue, 基于 Pipe+Semaphore) │
│                                              │
└──────────────────────────────────────────────┘
     │            │            │            │
     ▼            ▼            ▼            ▼
┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐
│ Worker0  │ │ Worker1  │ │ Worker2  │ │ Worker3  │
│          │ │          │ │          │ │          │
│ 取 index │ │ 取 index │ │ 取 index │ │ 取 index │
│ 读数据   │ │ 读数据   │ │ 读数据   │ │ 读数据   │
│ Transform│ │ Transform│ │ Transform│ │ Transform│
│ 放 Queue │ │ 放 Queue │ │ 放 Queue │ │ 放 Queue │
└──────────┘ └──────────┘ └──────────┘ └──────────┘
```

### 5.2 预取队列的水位管理

```
prefetch_factor=2, num_workers=4
→ 最多预取 2×4=8 个 batch 在队列中

队列水位变化示意 (训练开始阶段):

时间 ▶
0   1   2   3   4   5   6   7   8   9   10

主消费:          取    取    取    取    取    取
                 │     │     │     │     │     │
pin_mem:    pin  pin   pin   pin   pin   pin   pin
            │    │     │     │     │     │     │
Worker产出:  B0   B1    B2    B3    B4    B5    B6    B7
            W0   W1    W2    W3    W0    W1    W2    W3

队列水位:   1    2     3     4     4     4     4     4
                         ▲
                    达到水位, 自动限流
                    Worker 暂停取 index

稳态后:
  - 队列始终保持 ~4 个 batch
  - Worker 不堆积, 不空转
  - 主进程始终有数据可取
  - → GPU 利用率最大化
```

## 六、性能优化关键参数

### 6.1 num_workers 选择

```
经验法则: num_workers = min(8, CPU核心数, GPU批处理速度/单Worker产出速度)

num_workers=0:  主进程串行加载, GPU 大量等数据
num_workers=4:  4进程并行, 覆盖大部分场景
num_workers=8:  8进程并行, 适合 I/O 密集型

注意:
  - 过多 Worker → CPU 争抢, 内存压力, 反而变慢
  - 过少 Worker → GPU 饿死, 利用率低
  - 最佳值需要实测: 逐渐增加直到 GPU 利用率不再提升
```

### 6.2 pin_memory 的作用

```
普通内存 (Pageable Memory):
  CPU 数据 ──▶ CPU 内存 ──▶ [OS 可能换页到磁盘] ──▶ 拷贝到 staging buffer ──▶ DMA 到 GPU
                                                    ↑ 需要 1 次额外拷贝!

锁页内存 (Pinned Memory):
  CPU 数据 ──▶ 锁页内存 ──▶ DMA 直接到 GPU
                             ↑ 零额外拷贝, 带宽更高

实测差异:
  Pageable: ~12 GB/s (PCIe 4.0 x16)
  Pinned:   ~25 GB/s (PCIe 4.0 x16, 接近理论峰值)
  → pin_memory=True 在大 batch 时提速约 1.5-2x
```

### 6.3 persistent_workers

```
默认 (persistent_workers=False):
  Epoch 1: 启动 Worker ──▶ 训练 ──▶ 销毁 Worker
  Epoch 2: 启动 Worker ──▶ 训练 ──▶ 销毁 Worker
  Epoch 3: 启动 Worker ──▶ 训练 ──▶ 销毁 Worker
          ↑ 每次启动约 0.5-2s 浪费

persistent_workers=True:
  Epoch 1: 启动 Worker ──▶ 训练 ──▶ 保留
  Epoch 2:          ──▶ 训练 ──▶ 保留
  Epoch 3:          ──▶ 训练 ──▶ 保留
  → 多 epoch 训练节省启动开销
```

## 七、常见性能问题排查

```
问题: GPU 利用率低 (< 60%)

排查清单:
├── 1. num_workers 是否足够?
│     检查: nvidia-smi 看 GPU 利用率, 若时高时低 → 数据跟不上
│     解决: 增加 num_workers (从 4 开始尝试)
│
├── 2. 磁盘 I/O 是否瓶颈?
│     检查: iostat -x 1 看 %util, 若接近 100%
│     解决: 使用 SSD, 或增加缓存, 或减少 I/O (预解码存为 .npy/.lmdb)
│
├── 3. Transform 是否太重?
│     检查: Worker CPU 占用 100% 但队列不满
│     解决: 简化 Transform, 或用 GPU 预处理 (nvjpeg/kornia)
│
├── 4. batch_size 是否太小?
│     检查: GPU 显存大量空闲
│     解决: 增大 batch_size, 减少 iteration 频率
│
├── 5. 是否开了 pin_memory?
│     检查: batch.to(device) 耗时长
│     解决: pin_memory=True
│
└── 6. Collate 是否有瓶颈?
      检查: 自定义 collate_fn 耗时异常
      解决: 优化 collate, 或用 default_collate
```

## 八、总结

`torch.utils.data` 的本质是一个 **多进程生产者-消费者模型**:

```
  生产者 (Worker 进程)          缓冲 (预取队列)         消费者 (主进程)
  ┌─────────────┐          ┌──────────────┐        ┌────────────┐
  │ 读数据       │─────────▶│              │        │ 取 batch   │
  │ Transform    │─────────▶│  Prefetch    │───────▶│ to(cuda)   │──▶ GPU
  │ Collate      │─────────▶│  Queue       │        │            │
  └─────────────┘          │  (8 batches) │        └────────────┘
  ┌─────────────┐          │              │
  │ 读数据       │─────────▶│              │
  │ Transform    │─────────▶│              │
  │ Collate      │─────────▶│              │
  └─────────────┘          └──────────────┘
  ┌─────────────┐
  │ ...         │
  └─────────────┘

核心设计目标: 让队列始终有数据, 让 GPU 永远不空转
```
