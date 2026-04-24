# torch._inductor 深度解析

## 一、核心作用

`torch._inductor` 是 PyTorch 2.0 的**后端编译器**，解决一个核心问题：**把 FX Graph 编译成高性能的 GPU/CPU kernel**。

```
FX Graph (inductor 的输入):
  graph():
      %x = placeholder
      %a = call_module[target=conv1](%x)
      %b = call_module[target=bn1](%a)
      %c = call_function[target=relu](%b)
      %d = call_module[target=conv2](%c)
      return %d

inductor 编译后的输出 (GPU):
  一个或几个融合后的 Triton kernel:
    kernel_fused_0(input, conv1_w, conv1_b, bn1_w, ...):
      # conv1 + bn1 + relu 融合成一个 kernel
      for i, j, k in grid:
        acc = 0
        for ki in range(K):
          acc += input[i,ki] * conv1_w[j,ki]   # conv
        acc = (acc - bn1_mean) * bn1_inv_std * bn1_w + bn1_b  # bn
        if acc < 0: acc = 0                     # relu
        output[i,j] = acc

Eager Mode (无 inductor):
  4 次 kernel launch + 4 次 HBM 读写
  每次 Python 调用 ~10μs 开销

Inductor 模式:
  1~2 次 kernel launch + 1~2 次 HBM 读写
  Python 调用开销: 0 (编译后直接调用 kernel)
  → 整体加速 1.5x~3x
```

## 二、在编译栈中的位置

