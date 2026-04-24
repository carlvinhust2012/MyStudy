# torch.onnx 深度解析

## 一、核心作用

`torch.onnx` 是 PyTorch 的**模型导出工具**，解决一个核心问题：**把 PyTorch 模型转成 ONNX 格式，让非 PyTorch 框架/推理引擎能加载和运行**。

```
为什么需要 ONNX:

  训练: PyTorch (GPU)
    ↓ 导出
  推理: TensorRT / ONNX Runtime / OpenVINO / CoreML / ...

  不同推理引擎的适配:

  ┌──────────┐    torch.onnx    ┌──────────┐
  │ PyTorch  │ ──────────────▶  │ ONNX 文件 │
  │ .pth     │    torch.onnx     │ .onnx    │
  └──────────┘                  └────┬─────┘
                                    │
              ┌──────────┬───────────┼───────────┐
              ▼          ▼           ▼           ▼
         ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐
         │TensorRT│ │ORT    │ │OpenVINO│ │CoreML  │
         │(NVIDIA)│ │(微软) │ │(Intel) │ │(Apple) │
         │GPU推理 │ │跨平台 │ │CPU推理 │ │iOS/Mac │
         └────────┘ └────────┘ └────────┘ └────────┘

  ONNX 本质: 模型的 "中间语言"
    就像 C++ 可以编译成不同平台的机器码
    PyTorch 模型可以导出成 ONNX, 再被不同引擎加载运行
```

## 二、ONNX 格式本身

```
┌─────────────────────────────────────────────────────────┐
│                 ONNX 文件结构                             │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  ONNX = Open Neural Network Exchange                     │
│  基于 Protocol Buffer (protobuf)                         │
│  扩展名: .onnx (protobuf) 或 .onnxtext (文本格式)        │
│                                                         │
│  onnx.ModelProto:                                       │
│  ┌────────────────────────────────────────┐             │
│  │ opset_import: [opset 17]              │ ← 算子集版本│
│  │                                        │             │
│  │ graph: onnx.GraphProto                 │ ← 计算图   │
│  │   ┌────────────────────────────────┐   │             │
│  │   │ initializers: [weight, bias...] │   │ ← 权重数据│
│  │   │                                │   │             │
│  │   │ nodes: [                        │   │ ← 计算节点│
│  │   │   Conv(op_type, input, output)  │   │             │
│  │   │   BatchNorm(...)                │   │             │
│  │   │   Relu(...)                     │   │             │
│  │   │   Gemm(...)  (= Linear)         │   │             │
│  │   │ ]                               │   │             │
│  │   │                                │   │             │
│  │   │ inputs: [input_tensor]          │   │ ← 模型输入│
│  │   │ outputs: [output_tensor]        │   │ ← 模型输出│
│  │   └────────────────────────────────┘   │             │
│  └────────────────────────────────────────┘             │
│                                                         │
│  Opset 版本很重要:                                       │
│    opset 11: 基础算子 (Conv, MatMul, Relu, ...)         │
│    opset 13: 改进 Resize, Reduce                         │
│    opset 14: 改进 Reshape, Constant                      │
│    opset 15: 新增 LayerNormalization                     │
│    opset 17: 新增 ScatterND, 若干改进                    │
│    opset 18: 新增 GridSample 改进                        │
│    opset 20: 新增 Attention, DeformConv                 │
│    → 高 opset = 支持更多算子, 但部分引擎可能不支持       │
│    → 推理引擎选择合适的 opset 很关键                     │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 三、导出流水线

```
┌─────────────────────────────────────────────────────────┐
│              ONNX 导出流水线时序                           │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  阶段 1: TorchScript Tracing                            │
│                                                         │
│  model = MyModel()                                      │
│  model.eval()                                           │
│  dummy_input = torch.randn(1, 3, 224, 224)              │
│                                                         │
│  torch.onnx.export(                                     │
│      model,                                             │
│      dummy_input,          # 追踪输入 (确定 shape)       │
│      "model.onnx",         # 输出文件路径               │
│      opset_version=17,     # ONNX 算子集版本            │
│      input_names=["input"],                            │
│      output_names=["output"],                          │
│      dynamic_axes={        # 动态维度声明               │
│          "input": {0: "batch_size"},                   │
│          "output": {0: "batch_size"},                  │
│      },                                                │
│  )                                                     │
│                                                         │
│  export 内部流程:                                       │
│                                                         │
│  PyTorch Model                                          │
│       │                                                 │
│       ▼                                                 │
│  ┌──────────────────────────────────────┐               │
│  │ 1. TorchScript Trace                │               │
│  │    用 dummy_input 跑一遍 forward     │               │
│  │    记录所有经过的算子调用             │               │
│  │    → 生成 TorchScript Graph          │               │
│  └──────────────┬───────────────────────┘               │
│                 │                                       │
│  ┌──────────────▼───────────────────────┐               │
│  │ 2. 算子映射                          │               │
│  │                                      │               │
│  │    PyTorch 算子     →    ONNX 算子   │               │
│  │    ─────────────────────────────────│               │
│  │    nn.Conv2d         →    Conv       │               │
│  │    nn.Linear         →    Gemm       │               │
│  │    nn.BatchNorm2d    →    BatchNorm  │               │
│  │    torch.relu        →    Relu       │               │
│  │    torch.matmul      →    MatMul     │               │
│  │    torch.cat         →    Concat     │               │
│  │    torch.split       →    Split      │               │
│  │    torch.softmax     →    Softmax    │               │
│  │    torch.add         →    Add        │               │
│  │    torch.mean        →    ReduceMean │               │
│  │    F.layer_norm      →    LayerNorm  │ (opset >= 17) │
│  │    F.scaled_dot_product_attention     │ (opset >= 20) │
│  │         →    Attention               │               │
│  └──────────────┬───────────────────────┘               │
│                 │                                       │
│  ┌──────────────▼───────────────────────┐               │
│  │ 3. 图优化                            │               │
│  │    - 常量折叠                         │               │
│  │    - 死代码消除                       │               │
│  │    - 冗余 Cast 移除                   │               │
│  └──────────────┬───────────────────────┘               │
│                 │                                       │
│  ┌──────────────▼───────────────────────┐               │
│  │ 4. 序列化                            │               │
│  │    ONNX Graph → protobuf → .onnx 文件│               │
│  │    权重数据嵌入 .onnx (或分离的 .bin) │               │
│  └──────────────┬───────────────────────┘               │
│                 │                                       │
│                 ▼                                       │
│            model.onnx (导出完成)                         │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 四、动态 Shape 与 Dynamic Axes

