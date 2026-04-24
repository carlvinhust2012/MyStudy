# torch.fx 深度解析

## 一、核心作用

`torch.fx` 是 PyTorch 的**图级代码变换工具包**，解决一个核心问题：**把 Python 模型转成可操作的中间表示 (IR)，支持程序化地分析和修改模型**。

```
用户写的:
  class MyModel(nn.Module):
      def forward(self, x):
          x = self.conv1(x)
          x = self.bn1(x)
          x = torch.relu(x)
          return x

torch.fx 能做的:
  1. 提取计算图 — 把模型变成一个图结构
  2. 分析计算图 — 统计 FLOPs、分析数据流、检测算子
  3. 变换计算图 — 融合算子、替换层、插入 hook、量化
  4. 生成新代码 — 从变换后的图重建可运行的 Module

本质: 给 PyTorch 模型提供"源码级别的 AST 重构"能力
     但不是改源码, 而是在图级别做等价变换
```

## 二、核心概念

```
┌─────────────────────────────────────────────────────────┐
│                 FX 核心概念                                │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  FX Graph 由三种节点组成:                                 │
│                                                         │
│  1. placeholder — 输入占位符                             │
│     %x = placeholder[target=x]                          │
│     → 代表函数的输入参数                                  │
│                                                         │
│  2. call_function — 调用函数                             │
│     %relu = call_function[target=torch.relu](args=(%x,))│
│     → 调用 torch.relu 函数                               │
│                                                         │
│  3. call_module — 调用子模块                             │
│     %conv = call_module[target=conv1](args=(%x,))       │
│     → 调用 self.conv1 子模块                             │
│                                                         │
│  4. call_method — 调用对象方法                           │
│     %size = call_method[target=size](args=(%x,))        │
│     → 调用 x.size() 方法                                │
│                                                         │
│  5. get_attr — 获取模块属性                             │
│     %w = get_attr[target=weight]                        │
│     → 获取 self.weight 属性                              │
│                                                         │
│  6. output — 输出                                       │
│     return %relu                                        │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 三、三种 API 模式

### 3.1 Symbolic Tracing (符号追踪) — 最常用

```
┌─────────────────────────────────────────────────────────┐
│              Symbolic Tracing 工作流程                    │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  原始模型:                                               │
│                                                         │
│  class Model(nn.Module):                                │
│      def __init__(self):                                │
│          self.conv = nn.Conv2d(3, 64, 3)                │
│          self.bn = nn.BatchNorm2d(64)                   │
│          self.fc = nn.Linear(64*8*8, 10)                │
│                                                         │
│      def forward(self, x):                              │
│          x = self.conv(x)                               │
│          x = self.bn(x)                                 │
│          x = torch.relu(x)                              │
│          x = x.view(x.size(0), -1)                      │
│          x = self.fc(x)                                 │
│          return x                                       │
│                                                         │
│  Tracing 过程:                                          │
│                                                         │
│  fx.symbolic_trace(model)                               │
│       │                                                 │
│       ▼                                                 │
│  ┌─────────────────────────────────────────┐            │
│  │ 1. 创建 dummy TensorProxy 输入            │           │
│  │    x = TensorProxy(name='x', shape=(1,3,32,32))     │
│  │                                         │           │
│  │ 2. 逐行执行 forward():                    │           │
│  │                                         │           │
│  │    x = self.conv(x)                      │           │
│  │    → TensorProxy 调用 __call__           │           │
│  │    → fx 拦截, 记录:                      │           │
│  │      Node(name='conv',                   │           │
│  │           op='call_module',              │           │
│  │           target='conv',                 │           │
│  │           args=(Node('x'),))             │           │
│  │    → 返回新的 TensorProxy                │           │
│  │                                         │           │
│  │    x = self.bn(x)                        │           │
│  │    → 同样拦截, 记录 Node('bn', ...)       │           │
│  │                                         │           │
│  │    x = torch.relu(x)                     │           │
│  │    → 拦截, 记录 Node('relu',             │           │
│  │      op='call_function',                 │           │
│  │      target=torch.relu, ...)             │           │
│  │                                         │           │
│  │    x = x.view(x.size(0), -1)             │           │
│  │    → 拦截 size() → Node('size_1')        │           │
│  │    → 拦截 view() → Node('view')         │           │
│  │                                         │           │
│  │    x = self.fc(x)                        │           │
│  │    → 拦截, 记录 Node('fc', ...)          │           │
│  │                                         │           │
│  │ 3. 收集所有 Node, 构建 Graph              │           │
│  └─────────────────────────────────────────┘            │
│                                                         │
│  生成的 FX Graph:                                       │
│                                                         │
│  graph():                                               │
│      %x     = placeholder[target=x]                     │
│      %conv  = call_module[target=conv](args=(%x,))      │
│      %bn    = call_module[target=bn](args=(%conv,))     │
│      %relu  = call_function[target=torch.relu]          │
│                (args=(%bn,))                            │
│      %size  = call_method[target=size]                  │
│                (args=(%relu,), kwargs={0: 0})           │
│      %view  = call_method[target=view]                  │
│                (args=(%relu, -1))                       │
│      %fc    = call_module[target=fc](args=(%view,))     │
│      return %fc                                         │
│                                                         │
│  注意: 没有 self.conv.weight 的拷贝                      │
│       Graph 只记录调用关系, 权重仍在原 Module 中          │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

