# NVMe 协议控制流程分析

---

## 1. NVMe 协议概述

NVMe (Non-Volatile Memory Express) 是面向 PCIe SSD 的高性能存储协议，核心设计目标是**充分利用 PCIe 带宽、降低 I/O 延迟**。

### 1.1 核心架构

```
┌─────────────────────────────────────────────────────────────────────┐
│                          NVMe 系统架构                               │
│                                                                      │
│  ┌────────────┐     ┌────────────────────────────────────────────┐  │
│  │   Host     │     │              NVMe Controller               │  │
│  │  (主机端)  │     │               (控制器端)                    │  │
│  │            │     │                                            │  │
│  │ ┌────────┐ │     │ ┌──────────────┐  ┌──────────────────────┐ │  │
│  │ │ NVMe   │ │     │ │  Admin Queue │  │     I/O Queues       │ │  │
│  │ │ Driver │ │     │ │  (SQ0/CQ0)   │  │  (SQ1~N/CQ1~N)     │ │  │
│  │ └───┬────┘ │     │ └──────┬───────┘  └──────────┬───────────┘ │  │
│  │     │      │     │        │                      │             │  │
│  │ ┌───▼────┐ │     │ ┌──────▼──────────────────────▼───────────┐ │  │
│  │ │ SQ/CQ  │◄├─────┤ │            Command Processor            │ │  │
│  │ │ Pairs  │ │PCIe │ └────────────────────┬──────────────────┘ │  │
│  │ └───┬────┘ │     │                      │                    │  │
│  │     │      │     │ ┌────────────────────▼──────────────────┐ │  │
│  │ ┌───▼────┐ │     │ │         NVMe Subsystem                │ │  │
│  │ │ Door-  │ │     │ │  ┌─────────┐ ┌──────────┐ ┌────────┐ │ │  │
│  │ │ bell   │ │     │ │  │   NS 0  │ │   NS 1   │ │  NS N  │ │ │  │
│  │ │ Ring   │ │     │ │  │(Namespace)│ │(Namespace)│ │(...)   │ │ │  │
│  │ └────────┘ │     │ │  └─────────┘ └──────────┘ └────────┘ │ │  │
│  └────────────┘     │ └───────────────────────────────────────┘ │  │
│                      └────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────┘
```

### 1.2 关键概念

| 概念 | 说明 |
|------|------|
| **SQ (Submission Queue)** | 提交队列，Host 将命令写入此队列，最多 64K 条 |
| **CQ (Completion Queue)** | 完成队列，Controller 将命令完成状态写入此队列 |
| **Admin SQ/CQ** | 管理队列（SQ0/CQ0），用于控制器管理命令，系统启动时必须存在 |
| **I/O SQ/CQ** | I/O 队列，用于数据读写，可动态创建/删除，最多 64K 对 |
| **Doorbell** | 门铃寄存器，Host 写入 DB 通知 Controller 有新命令 |
| **Namespace (NS)** | 命名空间，逻辑上的存储单元，类似 LUN |
| **PRP/SGL** | Physical Region Page / Scatter Gather List，描述数据缓冲区的物理地址 |

---

## 2. NVMe 初始化流程

系统上电后，Host 需要经过一系列步骤完成 NVMe 控制器的发现与初始化。

