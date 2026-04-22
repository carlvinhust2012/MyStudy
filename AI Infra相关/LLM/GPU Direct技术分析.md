# GPUDirect 直连技术分析

## 一、GPUDirect 技术族

GPUDirect 并不是单一技术，而是一组技术，与 DPU 相关的主要有两个：

```
GPUDirect 家族
├── GPUDirect RDMA      — GPU 与远端 GPU/RDMA 网卡直接数据传输
├── GPUDirect Storage   — GPU 直接访问存储设备（与 DPU 最相关）
└── GPUDirect for Video — GPU 直接处理视频帧（不涉及 DPU）
```

BlueField-4 作为 DPU 主要涉及 **GPUDirect Storage**，即 GPU 绕过 Host CPU 直接读写 NVMe 存储。

## 二、传统路径 vs GPUDirect Storage

### 2.1 传统路径（无 GPUDirect）

```
GPU 发起存储读请求:
  NVMe SSD → DPU → Host DRAM → GPU VRAM
                       ↑
                  CPU 拷贝 (额外开销)
                  额外内存带宽占用
                  延迟: ~50~100us
```

存在 **两次内存拷贝**、CPU 参与、额外延迟和内存带宽浪费。

### 2.2 GPUDirect Storage 路径

```
GPU 发起存储读请求:
  NVMe SSD → DPU → GPU VRAM
                  ↑
             DMA 直传 (零拷贝)
             CPU 不参与
             延迟: ~10~20us
```

**一次拷贝，零 CPU 参与。**

### 2.3 核心区别

| 指标 | 传统路径 (无 GDS) | GPUDirect Storage | 提升幅度 |
|------|------------------|-------------------|---------|
| 数据拷贝次数 | 2 次 | 1 次 | 减少 50% |
| CPU 参与度 | 需 CPU 拷贝 | CPU 零参与 | 释放 CPU |
| 延迟 | ~50~100 us | ~10~20 us | 3~5x 降低 |
| 吞吐 (读) | ~12 GB/s (受限于 DRAM) | ~25 GB/s (PCIe Gen5) | ~2x |
| 吞吐 (写) | ~12 GB/s | ~25 GB/s | ~2x |
| Host DRAM 占用 | 需要 DRAM 缓冲 | 不需要 | 节省 DRAM |
| GPU 利用率 | 等待数据拷贝 | 减少等待 | 训练速度提升 |

> 实际性能取决于 PCIe 拓扑、GPU 型号、NVMe 设备性能等因素。

## 三、核心实现机制

### 3.1 三层硬件架构

```
+--------------------------------------------------------------+
|                        Host Server                           |
|                                                              |
|  +------------------+     PCIe Gen6 (Upstream)               |
|  | GPU              | <====================>  +-------------+ |
|  | +-------------+  |                         | BF4 DPU     | |
|  | | CUDA Core   |  |                         |             | |
|  | |             |  |                         | ARM Cores   | |
|  | +-------------+  |                         | SPDK        | |
|  | +-------------+  |    PCIe P2P DMA         | NVMe-oF     | |
|  | | GPU VRAM    |  | <==================>    | Target      | |
|  | | (HBM2/HBM3) |  |    (绕过 Host DRAM)     |             | |
|  | +-------------+  |                         | RDMA Engine | |
|  +------------------+                         +------+------+ |
|                                                 |            | |
|  +------------------+                          |  NVMe      | |
|  | Host DRAM        |                          |  Controller| |
|  | (系统内存)       |                          +------+-----+ |
|  +------------------+                                  |       |
+----------------------------------------------------------|-------+
                                                    PCIe Down (Gen6)
                                                           |
                                                    +------v------+
                                                    | NVMe SSD    |
                                                    | (本地存储)   |
                                                    +-------------+
```

### 3.2 关键硬件前提

