# torch.cuda 深度解析

## 一、核心作用

`torch.cuda` 是 PyTorch 的 GPU 计算桥接模块，解决一个关键问题：**让 PyTorch 能在 NVIDIA GPU 上跑计算**。

```
没有 torch.cuda:
  所有张量都在 CPU 内存
  所有计算都在 CPU 上
  → 训练一个 ResNet-50 要几天

有 torch.cuda:
  tensor = tensor.cuda()          # 把数据搬到 GPU 显存
  model = model.cuda()            # 把模型参数搬到 GPU
  loss = criterion(output, label) # 在 GPU 上计算
  → 训练一个 ResNet-50 只需几十分钟 (单卡)
```

**本质：屏蔽 GPU 硬件差异，提供统一的 GPU 内存管理 + 计算调度接口**

## 二、核心架构

```
用户代码
  │
  ▼
torch.cuda                    ← 用户接触的 API 层
  │
  ├── cuda.amp          # 自动混合精度 (FP16/FP32 自动切换)
  ├── cuda.comm         # 多卡通信原语 (broadcast, reduce, all_reduce)
  ├── cuda.memory       # 显存分配与管理
  ├── cuda.streams      # CUDA Stream 调度
  ├── cuda.nvtx         # 性能分析标记
  ├── cuda.random       # GPU 随机数生成
  ├── cuda.sparse       # 稀疏张量 GPU 操作
  └── cuda.profiler     # GPU 性能剖析
  │
  ▼
CuDNN / cuBLAS / cuSOLVER / NCCL   ← NVIDIA 底层库
  │
  ▼
CUDA Driver                              ← NVIDIA 驱动
  │
  ▼
GPU Hardware
```

## 三、核心工作流程

### 3.1 设备管理

```
torch.cuda.is_available()       # 检测是否有可用 GPU
torch.cuda.device_count()       # GPU 数量
torch.cuda.get_device_name(0)   # GPU 型号: "NVIDIA H100 SXM5"
torch.cuda.get_device_capability(0)  # 算力: (9, 0) for H100
torch.cuda.current_device()     # 当前默认 GPU 编号
torch.cuda.set_device(1)        # 切换默认 GPU
```

### 3.2 数据搬运：CPU ↔ GPU

```
这是 torch.cuda 最核心的工作

┌─────────────────────────────────────────────────────────┐
│                     数据搬运时序                          │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  CPU 内存 (DDR5)              GPU 显存 (HBM3)           │
│  ┌──────────┐                 ┌──────────┐              │
│  │ tensor   │─── .cuda() ────▶│ tensor   │              │
│  │ (numpy)  │   (同步阻塞)     │ (GPU)    │              │
│  └──────────┘                 └──────────┘              │
│       │                           │                     │
│       │◀── .cpu() ───────────────┤                     │
│       │   (同步阻塞)              │                     │
│                                                         │
│  底层: cudaMemcpy (默认) / cudaMemcpyAsync (异步)        │
│  路径: CPU → PCIe Gen5 → GPU                             │
│  带宽: PCIe Gen5 x16 ≈ 63 GB/s (双向)                   │
│  耗时: 1GB 数据 ≈ 16ms (同步拷贝)                        │
│                                                         │
└─────────────────────────────────────────────────────────┘

多种写法 (等价):
  t = t.cuda()
  t = t.to("cuda")
  t = t.to("cuda:0")
  t = t.to(device)        # device = torch.device("cuda:0")
```

### 3.3 显存分配与管理