```
┌─────────────────────────────────────────────────────────┐
│              Inductor 在编译栈中的位置                     │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  torch.compile(model)                                   │
│       │                                                 │
│       ▼                                                 │
│  torch._dynamo                                         │
│  字节码拦截 → FX Graph + Graph Break 分割                │
│       │                                                 │
│       ▼                                                 │
│  FX Passes (可选的图级优化)                              │
│  fuse / quantize / dead_code_elimination                 │
│       │                                                 │
│       ▼                                                 │
│  ┌──────────────────────────────────────┐               │
│  │       torch._inductor                │  ← 本文主角   │
│  │                                      │               │
│  │  输入: FX Graph                       │               │
│  │  输出: 优化的 kernel 代码              │               │
│  │  后端: Triton (GPU) / C++ (CPU)       │               │
│  └──────────┬───────────────────────────┘               │
│             │                                           │
│     ┌───────┴────────┐                                  │
│     ▼                ▼                                  │
│  Triton 编译器     C++ 编译器                            │
│  (GPU kernel)     (CPU kernel)                           │
│     │                │                                  │
│     ▼                ▼                                  │
│  PTX → Cubin      .so 共享库                            │
│     │                │                                  │
│     ▼                ▼                                  │
│  GPU 执行          CPU 执行                              │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 三、核心编译流水线

```
┌─────────────────────────────────────────────────────────┐
│              Inductor 编译流水线时序                       │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  FX Graph 输入:                                         │
│                                                         │
│  graph():                                               │
│      %x = placeholder                                   │
│      %a = call_module[target=l1](%x)     # Linear       │
│      %b = call_function[target=relu](%a)  # ReLU        │
│      %c = call_module[target=l2](%b)     # Linear       │
│      %d = call_function[target=add]      # bias add     │
│      %e = call_function[target=relu](%d)  # ReLU        │
│      return %e                                        │
│                                                         │
│  ═══════════════════════════════════════════════════    │
│  阶段 1: Lowering (IR 降级)                              │
│  ═══════════════════════════════════════════════════    │
│                                                         │
│  FX Graph → Inductor IR (基于 Loops)                    │
│                                                         │
│  将高层算子分解为循环 + 内存操作:                         │
│                                                         │
│  l1(x):                                                 │
│    for i in range(M):                                   │
│      for j in range(N):                                 │
│        out[i,j] = 0                                     │
│        for k in range(K):                               │
│          out[i,j] += x[i,k] * weight[j,k]  # matmul    │
│        out[i,j] += bias[j]                    # bias    │
│                                                         │
│  relu(out):                                             │
│    for i in range(M):                                   │
│      for j in range(N):                                 │
│        out[i,j] = max(0, out[i,j])        # relu       │
│                                                         │
│  ═══════════════════════════════════════════════════    │
│  阶段 2: Optimization Passes                            │
│  ═══════════════════════════════════════════════════    │
│                                                         │
│  优化前的 IR (分离的循环):                               │
│                                                         │
│  Loop 1: matmul (写回 HBM)                              │
│    for i, j:                                            │
│      for k: out[i,j] += x[i,k] * w[j,k]                 │
│                                                         │
│  Loop 2: relu (从 HBM 读取)                             │
│    for i, j:                                            │
│      out[i,j] = max(0, out[i,j])                        │
│                                                         │
│  优化后 (融合的循环):                                    │
│                                                         │
│  Fused Loop: matmul + relu (中间结果留在寄存器)          │
│    for i, j:                                            │
│      acc = 0                                            │
│      for k: acc += x[i,k] * w[j,k]                      │
│      acc = max(0, acc)         ← 在寄存器中完成 relu   │
│      out[i,j] = acc               ← 只写一次 HBM       │
│                                                         │
│  HBM 读写: 4 次 → 1 次                                  │
│  Kernel launch: 2 次 → 1 次                             │
│                                                         │
│  ═══════════════════════════════════════════════════    │
│  阶段 3: Scheduling (调度)                              │
│  ═══════════════════════════════════════════════════    │
│                                                         │
│  决定每个 kernel 的执行配置:                              │
│                                                         │
│  GPU (Triton):                                          │
│    - Block 大小: (128, 128)                              │
│    - 每个 Block 处理输出的一部分                          │
│    - 共享内存分配: acc 存在 shared memory                │
│    - 循环展开因子: k 维度 unroll 4                       │
│    - 向量化: 内存读取用 vec4 (128bit 一次读 4 个 float)  │
│                                                         │
│  CPU (C++):                                             │
│    - 线程数: OMP_NUM_THREADS                             │
│    - 循环分块: cache-friendly tiling                     │
│    - SIMD: AVX-512 向量化                                │
│                                                         │
│  ═══════════════════════════════════════════════════    │
│  阶段 4: Code Generation (代码生成)                      │
│  ═══════════════════════════════════════════════════    │
│                                                         │
│  GPU → 生成 Triton 代码:                                │
│                                                         │
│  import triton                                          │
│  import triton.language as tl                           │
│                                                         │
│  @triton.jit                                            │
│  def kernel(in_ptr, w_ptr, bias_ptr, out_ptr,           │
│             M, N, K, BLOCK_M, BLOCK_N, BLOCK_K):        │
│      pid_m = tl.program_id(0)                           │
│      pid_n = tl.program_id(1)                           │
│      rm = tl.arange(0, BLOCK_M) + pid_m * BLOCK_M      │
│      rn = tl.arange(0, BLOCK_N) + pid_n * BLOCK_N      │
│      rk = tl.arange(0, BLOCK_K)                         │
│                                                         │
│      acc = tl.zeros((BLOCK_M, BLOCK_N), tl.float32)     │
│      for k in range(0, K, BLOCK_K):                     │
│          x = tl.load(in_ptr + rm[:, None] * K +         │
│              (rk[None, :] + k))                          │
│          w = tl.load(w_ptr + rn[None, :] * K +         │
│              (rk[:, None] + k))                          │
│          acc += tl.dot(x, w)                             │
│                                                         │
│      bias = tl.load(bias_ptr + rn)                      │
│      acc += bias[None, :]                               │
│      acc = tl.maximum(acc, 0)         # relu            │
│                                                         │
│      tl.store(out_ptr + rm[:, None] * N + rn[None, :],  │
│               acc)                                       │
│                                                         │
│  Triton 编译 → PTX → Cubin → 加载到 GPU                │
│                                                         │
│  CPU → 生成 C++ / OpenMP 代码:                           │
│                                                         │
│  void kernel(float* in, float* w, float* out,           │
│              int M, int N, int K) {                     │
│      #pragma omp parallel for collapse(2)               │
│      for (int i = 0; i < M; i++) {                      │
│          for (int j = 0; j < N; j++) {                  │
│              float acc = 0;                              │
│              #pragma omp simd                           │
│              for (int k = 0; k < K; k++) {              │
│                  acc += in[i*K+k] * w[j*K+k];           │
│              }                                          │
│              acc = fmaxf(0, acc);                        │
│              out[i*N+j] = acc;                           │
│          }                                              │
│      }                                                  │
│  }                                                      │
│                                                         │
│  C++ → gcc/clang 编译 → .so → dlopen 加载              │
│                                                         │
│  ═══════════════════════════════════════════════════    │
│  阶段 5: 缓存 + 执行                                     │
│  ═══════════════════════════════════════════════════    │
│                                                         │
│  编译好的 kernel 缓存到磁盘 (默认 ~/.triton/cache)      │
│  下次相同 shape/dtype → 直接加载缓存的 Cubin/.so        │
│  首次编译: 10~60 秒                                      │
│  后续执行: 0 编译开销                                     │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 四、核心优化 Pass 详解