| 组件 | 所需能力 | 说明 |
|------|---------|------|
| **GPU** | 支持 GPUDirect Storage | A100/H100/B200 等 |
| **DPU** | PCIe ATS + P2P DMA | BlueField-3/4 均支持 |
| **PCIe 拓扑** | GPU 和 DPU 在同一 PCIe 交换域内 | 同一 Root Complex |
| **IOMMU/SMMU** | 支持 PASID、地址转换 | GPU 和 DPU 共享地址空间 |

核心硬件能力是 **PCIe ATS (Address Translation Services)** 和 **P2P (Peer-to-Peer) DMA：

```
PCIe ATS 机制:
+---------------+     翻译请求 (ATS)      +---------------+
| GPU (Requester| =======================> | IOMMU/SMMU    |
|  发起 DMA)    |     翻译响应 (Cache)     | (地址翻译)     |
|               | <======================= |               |
+---------------+                          +---------------+
       |                                        |
       | DMA 读写 (使用翻译后的物理地址)          |
       v                                        |
+---------------+     物理地址 DMA        +---------------+
| DPU           | <=====================>  | NVMe SSD      |
| (NVMe Ctrl)   |                         | (存储设备)     |
+---------------+                         +---------------+
```

### 3.3 关键机制：PRP/SGL 指向 GPU VRAM

```
传统 NVMe 读:
  NVMe 命令:
    PRP1 = Host DRAM 物理地址 (如 0x7F000000)
    → NVMe SSD DMA → Host DRAM
    → CPU memcpy → GPU VRAM

GPUDirect Storage NVMe 读:
  NVMe 命令:
    PRP1 = GPU VRAM 物理地址 (如 0xC000_0000_0000)
    → NVMe SSD DMA → GPU VRAM (直接!)
```

核心区别就是 **NVMe 命令中的数据缓冲区地址从 Host DRAM 改为了 GPU VRAM 物理地址**，
DPU 的 DMA 引擎通过 PCIe P2P 直接访问 GPU 显存。

## 四、软件栈实现

### 4.1 驱动与库层次

```
+================================================================+
|                    用户空间 (User Space)                          |
|                                                                 |
|  +----------------------------------------------------------+  |
|  | AI/ML 应用                                                |  |
|  | (PyTorch / TensorFlow / Megatron / DeepSpeed)             |  |
|  +----------------------------------------------------------+  |
|       |                    |                    |              |
|  +----v------+      +-----v------+      +------v-------+      |
|  | cuFile API |      | CUDA API   |      | NCCL         |     |
|  | (GDS 文件  |      | (GPU 内存   |      | (GPU 通信)   |     |
|  |  操作)     |      |  管理)      |      |              |     |
|  +----+------+      +-----+------+      +------+-------+     |
|       |                    |                    |              |
+================================================================+
|                    内核空间 (Kernel Space)                        |
|                                                                 |
|  +----v------+      +-----v------+      +------v-------+       |
|  | nvidia-fs  |      | NVIDIA     |      | nvme         |      |
|  | (GDS 内核  |      | GPU Driver |      | (NVMe 核心)  |      |
|  |  模块)     |      |            |      |              |      |
|  +----+------+      +-----+------+      +------+-------+       |
|       |                    |                    |               |
+================================================================+
|                    硬件层 (Hardware)                              |
|                                                                 |
|  +----v------+      +-----v------+      +------v-------+       |
|  | GPU        |      | IOMMU/SMMU |      | DPU (BF4)    |      |
|  | (HBM VRAM) | <--> | (地址翻译)  | <--> | NVMe Engine  |     |
|  +-------------+      +------------+      +--------------+       |
+================================================================+
```

### 4.2 核心组件：nvidia-fs 驱动

```
nvidia-fs 是 GPUDirect Storage 的核心内核模块

职责:
  1. GPU VRAM 的 DMA 映射管理
  2. 与 NVMe 驱动协作, 建立直连路径
  3. I/O 请求的调度和转发
  4. 内存屏障和同步
```

### 4.3 cuFile API

