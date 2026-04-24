# torch._dynamo 深度解析

## 一、核心作用

`torch._dynamo` 是 PyTorch 2.0 引入的**JIT 编译器入口**，解决一个核心问题：**把动态的 Python 代码编译成高效的静态图**。

```
PyTorch 1.x (Eager Mode):
  每次 forward:
    Python 解释器逐行执行 → 逐个调用 CUDA kernel → 慢!

  model(input)
   │
   ├── x = self.linear1(x)    # Python 调用开销 ~10μs
   ├── x = torch.relu(x)      # Python 调用开销 ~5μs
   ├── x = self.linear2(x)    # Python 调用开销 ~10μs
   ...
   → 100 层 = 1000+ 次 Python 调用 = 数毫秒额外开销
   → 小模型/小 batch 时, Python 开销占总时间 30-50%

PyTorch 2.0 (torch.compile + torch._dynamo):
  model = torch.compile(model)
   │
   │  torch._dynamo 做的事:
   │  ① 拦截 Python 字节码
    ② 分析计算图 (trace)
    ③ 把连续的 tensor 操作打包成一个大的 graph
    ④ 交给后端编译器 (inductor) 生成优化后的 CUDA kernel
    ⑤ 缓存编译结果, 下次直接执行
   │
   → Python 调用开销从 1000 次降到 1 次
   → 多个小 kernel 融合成大 kernel (算子融合)
   → 整体加速 1.5x~3x (某些场景更多)
```

## 二、核心架构

```
┌─────────────────────────────────────────────────────────┐
│              torch._dynamo 架构全览                       │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  用户代码                                                │
│    │                                                    │
│    ▼                                                    │
│  torch.compile(model)                                   │
│    │                                                    │
│    ▼                                                    │
│  ┌──────────────────────────────────────┐               │
│  │         torch._dynamo               │               │
│  │         (字节码拦截 + 图提取)         │               │
│  │                                      │               │
│  │  ① 拦截 model.forward 的字节码       │               │
│  │  ② 识别 tensor 操作, 构建 FX Graph   │               │
│  │  ③ 处理动态控制流 (graph break)       │               │
│  │  ④ 把图交给编译后端                   │               │
│  └──────────┬───────────────────────────┘               │
│             │                                           │
│     ┌───────┼───────┐                                  │
│     ▼       ▼       ▼                                  │
│  ┌──────┐ ┌──────┐ ┌──────┐                           │
│  │inductor│ │ cudnn│ │ triton│  ← 编译后端             │
│  │(默认) │ │ graph│ │      │                            │
│  └──┬───┘ └──────┘ └──────┘                           │
│     │                                                   │
│     ▼                                                   │
│  优化的 CUDA Kernel (Triton 代码 → PTX → Cubin)         │
│     │                                                   │
│     ▼                                                   │
│  GPU 执行 (缓存编译结果, 后续直接执行)                    │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 三、字节码拦截与图提取

### 3.1 工作原理

```
┌─────────────────────────────────────────────────────────┐
│              Dynamo 图提取时序                             │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  Python 源码:          Python 字节码 (dis):              │
│                                                         │
│  def forward(self, x):  0 LOAD_FAST self               │
│      x = self.l1(x)    2 LOAD_ATTR l1                  │
│      x = relu(x)       4 LOAD_FAST x                   │
│      x = self.l2(x)    6 CALL_FUNCTION 1               │
│      return x           8 STORE_FAST x                  │
│                        10 LOAD_GLOBAL relu             │
│                        12 LOAD_FAST x                   │
│                        14 CALL_FUNCTION 1               │
│                        16 STORE_FAST x                  │
│                        ...                              │
│                                                         │
│  Dynamo 拦截流程:                                       │
│                                                         │
│  ① 第 1 次调用 model(input):                            │
│     │                                                   │
│     ▼                                                   │
│  ┌─────────────────────────────────────────┐            │
│  │ Dynamo Patch: 替换 forward 函数         │            │
│  │                                         │            │
│  │ 原始字节码: LOAD_ATTR l1               │            │
│  │ Dynamo 分析: self.l1 是 nn.Module      │            │
│  │              x 是 Tensor               │            │
│  │              → tensor 操作, 记入 graph │            │
│  │                                         │            │
│  │ 下一条: LOAD_GLOBAL relu               │            │
│  │ Dynamo 分析: relu 是 torch.relu        │            │
│  │              → tensor 操作, 记入 graph │            │
│  │                                         │            │
│  │ 下一条: if x.sum() > threshold:        │            │
│  │ Dynamo 分析: x.sum() 结果依赖数据      │            │
│  │              → 数据依赖, graph break!   │            │
│  │                                         │            │
│  └─────────────────────────────────────────┘            │
│                                                         │
│  ② 生成的 FX Graph:                                     │
│                                                         │
│     graph():                                            │
│         x = placeholder[target=x]                       │
│         l1 = get_attr[target=l1]                        │
│         _0 = call_module[target=l1](args=(x,))          │
│         _1 = call_function[target=relu](args=(_0,))     │
│         return _1                                       │
│                                                         │
│  ③ Graph Break 后:                                      │
│     执行到 if 条件 → 退出编译图                          │
│     根据条件判断 → 可能再进入新的编译图                   │
│     → 模型被切分成多个子图 (subgraph)                    │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