```mermaid
sequenceDiagram
    participant BIOS as BIOS/UEFI
    participant PCI as PCIe Bus
    participant Host as NVMe Driver
    participant Ctrl as NVMe Controller

    Note over BIOS,Ctrl: Phase 1: PCIe 枚举

    BIOS->>PCI: PCIe 枚举扫描
    PCI-->>BIOS: 发现 NVMe 设备 (Vendor ID + Device ID)
    BIOS->>PCI: 分配 BAR 空间 (MMIO)
    BIOS->>PCI: 总线主控使能 (Bus Master Enable)

    Note over BIOS,Ctrl: Phase 2: 控制器使能

    BIOS->>Ctrl: 写入 NVME_CC.EN = 0 (确保禁用)
    BIOS->>Ctrl: 设置 NVME_CC.AMS (Arbitration Mechanism)
    BIOS->>Ctrl: 设置 NVME_CC.MPS (Memory Page Size)
    BIOS->>Ctrl: 设置 NVME_CC.CSS = 0 (标准 I/O 命令集)
    BIOS->>Ctrl: 写入 NVME_CC.EN = 1 (使能控制器)

    Note over BIOS,Ctrl: Phase 3: 控制器就绪

    Ctrl-->>Host: 设置 NVME_CSTS.RDY = 1 (Ready)
    Host->>Ctrl: 读取 NVME_CAP (控制器能力)
    Host->>Ctrl: 读取 NVME_VS (版本号)

    Note over BIOS,Ctrl: Phase 4: Admin Queue 创建

    Host->>Host: 分配 Admin SQ (SQ0) 和 Admin CQ (CQ0) 内存
    Host->>Ctrl: Admin CMD: Set Features (Number of Queues)
    Host->>Ctrl: Admin CMD: Create I/O Completion Queue (CQ1)
    Host->>Ctrl: Admin CMD: Create I/O Submission Queue (SQ1, 关联 CQ1)
    Host->>Ctrl: Admin CMD: Identify (Controller)
    Ctrl-->>Host: 返回 Identify Controller 数据
    Host->>Ctrl: Admin CMD: Identify (Namespace List)
    Ctrl-->>Host: 返回 Namespace 列表

    Note over Host,Ctrl: 初始化完成, 可以开始 I/O
```

### 2.1 初始化关键寄存器

```
NVMe Controller 寄存器 (PCIe BAR 空间内):

  偏移      寄存器         作用
  ─────────────────────────────────────────
  0x0000    NVME_CAP       控制器能力 (最大队列深度, MPS 支持等)
  0x0014    NVME_VS        版本号
  0x0018    NVME_INTMS     中断掩码设置
  0x001C    NVME_INTMC     中断掩码清除
  0x0020    NVME_CC        控制器配置 (EN, MPS, AMS, CSS, IOCQES, IOSQES)
  0x0024    NVME_CSTS      控制器状态 (RDY, SHST, CFS)
  0x0028    NVME_NSSR      NVM Subsystem Reset
  0x0030    NVME_AQA       Admin Queue 属性 (SQ/CQ 大小)
  0x0038    NVME_ASQ       Admin SQ 物理基地址
  0x0040    NVME_ACQ       Admin CQ 物理基地址
  0x1000+   Doorbell       门铃寄存器 (每对 SQ/CQ 一个)
```

---

## 3. 标准 I/O 读写流程

NVMe I/O 的核心是 **基于 SQ/CQ 的门铃通知机制**，避免了传统 SCSI 协议的多次握手开销。