```c
// cuFile 是 NVIDIA 提供的 GPUDirect Storage 用户态 API

// 1. 初始化
cuFileDriverOpen();
cuFileDriverSetProperties(&props);

// 2. 注册 GPU 内存 (用于 DMA)
CUdeviceptr d_buf;
cudaMalloc(&d_buf, size);
cuFileBufRegister(d_buf, size, CU_FILE_BUF_SIZE);

// 3. 直接从文件读到 GPU VRAM (零拷贝)
cuFileRead(cufile_handle, d_buf, size, file_offset, 0);
//   数据路径: NVMe SSD → DPU → GPU VRAM (DMA, 零 CPU 拷贝)

// 4. 直接从 GPU VRAM 写入文件 (零拷贝)
cuFileWrite(cufile_handle, d_buf, size, file_offset, 0);
//   数据路径: GPU VRAM → DPU → NVMe SSD (DMA, 零 CPU 拷贝)

// 5. 清理
cuFileBufDeregister(d_buf);
cuFileDriverClose();
```

## 五、数据流详细路径

### 5.1 读操作完整流程

```
  AI 应用调用 cuFileRead(handle, gpu_buf, size, offset)
  |
  v
+-- [1] cuFile 用户态库
|     +-- 检查 gpu_buf 是否已注册 (DMA 可用)
|     +-- 构造 cuFileRead 请求
|     +-- ioctl(nvidia-fs, READ) 下发到内核
|
v
+-- [2] nvidia-fs 内核模块
|     +-- 通过 NVIDIA GPU 驱动获取 gpu_buf 的物理地址
|     |   (GPU VRAM → 物理地址映射, 通过 ATS/SMMU)
|     +-- 构造 NVMe 读取命令:
|     |   - CID (Command ID)
|     |   - LBA (起始扇区)
|     |   - NLB (扇区数)
|     |   - PRP1/PRP2 = GPU VRAM 物理地址 (不是 Host DRAM!)
|     +-- 提交到 NVMe 子系统
|
v
+-- [3] NVMe 核心 (nvme_core)
|     +-- 路由到对应的 NVMe 控制器
|     +-- 将命令提交到 NVMe SQ (Submission Queue)
|
v
+-- [4] DPU (BlueField-4) 端处理
|     +-- [路径 A] 本地 NVMe (PCIe Downstream):
|     |     DPU NVMe Controller
|     |     → 从下游 NVMe SSD 读取数据
|     |     → 通过 PCIe Upstream DMA 直接写入 GPU VRAM
|     |     → 完成后通过 MSI 通知 Host
|     |
|     +-- [路径 B] NVMe-oF (远程存储):
|           DPU NVMe-oF Target
|           → 通过 RoCE RDMA 从远端 NVMe-oF 服务器接收数据
|           → RDMA 引擎将数据 DMA 直接写入 GPU VRAM
|           → 完成后通知 Host
|
v
+-- [5] 完成回调
      NVMe CQ (Completion Queue) 条目产生
      → nvidia-fs 收到完成通知
      → 唤醒 cuFileRead 调用
      → AI 应用继续使用 GPU VRAM 中的数据
```

### 5.2 写操作流程（方向相反）

```
  cuFileWrite(handle, gpu_buf, size, offset)
  |
  +-- 数据路径: GPU VRAM → (PCIe P2P DMA) → DPU → NVMe SSD
  +-- Host CPU 不参与数据搬运
```

## 六、内存地址转换机制

GPU VRAM 使用的是 GPU 的虚拟地址空间，需要经过多层转换才能被 DPU 的 DMA 引擎使用：

```
  GPU 虚拟地址 (cudaMalloc 返回)
       |
       v
  GPU 页表 (GPU MMU) ──────→ GPU VRAM 物理地址 (IOMMU 格式)
                                          |
                                          v
                                   IOMMU/SMMU 翻译
                                    (ATS 机制)
                                          |
                                          v
                                   PCIe 物理地址
                                          |
                                          v
                               DPU DMA 引擎可直接使用
```