### 3.2 Graph Break 详解

```
┌─────────────────────────────────────────────────────────┐
│              Graph Break 机制                             │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  什么会触发 Graph Break:                                  │
│                                                         │
│  1. 数据依赖的控制流                                      │
│                                                         │
│     # Graph Break! x 的值运行时才知道                     │
│     if x[0] > 0:                                        │
│         y = x * 2                                       │
│     else:                                               │
│         y = x * 3                                       │
│                                                         │
│     Dynamo 处理:                                         │
│     子图 1: [x] → 计算 x[0] → graph break               │
│     Python 执行: if 判断                                 │
│     子图 2: [y = x*2 或 y = x*3] → 继续                 │
│                                                         │
│  2. Python 副作用                                        │
│                                                         │
│     print(x)              # I/O, 有副作用 → graph break  │
│     global_list.append(x) # 修改全局变量 → graph break   │
│     x.item()              # 标量转换 → graph break       │
│                                                         │
│  3. 不支持的 Python 特性                                  │
│                                                         │
│     for i in range(x.shape[0]):  # 数据依赖的循环        │
│         ...                      # → graph break         │
│                                                         │
│     try/except (某些情况)  # → graph break               │
│     raise                   # → graph break             │
│                                                         │
│  Graph Break 的代价:                                      │
│                                                         │
│  无 break (理想):                                        │
│    ┌───────────────────────┐                            │
│    │ 整个 forward 编译成 1 个图 │                        │
│    │ 编译 1 次, 后续直接执行    │                        │
│    └───────────────────────┘                            │
│                                                         │
│  有 break (常见):                                        │
│    ┌──────┐  Python  ┌──────┐  Python  ┌──────┐        │
│    │子图 1 │→ 中间  →│子图 2 │→ 中间  →│子图 3 │        │
│    └──────┘  执行    └──────┘  执行    └──────┘        │
│                                                         │
│    每个 subgraph 之间有 Python 调用开销                  │
│    subgraph 越多 → 编译开销越大 → 加速比越低             │
│                                                         │
│  检查 graph break:                                       │
│    torch._dynamo.explain(model)(input)  # 查看断点位置  │
│    TORCH_LOGS="dynamo" python train.py  # 详细日志      │
│                                                         │
│  解决 graph break:                                       │
│    ❌ if x[0] > 0: y = x * 2                            │
│    ✅ y = torch.where(x > 0, x * 2, x * 3)  # tensor 运算│
│                                                         │
│    ❌ for i in range(n): out += layers[i](x)            │
│    ✅ for layer in layers: x = layer(x) # 固定循环次数   │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 四、编译缓存与重编译

```
┌─────────────────────────────────────────────────────────┐
│              编译缓存与 Guard 机制                        │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  Dynamo 编译一次后缓存结果, 但某些变化需要重新编译:        │
│                                                         │
│  输入 1: input shape (16, 768)                          │
│    → 首次编译, 生成 kernel_A, 缓存                      │
│                                                         │
│  输入 2: input shape (16, 768)                          │
│    → Guard 检查通过, 直接用 kernel_A (0 开销)           │
│                                                         │
│  输入 3: input shape (32, 768)  ← shape 变了!           │
│    → Guard 检查失败, 重新编译, 生成 kernel_B            │
│                                                         │
│  输入 4: input dtype float16 ← dtype 变了!              │
│    → Guard 检查失败, 重新编译, 生成 kernel_C            │
│                                                         │
│  Guard 检查的内容:                                       │
│    - tensor shape                                        │
│    - tensor dtype                                        │
│    - tensor stride                                       │
│    - 某些 Python 变量的值 (编译时常量)                   │
│                                                         │
│  重编译的代价:                                           │
│    首次编译: 10~60 秒 (取决于模型大小)                   │
│    重编译: 同样 10~60 秒                                 │
│    → 如果频繁切换 shape/dtype, 编译开销很大              │
│                                                         │
│  常见触发重编译的场景:                                    │
│    - 序列长度变 (NLP): batch 1 seq_len=100 → batch 1 seq_len=200│
│    - 图像尺寸变 (CV): 224x224 → 512x512                │
│    - 动态 batch: 最后一个 batch 可能不满                  │
│                                                         │
│  缓解方案:                                               │
│    torch.compile(model, dynamic=True)  # 允许动态 shape │
│    # 生成通用 kernel, 适应多种 shape                     │
│    # 代价: kernel 可能稍慢 (无法针对特定 shape 优化)     │
│                                                         │
│  缓存查看:                                               │
│    torch._dynamo.reset()  # 清空所有缓存                 │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 五、Dynamo → Inductor 编译流水线