```mermaid
sequenceDiagram
    participant App as 应用程序
    participant FS as 文件系统<br/>(Block Layer)
    participant Drv as NVMe Driver
    participant Mem as Host Memory<br/>(DMA)
    participant DB as Doorbell<br/>Registers
    participant Ctrl as NVMe Controller
    participant NS as NAND Flash<br/>(Namespace)

    Note over App,NS: ========== I/O 读操作 (Read) ==========

    App->>FS: read(fd, buf, len)
    FS->>FS: 计算逻辑块地址 (LBA)
    FS->>Drv: submit_bio() (提交 bio)

    Note over Drv: Step 1: 构造命令

    Drv->>Mem: 构造 NVMe Command (64B) 写入 SQ Tail 槽位
    Note right of Mem: Opcode = 0x02 (Read)<br/>NSID = Namespace ID<br/>SLBA = 起始 LBA<br/>NLB = 块数量<br/>PRP1 = buf 物理地址<br/>PRP2 = 连续物理页地址 (如需)

    Drv->>Drv: SQ Tail++
    Drv->>DB: 写 Doorbell (SQ.Tail = new_tail)
    Note right of DB: MMIO 写入, 触发通知

    Note over DB,Ctrl: Step 2: 控制器处理

    DB-->>Ctrl: Controller 检测到 SQ Tail 变化 (轮询/中断)
    Ctrl->>Ctrl: 从 SQ 取出 Command
    Ctrl->>Ctrl: 解析命令, 校验 PRP 地址
    Ctrl->>NS: 从 NAND 读取数据

    Note over Ctrl,Mem: Step 3: DMA 数据传输

    NS-->>Ctrl: 返回 NAND 数据
    Ctrl->>Mem: DMA Write: 数据直接写入 Host buf (PRP 指向的内存)
    Note right of Mem: 数据直接 DMA 到用户缓冲区,<br/>无需 Controller 中转

    Note over Ctrl,DB: Step 4: 命令完成

    Ctrl->>Ctrl: 构造 CQE (16B), 写入 CQ Head 槽位
    Note right of Ctrl: CQE 包含:<br/>SQ Head (标识完成的是哪条命令)<br/>Status Field (成功/失败)<br/>CID (Command ID)
    Ctrl->>Drv: 发送 MSI-X 中断 (或 Host 轮询 CQ)

    Note over Drv,App: Step 5: 完成处理

    Drv->>Mem: 读取 CQE, 检查 Status
    Drv->>Drv: CQ Head++
    Drv->>DB: 写 Doorbell (CQ.Head = new_head)
    Drv->>Drv: 回收 SQ 槽位 (SQ Head++)
    Drv->>FS: 通知 I/O 完成 (完成回调/中断上下文)
    FS->>App: read() 返回, 数据在 buf 中
```

### 3.1 I/O 写操作流程

写操作与读操作类似，主要区别在于数据传输方向相反：

```mermaid
sequenceDiagram
    participant App as 应用程序
    participant Drv as NVMe Driver
    participant Mem as Host Memory
    participant Ctrl as NVMe Controller
    participant NS as NAND Flash

    App->>Drv: write() → submit_bio()
    Drv->>Mem: 构造 NVMe Write Command (Opcode=0x01) 写入 SQ
    Drv->>Mem: 将用户数据拷贝到 DMA 缓冲区 (PRP 指向)
    Drv->>Drv: 更新 SQ Tail, 写 Doorbell

    Ctrl->>Mem: DMA Read: 从 Host 内存读取数据
    Ctrl->>NS: 将数据写入 NAND Flash
    NS-->>Ctrl: 写入完成 (含可能的 GC/磨损均衡)

    Ctrl->>Ctrl: 写 CQE, 发送中断
    Drv->>Drv: 处理完成, 释放资源
    Drv-->>App: write() 返回
```

---

## 4. SQ/CQ 与门铃机制详解

### 4.1 队列数据结构

```
每个 Submission Queue Entry (SQE) = 64 Bytes:

┌──────────────────────────────────────────────────────────────┐
│  Byte 0-3:   CDW0                                            │
│    [7:0]     OPC  - 操作码 (Read=0x02, Write=0x01, ...)      │
│    [15:8]    FUSE - Fused Operation                           │
│    [23:16]   Reserved                                         │
│    [31:24]   CID  - Command ID (由 Host 分配, 用于匹配完成)    │
│                                                              │
│  Byte 4-7:   NSID - Namespace ID                             │
│  Byte 8-15:  Reserved                                        │
│  Byte 16-19: PRP1 - Physical Region Page 1 (数据缓冲区地址)    │
│  Byte 20-23: PRP2 - Physical Region Page 2 (连续页链表)       │
│  Byte 24-39: Reserved                                        │
│  Byte 40-43: CDW10 - 命令特有参数 (Read: 起始 LBA 低32位)      │
│  Byte 44-47: CDW11 - 命令特有参数 (Read: LBA 高32位 + 块数)    │
│  Byte 48-63: CDW12~15 - 命令特有参数                          │
└──────────────────────────────────────────────────────────────┘


每个 Completion Queue Entry (CQE) = 16 Bytes:

┌──────────────────────────────────────────────────────────────┐
│  Byte 0-2:   DW0 - 命令特有返回值                              │
│  Byte 3:     SF - Submission Queue Head pointer (高16位)       │
│  Byte 4-7:   SQHD - SQ Head (低16位) + SQ Identifier          │
│  Byte 8-11:  CID - Command ID (匹配 SQE 中的 CID)             │
│  Byte 12-15: Status                                          │
│    [0]       P    - Phase Tag (阶段翻转, 用于检测新 CQE)       │
│    [1]       SC   - Status Code                               │
│    [7:2]     SCT  - Status Code Type                          │
│    [15:8]    Reserved                                          │
└──────────────────────────────────────────────────────────────┘
```