### 4.1 Operator Fusion (算子融合)

```
┌─────────────────────────────────────────────────────────┐
│              算子融合 — Inductor 最核心优化               │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  融合规则 (epilogue fusion):                             │
│  把 "读-改-写" 模式的后置操作融合进前一个算子             │
│                                                         │
│  可融合的模式:                                          │
│  ┌──────────────────────────────────────────┐           │
│  │ matmul  + bias_add + relu + dropout      │           │
│  │ matmul  + scale + softmax                 │           │
│  │ conv    + bn + relu                       │           │
│  │ add     + relu                            │           │
│  │ mul     + add (bias correction)           │           │
│  │ layer_norm + residual_add                │           │
│  │ any_pointwise_ops (逐元素操作链)          │           │
│  └──────────────────────────────────────────┘           │
│                                                         │
│  为什么能融合 (关键条件):                                │
│                                                         │
│  ✅ 可融合: 后一个算子的输出维度 = 前一个算子的输出维度   │
│     matmul: out[M,N] → relu: out[M,N] → 逐元素操作     │
│     → 每个输出元素独立计算, 可以合并到同一个循环体        │
│                                                         │
│  ❌ 不可融合: reshape, transpose, view                   │
│     matmul: out[M,N] → view: out[M*N] → 维度变化       │
│     → 内存布局改变, 不能在同一循环中完成                  │
│                                                         │
│  融合示例: MatMul + Bias + ReLU                          │
│                                                         │
│  融合前 (3 个 kernel):                                  │
│                                                         │
│  GPU 调度:                                              │
│  ── kernel_1 (matmul) ──┬── kernel_2 (add) ──┬── kernel_3 (relu) ──▶
│                         HBM RW                HBM RW               HBM RW
│  时间: t1=5ms           t2=1ms              t3=1ms    总计=7ms+
│                                                         │
│  融合后 (1 个 kernel):                                  │
│                                                         │
│  GPU 调度:                                              │
│  ── kernel_fused ──────────────────────────────────▶    │
│                     HBM RW (仅 1 次)                    │
│  时间: ~5.2ms (仅比 matmul 略长)                        │
│  加速: 7ms → 5.2ms ≈ 1.35x                             │
│                                                         │
│  更极端的情况 (更多可融合算子):                           │
│  matmul + add + gelu + dropout → 融合后接近 matmul 耗时 │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

### 4.2 Memory Planning (内存规划)

```
┌─────────────────────────────────────────────────────────┐
│              内存规划 — Inplace 优化                     │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  问题: FX Graph 中每个算子都产生新 tensor                │
│  → 中间 tensor 占用大量显存                              │
│                                                         │
│  原始图:                                                │
│                                                         │
│  %a = conv1(%x)        # 新分配 100MB                   │
│  %b = bn1(%a)          # 新分配 100MB                   │
│  %c = relu(%b)         # 新分配 100MB                   │
│  %d = conv2(%c)        # 新分配 200MB                   │
│  %e = relu(%d)         # 新分配 200MB                   │
│  显存峰值: 700MB                                │
│                                                         │
│  内存规划后 (inplace / buffer reuse):                    │
│                                                         │
│  %a = conv1(%x)        # 分配 100MB (buffer_0)         │
│  %b = bn1(%a)          # 复用 buffer_0 (inplace)       │
│                         # %a 不再被使用, 安全覆写        │
│  %c = relu(%b)         # 复用 buffer_0 (inplace)       │
│                         # %b 不再被使用                  │
│  %d = conv2(%c)        # 分配 200MB (buffer_1)         │
│                         # %c 被 conv2 消费, 之后不用     │
│  %e = relu(%d)         # 复用 buffer_1 (inplace)       │
│  显存峰值: 300MB (buffer_0 + buffer_1)                  │
│                                                         │
│  算法: 生命期分析 (Liveness Analysis)                    │
│                                                         │
│  Time ─────────────────────────────────────▶            │
│        t0  t1  t2  t3  t4  t5                           │
│  %a:  [████████]                                        │
│  %b:       [████████]                                   │
│  %c:            [████████]                              │
│  %d:                 [████████████████]                 │
│  %e:                      [████████████████]             │
│                                                         │
│  同一时刻最多 2 个 buffer 存活 → 只需分配 2 个           │
│  t0: 分配 buffer_0 给 %a                               │
│  t1: %a 已死, buffer_0 复用给 %b                       │
│  t2: %b 已死, buffer_0 复用给 %c                       │
│  t3: %c 被 conv2 消费完, %d 需要新 buffer_1            │
│  t4: %d 已死, buffer_1 复用给 %e                       │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