```
┌─────────────────────────────────────────────────────────┐
│              动态 Shape 声明                              │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  问题: Tracing 用固定 dummy_input, 导出的 shape 被写死   │
│                                                         │
│  dummy_input = torch.randn(1, 3, 224, 224)             │
│  # 导出后 ONNX 输入 shape = [1, 3, 224, 224]           │
│  # 实际推理: batch=16 → shape 不匹配, 报错!             │
│                                                         │
│  解决: dynamic_axes 声明哪些维度是动态的                 │
│                                                         │
│  torch.onnx.export(model, dummy_input, "model.onnx",   │
│      dynamic_axes={                                     │
│          "input": {                                    │
│              0: "batch_size",    # 第 0 维动态           │
│              2: "height",       # 第 2 维动态            │
│          },                                            │
│          "output": {                                   │
│              0: "batch_size",                           │
│          },                                            │
│      }                                                 │
│  )                                                     │
│                                                         │
│  导出后:                                                │
│    input shape:  [batch_size, 3, height, 224]          │
│    output shape: [batch_size, 1000]                    │
│                                                         │
│  → 推理时 batch_size 和 height 可以是任意值             │
│                                                         │
│  常见场景:                                              │
│                                                         │
│  图像分类:                                              │
│    dynamic_axes = {"input": {0: "batch"}}              │
│    # 固定 224x224, 只动态 batch                         │
│                                                         │
│  目标检测:                                              │
│    dynamic_axes = {"input": {0: "batch", 2: "h", 3: "w"}│
│    # batch + 图像尺寸都动态 (多尺度推理)                  │
│                                                         │
│  NLP (BERT):                                           │
│    dynamic_axes = {"input": {0: "batch", 1: "seq_len"}}│
│    # batch + 序列长度都动态                              │
│                                                         │
│  语音 (Whisper):                                        │
│    dynamic_axes = {"input": {1: "audio_len"}}          │
│    # 音频长度动态                                        │
│                                                         │
│  注意事项:                                              │
│    动态维度越多 → 推理引擎优化空间越小 → 可能变慢       │
│    ✅ 能固定就固定, 只声明真正需要动态的维度             │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 五、完整导出示例

```
┌─────────────────────────────────────────────────────────┐
│              ResNet-50 完整导出示例                       │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  import torch                                           │
│  import torchvision                                     │
│                                                         │
│  # 1. 准备模型                                          │
│  model = torchvision.models.resnet50(pretrained=True)   │
│  model.eval()                                           │
│                                                         │
│  # 2. 准备 dummy input                                 │
│  dummy = torch.randn(1, 3, 224, 224)                    │
│                                                         │
│  # 3. 导出 ONNX                                         │
│  torch.onnx.export(                                     │
│      model,                                             │
│      dummy,                                             │
│      "resnet50.onnx",                                   │
│      opset_version=17,                                  │
│      input_names=["images"],                            │
│      output_names=["logits"],                           │
│      dynamic_axes={                                     │
│          "images":  {0: "batch_size"},                 │
│          "logits":  {0: "batch_size"},                 │
│      },                                                │
│      do_constant_folding=True,  # 常量折叠              │
│  )                                                     │
│                                                         │
│  # 4. 验证导出结果                                      │
│  import onnx                                            │
│  import onnxruntime as ort                              │
│                                                         │
│  # 4a. 检查 ONNX 模型合法性                             │
│  onnx.checker.check_model("resnet50.onnx")              │
│                                                         │
│  # 4b. ONNX Runtime 推理                                │
│  sess = ort.InferenceSession("resnet50.onnx")           │
│  ort_input = {"images": dummy.numpy()}                  │
│  ort_output = sess.run(None, ort_input)[0]              │
│                                                         │
│  # 4c. 对比 PyTorch 和 ORT 输出                         │
│  with torch.no_grad():                                  │
│      pt_output = model(dummy).numpy()                   │
│                                                         │
│  max_diff = abs(pt_output - ort_output).max()           │
│  print(f"Max diff: {max_diff}")  # 应该 < 1e-5         │
│                                                         │
│  # 5. 可视化 ONNX 图 (可选)                             │
│  # pip install netron                                   │
│  # netron resnet50.onnx                                 │
│                                                         │
│  导出后文件:                                            │
│    resnet50.onnx  (约 97MB, 包含权重)                   │
│    节点数: ~100 个 ONNX node                            │
│    Opset: 17                                            │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 六、导出后验证流水线