```
┌─────────────────────────────────────────────────────────┐
│              完整编译流水线时序                             │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  model = torch.compile(model)                            │
│  output = model(input)                                   │
│                                                         │
│  阶段 1: Dynamo (字节码 → FX Graph)                      │
│  ┌──────────────────────────────────────────┐            │
│  │ Python 字节码                              │           │
│  │   │                                       │           │
│  │   ▼                                       │           │
│  │ 逐条分析字节码                             │           │
│  │   │                                       │           │
│  │   ├── tensor 操作 → 加入 FX Graph         │           │
│  │   ├── 数据依赖 → graph break              │           │
│  │   └── 不支持 → graph break (fallback)     │           │
│  │   │                                       │           │
│  │   ▼                                       │           │
│  │ FX Graph (IR)                             │           │
│  └──────────┬───────────────────────────────┘            │
│             │                                           │
│  阶段 2: FX Passes (图优化)                              │
│  ┌──────────▼───────────────────────────────┐            │
│  │ FX Graph                                  │           │
│  │   │                                       │           │
│  │   ▼                                       │           │
│  │ Fuse operations (算子融合)                 │           │
│  │   - conv + bn + relu → fused_conv_bn_relu │           │
│  │   - matmul + add + relu → fused_mm_add_relu│          │
│  │   - dropout (eval 模式) → 移除             │           │
│  │   │                                       │           │
│  │   ▼                                       │           │
│  │ 优化后的 FX Graph                         │           │
│  └──────────┬───────────────────────────────┘            │
│             │                                           │
│  阶段 3: Inductor (图 → Triton/C++ kernel)              │
│  ┌──────────▼───────────────────────────────┐            │
│  │ 优化后的 FX Graph                         │           │
│  │   │                                       │           │
│  │   ▼                                       │           │
│  │ Backend 选择                               │           │
│  │   │                                       │           │
│  │   ├── GPU: 生成 Triton 代码                │           │
│  │   │     Triton 代码 → 编译 → PTX → Cubin  │           │
│  │   │                                       │           │
│  │   └── CPU: 生成 C++ / OpenMP 代码         │           │
│  │         C++ → 编译 → 共享库 (.so)          │           │
│  │   │                                       │           │
│  │   ▼                                       │           │
│  │ 优化的 kernel 代码                        │           │
│  └──────────┬───────────────────────────────┘            │
│             │                                           │
│  阶段 4: 执行 (缓存 + 直接调用)                           │
│  ┌──────────▼───────────────────────────────┐            │
│  │ 下次调用:                                 │           │
│  │   Guard 检查 (shape/dtype/stride)         │           │
│  │     │                                    │           │
│  │     ├── 通过 → 直接调用编译好的 kernel    │           │
│  │     │        (0 编译开销, 极快)           │           │
│  │     │                                    │           │
│  │     └── 失败 → 重新走阶段 1~3             │           │
│  └──────────────────────────────────────────┘            │
│                                                         │
│  算子融合示例 (Inductor 的核心优化):                      │
│                                                         │
│  融合前 (3 个 kernel, 3 次全局内存读写):                  │
│    kernel_1: C = A @ B          (写回 HBM)              │
│    kernel_2: D = C + bias       (读 HBM, 写 HBM)        │
│    kernel_3: E = relu(D)        (读 HBM, 写 HBM)        │
│    → 3 × kernel launch 开销 + 3 × HBM 读写              │
│                                                         │
│  融合后 (1 个 kernel, 1 次 HBM 读写):                    │
│    kernel_fused:                                       │
│      for i in range(M):                                │
│        for j in range(N):                              │
│          acc = 0                                        │
│          for k in range(K):                             │
│            acc += A[i,k] * B[k,j]  # matmul            │
│          acc += bias[j]              # add (寄存器操作) │
│          E[i,j] = max(0, acc)       # relu (寄存器操作) │
│    → 1 × kernel launch + 1 × HBM 读写                    │
│    → 寄存器完成 add 和 relu, 不需要写回显存              │
│    → 带宽节省 3x, latency 降低 ~3x                       │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 六、核心 API

```
# 基本用法
model = torch.compile(model)                    # 默认 inductor 后端
model = torch.compile(model, mode="reduce-overhead")  # CUDA graph 模式
model = torch.compile(model, mode="max-autotune")     # 搜索最优 kernel