### 3.2 Graph Module — 从图重建可运行模块

```
┌─────────────────────────────────────────────────────────┐
│              GraphModule 生成流程                          │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  traced = fx.symbolic_trace(model)                      │
│  # traced 是 GraphModule, 可以像普通 nn.Module 一样用   │
│                                                         │
│  GraphModule 内部结构:                                    │
│                                                         │
│  ┌──────────────────────────────────────┐               │
│  │ GraphModule                          │               │
│  │                                      │               │
│  │  self.graph = Graph(...)             │  ← 图结构    │
│  │  self.conv = original_conv           │  ← 子模块    │
│  │  self.bn   = original_bn             │  ← 子模块    │
│  │  self.fc   = original_fc             │  ← 子模块    │
│  │                                      │               │
│  │  def forward(self, x):               │  ← 自动生成  │
│  │      x = self.conv(x)                │               │
│  │      x = self.bn(x)                  │               │
│  │      x = torch.relu(x)               │               │
│  │      x = x.view(x.size(0), -1)       │               │
│  │      x = self.fc(x)                  │               │
│  │      return x                         │               │
│  └──────────────────────────────────────┘               │
│                                                         │
│  forward 代码从 graph 自动生成 (graph.code):            │
│  print(traced.graph.code)                               │
│                                                         │
│  验证等价性:                                             │
│  input = torch.randn(1, 3, 32, 32)                     │
│  assert torch.allclose(model(input), traced(input))     │
│  # traced 和原始 model 行为完全一致                       │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

### 3.3 Graph Transformation (图变换) — 最强大

```
┌─────────────────────────────────────────────────────────┐
│              图变换工作流程                                │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  变换模板:                                               │
│                                                         │
│  def my_transform(gm: GraphModule, example_inputs):     │
│      # 遍历图中的所有节点                                 │
│      for node in gm.graph.nodes:                        │
│          # 找到目标节点, 进行变换                         │
│          if node.op == 'call_function':                 │
│              if node.target == torch.relu:              │
│                  # 把 relu 替换成 silu                   │
│                  with gm.graph.inserting_before(node):  │
│                      new_node = gm.graph.call_function( │
│                          torch.nn.functional.silu,      │
│                          node.args)                      │
│                  node.replace_all_uses_with(new_node)   │
│                  gm.graph.erase_node(node)              │
│      # 变换后重新编译                                    │
│      gm.recompile()                                     │
│      return gm                                          │
│                                                         │
│  使用:                                                   │
│  transformed = my_transform(traced, (input,))           │
│  # 现在 forward 中的 relu 变成了 silu                    │
│                                                         │
│  完整变换时序:                                           │
│                                                         │
│  原始图:        变换扫描:        变换后图:               │
│                                                         │
│  [x]           [x]             [x]                      │
│   │             │               │                        │
│  [conv]        扫描 conv        [conv]                   │
│   │          → 跳过             │                        │
│  [bn]           [bn]             [bn]                    │
│   │          → 跳过             │                        │
│  [relu]        扫描 relu       [silu]  ← 替换!         │
│   │          → 替换为 silu       │                        │
│  [view]         [view]           [view]                  │
│   │          → 跳过             │                        │
│  [fc]           [fc]             [fc]                    │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 四、图变换实战案例