```
┌─────────────────────────────────────────────────────────┐
│              ONNX 导出验证时序                             │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  导出后必须验证! Tracing 可能有遗漏                      │
│                                                         │
│  验证步骤:                                              │
│                                                         │
│  Step 1: 模型结构检查                                    │
│  ┌──────────────────────────────────────────┐           │
│  │ import onnx                              │           │
│  │ onnx.checker.check_model("model.onnx")   │           │
│  │ # 检查: 图结构合法? 类型正确? 引用完整?  │           │
│  │ # 报错 → 导出有问题, 需要修复            │           │
│  └──────────────────────────────────────────┘           │
│                                                         │
│  Step 2: 数值一致性检查                                  │
│  ┌──────────────────────────────────────────┐           │
│  │ test_input = torch.randn(4, 3, 224, 224)│           │
│  │                                         │           │
│  │ # PyTorch 推理                           │           │
│  │ with torch.no_grad():                   │           │
│  │     pt_out = model(test_input).numpy()   │           │
│  │                                         │           │
│  │ # ORT 推理                              │           │
│  │ sess = ort.InferenceSession("model.onnx")│           │
│  │ ort_out = sess.run(None,                │           │
│  │     {"input": test_input.numpy()})[0]    │           │
│  │                                         │           │
│  │ # 对比                                  │           │
│  │ assert np.allclose(pt_out, ort_out,      │           │
│  │                    atol=1e-5)            │           │
│  └──────────────────────────────────────────┘           │
│                                                         │
│  Step 3: 多组输入测试                                    │
│  ┌──────────────────────────────────────────┐           │
│  │ for shape in [(1,3,224,224),             │           │
│  │              (4,3,224,224),             │           │
│  │              (8,3,224,224)]:            │           │
│  │     x = torch.randn(*shape)              │           │
│  │     # ... 对比 PyTorch vs ORT            │           │
│  │ # 特别是 dynamic_axes 声明的维度          │           │
│  └──────────────────────────────────────────┘           │
│                                                         │
│  Step 4: ONNX Simplifier 优化                            │
│  ┌──────────────────────────────────────────┐           │
│  │ import onnxsim                           │           │
│  │                                         │           │
│  │ # 简化: 移除无用节点, 融合常量            │           │
│  │ sim_model = onnxsim.simplify(            │           │
│  │     "model.onnx",                        │           │
│  │     test_input_shapes=[...]              │           │
│  │ )                                       │           │
│  │ onnx.save(sim_model, "model_sim.onnx")  │           │
│  │                                         │           │
│  │ 效果: 节点数减少 10~30%                  │           │
│  │ 文件大小不变 (权重不变)                   │           │
│  └──────────────────────────────────────────┘           │
│                                                         │
│  Step 5: 目标引擎测试                                    │
│  ┌──────────────────────────────────────────┐           │
│  │ # TensorRT                               │           │
│  │ trtexec --onnx=model.onnx                │           │
│  │                                         │           │
│  │ # ONNX Runtime GPU                       │           │
│  │ sess = ort.InferenceSession(             │           │
│  │     "model.onnx",                        │           │
│  │     providers=["CUDAExecutionProvider"]) │           │
│  └──────────────────────────────────────────┘           │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 七、常见导出问题与解决

### 7.1 不支持的算子

```
┌─────────────────────────────────────────────────────────┐
│              不支持算子的处理                              │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  问题: PyTorch 有 ~2000 个算子, ONNX 只有 ~200 个       │
│        很多 PyTorch 算子没有对应的 ONNX 算子             │
│                                                         │
│  常见不支持的算子:                                       │
│                                                         │
│  torch.nn.functional.scaled_dot_product_attention        │
│    → opset < 20: 不支持, 需要手动拆分为 Q/K/V + MatMul  │
│    → opset >= 20: 支持 Attention 算子                   │
│                                                         │
│  torch.unique / torch.argsort                           │
│    → 无直接 ONNX 等价, 需要自定义脚本                    │
│                                                         │
│  自定义 CUDA kernel                                     │
│    → 完全不支持, 必须拆解为基础算子或注册自定义 op       │
│                                                         │
│  解决方案 1: 自定义导出脚本 (torch.onnx.register_custom) │
│                                                         │
│  @torch.onnx.symbolic(g namespace, "my_custom_op")     │
│  def my_custom_op_symbolic(g, input, scale):            │
│      # 用基础 ONNX 算子等价实现                          │
│      return g.op("Mul", input, g.op("Constant",         │
│          value_t=torch.tensor(scale)))                  │
│                                                         │
│  解决方案 2: 修改模型, 拆解为支持的基础算子              │
│                                                         │
│  # 不支持:                                              │
│  x = F.scaled_dot_product_attention(q, k, v)            │
│                                                         │
│  # 改为:                                                │
│  attn = torch.matmul(q, k.transpose(-2, -1))            │
│  attn = attn / math.sqrt(d_k)                           │
│  attn = torch.softmax(attn, dim=-1)                     │
│  x = torch.matmul(attn, v)                              │
│  # → 每个 PyTorch 算子都有 ONNX 对应                    │
│                                                         │
│  解决方案 3: 升级 opset 版本                             │
│  torch.onnx.export(..., opset_version=20)               │
│  # 高版本 opset 支持更多算子                             │
│  # 注意: 目标推理引擎也要支持该 opset                    │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