# mode 对比:
#   default:         平衡编译时间和运行速度
#   reduce-overhead: 使用 CUDA Graph 减少内核启动开销 (小 batch 最佳)
#   max-autotune:    自动调优, 搜索最优参数 (编译慢, 但运行最快)

# 动态 shape
model = torch.compile(model, dynamic=True)       # 允许动态 shape

# 指定后端
model = torch.compile(model, backend="inductor") # 默认, 推荐
model = torch.compile(model, backend="cudagraphs")
model = torch.compile(model, backend="eager")    # 不编译, 仅测试

# 排查工具
torch._dynamo.explain(model)(input)              # 查看 graph break 原因
torch._dynamo.config.verbose = True              # 详细日志
TORCH_LOGS="dynamo" python train.py              # 环境变量方式
torch._dynamo.reset()                            # 清空缓存
torch.compiler.reset()                           # 清空所有编译缓存

# 跳过编译
@torch.compiler.disable                         # 装饰器跳过
def my_function(x):
    return x + 1

# 强制 inline
@torch.compiler.allow_in_graph                   # 允许自定义函数进图
def my_activation(x):
    return custom_op(x)
```

## 七、实际加速效果

```
┌─────────────────────────────────────────────────────────┐
│              torch.compile 加速效果参考                    │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  模型                 Eager    Compile    加速比          │
│  ───────────────────────────────────────────────────     │
│  ResNet-50 (bs=64)   5.2ms    2.8ms     1.86x           │
│  ResNet-50 (bs=1)    3.8ms    1.1ms     3.45x           │
│  BERT-base (bs=8)    12ms     6.5ms     1.85x           │
│  GPT-2 small (bs=1)  85ms     52ms      1.63x           │
│  ViT-Large (bs=16)   45ms     28ms      1.61x           │
│  Whisper (bs=1)      320ms    180ms     1.78x           │
│                                                         │
│  规律:                                                  │
│    - 小 batch 加速比更高 (Python 开销占比大, 编译收益大) │
│    - 大 batch 加速比低 (计算密集, 编译带来的提升被稀释)  │
│    - Graph Break 多的模型加速比低                        │
│    - 首次运行有编译开销 (warmup)                         │
│                                                         │
│  首次编译耗时参考:                                       │
│    小模型 (ResNet-50):  ~5-10 秒                        │
│    中模型 (BERT):        ~20-40 秒                      │
│    大模型 (GPT-2):       ~60-120 秒                     │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 八、与 torch.jit 的对比