### 4.1 算子融合 (Conv + BN + ReLU)

```
┌─────────────────────────────────────────────────────────┐
│              算子融合变换时序                              │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  融合前:                                                │
│                                                         │
│  [x]                                                    │
│   │                                                     │
│  [conv2d] → [a]                                        │
│   │                                                     │
│  [batch_norm] → [b]                                    │
│   │                                                     │
│  [relu] → [c]                                          │
│   │                                                     │
│  return c                                               │
│                                                         │
│  3 次 kernel launch + 3 次 HBM 读写                     │
│                                                         │
│  融合后:                                                │
│                                                         │
│  [x]                                                    │
│   │                                                     │
│  [fused_conv_bn_relu] → [c]    ← 1 个 kernel           │
│   │                                                     │
│  return c                                               │
│                                                         │
│  1 次 kernel launch + 1 次 HBM 读写                     │
│                                                         │
│  融合的数学原理 (BN 参数折叠进 Conv):                     │
│                                                         │
│  Conv: y = W * x + b_conv                              │
│  BN:   z = γ * (y - μ) / √(σ² + ε) + β                │
│  ReLU: out = max(0, z)                                 │
│                                                         │
│  推理时 (BN 参数固定):                                   │
│    fused_W = W * γ / √(σ² + ε)                         │
│    fused_b = (b_conv - μ) * γ / √(σ² + ε) + β         │
│    out = max(0, fused_W * x + fused_b)                 │
│                                                         │
│  → 3 个操作变成 1 个, 参数量不变                         │
│  → 训练时不能融合 (BN 统计量在变)                        │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

### 4.2 量化感知变换

```
┌─────────────────────────────────────────────────────────┐
│              量化变换时序                                  │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  FP32 原始图:          量化后图:                          │
│                                                         │
│  [x] (float32)       [x] (float32)                     │
│   │                    │                                │
│   │                  [quantize] ← 插入量化节点           │
│   │                    │                                │
│  [conv] (float32)    [conv] (int8) ← 权重量化           │
│   │                    │                                │
│  [bn] (float32)      [bn] (folded into conv)           │
│   │                    │                                │
│  [relu]              [relu] (int8)                      │
│   │                    │                                │
│  [fc] (float32)      [dequantize] ← 反量化回 float32    │
│   │                    │                                │
│  [output]            [fc] (int8)                        │
│                       │                                │
│                      [dequantize]                       │
│                       │                                │
│                      [output]                           │
│                                                         │
│  变换步骤:                                              │
│  1. fuse_conv_bn: 把 BN 折叠进 Conv                    │
│  2. insert_quant: 在 Conv 前插入 quantize 节点          │
│  3. insert_dequant: 在需要 FP32 的地方插入 dequant      │
│  4. convert_weights: 把 Conv/Linear 权重转成 int8       │
│  5. recompile: 重新生成代码                              │
│                                                         │
│  收益:                                                  │
│    模型大小: -75% (float32 → int8)                      │
│    推理速度: +2~4x (int8 算力是 fp32 的 2~4x)          │
│    精度损失: 通常 < 1% (QAT 量化感知训练)               │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

### 4.3 插入 Profiling Hook