```
torch.cuda 的显存分配器 (Caching Allocator):

┌─────────────────────────────────────────────────────────┐
│                   显存分配时序                            │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  第一次申请 512MB:                                        │
│  ┌──────────────────────────────────────┐               │
│  │ 512MB (从 CUDA malloc 分配, 真实分配) │               │
│  └──────────────────────────────────────┘               │
│                                                         │
│  释放后再次申请 256MB:                                    │
│  ┌──────────────────────────────────────┐               │
│  │ 256MB (复用之前 512MB 的块, 无需分配)  │ 空闲 256MB   │
│  ├──────────────────────────────────────┤ (不归还给     │
│  │          空闲 256MB                   │  CUDA)       │
│  └──────────────────────────────────────┘               │
│                                                         │
│  → 关键: 释放显存不归还给 CUDA runtime                   │
│    而是缓存起来给后续复用                                 │
│    → 避免频繁调用 cudaMalloc/cudaFree 的开销             │
│                                                         │
└─────────────────────────────────────────────────────────┘

为什么需要缓存分配器:
  CUDA malloc/free:  每次都要锁 + 碎片整理 → 慢 (~100μs/次)
  Caching Allocator: 池化复用, 平均 ~1μs/次

显存碎片管理:
  按 block size 分桶: 256B, 512B, 1KB, 2KB, ..., 2MB (32 级)
  每个 bucket 内用空闲链表管理
  大块分配直接走 CUDA malloc
```

### 3.4 CUDA Stream — 并行调度

```
CUDA Stream 是 GPU 任务的调度队列

┌─────────────────────────────────────────────────────────┐
│                  Stream 并行执行时序                      │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  默认 Stream (stream 0):                                 │
│  ┌── matmul ──┐                                         │
│  │            ├── relu ──┐                              │
│  │            │         ├── add ──→                     │
│  │            │         │                               │
│  └────────────┘         │                               │
│                          │  串行! 每个操作等上一个完成     │
│                                                         │
│  多 Stream 并行:                                        │
│                                                         │
│  Stream 0:  ┌─ matmul_A ── relu_A ──┐                  │
│  Stream 1:  ┌─ matmul_B ── relu_B ──┐                  │
│  Stream 2:       ┌─ matmul_C ── relu_C ──┐             │
│             ─────┴────┴────┴────┴────┴────┴──▶ Clock    │
│                                                         │
│  GPU 上同一 Stream 内串行, 不同 Stream 可并行             │
│  (取决于 SM 数量和 kernel 占用)                          │
│                                                         │
└─────────────────────────────────────────────────────────┘

在 PyTorch 中的使用:
  s1 = torch.cuda.Stream()     # 创建 stream
  s2 = torch.cuda.Stream()

  with torch.cuda.stream(s1):  # 后续 GPU 操作提交到 s1
      out1 = model1(x)

  with torch.cuda.stream(s2):  # 后续 GPU 操作提交到 s2
      out2 = model2(x)

  torch.cuda.synchronize()     # 等待所有 stream 完成
```

### 3.5 端到端训练完整流程

```
┌──────────────────────────────────────────────────────────────────┐
│                  GPU 训练完整时序 (一个 step)                       │
├──────────────────────────────────────────────────────────────────┤
│                                                                  │
│  CPU                    PCIe              GPU                    │
│  │                      │                │                      │
│  │ ① DataLoader        │                │                      │
│  │   取 batch (CPU)    │                │                      │
│  │                      │                │                      │
│  │ ② .cuda() ──────────│─ PCIe DMA ────▶│ 显存就绪              │
│  │   数据搬运到 GPU     │  (~16ms/GB)    │                      │
│  │                      │                │                      │
│  │ ③ 触发 forward      │                │ ④ 前向计算             │
│  │   (CPU 发指令)       │                │   MatMul+ReLU+...     │
│  │   (立即返回)         │                │   autograd 建计算图   │
│  │                      │                │                      │
│  │ ⑦ 触发 backward     │                │ ⑤ 计算结果写回显存     │
│  │   (CPU 发指令)       │                │                      │
│  │   (立即返回)         │                │                      │
│  │                      │                │ ⑥ 反向传播            │
│  │                      │                │   计算梯度 (GPU)      │
│  │                      │                │                      │
│  │                      │                │ ⑧ 梯度存入显存         │
│  │                      │                │                      │
│  │ ⑨ optimizer.step()  │                │ ⑩ 参数更新             │
│  │                      │                │   (原地修改, GPU)     │
│  │                      │                │                      │
│  │ ⑪ loss.item() ──────│◀── PCIe DMA ──│ ⑫ 拷贝标量回 CPU      │
│  │   .cuda() 同步点!    │  (默认 stream │   (仅 1 个 float)     │
│  │   (CPU 等待 GPU)     │   阻塞)       │                      │
│  │                      │                │                      │
│  │ ③④ CPU 发指令立即返回 → GPU 异步执行                       │
│  │ ⑪ 隐式同步 → CPU 这时才真正等 GPU 完成                     │
│                                                                  │
│  关键洞察:                                                       │
│    CPU 端是"发指令"的, GPU 端是"异步执行"的                      │
│    除非遇到同步点, 否则 CPU 会跑到 GPU 前面去                     │
│    同步点: .item() .cpu() .numpy() .cuda() print()              │
│    → 这是 PyTorch 异步执行的精髓                                  │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
```