### 4.3 Tiling (分块)

```
┌─────────────────────────────────────────────────────────┐
│              Tiling — 缓存友好的分块                     │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  问题: 大矩阵乘法, 整个矩阵放不进 SRAM                   │
│  → 每次 HBM 读取都要等 ~400 cycles                      │
│                                                         │
│  无 Tiling:                                             │
│                                                         │
│  for i in range(1024):     # 1024×1024 矩阵             │
│    for j in range(1024):                                │
│      for k in range(1024):                              │
│        C[i,j] += A[i,k] * B[k,j]                        │
│                                                         │
│  HBM 访问:                                              │
│    A: 1024×1024×1024 = 10^9 次读取                     │
│    B: 1024×1024×1024 = 10^9 次读取                     │
│    → 大量重复读取, 缓存命中率低                         │
│                                                         │
│  有 Tiling (block = 128):                               │
│                                                         │
│  # 把矩阵切成 128×128 的小块                            │
│  for bi in range(0, 1024, 128):                         │
│    for bj in range(0, 1024, 128):                       │
│      # 加载 A 的一块到 shared memory (SRAM)             │
│      A_tile = A[bi:bi+128, :]     # 从 HBM 加载        │
│      for k in range(0, 1024, 128):                      │
│        B_tile = B[k:k+128, bj:bj+128]  # 从 HBM 加载   │
│        # 在 SRAM 中做小块乘法 (快!)                     │
│        C_tile += A_tile_sub @ B_tile                    │
│      # 写回 C 的一块到 HBM                              │
│      C[bi:bi+128, bj:bj+128] = C_tile                  │
│                                                         │
│  HBM 访问大幅减少:                                      │
│    A: 1024×1024 × 8 = 8MB (每个 block 行复用 8 次)     │
│    B: 1024×1024 × 8 = 8MB                              │
│    → 缓存命中率接近 100%                                │
│                                                         │
│  Inductor 自动选择最优 tile size:                       │
│    考虑: shared memory 大小, 寄存器数量, warp 数量      │
│    Triton 的 BLOCK 参数就是 tile size                   │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

### 4.4 其他优化 Pass

```
┌─────────────────────────────────────────────────────────┐
│              其他 Inductor 优化 Pass                     │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  1. Loop Unrolling (循环展开)                           │
│                                                         │
│     展开 4 次:                                          │
│     for k in range(0, K, 4):                           │
│         acc += a[k]*b[k] + a[k+1]*b[k+1]               │
│              + a[k+2]*b[k+2] + a[k+3]*b[k+3]           │
│     → 减少循环判断开销, 更好利用指令流水线               │
│                                                         │
│  2. Vectorization (向量化)                              │
│                                                         │
│     # 不用向量化的 Triton 代码:                          │
│     for j in range(N):                                  │
│         out[j] = in[j] * scale[j]                       │
│                                                         │
│     # 向量化后 (一次处理 4 个元素):                     │
│     for j in range(0, N, 4):                           │
│         out_vec = tl.load(in_ptr + j, mask=...)         │
│         scale_vec = tl.load(scale_ptr + j, mask=...)    │
│         tl.store(out_ptr + j, out_vec * scale_vec)      │
│     → 1 条 load/store 指令处理 4 个 float (128bit)      │
│                                                         │
│  3. Constant Folding (常量折叠)                         │
│                                                         │
│     # 编译时确定值的操作直接算好                          │
│     scale = 1.0 / (std + 1e-5)  # 如果 std 是常量     │
│     → 编译时直接算出 scale 的值, 运行时不再计算          │
│                                                         │
│  4. Dead Code Elimination (死代码消除)                   │
│                                                         │
│     # dropout 训练模式下保留, 评估模式下删除             │
│     if self.training:                                   │
│         x = F.dropout(x, p=0.5)                         │
│     # eval 时: 上面的分支直接删除, 不生成代码            │
│                                                         │
│  5. View/Squeeze/Unsqueeze 消除                         │
│                                                         │
│     # 很多 reshape/view 操作只是改 metadata, 不改数据   │
│     x = x.view(-1, 256)                                │
│     x = x.unsqueeze(1)                                 │
│     x = x.expand(-1, 3, -1)                            │
│     → 全部消除, 只影响后续操作的 stride 计算             │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 五、Inductor 与 Triton 的关系