```
┌─────────────────────────────────────────────────────────┐
│              插入 Profiling 节点时序                      │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  目标: 在每个算子前后插入计时节点, 统计每个算子的耗时     │
│                                                         │
│  原始图:          变换后图:                               │
│                                                         │
│  [x]             [x]                                    │
│   │               │                                     │
│  [conv]          [timer_start('conv')] ← 插入           │
│   │               │                                     │
│  [bn]            [conv]                                 │
│   │               │                                     │
│  [relu]          [timer_end('conv')]   ← 插入           │
│   │               │                                     │
│  [fc]            [timer_start('bn')]                    │
│   │               │                                     │
│                  [bn]                                   │
│                  │                                      │
│                  [timer_end('bn')]                       │
│                  │                                      │
│                  [timer_start('relu')]                   │
│                  │                                      │
│                  [relu]                                 │
│                  │                                      │
│                  [timer_end('relu')]                     │
│                  │                                      │
│                  ...                                    │
│                                                         │
│  生成代码 (自动):                                        │
│  def forward(self, x):                                  │
│      profiler.start('conv')                             │
│      x = self.conv(x)                                  │
│      profiler.end('conv')                               │
│      profiler.start('bn')                               │
│      x = self.bn(x)                                    │
│      profiler.end('bn')                                 │
│      ...                                               │
│                                                         │
│  每次运行自动收集每个算子的耗时分布                       │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 五、在 PyTorch 生态中的位置

```
┌─────────────────────────────────────────────────────────┐
│              FX 在 PyTorch 编译栈中的位置                  │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  torch.compile()                                        │
│       │                                                 │
│       ▼                                                 │
│  torch._dynamo (字节码拦截)                              │
│       │                                                 │
│       │  产出 FX Graph                                   │
│       ▼                                                 │
│  ┌─────────────────────────────────────────┐            │
│  │           torch.fx                        │           │
│  │                                          │           │
│  │  FX Graph 作为中间表示 (IR)              │           │
│  │  所有后续优化都在 FX Graph 上进行         │           │
│  │                                          │           │
│  │  ① FX Passes (图优化)                    │           │
│  │     - fuse_modules (算子融合)             │           │
│  │     - quantize (量化)                     │           │
│  │     - split_module (模型切分)             │           │
│  │     - replace_pattern (模式替换)          │           │
│  │                                          │           │
│  │  ② 用户自定义 Pass                       │           │
│  │     - 遍历/修改/插入/删除节点             │           │
│  │     - 自定义优化逻辑                      │           │
│  └──────────┬──────────────────────────────┘            │
│             │                                           │
│             ▼                                           │
│  torch._inductor (后端编译)                              │
│       │                                                 │
│       ▼                                                 │
│  Triton / C++ Kernel                                    │
│                                                         │
│  FX 也是独立可用的:                                      │
│  - 不依赖 torch.compile, 可以单独使用                    │
│  - PyTorch 量化 (torch.quantization) 内部用 FX           │
│  - TorchVision 模型优化内部用 FX                         │
│  - TensorRT 导出路径也用 FX                              │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 六、Tracing 的局限性