### 7.2 Tracing vs Scripting

```
┌─────────────────────────────────────────────────────────┐
│              Tracing vs Scripting 对比                    │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  torch.onnx.export 默认使用 Tracing:                    │
│    用 dummy_input 跑一遍, 记录经过的算子                 │
│                                                         │
│  Tracing 的问题 (和 FX Tracing 一样):                    │
│                                                         │
│  def forward(self, x):                                  │
│      if x.shape[0] > 16:           # ← 数据依赖        │
│          return self.big(x)                            │
│      else:                                             │
│          return self.small(x)                           │
│                                                         │
│  Tracing 只走 dummy_input 的一条路径:                    │
│    dummy shape=(1,3,224,224) → 走 else → 只记录 small   │
│    ONNX 中丢失了 big 分支!                              │
│                                                         │
│  解决方案: TorchScript (script 模式)                     │
│                                                         │
│  # 方法 1: torch.jit.script                             │
│  scripted = torch.jit.script(model)                    │
│  torch.onnx.export(scripted, dummy, "model.onnx")      │
│                                                         │
│  # 方法 2: 导出时混合使用                                │
│  torch.onnx.export(                                    │
│      model, dummy, "model.onnx",                       │
│      dynamo=False,        # 不用 dynamo (旧路径)        │
│  )                                                     │
│                                                         │
│  # 方法 3: torch.onnx.dynamo_export (PyTorch 2.0+)     │
│  # 使用 dynamo + FX 做导出, 更强大                      │
│  torch.onnx.dynamo_export(model, dummy).save("m.onnx") │
│                                                         │
│  对比:                                                  │
│                                                         │
│  方式              优点              缺点               │
│  ───────────────────────────────────────────────────     │
│  Tracing           简单, 快速      丢失动态分支         │
│  (默认)                                             │
│                    无需改代码                          │
│                                                         │
│  TorchScript       保留控制流     需要改代码           │
│  (script)          更准确          type hint 要求多     │
│                    完整语义        某些写法不支持       │
│                                                         │
│  Dynamo Export     PyTorch 2.0+   实验性               │
│  (推荐)            保留动态控制流  可能有 bug           │
│                    无需改代码      依赖 dynamo          │
│                                                         │
│  推荐:                                                  │
│    简单模型 (无动态控制流) → 默认 Tracing               │
│    复杂模型 (有动态分支)   → dynamo_export              │
│    兼容旧版                → torch.jit.script           │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

### 7.3 常见导出错误速查

```
┌─────────────────────────────────────────────────────────┐
│              常见导出错误速查                              │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  错误 1: "Unsupported op"                                │
│  → PyTorch 算子无 ONNX 对应                              │
│  ✅ 注册自定义 symbolic, 或拆解为基础算子                │
│  ✅ 升级 opset 版本                                      │
│                                                         │
│  错误 2: "Shape inference failed"                       │
│  → 某些操作的输出 shape 无法推断                         │
│  ✅ 用 torch.onnx.enable_fake_mode()                    │
│  ✅ 手动指定输出 shape                                   │
│                                                         │
│  错误 3: 导出和推理结果不一致                             │
│  → Tracing 丢失了某些分支                                │
│  → dropout/batchnorm 在 train vs eval 模式不同           │
│  ✅ model.eval() 再导出                                  │
│  ✅ torch.manual_seed(0) 固定随机种子                   │
│  ✅ 用 script 替代 trace                                │
│                                                         │
│  错误 4: 导出后文件特别大                                 │
│  → 大模型权重 + protobuf 序列化                          │
│  ✅ 导出后用 onnx-simplifier 优化                       │
│  ✅ 分离权重: torch.onnx.export(..., export_params=False)│
│  ✅ 量化后再导出 (int8 模型小 4x)                       │
│                                                         │
│  错误 5: dynamic_axes 声明后推理引擎不支持               │
│  → TensorRT 对某些动态 shape 支持有限                   │
│  ✅ 用 trtexec --minShapes --optShapes --maxShapes      │
│  ✅ 用 ONNX Runtime 的 shape 推断                       │
│  ✅ 固定不需要动态的维度                                 │
│                                                         │
│  错误 6: "Found instance norm group"                    │
│  → GroupNorm/InstanceNorm 导出问题                       │
│  ✅ opset >= 17 或手动拆解                               │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 八、ONNX → 推理引擎路径