```
┌─────────────────────────────────────────────────────────┐
│              Inductor ↔ Triton 协作关系                   │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  Inductor: 不直接写 CUDA kernel                         │
│           而是生成 Triton Python 代码                    │
│           再由 Triton 编译器编译成 GPU 机器码            │
│                                                         │
│  ┌──────────────┐    生成     ┌──────────────┐         │
│  │  Inductor    │───────────▶│  Triton 代码  │         │
│  │  (优化 IR)   │             │  (Python DSL) │         │
│  └──────────────┘             └──────┬───────┘         │
│                                      │                 │
│                               Triton 编译器            │
│                                      │                 │
│                                      ▼                 │
│                               ┌──────────────┐         │
│                               │  LLVM IR      │         │
│                               └──────┬───────┘         │
│                                      │                 │
│                                      ▼                 │
│                               ┌──────────────┐         │
│                               │  PTX          │         │
│                               │  (NVIDIA IR)  │         │
│                               └──────┬───────┘         │
│                                      │                 │
│                                      ▼                 │
│                               ┌──────────────┐         │
│                               │  Cubin        │         │
│                               │  (GPU 机器码) │         │
│                               └──────────────┘         │
│                                                         │
│  为什么用 Triton 而不直接写 CUDA:                       │
│                                                         │
│  写 CUDA kernel:                                        │
│    - 需要手写 memory coalescing                         │
│    - 需要手写 shared memory 管理                        │
│    - 需要手写 block/grid 维度计算                       │
│    - 需要手动处理 bank conflict                         │
│    - 开发周期: 天级                                     │
│                                                         │
│  Triton:                                                │
│    - 自动处理内存合并                                    │
│    - 自动管理 shared memory                             │
│    - 自动选择 block/grid 维度                           │
│    - 自动避免 bank conflict                             │
│    - 开发周期: 分钟级                                   │
│                                                         │
│  Inductor 的角色:                                       │
│    - 自动生成 Triton 代码 (用户不需要写)                │
│    - 自动选择最优的 tiling/block 参数                   │
│    - 自动做算子融合                                     │
│    - 自动做内存规划                                     │
│                                                         │
│  本质: Inductor = 自动 Triton 代码生成器                │
│  目标: 让 PyTorch 用户不需要手动写 Triton/CUDA           │
│        就能获得接近手写 kernel 的性能                    │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 六、Autotuning (自动调优)

```
┌─────────────────────────────────────────────────────────┐
│              Inductor Autotuning 流程                    │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  问题: 同一个算子, 不同的参数组合有不同的最优配置         │
│        Block size=64 在 M=1024 最快                     │
│        Block size=128 在 M=4096 最快                    │
│                                                         │
│  Inductor Autotuning 策略:                              │
│                                                         │
│  阶段 1: 生成候选配置                                   │
│  ┌──────────────────────────────────────────┐           │
│  │ 候选 1: BLOCK=(64, 64),   num_warps=4   │           │
│  │ 候选 2: BLOCK=(128, 64),  num_warps=4   │           │
│  │ 候选 3: BLOCK=(128, 128), num_warps=8   │           │
│  │ 候选 4: BLOCK=(64, 128),  num_warps=4   │           │
│  │ 候选 5: BLOCK=(32, 64),   num_warps=2   │           │
│  │ ...                                    │           │
│  │ (Inductor 自动生成, 基于经验启发式)      │           │
│  └──────────────────────────────────────────┘           │
│                                                         │
│  阶段 2: 逐个 benchmark                                 │
│  ┌──────────────────────────────────────────┐           │
│  │ 候选 1: 编译 → 运行 25 次 → 均值 3.2ms  │           │
│  │ 候选 2: 编译 → 运行 25 次 → 均值 2.1ms  │           │
│  │ 候选 3: 编译 → 运行 25 次 → 均值 1.8ms  │ ← 最快   │
│  │ 候选 4: 编译 → 运行 25 次 → 均值 2.5ms  │           │
│  │ 候选 5: 编译 → 运行 25 次 → 均值 4.1ms  │           │
│  └──────────────────────────────────────────┘           │
│                                                         │
│  阶段 3: 选择最优 → 缓存                                │
│  ┌──────────────────────────────────────────┐           │
│  │ 选择: BLOCK=(128,128), num_warps=8      │           │
│  │ 缓存: key=(op_type, dtype, M, N, K)     │           │
│  │       → best_config                      │           │
│  │                                         │           │
│  │ 下次相同 shape → 直接用缓存的最优配置    │           │
│  └──────────────────────────────────────────┘           │
│                                                         │
│  触发 autotuning 的方式:                                 │
│                                                         │
│  # default: 使用缓存或启发式选择, 不 benchmark           │
│  torch.compile(model)                                   │
│                                                         │
│  # max-autotune: 每个算子都 benchmark, 找最优           │
│  # 编译慢 5~10x, 但运行最快                             │
│  torch.compile(model, mode="max-autotune")             │
│                                                         │
│  # reduce-overhead: 使用 CUDA Graph 减少启动开销         │
│  torch.compile(model, mode="reduce-overhead")          │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 七、端到端示例