```
┌─────────────────────────────────────────────────────────┐
│              Symbolic Tracing 局限性                      │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  1. 动态控制流 — 无法追踪                                │
│                                                         │
│     def forward(self, x):                               │
│         if x.shape[0] > 16:        # 数据依赖!         │
│             return self.large_model(x)                  │
│         else:                                           │
│             return self.small_model(x)                  │
│                                                         │
│     Tracing 时 x 是 TensorProxy,                       │
│     x.shape[0] 无法求值 → 报错!                        │
│                                                         │
│     解决方案:                                            │
│     ✅ torch.fx.wrap(func)  # 跳过不追踪                 │
│     ✅ 用 concrete_args 传入特定值:                      │
│        fx.symbolic_trace(model,                          │
│            concrete_args={'training': False})           │
│     ✅ 用 torch._dynamo (支持动态控制流, 自动 graph break)│
│                                                         │
│  2. 列表/字典动态操作                                    │
│                                                         │
│     def forward(self, x):                               │
│         layers = [self.l1, self.l2, self.l3]            │
│         for l in layers:                                │
│             x = l(x)                                    │
│         return x                                        │
│                                                         │
│     ✅ 用 nn.Sequential 替代手动列表                     │
│     ✅ 用 self.add_module() 注册子模块                   │
│                                                         │
│  3. 全局状态/外部依赖                                    │
│                                                         │
│     counter = 0                                         │
│     def forward(self, x):                               │
│         global counter                                   │
│         counter += 1    # 外部状态, tracing 抓不到       │
│         ...                                             │
│                                                         │
│     ✅ 用 self.register_buffer() 保存状态               │
│                                                         │
│  4. __call__ 间接调用                                   │
│                                                         │
│     def forward(self, x):                               │
│         fn = torch.relu    # 动态选择函数                │
│         return fn(x)                                     │
│                                                         │
│     ✅ 直接调用 torch.relu(x), 不要间接调用              │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 七、常用内置 Pass

```
┌─────────────────────────────────────────────────────────┐
│              FX 内置 Pass 速查                            │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  1. fuse_modules — 算子融合                              │
│     from torch.fx.passes.fuser import fuse_modules      │
│     fused = fuse_modules(model, [['conv', 'bn', 'relu']])│
│                                                         │
│  2. split_module — 按层切分模型                          │
│     from torch.fx.passes.split_module import split_module│
│     parts = split_module(model, example_input,          │
│         lambda node: node.name in ['conv1', 'bn1'])      │
│     # parts[0] 和 parts[1] 分别持有不同子图              │
│                                                         │
│  3. replace_pattern — 模式匹配替换                      │
│     from torch.fx.passes.pattern_matcher import ...      │
│     # 用通配符匹配子图模式, 批量替换                     │
│                                                         │
│  4. graph_manipulation — 图操作基础                     │
│     gm.graph.inserting_before(node)  # 上下文管理器      │
│     gm.graph.inserting_after(node)                      │
│     gm.graph.call_function(fn, args)                    │
│     gm.graph.call_module(name, args)                    │
│     node.replace_all_uses_with(new_node)                │
│     gm.graph.erase_node(node)                           │
│     gm.graph.erase_unused_submodules()                  │
│                                                         │
│  5. 移除无用节点                                         │
│     # 变换后可能产生悬空节点                              │
│     gm.graph.eliminate_dead_code()                      │
│     gm.recompile()                                      │
│                                                         │
│  6. 反量化模拟                                           │
│     from torch.ao.quantization.fx import ...            │
│     # prepare_qat / convert 将 FP32 图转为量化图        │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 八、手写 Transform 完整示例