## 四、自动混合精度 (AMP)

```
torch.cuda.amp 解决的问题:
  FP32: 精度高, 显存占用大, 计算慢
  FP16: 精度低, 显存占用减半, 计算快 2x (Tensor Core 加速)
  → 能不能自动混合用?

┌─────────────────────────────────────────────────────────┐
│                  AMP 工作流程                             │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  with torch.autocast(device_type="cuda"):                │
│      # AMP 自动做的类型转换:                              │
│                                                         │
│      ┌─────────────┐   autocast    ┌─────────────┐     │
│      │ MatMul (FP32)│ ──────────→  │ MatMul (FP16)│     │
│      │ Conv  (FP32) │ ──────────→  │ Conv  (FP16)│     │
│      │ ReLU  (FP32) │ ──────────→  │ ReLU  (FP16)│     │
│      └─────────────┘              └─────────────┘     │
│                                                         │
│      ┌─────────────┐   不转换      ┌─────────────┐     │
│      │ Softmax(FP32)│ ──────────→  │ Softmax(FP32)│     │
│      │ LayerNorm   │ │ ──────────→  │ LayerNorm   │ │     │
│      │ Loss        │ │ ──────────→  │ Loss (FP32) │ │     │
│      └─────────────┘              └─────────────┘     │
│                                                         │
│  scaler = torch.cuda.amp.GradScaler()                   │
│  scaler.scale(loss).backward()  # 梯度缩放, 防 underflow│
│  scaler.step(optimizer)        # 反缩放 + skip 异常梯度 │
│  scaler.update()               # 动态调整缩放因子       │
│                                                         │
│  为什么需要 GradScaler:                                  │
│    FP16 最小正值: 6e-8 → 小梯度直接变成 0 (underflow)   │
│    缩放策略: loss × 2^16, backward, 梯度 ÷ 2^16         │
│    → 大梯度不会溢出, 小梯度不会丢失                      │
│                                                         │
└─────────────────────────────────────────────────────────┘

收益:
  显存: -30~50% (权重/激活从 FP32 → FP16)
  速度: +1.5~3x (Tensor Core FP16 算力是 FP32 的 2x)
  精度: 几乎无损 (数值敏感操作保持 FP32)
```

## 五、多卡并行 (NCCL)