### 4.2 Doorbell 通知时序

```
时间线 ──────────────────────────────────────────────────────────►

Host 端                          Controller 端
───────                          ──────────────

  ① 构造 SQE, 写入 SQ[Tail]
  ② SQ.Tail++
  ③ MMIO Write: Doorbell[SQn]
       │
       ├──────────────────────────► ④ 检测 Doorbell 更新
       │                           ⑤ 读取 SQ[Head..Tail-1]
       │                           ⑥ 执行命令
       │                           ⑦ DMA 数据传输
       │                           ⑧ 写 CQE 到 CQ[CQ.Tail]
       │                           ⑨ CQ.Tail++
       │  ◄── MSI-X 中断 ──────── ⑩ 发送中断
  ⑪ 读取 CQ[Head..Tail-1]
  ⑫ CQ.Head++
  ⑬ MMIO Write: Doorbell[CQn]
       │
       ├──────────────────────────► ⑭ 确认完成, 更新 SQ Head
       │                           ⑮ 回收 SQ 槽位
```

---

## 5. Admin 命令控制流程

Admin 命令通过专用的 Admin Queue (SQ0/CQ0) 发送，用于控制器管理。

```mermaid
sequenceDiagram
    participant Host as NVMe Driver
    participant ASQ as Admin SQ<br/>(SQ0)
    participant Ctrl as NVMe Controller
    participant ACQ as Admin CQ<br/>(CQ0)

    Note over Host,ACQ: ── Identify Controller ──

    Host->>ASQ: 写入 Identify CMD (OPC=0x06, CNS=0x01)
    Host->>Ctrl: Doorbell[SQ0] = Tail
    Ctrl->>Ctrl: 读取 Identify 数据结构 (4KB)
    Ctrl->>ACQ: 写 CQE (数据通过 DMA 返回)
    ACQ-->>Host: 中断通知
    Host->>Host: 解析 Identify Controller

    Note over Host,ACQ: ── Create I/O Queue ──

    Host->>ASQ: Create I/O CQ (OPC=0x05, QID=1, QSize=256)
    Host->>Ctrl: Doorbell[SQ0]
    Ctrl->>Ctrl: 分配 CQ1 资源, 配置中断向量
    Ctrl->>ACQ: 写 CQE (成功)

    Host->>ASQ: Create I/O SQ (OPC=0x01, QID=1, CQID=1, QSize=256)
    Host->>Ctrl: Doorbell[SQ0]
    Ctrl->>Ctrl: 分配 SQ1 资源, 关联 CQ1
    Ctrl->>ACQ: 写 CQE (成功)

    Note over Host,ACQ: ── Set Features ──

    Host->>ASQ: Set Features (OPC=0x09, FID=Number of Queues)
    Note right of ASQ: 协商 I/O 队列数量
    Host->>Ctrl: Doorbell[SQ0]
    Ctrl->>ACQ: 写 CQE (返回实际支持的队列数)

    Note over Host,ACQ: ── Get Log Page ──

    Host->>ASQ: Get Log Page (OPC=0x02, LID=Error Log)
    Host->>Ctrl: Doorbell[SQ0]
    Ctrl->>ACQ: 写 CQE + DMA 返回日志数据
```

### 5.1 主要 Admin 命令列表