```
┌─────────────────────────────────────────────────────────┐
│              端到端: 从模型到 GPU kernel                  │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  import torch                                           │
│                                                         │
│  class Model(torch.nn.Module):                          │
│      def __init__(self):                                │
│          super().__init__()                             │
│          self.l1 = torch.nn.Linear(768, 3072)          │
│          self.l2 = torch.nn.Linear(3072, 768)          │
│                                                         │
│      def forward(self, x):                              │
│          x = self.l1(x)                                 │
│          x = torch.relu(x)                              │
│          x = self.l2(x)                                 │
│          x = torch.relu(x)                              │
│          return x                                       │
│                                                         │
│  model = Model().cuda()                                 │
│  compiled = torch.compile(model)                        │
│                                                         │
│  # 第 1 次调用 (触发编译):                               │
│  input = torch.randn(32, 768, device="cuda")            │
│  output = compiled(input)  # ← 这里发生编译             │
│                                                         │
│  编译过程 (第 1 次调用内部):                             │
│                                                         │
│  1. Dynamo 拦截 forward 字节码                           │
│     → 生成 FX Graph (无 graph break)                    │
│                                                         │
│  2. FX Graph 传递给 Inductor                            │
│     graph():                                            │
│         %x = placeholder                                │
│         %a = call_module[target=l1](%x)                 │
│         %b = call_function[target=relu](%a)              │
│         %c = call_module[target=l2](%b)                 │
│         %d = call_function[target=relu](%c)              │
│         return %d                                       │
│                                                         │
│  3. Inductor Lowering                                   │
│     → 分解为 loop IR                                    │
│                                                         │
│  4. Inductor Optimization                               │
│     → 融合 l1 + relu → kernel_0                         │
│     → 融合 l2 + relu → kernel_1                         │
│     → 内存规划: 2 个 buffer 而非 4 个                   │
│                                                         │
│  5. Inductor Code Generation                            │
│     → 生成 2 个 Triton kernel                           │
│     → Triton → LLVM → PTX → Cubin                      │
│                                                         │
│  6. 缓存 Cubin 到 ~/.triton/cache/                      │
│                                                         │
│  7. 执行 Cubin → 得到 output                           │
│                                                         │
│  # 第 2 次调用 (命中缓存):                               │
│  output2 = compiled(input)  # ← 直接执行, 0 编译开销   │
│                                                         │
│  查看生成的 Triton 代码:                                 │
│  TORCH_COMPILE_DEBUG=1 python train.py                 │
│  # 生成的代码在 /tmp/torchcompile/ 目录下               │
│                                                         │
│  查看 Inductor 优化决策:                                 │
│  TORCH_LOGS="inductor" python train.py                  │
│                                                         │
│  性能对比 (典型结果):                                    │
│  Eager:      2.1 ms                                     │
│  Compile:    1.2 ms  (1.75x 加速)                       │
│  MaxAT:      1.05 ms (2.0x 加速, 编译慢 5x)            │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 八、不支持降级到 Eager 的处理

```
┌─────────────────────────────────────────────────────────┐
│              Inductor Fallback 机制                      │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  某些操作 Inductor 无法优化 (或 Triton 不支持):         │
│  - 稀疏矩阵操作                                        │
│  - 某些自定义 CUDA kernel                               │
│  - 特殊的 reduction 操作                                │
│  - 某些 CPU-only 操作                                   │
│                                                         │
│  处理策略:                                              │
│                                                         │
│  FX Graph:                                              │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌────────┐ │
│  │ conv     │  │ bn       │  │ my_custom │  │ fc     │ │
│  │ (可融合) │  │ (可融合) │  │ (不支持)  │  │ (可融合)│ │
│  └──────────┘  └──────────┘  └──────────┘  └────────┘ │
│                                                         │
│  Inductor 编译结果:                                      │
│                                                         │
│  ┌────────────────────────┐  ┌──────────────────────┐  │
│  │ Triton Kernel (融合)    │  │ Eager Fallback       │  │
│  │                        │  │                      │  │
│  │ conv + bn → 1 kernel   │  │ my_custom_op         │  │
│  │                        │  │ (保持原始 eager 执行) │  │
│  └──────────┬─────────────┘  └──────────┬───────────┘  │
│             │                            │              │
│             └───────── GPU ──────────────┘              │
│                                                         │
│  ┌────────────────────────┐                             │
│  │ Triton Kernel (融合)    │                             │
│  │ fc → 1 kernel          │                             │
│  └────────────────────────┘                             │
│                                                         │
│  结果: 3 个执行单元而非 1 个融合 kernel                  │
│  → 部分优化, 但不会失败                                 │
│  → 用户无需手动处理                                     │
│                                                         │
│  排查 fallback:                                         │
│  TORCH_LOGS="inductor" python train.py                 │
│  # 搜索 "graph break" 或 "fallback" 关键词              │
│                                                         │
│  自定义 fallback 策略:                                  │
│  @torch.compiler.disable                               │
│  def my_custom_op(x):                                   │
│      ... # 跳过编译, 用 eager                           │
│                                                         │
│  # 或用 allow_in_graph 注册为可编译:                    │
│  @torch.compiler.allow_in_graph                        │
│  def my_op(x):                                         │
│      return torch.add(x, 1)                             │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 九、Inductor 配置选项