```
┌─────────────────────────────────────────────────────────┐
│            torch._dynamo vs torch.jit 对比                │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  维度           torch.jit (旧)     torch._dynamo (新)    │
│  ───────────────────────────────────────────────────     │
│  编译方式       TorchScript       字节码分析 + FX Graph  │
│                (script/trace)     (自动, 无需改代码)     │
│                                                         │
│  侵入性         需要改代码          不需要改代码           │
│                @torch.jit.script  torch.compile(model)   │
│                @torch.jit.trace                         │
│                                                         │
│  动态控制流    支持有限           自动处理                │
│               (需要 type hint)   (graph break + 重新编译)│
│                                                         │
│  后端          自定义 (TorchScript  inductor (Triton)    │
│                IR)                                      │
│                                                         │
│  算子融合      有限               大量 (inductor 自动)   │
│                                                         │
│  Python 生态   有限兼容           完全兼容                │
│                                                         │
│  性能          好                 更好 (1.5x~3x)         │
│                                                         │
│  状态          维护模式           活跃开发                │
│                                                         │
│  结论: torch._dynamo 是 torch.jit 的替代品               │
│  新项目用 torch.compile, 不要再用 torch.jit              │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 九、踩坑与最佳实践

```
1. Graph Break 排查
   ✅ torch._dynamo.explain(model)(input)
   ✅ TORCH_LOGS="dynamo" python train.py
   ✅ 替换数据依赖的 if → torch.where / torch.cond
   ✅ 替换数据依赖的 for → 固定次数循环 / torch.nn.Layer

2. 编译太慢
   ❌ 每个 epoch 都调用 torch.compile (会重复编译)
   ✅ 只编译一次, 缓存结果
   ✅ torch._dynamo.config.cache_size_limit = 128 (增大缓存)

3. 动态 Shape 处理
   ❌ 每次输入 shape 不同 → 频繁重编译
   ✅ torch.compile(model, dynamic=True)
   ✅ pad 到固定 shape (NLP 常用)
   ✅ 按 shape 分桶 (CV 推理常用)

4. 不兼容的操作
   某些自定义操作可能不支持 → 自动 fallback 到 eager mode
   ✅ @torch.compiler.allow_in_graph 让自定义函数进图
   ✅ torch.ops.my_custom.op 注册自定义算子

5. 内存增加
   编译过程需要额外内存 (存储图 + kernel 代码)
   ✅ 大模型注意 OOM
   ✅ torch._dynamo.config.suppress_errors = True

6. 调试困难
   编译后的代码不好调试 (stack trace 不直观)
   ✅ torch._dynamo.config.disable = True 临时禁用
   ✅ TORCH_COMPILE_DEBUG=1 保留中间文件

7. 不适合 compile 的场景
   - 模型经常变化 (NAS, 动态架构搜索)
   - 输入 shape 极度多变且无法预测
   - 调试阶段 (用 eager mode)
   - 模型只用一次 (编译开销 > 节省的时间)
```
