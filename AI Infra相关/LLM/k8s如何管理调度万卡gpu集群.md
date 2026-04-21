# Kubernetes 万卡 GPU 集群调度与大模型分布式通信分析

## 目录

1. [总体架构概览](#1-总体架构概览)
2. [GPU 资源管理与抽象](#2-gpu-资源管理与抽象)
3. [K8s 任务调度机制](#3-k8s-任务调度机制)
4. [万卡 Gang 调度与排队](#4-万卡-gang-调度与排队)
5. [跨站点多数据中心训练调度](#5-跨站点多数据中心训练调度)
6. [大模型分布式通信](#6-大模型分布式通信)
7. [端到端训练任务流程](#7-端到端训练任务流程)
8. [典型万卡集群部署架构](#8-典型万卡集群部署架构)

---

## 1. 总体架构概览

万卡 GPU 集群通常由多个数据中心（Site）组成，每个站点包含多个 GPU 计算节点，通过高速网络互联。Kubernetes 作为统一调度平台，管理从数百到数万张 GPU 的资源分配。

### 1.1 整体架构图

```mermaid
graph TB
    subgraph 用户侧
        User["用户 / 训练平台"]
        Kubectl["kubectl / SDK 提交任务"]
    end

    subgraph K8s 控制平面
        APIServer["API Server\n任务入口"]
        Scheduler["Scheduler / Volcano\n调度决策"]
        Controller["Controller Manager"]
        Etcd["etcd\n集群状态存储"]
    end

    subgraph 调度扩展
        GangSched["Gang Scheduler\nCo-scheduler\nVolcano"]
        TopoAware["拓扑感知调度\nNode Affinity / Topology"]
        QueueMgr["队列管理\n优先级抢占"]
    end

    subgraph 站点A - Site A
        KubeletA1["Kubelet"]
        KubeletA2["Kubelet"]
        NodeA1["GPU Node\n8x A100/H100"]
        NodeA2["GPU Node\n8x A100/H100"]
        NodeAN["GPU Node x N"]
        DevicePluginA["GPU Device Plugin"]
    end

    subgraph 站点B - Site B
        KubeletB1["Kubelet"]
        NodeB1["GPU Node\n8x A100/H100"]
        NodeBN["GPU Node x M"]
        DevicePluginB["GPU Device Plugin"]
    end

    subgraph 网络层
        IBFabric["InfiniBand Fabric\n跨节点 RDMA"]
        RoCE["RoCE v2\n跨交换机 PFC"]
        WANLink["跨数据中心互联\n100G/400G"]
    end

    User --> Kubectl --> APIServer
    APIServer --> Etcd
    APIServer --> Scheduler
    Scheduler --> GangSched
    Scheduler --> TopoAware
    GangSched --> QueueMgr
    Scheduler --> Controller

    Controller --> KubeletA1
    Controller --> KubeletA2
    Controller --> KubeletB1

    NodeA1 --> DevicePluginA
    NodeA2 --> DevicePluginA
    NodeB1 --> DevicePluginB

    NodeA1 --> IBFabric
    NodeA2 --> IBFabric
    NodeB1 --> RoCE
    IBFabric --> WANLink
    RoCE --> WANLink
```

---

## 2. GPU 资源管理与抽象

### 2.1 GPU Device Plugin 机制

Kubernetes 通过 Device Plugin 框架将 GPU 资源暴露给调度器：

```mermaid
sequenceDiagram
    participant Kubelet
    participant DevicePlugin as NVIDIA GPU Device Plugin
    participant APIServer
    participant Scheduler

    Kubelet->>DevicePlugin: 启动时注册 gRPC 服务
    DevicePlugin->>Kubelet: ListAndWatch 持续上报设备状态

    loop 设备状态变化
        DevicePlugin->>Kubelet: 上报 GPU 列表和健康状态
        Kubelet->>APIServer: 更新 Node Status\nnvidia.com/gpu=8
    end

    Note over APIServer: Node 资源更新\nnvidia.com/gpu 可调度数量

    APIServer->>Scheduler: 调度周期触发
    Scheduler->>APIServer: 查询 Node 资源
    APIServer-->>Scheduler: 返回可用 GPU 数量
```

### 2.2 GPU 资源扩展

| 资源类型 | Device Plugin | 说明 |
|----------|---------------|------|
| `nvidia.com/gpu` | NVIDIA GPU Operator | 整卡分配 |
| `nvidia.com/mig-4g.20gb` | NVIDIA MIG | A100/H100 切片 |
| `nvidia.com/gpumem` | GPU Memory Operator | 显存按需分配 |
| `rdma/hca` | RDMA Device Plugin | InfiniBand 网卡 |
| `huawei.com/Ascend910` | Ascend NPU Plugin | 昇腾 NPU |

### 2.3 节点标签与拓扑标注

万卡集群需要丰富的拓扑标注来指导调度：

```yaml
# 节点拓扑标注示例
labels:
  topology.kubernetes.io/region: cn-beijing
  topology.kubernetes.io/zone: zone-a
  topology.kubernetes.io/rack: rack-01
  gpu-topology: nvlink-connected  # 同节点 NVLink
  network-topology: ib-fabric-a    # InfiniBand 子网
annotations:
  gpu-array: "dgx-a100"            # 服务器型号
  nccl-topo: "NODE0-NODE1..."      # NCCL 拓扑描述
```

---

## 3. K8s 任务调度机制

### 3.1 标准 K8s 调度流程

```mermaid
sequenceDiagram
    participant User
    participant APIServer
    participant Queue as 调度队列
    participant Scheduler as kube-scheduler
    participant Cache as Scheduler Cache
    participant NodeA
    participant NodeB
    participant KubeletA

    User->>APIServer: 提交 Pod / Job YAML
    APIServer->>APIServer: 验证 + 存入 etcd
    APIServer-->>User: 返回已接受

    Note over Scheduler: Informer Watch 到新 Pod
    APIServer->>Queue: Pod 加入调度队列
    Queue->>Scheduler: Pop 待调度 Pod

    Scheduler->>Cache: 获取 Node 快照
    Cache-->>Scheduler: 返回所有可用 Node 信息

    Note over Scheduler: 调度算法执行

    Scheduler->>Scheduler: Filter 阶段\n- 资源足够?\n- NodeSelector 匹配?\n- Taint/Toleration?\n- GPU 数量满足?
    Scheduler->>Scheduler: Score 阶段\n- 资源均衡\n- 拓扑亲和\n- Pod 打散

    Scheduler->>NodeA: Bind 决策 (最优节点)
    Scheduler->>APIServer: 创建 Binding 对象
    APIServer->>APIServer: 更新 Pod 的 nodeName

    APIServer->>KubeletA: Watch 通知
    KubeletA->>KubeletA: 拉取 Pod Spec
    KubeletA->>KubeletA: 调用 CRI 创建容器
    KubeletA->>KubeletA: 调用 Device Plugin 分配 GPU
    KubeletA-->>APIServer: 更新 Pod Status: Running
```

### 3.2 大模型训练任务的 Pod/Job 结构

大模型分布式训练通常以 Job (如 `PyTorchJob` / `MPIJob`) 方式提交，内含多个 Pod：

```
TrainingJob (Volcano / KubeFlow)
├── Launcher Pod          (1个, 协调训练)
│   └── 启动 torchrun/accelerate
└── Worker Pod Group      (N个, 执行训练)
    ├── Worker-0  (8 GPUs)
    ├── Worker-1  (8 GPUs)
    ├── ...
    └── Worker-N  (8 GPUs)
```

### 3.3 Pod 模板示例

```yaml
apiVersion: batch.volcano.sh/v1alpha1
kind: Job
metadata:
  name: llama3-70b-pretrain
spec:
  schedulerName: volcano
  queue: high-priority
  tasks:
    - replicas: 256          # 256 个 Worker Pod
      name: worker
      template:
        spec:
          containers:
            - name: train
              image: pytorch:2.1-cuda12
              resources:
                limits:
                  nvidia.com/gpu: 8     # 每 Pod 8 卡
                  cpu: 64
                  memory: 256Gi
              command: ["torchrun", "--nnodes=256", "--nproc_per_node=8", "train.py"]
          nodeSelector:
            topology.kubernetes.io/zone: zone-a
          affinity:
            podAntiAffinity:
              preferredDuringScheduling:
                - weight: 100
                  podAffinityTerm:
                    labelSelector:
                      matchLabels:
                        job-name: llama3-70b-pretrain
                    topologyKey: kubernetes.io/hostname
```

---

## 4. 万卡 Gang 调度与排队

### 4.1 为什么需要 Gang 调度

标准 K8s 调度器逐个调度 Pod，对大模型训练会导致：
- **死锁**：256 个 Pod 需要 256 节点，但部分节点空闲导致部分 Pod 调度成功，其余永远等待
- **资源浪费**：已调度的 Pod 空等，占据其他任务需要的资源
- **All-or-Nothing 语义**：分布式训练需要所有 Worker 同时就绪

**Gang Scheduling** 保证：一个 Job 的所有 Pod 要么全部调度成功，要么全部不调度。

### 4.2 Volcano Gang 调度流程

```mermaid
sequenceDiagram
    participant User
    participant APIServer
    participant Volcano as Volcano Scheduler
    participant Queue as vcjob Queue
    participant Cache as Scheduler Cache
    participant Nodes as GPU Node 集群

    User->>APIServer: 提交 Volcano Job\n256 Workers x 8 GPUs = 2048 GPUs
    APIServer->>Queue: Job 进入 pending 状态

    Note over Volcano: 调度周期触发
    Volcano->>Queue: 按优先级排序队列

    loop 遍历 Queue 中的 Job
        Volcano->>Cache: 获取集群资源快照

        alt 集群可用 GPU >= 2048
            Volcano->>Volcano: Gang 调度 - Session
            Note over Volcano: 为所有 256 个 Worker\n同时分配 Node

            Volcano->>Nodes: 批量绑定 256 个 Pod
            Nodes-->>Volcano: 绑定成功

            Volcano->>APIServer: 更新所有 Pod 为 Running
            Note over Nodes: 所有 Worker 同时启动\nNCCL 互连建立
        else 集群可用 GPU < 2048
            Volcano->>Queue: Job 保持 pending
            Note over Volcano: 等待其他 Job 释放资源\n或触发抢占
        end
    end
```

### 4.3 抢占与优先级

```mermaid
sequenceDiagram
    participant HighJob as 高优先级 Job A\n需要 2048 GPU
    participant LowJob as 低优先级 Job B\n已占用 1024 GPU
    participant Volcano as Volcano Scheduler
    participant Cluster as 集群资源

    Note over Cluster: 总计 3072 GPU\nJob B 占用 1024\n空闲 2048

    HighJob->>Volcano: Job A 需要 2048 GPU
    Volcano->>Cluster: 检查可用资源
    Cluster-->>Volcano: 空闲 2048 GPU -- 刚好够

    Note over Volcano: 但考虑碎片化后的\n实际可分配可能不足

    alt 碎片化导致无法分配
        Volcano->>LowJob: Preempt 低优先级 Job B
        LowJob->>LowJob: 优雅停止训练\n保存 checkpoint
        LowJob->>Volcano: 释放 1024 GPU
        Volcano->>HighJob: Gang 调度 Job A\n2048 GPU 就绪
    end
```

### 4.4 主流调度方案对比

| 方案 | 核心机制 | Gang 调度 | 适用规模 | 社区状态 |
|------|----------|-----------|----------|----------|
| kube-scheduler | 逐 Pod 调度 | 不支持 | 小规模 | K8s 内置 |
| Volcano | PodGroup + Gang | 原生支持 | 万卡 | CNCF 项目 |
| Kueue | ClusterQueue | 原生支持 | 中大规模 | K8s SIG |
| YuniKorn | 异构资源感知 | 原生支持 | 万卡 | Apache 项目 |
| Slurm on K8s | Slurm 调度器桥接 | Slurm 原生 | 超大规模 | 混合部署 |

---

## 5. 跨站点多数据中心训练调度

### 5.1 多站点资源视图

```mermaid
graph TB
    subgraph 全局调度层
        GScheduler["全局调度器\nVolcano / Kueue"]
        GQueue["全局队列"]
    end

    subgraph SiteA["站点 A - 北京"]
        SKA["Site Kubelet"]
        NA1["Node Group 1\n512 GPUs"]
        NA2["Node Group 2\n512 GPUs"]
        NA1 --> SKA
        NA2 --> SKA
    end

    subgraph SiteB["站点 B - 上海"]
        SKB["Site Kubelet"]
        NB1["Node Group 1\n256 GPUs"]
        NB2["Node Group 2\n256 GPUs"]
        NB1 --> SKB
        NB2 --> SKB
    end

    subgraph SiteC["站点 C - 深圳"]
        SKC["Site Kubelet"]
        NC1["Node Group 1\n1024 GPUs"]
        NC1 --> SKC
    end

    GQueue --> GScheduler
    GScheduler --> SKA
    GScheduler --> SKB
    GScheduler --> SKC
```

### 5.2 跨站点调度策略

跨站点调度需要考虑网络延迟和带宽差异：

```mermaid
flowchart TB
    Start["Job 提交\n需要 2048 GPU"]
    Strategy{"调度策略选择"}

    Strategy -->|"站点内优先"| IntraSite["优先在单站点内分配\n延迟最低 < 1us"]
    Strategy -->|"跨站点最优"| CrossSite["跨站点负载均衡\n考虑站点间带宽"]
    Strategy -->|"亲和性约束"| Affinity["按数据局部性调度\n训练数据所在站点优先"]

    IntraSite --> CheckA{"单站点\nGPU 足够?"}
    CheckA -->|"是"| AssignA["全部分配到站点 A"]
    CheckA -->|"否"| CrossSite

    CrossSite --> Bandwidth["按站点间带宽权重分配\n北京-上海 400G\n北京-深圳 200G"]
    Bandwidth --> TopoLabel["标注网络拓扑标签\ncross-site: true"]

    Affinity --> DataLocality["检查数据存储位置\nCephFS / JuiceFS\n挂载点所在站点"]
    DataLocality --> CoLocate["数据与计算同站点部署"]
```

### 5.3 跨站点网络拓扑感知调度

```mermaid
sequenceDiagram
    participant Job
    participant Scheduler
    participant SiteA
    participant SiteB
    participant Network

    Job->>Scheduler: 提交跨站训练 Job\n2048 GPU
    Scheduler->>Scheduler: 查询站点资源状态

    Scheduler->>SiteA: 可用 1024 GPU?
    SiteA-->>Scheduler: 1024 GPU 可用

    Scheduler->>SiteB: 可用 1024 GPU?
    SiteB-->>Scheduler: 1024 GPU 可用

    Scheduler->>Network: 查询站点间带宽/延迟
    Network-->>Scheduler: 北京-上海 400Gbps RTT 2ms

    Scheduler->>Scheduler: 分配策略\n1024 GPU / 站点

    Note over Scheduler: 关键：确保跨站 Pod\n被分配到同一 IB 子网\n或 RDMA 可达的节点

    Scheduler->>SiteA: 调度 128 Worker Pod
    Scheduler->>SiteB: 调度 128 Worker Pod

    Note over SiteA,SiteB: NCCL_ID 交换\n确保跨站 NCCL 通信建立
```

---

## 6. 大模型分布式通信

### 6.1 通信原语层级

```mermaid
graph TB
    subgraph 应用层
        DDP["DistributedDataParallel\nPyTorch DDP / FSDP"]
        Megatron["Megatron-LM\nTensor + Pipeline Parallel"]
        DeepSpeed["DeepSpeed ZeRO\n1/2/3 Stage"]
    end

    subgraph 通信层
        NCCL["NCCL\nNVIDIA Collective\n通信库"]
        Gloo["Gloo\nCPU 通信后备"]
        UCX["UCX\n统一通信框架"]
    end

    subgraph 传输层
        IB["InfiniBand\nRDMA Verbs"]
        RoCE["RoCE v2\nRDMA over Ethernet"]
        NVLink["NVLink / NVSwitch\n节点内互联"]
        TCP["TCP/IP\n远程后备"]
    end

    subgraph 物理层
        GPUDirect["GPUDirect RDMA\nGPU 直连网卡"]
        NIC["NIC\nConnectX-7 / BlueField"]
        Switch["Switch\nQuantum HDR"]
        Cable["光缆 / DAC"]
    end

    DDP --> NCCL
    DDP --> Gloo
    Megatron --> NCCL
    DeepSpeed --> NCCL

    NCCL --> IB
    NCCL --> RoCE
    NCCL --> NVLink
    Gloo --> TCP
    UCX --> IB
    UCX --> RoCE

    IB --> GPUDirect
    RoCE --> GPUDirect
    NVLink --> GPUDirect
    IB --> NIC
    RoCE --> NIC
    NIC --> Switch
    Switch --> Cable
```

### 6.2 NCCL 通信初始化流程

```mermaid
sequenceDiagram
    participant W0 as Worker 0 (Rank 0)
    participant W1 as Worker 1 (Rank 1)
    participant WN as Worker N
    participant NCCL as NCCL Runtime
    participant NET as Network Fabric

    Note over W0,W1: 每个 Worker 进程独立启动

    W0->>W0: 初始化进程组\ninit_process_group
    W1->>W1: 初始化进程组

    Note over W0: rank=0, world_size=N\n作为 NCCL 通信协调者

    W0->>NCCL: ncclCommInitRank\n生成唯一 ncclId

    Note over W0: ncclId 通过 TCP 共享\n给所有其他 Worker

    W0->>W1: TCP 广播 ncclId
    W0->>WN: TCP 广播 ncclId

    W1->>NCCL: ncclCommInitRank\n传入相同 ncclId
    WN->>NCCL: ncclCommInitRank\n传入相同 ncclId

    NCCL->>NET: 建立通信通道
    NET->>NET: 拓扑探测\nNVLink 优先 > IB > RoCE > TCP

    Note over NCCL: 根据 GPU 拓扑\n选择最优通信路径

    NCCL-->>W0: 通信建立完成
    NCCL-->>W1: 通信建立完成
    NCCL-->>WN: 通信建立完成

    Note over W0,WN: 所有 Rank 就绪\n开始训练循环
```

### 6.3 单步训练中的通信模式

```mermaid
sequenceDiagram
    participant AllRanks as 所有 Rank 0...N-1
    participant NCCL as NCCL Collectives
    participant NVLink as NVLink 节点内
    participant IB as InfiniBand 节点间

    Note over AllRanks: === 前向计算 Forward ===
    AllRanks->>AllRanks: 各 Rank 独立计算前向

    Note over AllRanks: === 反向计算 Backward ===
    AllRanks->>AllRanks: 各 Rank 计算本地梯度

    Note over AllRanks: === 梯度同步 DDP AllReduce ===

    AllRanks->>NCCL: 发起 AllReduce

    NCCL->>NVLink: 节点内 AllReduce\nNVLink 全连接拓扑
    Note over NVLink: 8 卡 NVSwitch 互联\n带宽 600 GB/s

    NCCL->>IB: 节点间 AllReduce\nRing / Tree / Sharp
    Note over IB: HDR InfiniBand\n带宽 400 Gb/s per link

    IB-->>NCCL: 跨节点梯度聚合完成
    NVLink-->>NCCL: 节点内聚合完成

    NCCL-->>AllRanks: 所有 Rank 获得全局平均梯度

    Note over AllRanks: === 参数更新 Optimizer Step ===
    AllRanks->>AllRanks: 各 Rank 独立更新参数

    Note over AllRanks: === 进入下一步 ===
```

### 6.4 并行策略与通信模式

```mermaid
flowchart TB
    Parallel["并行策略选择"]

    Parallel --> DP["数据并行 Data Parallel"]
    Parallel --> TP["张量并行 Tensor Parallel"]
    Parallel --> PP["流水线并行 Pipeline Parallel"]
    Parallel --> ZeRO["ZeRO 优化"]

    DP --> DPComm["通信类型: AllReduce\n频率: 每步 1 次\n数据量: 梯度大小\n瓶颈: 带宽"]
    DP --> DP2D["2D Hybrid: TP + DP\n节点内 TP + 节点间 DP"]

    TP --> TPComm["通信类型: AllReduce\n频率: 每 Layer 2 次\n数据量: 激活值\n瓶颈: 延迟\n约束: 必须节点内 NVLink"]
    TP --> TPNote["通信量小但频繁\n需要 NVLink 低延迟"]

    PP --> PPComm["通信类型: Point-to-Point\n频率: 每 Micro-batch\n数据量: 隐藏层状态\n瓶颈: 流水线气泡"]
    PP --> PPNote["1F1B 调度减少气泡\n可跨节点通信"]

    ZeRO --> ZeRO1["ZeRO Stage 1\n分片优化器状态\n通信: AllGather"]
    ZeRO --> ZeRO2["ZeRO Stage 2\n分片梯度\n通信: ReduceScatter + AllGather"]
    ZeRO --> ZeRO3["ZeRO Stage 3\n分片参数\n通信: AllGather 前向 + 反向"]

    DP2D --> HybridComm["节点内: TP AllReduce via NVLink\n节点间: DP AllReduce via IB\n减少跨节点通信"]
```

### 6.5 NCCL 跨站点通信

```mermaid
sequenceDiagram
    participant W0A as Worker 北京 Rank 0
    participant WNA as Worker 北京 Rank N
    participant NCCLA as NCCL 北京
    participant W0B as Worker 上海 Rank 0
    participant WNB as Worker 上海 Rank N
    participant NCCLB as NCCL 上海
    participant WAN as 跨数据中心链路 400G

    W0A->>NCCLA: 发起跨站 AllReduce
    NCCLA->>NCCLA: 建立跨站 TCP 通道

    NCCLA->>NCCLB: 通过跨数据中心网络建立 NCCL 连接\nncclId 通过 envoy/proxy 交换
    NCCLB->>W0B: 通知参与通信

    Note over NCCLA,NCCLB: 跨站通信可能使用\nRing AllReduce 或\nTree-based Reduce

    loop 每次 AllReduce 操作
        W0A->>NCCLA: 本地梯度 reduce
        WNA->>NCCLA: 本地梯度 reduce
        NCCLA->>NCCLA: 北京站内聚合完成

        W0B->>NCCLB: 本地梯度 reduce
        WNB->>NCCLB: 本地梯度 reduce
        NCCLB->>NCCLB: 上海站内聚合完成

        NCCLA->>WAN: 发送聚合结果到上海
        NCCLB->>WAN: 发送聚合结果到北京
        WAN-->>NCCLB: 接收北京数据
        WAN-->>NCCLA: 接收上海数据

        NCCLA->>NCCLA: 全局 reduce 完成
        NCCLB->>NCCLB: 全局 reduce 完成
    end

    NCCLA->>W0A: 广播全局平均梯度
    NCCLA->>WNA: 广播全局平均梯度
    NCCLB->>W0B: 广播全局平均梯度
    NCCLB->>WNB: 广播全局平均梯度
```

---

## 7. 端到端训练任务流程

### 7.1 从提交到训练启动全流程

```mermaid
sequenceDiagram
    participant User as 用户
    participant Platform as 训练平台
    participant APIServer as K8s API Server
    participant Volcano as Volcano Scheduler
    participant Storage as 分布式存储
    participant NodeA as GPU Node A
    participant NodeB as GPU Node B
    participant NCCL as NCCL 通信
    participant Training as 训练进程

    User->>Platform: 提交训练任务\n模型代码 + 数据集 + 超参
    Platform->>Platform: 任务解析\n生成 Job YAML
    Platform->>Storage: 准备训练数据\n上传 / 确认数据路径
    Platform->>APIServer: 提交 Volcano Job\n256 Workers x 8 GPU

    APIServer->>APIServer: 存入 etcd\nJob 状态: Pending
    APIServer-->>Platform: 返回 Job ID

    Note over Volcano: 调度周期触发

    Volcano->>Volcano: Gang 调度\n检查 2048 GPU 是否可用
    Volcano->>NodeA: 分配 Worker Pod 组
    Volcano->>NodeB: 分配 Worker Pod 组

    NodeA->>Storage: 挂载分布式文件系统
    NodeB->>Storage: 挂载分布式文件系统

    NodeA->>NodeA: Kubelet 拉取镜像\n启动训练容器
    NodeB->>NodeB: Kubelet 拉取镜像\n启动训练容器

    NodeA->>NodeA: Device Plugin 分配 GPU
    NodeB->>NodeB: Device Plugin 分配 GPU

    NodeA->>Training: 启动 torchrun
    NodeB->>Training: 启动 torchrun

    Training->>Training: 初始化进程组\n rendezvous
    Training->>NCCL: 建立 NCCL 通信
    NCCL-->>Training: 通信就绪

    Training->>Storage: 加载 checkpoint 或 初始化模型
    Training->>Training: 开始训练循环

    loop 每个 training step
        Training->>Training: Forward + Backward
        Training->>NCCL: 梯度 AllReduce
        NCCL->>NCCL: 节点内 NVLink + 节点间 IB
        Training->>Training: Optimizer Step
    end

    Training->>Storage: 定期保存 Checkpoint
    Platform->>APIServer: 查询训练状态/日志
```

### 7.2 故障恢复流程

```mermaid
sequenceDiagram
    participant HealthCheck as 健康检查
    participant FailedNode as 故障 Node
    participant APIServer
    participant Volcano
    participant NewNode as 新 Node
    participant Training as 训练进程
    participant Storage

    HealthCheck->>FailedNode: 检测 Node 心跳超时
    FailedNode-->>HealthCheck: 无响应
    HealthCheck->>APIServer: 标记 Node NotReady

    Note over APIServer: 故障 Node 上的 Pod\n状态变为 Unknown/Failed

    APIServer->>Volcano: 通知 Pod 失败
    Volcano->>Volcano: 触发 PodGroup 重调度

    Note over Training: 其他 Worker 检测到 Rank 缺失\nNCCL 通信超时

    Training->>Training: 训练暂停\n报错: Rank not responding

    Note over Storage: 从最新 Checkpoint 恢复

    Volcano->>NewNode: 在健康 Node 上重新调度 Pod
    NewNode->>NewNode: 拉取镜像 + 分配 GPU

    Training->>Storage: 加载最新 Checkpoint
    Training->>Training: 重建进程组
    Training->>Training: 从断点继续训练
```

---

## 8. 典型万卡集群部署架构

### 8.1 物理拓扑与 K8s 映射

```mermaid
graph TB
    subgraph 物理层
        subgraph RowA["Row A - 32 Nodes x 8 GPU = 256 GPU"]
            RASwitch["Spine Switch"]
            RA1["DGX Node 01"]
            RA2["DGX Node 02"]
            RA32["DGX Node 32"]
            RASwitch --- RA1
            RASwitch --- RA2
            RASwitch --- RA32
        end

        subgraph RowB["Row B - 32 Nodes x 8 GPU = 256 GPU"]
            RBSwitch["Spine Switch"]
            RB1["DGX Node 01"]
            RBSwitch --- RB1
        end

        CoreSwitch["Core Switch\nInfiniBand HDR"]
        CoreSwitch --- RASwitch
        CoreSwitch --- RBSwitch
    end

    subgraph K8s映射
        K8sRowA["Node Pool: row-a\nlabels: rack=row-a\nnetwork: ib-subnet-a"]
        K8sRowB["Node Pool: row-b\nlabels: rack=row-b\nnetwork: ib-subnet-b"]
        K8sCore["Taint: dedicated=ml-training"]
    end

    RowA -.->|"映射"| K8sRowA
    RowB -.->|"映射"| K8sRowB
    CoreSwitch -.->|"映射"| K8sCore
```

### 8.2 调度关键配置

| 配置项 | 说明 | 典型值 |
|--------|------|--------|
| `podGroups.minMember` | Gang 调度最小就绪 Pod 数 | 等于总 Worker 数 |
| `scheduler.volcano.sh/preemptable` | 是否允许被抢占 | true/false |
| `queue.capability.gpu` | 队列 GPU 资源上限 | 4096 |
| `nodeSelector.gpu-type` | GPU 型号约束 | a100-80g/h100 |
| `toleration.dedicated` | 专用节点容忍度 | ml-training |
| `priorityClassName` | 优先级 | production/staging/dev |
| `overcommit.gpu` | GPU 超分比 | 1.0 不超分 |

### 8.3 性能优化关键点

| 优化方向 | 具体措施 | 效果 |
|----------|----------|------|
| 调度延迟 | Gang 批量绑定 + 缓存预分配 | 分钟级 -> 秒级 |
| 通信拓扑 | 拓扑感知调度 + NVLink 优先 | 延迟降低 10x |
| 跨站带宽 | 梯度压缩 + 通信重叠计算 | 跨数据中心训练可行 |
| 容错恢复 | 自动 Checkpoint + 快速重调度 | MTBF 小时级 |
| 资源利用率 | 弹性队列 + 细粒度 GPU 分配 | 利用率 > 90% |
| 冷启动 | 镜像预热 + 容器热池 | Pod 启动 < 30s |

---

## 附录：术语表

| 术语 | 全称 | 说明 |
|------|------|------|
| NCCL | NVIDIA Collective Communications Library | NVIDIA 集合通信库 |
| RDMA | Remote Direct Memory Access | 远程直接内存访问 |
| NVLink | NVIDIA NVLink | GPU 间高速互联 |
| IB | InfiniBand | 高性能网络互联 |
| RoCE | RDMA over Converged Ethernet | 以太网上的 RDMA |
| DDP | Distributed Data Parallel | PyTorch 分布式数据并行 |
| FSDP | Fully Sharded Data Parallel | PyTorch 全分片数据并行 |
| ZeRO | Zero Redundancy Optimizer | DeepSpeed 零冗余优化器 |
| TP | Tensor Parallelism | 张量并行 |
| PP | Pipeline Parallelism | 流水线并行 |
| GPUDirect | GPUDirect RDMA | GPU 直接网卡访问 |
| MPI | Message Passing Interface | 消息传递接口 |