```
┌─────────────────────────────────────────────────────────┐
│              ONNX 下游推理路径                             │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  model.onnx 导出后, 常用推理路径:                        │
│                                                         │
│  路径 1: ONNX Runtime (ORT) — 最通用                     │
│  ┌──────────────────────────────────────────┐           │
│  │ .onnx → ORT 加载 → 直接推理             │           │
│  │                                        │           │
│  │ 优点: 跨平台 (CPU/GPU/移动端)           │           │
│  │       支持 Python/C++/Java/C#            │           │
│  │       无需额外编译                      │           │
│  │ 缺点: 性能一般 (不如 TensorRT)          │           │
│  │ 适用: 快速验证, 通用部署                │           │
│  │                                        │           │
│  │ pip install onnxruntime-gpu            │           │
│  │ sess = ort.InferenceSession(           │           │
│  │     "model.onnx",                      │           │
│  │     providers=["CUDAExecutionProvider"])│           │
│  │ output = sess.run(None, {"input": x})  │           │
│  └──────────────────────────────────────────┘           │
│                                                         │
│  路径 2: TensorRT — NVIDIA GPU 最快                     │
│  ┌──────────────────────────────────────────┐           │
│  │ .onnx → trtexec → .engine → 推理        │           │
│  │                                        │           │
│  │ 优点: GPU 推理最快 (算子融合 + kernel优化)│          │
│  │       FP16/INT8 量化支持                 │           │
│  │       低延迟, 高吞吐                    │           │
│  │ 缺点: 仅 NVIDIA GPU                   │           │
│  │       编译慢 (分钟级)                   │           │
│  │       某些 ONNX 算子不支持              │           │
│  │ 适用: 生产环境 NVIDIA GPU 部署          │           │
│  │                                        │           │
│  │ trtexec --onnx=model.onnx \            │           │
│  │     --fp16 --workspace=4GB              │           │
│  └──────────────────────────────────────────┘           │
│                                                         │
│  路径 3: OpenVINO — Intel CPU 最快                      │
│  ┌──────────────────────────────────────────┐           │
│  │ .onnx → openvino mo → .xml + .bin        │           │
│  │                                        │           │
│  │ 优点: Intel CPU 优化 (AVX-512/VNNI)     │           │
│  │       支持 CPU/GPU/VPU/NPU              │           │
│  │ 缺点: Intel 平台限定                   │           │
│  │ 适用: 边缘设备, CPU 推理               │           │
│  └──────────────────────────────────────────┘           │
│                                                         │
│  路径 4: CoreML — Apple 设备                             │
│  ┌──────────────────────────────────────────┐           │
│  │ .onnx → coremltools → .mlpackage         │           │
│  │                                        │           │
│  │ 优点: iOS/macOS 原生加速 (Neural Engine)│           │
│  │ 缺点: Apple 设备限定                   │           │
│  │ 适用: iOS App 部署                      │           │
│  └──────────────────────────────────────────┘           │
│                                                         │
│  典型部署链路:                                          │
│                                                         │
│  PyTorch 训练 → ONNX 导出 → ORT 验证                    │
│                             │                           │
│                    ┌────────┴────────┐                  │
│                    ▼                 ▼                  │
│               TensorRT           OpenVINO              │
│               (GPU 服务器)        (CPU/边缘)            │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 九、ONNX 优化 (Graph Optimization)

```
┌─────────────────────────────────────────────────────────┐
│              ONNX Graph 优化                               │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  导出的 ONNX 图通常有冗余, 需要优化:                     │
│                                                         │
│  优化 1: onnx-simplifier (最常用)                        │
│                                                         │
│  pip install onnxsim                                    │
│  python -m onnxsim model.onnx model_sim.onnx            │
│                                                         │
│  做了什么:                                              │
│    - 常量折叠: Conv 后面跟 Constant bias → 直接写入权重  │
│    - 冗余节点消除: Shape → Gather → Unsqueeze           │
│    - 冗余 Cast 消除: float32 → float32 (无意义转换)     │
│    - 冗余 Identity 消除                                 │
│    - 冗余 Reshape/Transpose 组合简化                    │
│                                                         │
│  典型效果:                                              │
│    ResNet-50:  102 节点 → 78 节点 (减少 23%)            │
│    BERT:       212 节点 → 184 节点 (减少 13%)            │
│                                                         │
│  优化 2: ONNX Runtime 自带优化                           │
│                                                         │
│  sess = ort.InferenceSession("model.onnx",              │
│      sess_options=ort.SessionOptions(),                 │
│      providers=["CUDAExecutionProvider"])               │
│  # ORT 自动做:                                          │
│  # - 算子融合 (Conv+BN, MatMul+Add)                     │
│  # - 常量折叠                                           │
│  # - 冗余节点消除                                       │
│  # - 内存布局优化                                       │
│                                                         │
│  优化 3: TensorRT 编译期优化                             │
│                                                         │
│  TensorRT 编译时做的优化最多:                             │
│  - 算子融合 (Conv+BN+ReLU → 1 layer)                    │
│  - Kernel 自动调优 (每种参数组合选最快 kernel)           │
│  - FP16/INT8 量化                                       │
│  - Tensor 内存复用                                      │
│  - CUDA Graph 集成                                      │
│                                                         │
│  优化 4: PyTorch 导出前优化                              │
│                                                         │
│  # 在导出前先融合 (减少 ONNX 节点数)                    │
│  model = torch.quantization.fuse_modules(model,         │
│      [["conv1", "bn1", "relu1"]])                      │
│  torch.onnx.export(model, ...)                          │
│                                                         │
│  优化前后对比:                                          │
│                                                         │
│  原始导出:                                              │
│  [Conv] → [BatchNorm] → [Relu] → [Conv] → [BN] → [Relu]│
│  6 个节点, 6 次 kernel launch                           │
│                                                         │
│  优化后 (导出前融合 + onnxsim):                         │
│  [Conv_BN_Relu_fused] → [Conv_BN_Relu_fused]          │
│  2 个节点, 2 次 kernel launch                           │
│  → TensorRT 编译后可能变成 1 个 fused kernel            │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 十、踩坑与最佳实践