```
┌─────────────────────────────────────────────────────────┐
│              Inductor 配置速查                            │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  # 编译模式                                             │
│  torch.compile(model, mode="default")          # 平衡   │
│  torch.compile(model, mode="reduce-overhead")  # 小batch │
│  torch.compile(model, mode="max-autotune")    # 最快   │
│                                                         │
│  # 动态 shape                                           │
│  torch.compile(model, dynamic=True)             # 通用   │
│  torch.compile(model, dynamic=[2])             # 第2维动态│
│                                                         │
│  # 后端选择                                             │
│  torch.compile(model, backend="inductor")       # Triton│
│  torch.compile(model, backend="cudagraphs")    # CUDA Graph│
│  torch.compile(model, backend="eager")         # 不编译 │
│                                                         │
│  # 环境变量调优                                         │
│  TORCH_LOGS="inductor"               # indutor 日志    │
│  TORCH_COMPILE_DEBUG=1                # 保存调试文件    │
│  INDUCTOR_DISABLE=1                   # 禁用 inductor  │
│  TRITON_CACHE_DIR=/tmp/triton         # 自定义缓存目录  │
│                                                         │
│  # 高级配置                                             │
│  torch._inductor.config.cpp_wrapper = True    # CPU 模式 │
│  torch._inductor.config.fallback_random = True # 随机降级│
│  torch._inductor.config.size_asserts = False  # 关闭断言│
│  torch._inductor.config.triton.cudagraphs = True # CUDA Graph│
│                                                         │
│  # 调试工具                                             │
│  print(compiled.graph)                    # 查看编译图  │
│  print(compiled.graph.code)              # 查看生成代码│
│  torch._inductor.config.debug = True     # 详细调试    │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 十、踩坑与最佳实践

```
1. 首次编译很慢
   原因: 生成 Triton 代码 → 编译 PTX → 缓存
   ✅ 生产环境: 预热阶段先跑几步, 编译结果被缓存
   ✅ 训练启动: warmup_iters = 5 (前 5 步有编译开销)