```
┌─────────────────────────────────────────────────────────┐
│          自定义 Transform: 替换所有 ReLU → GELU           │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  from torch.fx import GraphModule                       │
│  import torch                                           │
│  import torch.nn.functional as F                        │
│                                                         │
│  def replace_relu_with_gelu(gm: GraphModule,            │
│                             example_inputs):            │
│      """把图中所有 relu 替换成 gelu"""                    │
│                                                         │
│      modified = False                                   │
│                                                         │
│      for node in gm.graph.nodes:                        │
│                                                         │
│          # 找到 call_function 且目标是 relu 的节点       │
│          if (node.op == 'call_function' and             │
│              node.target == torch.relu):                 │
│                                                         │
│              # 在 relu 节点之前插入 gelu 节点            │
│              with gm.graph.inserting_before(node):      │
│                  new_node = gm.graph.call_function(     │
│                      F.gelu,                            │
│                      node.args)                         │
│                                                         │
│              # 把所有使用 relu 输出的地方, 改用 gelu 输出│
│              node.replace_all_uses_with(new_node)       │
│                                                         │
│              # 删除旧的 relu 节点                        │
│              gm.graph.erase_node(node)                  │
│              modified = True                            │
│                                                         │
│          # 也处理 nn.ReLU 模块                           │
│          elif (node.op == 'call_module' and             │
│                isinstance(                              │
│                    getattr(gm, node.target),            │
│                    torch.nn.ReLU)):                      │
│                                                         │
│              with gm.graph.inserting_before(node):      │
│                  new_node = gm.graph.call_function(     │
│                      F.gelu, node.args)                 │
│                                                         │
│              node.replace_all_uses_with(new_node)       │
│              gm.graph.erase_node(node)                  │
│              modified = True                            │
│                                                         │
│      if modified:                                       │
│          gm.graph.eliminate_dead_code()  # 清理残留     │
│          gm.recompile()                      # 重新生成 │
│                                                         │
│      return gm                                          │
│                                                         │
│  # 使用:                                                 │
│  traced = fx.symbolic_trace(model)                      │
│  transformed = replace_relu_with_gelu(traced, (input,))│
│  output = transformed(input)                            │
│                                                         │
│  图变换前后对比:                                         │
│                                                         │
│  变换前:              变换后:                              │
│  [conv] → [bn]       [conv] → [bn]                     │
│     → [relu] → [fc]     → [gelu] → [fc]  ← 全部替换   │
│                                                         │
│  print(transformed.graph.python_code())                │
│  # def forward(self, x):                                │
│  #     x = self.conv(x)                                │
│  #     x = self.bn(x)                                  │
│  #     x = torch.nn.functional.gelu(x)  ← 替换完成      │
│  #     x = self.fc(x)                                  │
│  #     return x                                        │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 九、与其他框架 IR 的对比

```
┌─────────────────────────────────────────────────────────┐
│              中间表示 (IR) 对比                            │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  框架         IR 名称       特点                         │
│  ───────────────────────────────────────────────────     │
│  PyTorch      FX Graph     Python 级别, 易读易改        │
│                            保留了模块结构                  │
│                            适合做高层优化                  │
│                                                         │
│  PyTorch      TorchScript  接近 C++ 级别                │
│                            支持 JIT 编译                  │
│                            可序列化导出                   │
│                                                         │
│  TensorFlow  GraphDef      数据流图                     │
│                            operation + tensor           │
│                            无 Python 语义                │
│                                                         │
│  JAX         Jaxpr        函数式 IR                     │
│                            纯函数, 无副作用              │
│                            强类型                        │
│                                                         │
│  ONNX        ONNX Graph    跨框架标准                    │
│                            opset 版本化                  │
│                            适合模型部署                   │
│                                                         │
│  TVM         Relay         算子级 IR                    │
│                            面向后端编译优化                │
│                            类型推断 + Layout 推断        │
│                                                         │
│  FX 的独特优势:                                          │
│    1. 与 Python 深度集成, 写 Pass 就是写 Python          │
│    2. 保留 nn.Module 层次结构, 不会"拍平"               │
│    3. 可以反编译回 Python 代码 (graph.python_code())    │
│    4. 零学习成本 (会 Python 就会写 FX Pass)             │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 十、踩坑与最佳实践

```
1. Tracing 报错 "Could not find node"
   → forward 中用了未注册的子模块或函数
   ✅ 用 self.add_module() 注册所有子模块
   ✅ 用 torch.fx.wrap() 跳过不需要追踪的函数

2. 变换后结果不对
   → 节点替换顺序不对, 或者忘记 eliminate_dead_code
   ✅ 变换后打印 graph.python_code() 检查
   ✅ 用 concrete input 验证: torch.allclose(before, after)

3. 内存泄漏
   → erase_node 后引用没清干净
   ✅ 先 replace_all_uses_with, 再 erase_node
   ✅ 最后调用 eliminate_dead_code()

4. GraphModule 打印
   ✅ print(gm.graph)              # 打印图结构
   ✅ print(gm.graph.python_code()) # 打印生成的 Python 代码
   ✅ print(gm.code)               # 打印 forward 代码

5. 性能考虑
   → symbolic_trace 本身有开销 (~ms 级)
   → 只在构建/导出时做一次, 不要在训练循环里反复 trace
   ✅ traced = fx.symbolic_trace(model)  # 初始化时做一次
   ✅ 训练循环里只用 traced(input)

6. torch._dynamo vs fx.symbolic_trace
   → dynamo 内部也用 FX Graph, 但能力更强
   → dynamo 支持动态控制流 (graph break + 重编译)
   → symbolic_trace 更简单, 适合静态图变换
   ✅ 简单变换 → fx.symbolic_trace
   ✅ 复杂模型/动态控制流 → torch.compile (内部用 dynamo + fx)

7. 版本注意
   FX 从 PyTorch 1.8 开始引入
   FX API 在 1.10 和 2.0 有较大变动
   ✅ PyTorch >= 2.0 使用最新 API
```