### 6.1 ATS (Address Translation Services) 工作流

```
步骤 1: GPU 驱动注册 VRAM 区域
====================================
  cuFileBufRegister(gpu_buf, size)
    → 调用 NVIDIA GPU 驱动
    → GPU 驱动将 VRAM 物理页注册到 IOMMU
    → 返回 IOMMU 可用的地址范围给 DPU

步骤 2: DPU 发起 ATS 翻译请求
====================================
  DPU DMA 引擎需要访问 GPU VRAM:
    → 发送 ATS Translation Request (通过 PCIe TLP)
    → IOMMU 翻译 GPU 地址 → 系统物理地址
    → 返回 Translation Complete (带缓存)

步骤 3: DPU 执行 DMA 传输
====================================
  DPU 使用翻译后的物理地址:
    → DMA Read/Write 直接访问 GPU VRAM
    → PCIe Gen6 x16 带宽: ~256 GB/s (理论)
```

## 七、DPU 在 GPUDirect 中的三种角色

### 7.1 角色 1：本地存储网关 (NVMe-oF Target)

DPU 作为存储服务端，通过 NVMe-oF 向远程 GPU 服务器提供存储。

```
远程 AI 服务器:
+------------------+     800GbE RoCE      +------------------+
| GPU              | <==================> | DPU (BlueField)  |
| +-------------+  |                      | +-------------+ |
| | CUDA Core   |  |   NVMe-oF 协议       | | SPDK        | |
| +-------------+  |   RDMA 直传到        | | NVMe-oF     | |
| | GPU VRAM    |←-|←--- GPU VRAM         | | Target      | |
| +-------------+  |                      | +------+------+ |
+------------------+                       +------+--------+
                                                    |
                                               NVMe SSD 阵列

数据路径: GPU VRAM ←(RoCE RDMA)← DPU ←(NVMe)← SSD
```

### 7.2 角色 2：远程存储客户端 (NVMe-oF Initiator)

DPU 作为存储客户端，代表本地 GPU 访问远程存储。

```
本地 AI 服务器:
+------------------+     PCIe            +------------------+
| GPU              | <==================> | DPU (BlueField)  |
| +-------------+  |                      | +-------------+ |
| | CUDA Core   |  |   PCIe P2P DMA      | | SPDK        | |
| +-------------+  |   直接到 GPU VRAM    | | NVMe-oF     | |
| | GPU VRAM    |←-|←--------------------| | Initiator   | |
| +-------------+  |                      | +------+------+ |
+------------------+                       +------+--------+
                                                    |
                                              800GbE RoCE
                                                    |
                                               远端存储集群
```

### 7.3 角色 3：GPUDirect RDMA 中继

DPU 作为 RDMA 网桥，连接不同 GPU 服务器之间的直连通信。

```
+------------------+     PCIe P2P      +------------------+
| GPU Server 0     | <=============>   | DPU              |
| GPU 0            |    DMA 直传        | RDMA/RoCE Engine |
+------------------+                   +--------+---------+
                                               |
                                          800GbE RoCE
                                               |
+------------------+     PCIe P2P      +------v----------+
| GPU Server 1     | <=============>   | DPU              |
| GPU 1            |    DMA 直传        | RDMA/RoCE Engine |
+------------------+                   +------------------+

GPU 间通信: GPU 0 ←(P2P DMA)→ DPU ←(RoCE)→ DPU ←(P2P DMA)→ GPU 1
```

## 八、部署要求与限制

### 8.1 硬件要求