2. 动态 Shape 频繁重编译
   原因: 每次 shape 变化都重新编译
   ✅ torch.compile(model, dynamic=True)
   ✅ NLP: pad 到固定长度
   ✅ CV: 固定输入尺寸

3. CUDA Graph 模式 (reduce-overhead) 的限制
   只支持固定 shape, 不支持:
   - 动态 shape
   - Python 副作用 (print, list.append)
   - CPU-GPU 同步
   ✅ 适合: 推理服务 (固定 batch size, 固定 seq len)

4. 显存不足
   编译过程本身需要额外显存
   ✅ 减小 batch size
   ✅ 关闭 autotuning: mode="default"
   ✅ 检查是否有过多 graph break (导致 buffer 无法复用)

5. 性能没有提升甚至变慢
   排查步骤:
   1. TORCH_LOGS="inductor" 查看是否有大量 fallback
   2. torch._dynamo.explain() 查看 graph break
   3. 对比: model(input).mean().backward() eager vs compile
   4. 大 batch + 计算密集: eager 本身就很快, compile 收益小
   ✅ 小 batch + Python 开销大: compile 收益最大

6. Triton 不支持的操作
   某些复杂操作 (如 scatter/gather 特定模式)
   Triton 尚未覆盖 → 自动 fallback 到 eager
   ✅ Triton 0.x → 1.x → 2.x 覆盖率持续提升
   ✅ 自定义: @torch.compiler.allow_in_graph

7. 查看 Inductor 做了什么优化
   TORCH_COMPILE_DEBUG=1 python train.py
   # 生成的文件:
   # /tmp/torchcompile/debug/xxx/__compiled_fn_0.py   # Triton 源码
   # /tmp/torchcompile/debug/xxx/__compiled_fn_0.cubin # GPU 机器码

8. Inductor vs 手写 Triton/CUDA
   Inductor 生成的 kernel 通常达到手写 80~95% 的性能
   极端优化场景 (稀疏/自定义算子) 仍需手写
   ✅ 99% 场景: torch.compile 足够
   ✅ 1% 场景: 手写 Triton kernel + torch.ops 注册
```