```
1. 忘记 model.eval()
   ❌ 训练模式导出: BN 用 batch 统计量, Dropout 保留
   ✅ model.eval() 后导出: BN 用全局统计量, Dropout 被移除

2. 导出验证不充分
   ❌ 只用一组 dummy 验证
   ✅ 多组 shape, 多组数据, 对比 FP32/FP16 结果

3. 动态维度滥用
   ❌ 所有维度都声明 dynamic → 推理引擎无法优化
   ✅ 只声明真正需要动态的维度 (通常只有 batch)

4. Opset 版本选择
   ❌ opset=11 (太旧, 很多算子不支持)
   ❌ opset=20 (太新, 部分推理引擎不支持)
   ✅ opset=17 (推荐, 兼容性好, 算子丰富)

5. 大模型导出 OOM
   ❌ 巨大模型直接导出, 内存不够
   ✅ 分块导出 / 分离权重
   ✅ torch.onnx.export(..., export_params_as_float16=True)

6. 调试技巧
   # 查看导出的图结构
   import onnx
   model = onnx.load("model.onnx")
   print(onnx.helper.printable_graph(model.graph))

   # 可视化
   netron model.onnx  # 浏览器打开, 交互式查看图

   # 算子统计
   op_counts = {}
   for node in model.graph.node:
       op_counts[node.op_type] = op_counts.get(node.op_type, 0) + 1
   print(op_counts)

7. PyTorch 2.0+ 推荐用 dynamo_export
   torch.onnx.dynamo_export(model, dummy_input).save("m.onnx")
   # 更准确, 支持动态控制流, 兼容 torch.compile 生态

8. 部署清单
   导出 → 验证数值 → onnxsim 优化 → ORT 测试
   → 目标引擎编译 → 性能 benchmark → 上线
```