```
必需:
  [✓] NVIDIA A100 / H100 / B200 等 (支持 GPUDirect Storage)
  [✓] BlueField-3 / BlueField-4 DPU
  [✓] GPU 和 DPU 在同一 PCIe Root Complex 下

支持的拓扑:
  ┌─────────┐
  │ PCIe RC │ (Root Complex)
  └────┬────┘
    ┌──┴──┐
    │ PCIe│  ← GPU 和 DPU 必须在同一交换域
    │Switch│
    └──┬──┘
   ┌──┴──────┐
   │    │    │
  GPU  DPU  NIC

不支持的拓扑:
  GPU ←→ CPU Socket 0  (Root Complex 0)
  DPU ←→ CPU Socket 1  (Root Complex 1)  ← 跨 Socket, P2P 不支持或极慢
```

### 8.2 软件要求

```
必需组件:
  - Linux Kernel 5.10+
  - NVIDIA GPU Driver (525+)
  - nvidia-fs 内核模块 (GPUDirect Storage 驱动)
  - CUDA Toolkit 12.0+ (cuFile 库)
  - DOCA SDK (BlueField-4 上)
  - SPDK (NVMe-oF Target)
```

### 8.3 已知限制

| 限制 | 说明 |
|------|------|
| I/O 大小 | 小 I/O (<4KB) 无法体现优势，建议 128KB+ |
| 内存注册 | GPU VRAM 需预先注册 cuFileBufRegister |
| PCIe 拓扑 | 跨 NUMA/Socket 的 P2P 性能极差 |
| GPU 计算重叠 | DMA 传输期间不能使用该 VRAM 区域 |
| 错误恢复 | GPU 页错误处理复杂，需完善错误路径 |

## 九、参考代码

```c
#include <cuda.h>
#include <cufile.h>

#define FILE_SIZE  (256 * 1024 * 1024)  // 256MB
#define IO_SIZE    (1024 * 1024)        // 1MB 每次 IO

int main() {
    // 1. 初始化 cuFile
    cuFileDriverOpen();
    cuFileDriverSetProps(&(CUfileDrvProps_t){
        .version = 1,
        .flags   = CU_FILE_DEFAULT,
    });

    // 2. 分配 GPU 内存
    CUdeviceptr d_buf;
    cudaMalloc(&d_buf, FILE_SIZE);

    // 3. 注册 GPU 内存为 DMA 可用
    cuFileBufRegister(d_buf, FILE_SIZE, CU_FILE_BUF_SIZE);

    // 4. 打开文件 (cuFile 句柄)
    CUfileDescr_t desc = {
        .type = CU_FILE_HANDLE_TYPE_OPAQUE_FD,
        .handle.fd = open("/mnt/gds/data.bin", O_RDWR | O_DIRECT),
    };
    CUfileHandle_t cf_handle;
    cuFileHandleRegister(&cf_handle, &desc);

    // 5. 直接读取数据到 GPU VRAM (零拷贝!)
    //    路径: NVMe SSD → DPU → GPU VRAM (DMA 直传)
    cuFileRead(cf_handle, (void *)d_buf, IO_SIZE, 0, 0);

    // 6. GPU 可以直接使用数据 (已在 VRAM 中)
    my_kernel<<<blocks, threads>>>((float *)d_buf);

    // 7. 将处理结果写回存储 (零拷贝!)
    //    路径: GPU VRAM → DPU → NVMe SSD (DMA 直传)
    cuFileWrite(cf_handle, (void *)d_buf, IO_SIZE, 0, 0);

    // 8. 清理
    cuFileHandleDeregister(cf_handle);
    cuFileBufDeregister(d_buf);
    cudaFree((void *)d_buf);
    cuFileDriverClose();

    return 0;
}
```

## 十、开发资源

| 资源 | 链接 |
|------|------|
| GPUDirect Storage 文档 | https://docs.nvidia.com/gpudirect-storage/ |
| cuFile API 文档 | https://docs.nvidia.com/cuda/cufile/ |
| DOCA SDK 文档 | https://docs.nvidia.com/doca/ |
| SPDK 官网 | https://spdk.io/ |
| NVIDIA BlueField 产品页 | https://www.nvidia.com/en-us/networking/products/bluefield-4-dpu/ |