```
torch.cuda 封装了 NCCL 进行多卡通信:

┌─────────────────────────────────────────────────────────┐
│               NCCL 多卡通信原语                           │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  GPU 0    GPU 1    GPU 2    GPU 3                       │
│  ┌──┐    ┌──┐    ┌──┐    ┌──┐                          │
│  │a0│    │b0│    │c0│    │d0│   各自不同的数据           │
│  └──┘    └──┘    └──┘    └──┘                          │
│   │       │       │       │                             │
│   │  ┌────┴───────┴───────┴────┐                       │
│   │  │                          │                       │
│   ▼  │    NCCL AllReduce       │                       │
│   │  │  (Ring / Tree / NVLink) │                       │
│   │  │                          │                       │
│   │  └────┬───────┬───────┬────┘                       │
│   ▼       ▼       ▼       ▼                             │
│  ┌──┐    ┌──┐    ┌──┐    ┌──┐                          │
│  │Σ │    │Σ │    │Σ │    │Σ │   每卡都有总和            │
│  └──┘    └──┘    └──┘    └──┘                          │
│                                                         │
│  PyTorch API:                                           │
│    torch.distributed.all_reduce(tensor, op=ReduceOp.SUM)│
│    torch.distributed.broadcast(tensor, src=0)           │
│    torch.distributed.all_gather(tensor_list, tensor)    │
│                                                         │
│  通信后端选择:                                           │
│    同机 8 卡:  NCCL + NVLink (900 GB/s 全互联)          │
│    跨机多卡:  NCCL + InfiniBand / RoCE                  │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 六、常见 API 速查

```
# 设备管理
torch.cuda.is_available()                          # 有无 GPU
torch.cuda.device_count()                          # GPU 数量
torch.cuda.get_device_name(i)                      # 型号
torch.cuda.get_device_capability(i)                # 算力版本
torch.cuda.set_device(i)                           # 切换设备
torch.cuda.memory_allocated(i)                     # 已用显存
torch.cuda.memory_reserved(i)                      # 缓存显存
torch.cuda.max_memory_allocated(i)                 # 峰值显存
torch.cuda.empty_cache()                           # 释放缓存显存 (归还给 CUDA)

# 同步
torch.cuda.synchronize()                           # 等待当前设备所有 stream 完成
torch.cuda.synchronize(stream)                     # 等待指定 stream
stream.synchronize()                               # 等待 stream
torch.cuda.current_stream()                        # 获取默认 stream

# Stream
s = torch.cuda.Stream()
s = torch.cuda.Stream(priority=-1)                # 高优先级 (-1=高, 0=正常)
torch.cuda.stream(s)                               # 上下文管理器
s.record_event(event)                              # 记录事件
s.wait_event(event)                                # 等待事件

# 随机数
torch.cuda.manual_seed(42)                         # 设置 GPU 随机种子
torch.cuda.seed()                                  # 随机种子
torch.cuda.initial_seed()                          # 获取当前种子

# Profiler
torch.cuda.profiler.start()
torch.cuda.profiler.stop()
with torch.autograd.profiler.profile(use_cuda=True) as prof:
    ...
```

## 七、踩坑与最佳实践

```
1. 隐式同步陷阱
   ❌ loss.item()        # 触发同步, CPU 等待 GPU
   ❌ print(tensor)      # 触发同步 (需要把数据拉到 CPU)
   ❌ tensor.cpu()       # 触发同步
   ✅ 每 100 个 step 才调用一次 loss.item()
   ✅ 用 torch.utils.tensorboard.SummaryWriter (异步写入)

2. 显存泄漏排查
   torch.cuda.memory_summary()        # 查看分配器统计
   torch.cuda.memory_snapshot()       # 分配器快照
   python -m torch.cuda.memory_summary

3. 设备不匹配
   RuntimeError: Expected all tensors to be on the same device
   → model 和 tensor 不在同一设备
   ✅ input = input.to(next(model.parameters()).device)

4. 多卡设置
   torch.cuda.set_device(local_rank)   # 每个进程绑不同 GPU
   model = model.cuda(local_rank)      # 模型放对应 GPU
   # 或更简洁: model.to(f"cuda:{local_rank}")

5. 想释放显存但没释放?
   del tensor                    # 仅删除引用, 显存还在分配器缓存
   torch.cuda.empty_cache()      # 释放缓存到 CUDA runtime
   # 但 Python 进程持有的显存不会归还给 OS (直到进程退出)

6. CUDA OOM
   ✅ 减小 batch_size
   ✅ 开启 gradient_checkpointing (用计算换显存)
   ✅ 用 torch.cuda.amp (FP16 省显存)
   ✅ 用 DeepSpeed ZeRO / FSDP 切分参数
```