| Opcode | 命令 | 功能 |
|--------|------|------|
| 0x01 | Create I/O SQ | 创建 I/O 提交队列 |
| 0x04 | Delete I/O SQ | 删除 I/O 提交队列 |
| 0x05 | Create I/O CQ | 创建 I/O 完成队列 |
| 0x08 | Delete I/O CQ | 删除 I/O 完成队列 |
| 0x06 | Identify | 获取控制器/Namespace 信息 |
| 0x09 | Set Features | 设置控制器特性 |
| 0x0A | Get Features | 获取控制器特性 |
| 0x02 | Get Log Page | 获取错误日志/SMART 信息 |
| 0x0C | Abort | 中止指定命令 |
| 0x08 | Firmware Commit | 提交固件更新 |
| 0x10 | Firmware Image Download | 下载固件镜像 |
| 0xFF | Namespace Management | Namespace Attach/Detach |
| 0x0E | Format NVM | 格式化 NVM (安全擦除) |

---

## 6. 中断处理流程

NVMe 支持 MSI-X 中断机制，每个 CQ 可独立绑定中断向量。

```mermaid
sequenceDiagram
    participant Ctrl as NVMe Controller
    participant PCIe as PCIe MSI-X
    participant CPU as CPU<br/>(Interrupt Handler)
    participant Drv as NVMe Driver
    participant CQ as Completion Queue

    Note over Ctrl,CQ: 方式 A: 中断驱动 (Interrupt-driven)

    Ctrl->>CQ: 写入 CQE, CQ.Tail++
    Ctrl->>PCIe: 触发 MSI-X 中断 (Vector = CQ 绑定向量)
    PCIe->>CPU: 中断投递到指定 CPU 核
    CPU->>Drv: 调用 IRQ Handler
    Drv->>CQ: 读取 CQ[Head..Tail-1], 批量处理所有 CQE
    Drv->>Drv: 根据 CQE.SQID + CID 找到对应 SQE, 执行完成回调
    Drv->>Ctrl: Doorbell[CQ] = Head (通知 Controller, 回收资源)

    Note over Ctrl,CQ: 方式 B: 轮询模式 (Polling)

    loop 每隔一定间隔 或 在提交命令后主动轮询
        Drv->>CQ: 检查 CQ[Head] 的 Phase Tag
        alt Phase Tag 翻转 (有新 CQE)
            Drv->>Drv: 处理 CQE
            Drv->>Ctrl: Doorbell[CQ] = Head
        else Phase Tag 未翻转 (无新完成)
            Drv->>Drv: 跳过, 继续其他工作
        end
    end

    Note over Ctrl,CQ: 方式 C: 混合模式 (Hybrid, NVMe 1.x+)

    Note right of Drv: 低负载: 使用中断, 减少 CPU 占用<br/>高负载: 自动切换到轮询, 降低延迟<br/>利用 IRQ 退出轮询, 避免空转
```

### 6.1 中断合并与亲和性

```
┌──────────────────────────────────────────────────────────────────┐
│                      中断优化策略                                  │
│                                                                   │
│  ① Interrupt Coalescing (中断合并):                                │
│     ─────────────────────────────────                              │
│     Controller 不每完成一个命令就中断                               │
│     而是等待多个命令完成 或 超时后批量通知                           │
│     Set Features: Interrupt Coalescing                            │
│     - AGG: 中断聚合时间阈值 (单位 100us)                           │
│     - THR: 中断聚合命令数阈值                                      │
│                                                                   │
│  ② Interrupt Vector Affinity (中断亲和性):                         │
│     ─────────────────────────────────                              │
│     每个 CQ 绑定一个 MSI-X 向量                                    │
│     操作系统将向量亲和到特定 CPU 核                                 │
│     实现多队列的 CPU 亲和性调度                                     │
│                                                                   │
│     CQ1 → MSI-X Vector 1 → CPU Core 0                             │
│     CQ2 → MSI-X Vector 2 → CPU Core 1                             │
│     CQ3 → MSI-X Vector 3 → CPU Core 2                             │
│     ...                                                            │
│                                                                   │
│  ③ 多队列减少锁竞争:                                               │
│     ─────────────────────────────────                              │
│     不同 CPU 核使用不同的 SQ/CQ                                    │
│     无需共享队列锁, 消除并发瓶颈                                    │
└──────────────────────────────────────────────────────────────────┘
```

---

## 7. Namespace 管理流程

Namespace 是 NVMe 中的逻辑存储单元，支持动态创建、附加、分离和删除。

```mermaid
sequenceDiagram
    participant Host as NVMe Driver
    participant ASQ as Admin SQ
    participant Ctrl as NVMe Controller
    participant NS as Namespaces

    Note over Host,NS: ── Namespace 附加/分离 ──

    Host->>ASQ: Namespace Attachment (OPC=0x15)
    Note right of ASQ: SEL=0x01 (Attach)<br/>NSID=0xFFFFFFFF (全部)
    Host->>Ctrl: Doorbell[SQ0]
    Ctrl->>NS: 将指定 NS 附加到当前 Controller
    Ctrl-->>Host: CQE: 成功

    Note over Host,NS: ── Namespace 管理 ──

    Host->>ASQ: Namespace Management (OPC=0x0D, NSID=0)
    Note right of ASQ: SEL=0x01 (Create)<br/>NSZE = 容量大小
    Host->>Ctrl: Doorbell[SQ0]
    Ctrl->>Ctrl: 分配 NAND 空间, 创建 NS
    Ctrl->>NS: 新 NS 加入 Subsystem
    Ctrl-->>Host: CQE: 返回新 NSID

    Note over Host,NS: ── 识别 Namespace ──

    Host->>ASQ: Identify (OPC=0x06, CNS=0x00)
    Note right of ASQ: NSID = 指定 Namespace
    Host->>Ctrl: Doorbell[SQ0]
    Ctrl-->>Host: DMA 返回 Identify Namespace 数据 (4KB)
    Note right of Host: 包含: LBA Size, Capacity,<br/>FLBAS, MC, DPS 等
```

---

## 8. 坚固状态电源管理 (PSM) 流程

NVMe 支持通过 Power State Management 实现电源状态切换。

```mermaid
sequenceDiagram
    participant OS as 操作系统
    participant Drv as NVMe Driver
    participant ASQ as Admin SQ
    participant Ctrl as NVMe Controller

    Note over OS,Ctrl: ── 正常运行 → 低功耗 ──

    OS->>Drv: 空闲, 进入低功耗
    Drv->>Drv: Set Features - Power Management (FID=0x02)
    Drv->>ASQ: 写入 Set Features 命令 (PL=目标电源状态)
    Drv->>Ctrl: Doorbell[SQ0]
    Ctrl->>Ctrl: 切换到低功耗电源状态
    Note right of Ctrl: PS0: 最大功耗<br/>PS1-3: 依次降低功耗,<br/>唤醒延迟递增
    Ctrl-->>Drv: CQE: 成功

    Note over OS,Ctrl: ── 低功耗 → 恢复运行 ──

    OS->>Drv: 有新 I/O 请求
    Drv->>Drv: Set Features - Power Management (PL=PS0)
    Drv->>ASQ: 写入命令
    Drv->>Ctrl: Doorbell[SQ0]
    Ctrl->>Ctrl: 恢复到 PS0 (完全运行状态)
    Ctrl-->>Drv: CQE: 成功

    Note over OS,Ctrl: ── 优雅关机 (Power State = 4, PS4 Non-Operational) ──

    OS->>Drv: 系统关机
    Drv->>ASQ: Shutdown (OPC=0x04, SST=0x01 Normal)
    Drv->>Ctrl: Doorbell[SQ0]
    Ctrl->>Ctrl: 将所有缓存数据刷入 NAND
    Ctrl->>Ctrl: 进入 Non-Operational 状态
    Ctrl-->>Drv: CQE: 关机完成
    Drv->>Ctrl: 写 NVME_CC.EN = 0 (禁用控制器)
```

---

## 9. NVMe 与 SCSI 协议对比

```
┌────────────────────────────────────────────────────────────────────┐
│                     NVMe vs SCSI/SAS 协议对比                       │
│                                                                     │
│  维度           │  SCSI/SAS              │  NVMe                   │
│  ──────────────┼────────────────────────┼─────────────────────────  │
│  传输层         │  SCSI over SAS/FC      │  直接 PCIe              │
│  命令深度       │  单队列, 32 深度        │  多队列, 64K 深度       │
│  命令开销       │  ~96 字节 (CDB)        │  64 字节 (SQE)          │
│  完成开销       │  Sense 数据 (~256B)    │  16 字节 (CQE)         │
│  通知机制       │  单中断, 需轮询         │  MSI-X 多向量          │
│  寄存器访问     │  IO 端口 (慢)           │  MMIO (内存映射, 快)   │
│  CPU 开销       │  每次访问 SCSI 层       │  驱动直接操作, 极低     │
│  最大队列数     │  1                      │  64K                    │
│  每队列深度     │  32                     │  64K                    │
│  地址映射       │  固定映射               │  PRP/SGL 灵活映射       │
└────────────────────────────────────────────────────────────────────┘
```

---

## 10. 端到端完整 I/O 路径

从应用程序到 NAND Flash 的完整数据通路：

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        NVMe 端到端 I/O 数据通路                              │
│                                                                              │
│  ┌──────────┐   ┌──────────────┐   ┌─────────────┐   ┌──────────────────┐   │
│  │ App      │   │ VFS/Block    │   │ NVMe Driver │   │ NVMe Controller  │   │
│  │ read()   │──▶│ Layer        │──▶│             │──▶│                  │   │
│  │ write()  │   │ (bio/req)    │   │ 构造 SQE    │   │ 解析命令         │   │
│  └──────────┘   └──────────────┘   │ PRP 映射    │   │ DMA 引擎         │   │
│                                    └──────┬──────┘   └────────┬─────────┘   │
│                                           │                    │             │
│                                           ▼                    ▼             │
│                                    ┌──────────────┐   ┌──────────────────┐   │
│                                    │ Host Memory  │   │ NAND Flash       │   │
│                                    │ (DMA Buffer) │◄──│ (FTL 翻译层)     │   │
│                                    │ PRP 描述     │──▶│ 多通道并行       │   │
│                                    └──────────────┘   └──────────────────┘   │
│                                                                              │
│  完成路径:                                                                    │
│  NAND → DMA → CQ → 中断/轮询 → Driver 完成回调 → Block Layer → App          │
└─────────────────────────────────────────────────────────────────────────────┘

各阶段延迟参考 (典型值):
  ┌─────────────────────────────┬──────────────┐
  │ 阶段                        │ 延迟          │
  ├─────────────────────────────┼──────────────┤
  │ VFS + Block Layer           │ ~1-2 us      │
  │ NVMe Driver (构造 SQE+DB)   │ ~0.5-1 us    │
  │ PCIe 传输 + Controller 解析  │ ~1-2 us      │
  │ NAND 读取 (4K)              │ ~15-100 us   │
  │ DMA 回传 + CQE + 中断       │ ~1-2 us      │
  ├─────────────────────────────┼──────────────┤
  │ 端到端总延迟 (4K Read)      │ ~20-110 us   │
  └─────────────────────────────┴──────────────┘
```

---

## 11. 关键设计总结

| 设计要点 | NVMe 方案 | 优势 |
|---------|----------|------|
| **多队列** | 最多 64K 对 SQ/CQ | 消除锁竞争, 充分利用多核 |
| **门铃通知** | MMIO 写 Doorbell 寄存器 | 单次寄存器写入即可提交命令, 极低开销 |
| **DMA 直传** | PRP/SGL 描述物理内存 | 数据直接在 App Buffer 和 SSD 间传输, 零拷贝 |
| **轻量命令** | 64B SQE + 16B CQE | 远小于 SCSI CDB + Sense Data |
| **MSI-X 多中断** | 每 CQ 独立中断向量 | 中断亲和到不同 CPU 核, 减少中断处理开销 |
| **PCIe 直连** | 无中间协议层 (无 SCSI) | 降低协议栈开销, 减少延迟 |
| **Phase Tag** | CQE Phase 翻转检测 | 无需额外的"有效"标志, 简化轮询逻辑 |
