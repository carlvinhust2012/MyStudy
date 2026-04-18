# Linux NVMe Driver 功能与时序流程分析

---

## 1. NVMe Driver 概述

Linux NVMe Driver 位于内核 `drivers/nvme/` 目录，是 Block Layer 与 NVMe Controller 之间的桥梁，承担请求转换、队列管理、控制器生命周期管理、错误恢复、健康监控等职责。

### 1.1 源码结构

```
drivers/nvme/
├── host/
│   ├── core.c            # 核心框架: 初始化, 注册, 管理
│   ├── pci.c             # PCIe 传输层: 探测, 映射, DB/中断
│   ├── Fabrics.c         # NVMe over Fabrics (TCP/RDMA) 框架
│   ├── tcp.c             # NVMe over TCP 传输层
│   ├── rdma.c            # NVMe over RDMA 传输层
│   └── nvme.h            # 内部数据结构定义
├── target/
│   └── ...               # NVMe Target (将本机作为 NVMe Controller)
└── common/
    └── ...               # Host/Target 共用代码
```

### 1.2 核心架构

```
┌─────────────────────────────────────────────────────────────────────┐
│                        Linux Kernel                                  │
│                                                                      │
│  ┌────────────┐    ┌─────────────┐    ┌──────────────────────────┐  │
│  │  VFS 层     │    │ Block Layer │    │     NVMe Driver          │  │
│  │  (ext4/     │───▶│ (blk-mq)   │───▶│                          │  │
│  │   xfs/...)  │    │ 多队列调度  │    │ ┌────────────────────┐  │  │
│  └────────────┘    └──────┬──────┘    │ │  nvme_pci_ctrl     │  │  │
│                          │           │ │  (PCIe 控制器实例)  │  │  │
│                          │           │ ├────────────────────┤  │  │
│                          │           │ │  nvme_queue (SQ)   │  │  │
│                          │           │ │  nvme_queue (CQ)   │  │  │
│                          │           │ ├────────────────────┤  │  │
│                          │           │ │  Admin Queue       │  │  │
│                          │           │ │  (SQ0 / CQ0)       │  │  │
│                          │           │ ├────────────────────┤  │  │
│                          │           │ │  blk-mq ops        │  │  │
│                          │           │ │  (与 Block Layer   │  │  │
│                          │           │ │   的对接接口)       │  │  │
│                          │           │ └────────────────────┘  │  │
│                          │           └──────────┬───────────────┘  │
│                          │                      │                    │
│                          │           ┌──────────▼───────────────┐  │
│                          │           │   PCIe Core (kernel)     │  │
│                          │           │   MMIO / MSI-X / DMA    │  │
│                          │           └──────────┬───────────────┘  │
└──────────────────────────────────────┼─────────────────────────────┘
                                       │ PCIe Bus
                                       ▼
                          ┌────────────────────────┐
                          │   NVMe Controller      │
                          │   (SSD 硬件)           │
                          └────────────────────────┘
```

---

## 2. 驱动加载与控制器初始化

驱动模块加载后，通过 PCI 子系统探测 NVMe 设备，完成从 PCIe 枚举到注册 block device 的全过程。

```mermaid
sequenceDiagram
    participant Kmod as 内核 (module_init)
    participant PCI as PCI Subsystem
    participant Drv as NVMe Driver (nvme_pci)
    participant Ctrl as NVMe Controller
    participant BLK as Block Layer
    participant SYS as sysfs

    Kmod->>PCI: 注册 nvme_pci_driver (probe/remove)
    PCI->>PCI: 扫描 PCIe 总线, 发现 NVMe 设备
    PCI->>Drv: nvme_probe(pci_dev)

    Note over Drv,Ctrl: Phase 1: PCIe 配置

    Drv->>PCI: pci_enable_device_mem()
    Drv->>PCI: pci_request_mem_regions()
    Drv->>PCI: pci_set_master() (使能 Bus Master DMA)
    Drv->>Ctrl: pci_enable_pcie_error_reporting()
    Drv->>PCI: pci_alloc_irq_vectors() (分配 MSI-X 向量)
    Drv->>PCI: ioremap BAR (映射寄存器到内核虚拟地址)

    Note over Drv,Ctrl: Phase 2: 控制器使能

    Drv->>Ctrl: 读取 NVME_CAP (Max Queue Entries, MPS, 等)
    Drv->>Ctrl: NVME_CC.EN = 0 (确保禁用)
    Drv->>Ctrl: 配置 NVME_CC (MPS, AMS, CSS, IOSQES, IOCQES)
    Drv->>Ctrl: NVME_CC.EN = 1 (使能控制器)
    Drv->>Ctrl: 轮询 NVME_CSTS.RDY = 1 (等待就绪)

    Note over Drv,Ctrl: Phase 3: Admin Queue

    Drv->>Drv: nvme_alloc_queue(0, NVME_AQ_DEPTH)
    Note right of Drv: 分配 SQ0/CQ0 DMA 内存
    Drv->>Ctrl: 设置 NVME_AQA, NVME_ASQ, NVME_ACQ
    Drv->>PCI: request_irq(Admin CQ 中断向量)

    Note over Drv,Ctrl: Phase 4: Identify

    Drv->>Ctrl: Admin CMD: Identify Controller
    Ctrl-->>Drv: 返回 nvme_id_ctrl (4KB DMA)
    Drv->>Drv: 解析: MDTS, VWC, NN(最大NS数), APSD 等
    Drv->>Ctrl: Admin CMD: Identify Namespace List
    Ctrl-->>Drv: 返回 NS 列表

    Note over Drv,BLK: Phase 5: I/O Queue 创建

    loop 为每个 CPU 或每个硬件队列
        Drv->>Drv: nvme_alloc_queue(qid, depth)
        Drv->>Ctrl: Admin CMD: Create I/O CQ (qid, irq_vector)
        Ctrl-->>Drv: 成功
        Drv->>Ctrl: Admin CMD: Create I/O SQ (qid, cqid)
        Ctrl-->>Drv: 成功
        Drv->>PCI: request_irq(CQ 中断向量, 亲和到对应 CPU)
    end

    Note over Drv,BLK: Phase 6: 注册 Block Device

    Drv->>BLK: nvme_init_identify() 解析 NS 信息
    Drv->>BLK: blk_mq_init_tag_set() (分配 tag set)
    Drv->>BLK: blk_mq_alloc_queue() (创建 blk-mq 硬件队列)
    Drv->>BLK: add_disk() (注册为 block device)
    BLK-->>SYS: 创建 /dev/nvme0n1, /sys/block/nvme0n1
    SYS-->>SYS: udev 规则创建 /dev/disk/by-id/... 软链接

    Note over Drv,SYS: 初始化完成, 设备就绪
```

---

## 3. I/O 提交路径

这是 Driver 最核心的功能：将 Block Layer 的 `struct request` 转换为 NVMe SQE，提交给 Controller。

```mermaid
sequenceDiagram
    participant App as 应用程序
    participant VFS as VFS
    participant BLK as Block Layer (blk-mq)
    participant Drv as NVMe Driver (nvme_queue_rq)
    participant SQ as Submission Queue
    participant DMA as Host Memory (DMA Buffer)
    participant DB as Doorbell
    participant Ctrl as NVMe Controller

    App->>VFS: read() / write()
    VFS->>BLK: submit_bio()

    Note over BLK,Drv: blk-mq 调度

    BLK->>BLK: blk_mq_sched_insert_request()
    BLK->>BLK: 根据 CPU 亲和性选择 hardware context
    BLK->>Drv: nvme_queue_rq(hctx, rq)

    Note over Drv,DB: Step 1: 请求转换

    Drv->>Drv: 检查 nvme_req(rq)->flags
    Drv->>Drv: 从 rq 中提取: sector, nr_sectors, rw direction
    Drv->>Drv: 计算: SLBA = sector / (queue_max_sectors), NLB = nr_sectors - 1

    Note over Drv,DMA: Step 2: PRP 构建

    Drv->>DMA: 遍历 rq->bio 的 scatterlist
    Drv->>DMA: 将每个物理页地址填入 PRP list
    Note right of DMA: PRP1: 第一个物理页地址<br>PRP2: PRP list 指针 (多页时)

    Note over Drv,SQ: Step 3: 构造 SQE

    Drv->>Drv: 填充 SQE (64 字节):
    Note right of Drv: CDW0.opc = Read(0x02) 或 Write(0x01)
    Note right of Drv: CDW0.cid = command_id (tag)
    Note right of Drv: nsid = rq->rq_disk->part0->bd_device->...
    Note right of Drv: cdw10 = SLBA 低 32 位
    Note right of Drv: cdw11 = SLBA 高 32 位 + NLB
    Note right of Drv: prp1 = 第一个 DMA 物理地址
    Note right of Drv: prp2 = PRP list 物理地址

    Drv->>SQ: sqe = &nvme_sq->sqes[tail]
    Note right of SQ: 将 SQE 拷贝到 SQ 的 Tail 槽位

    Note over Drv,DB: Step 4: 通知 Controller

    Drv->>Drv: nvme_sq->sq_tail++
    Drv->>DB: writel(sq_tail, nvme_sq->q_db) (MMIO 写 Doorbell)

    DB-->>Ctrl: Controller 检测到 SQ Tail 变化
    Ctrl->>Ctrl: 开始执行命令...

    Drv-->>BLK: 返回 BLK_STS_OK (异步, 不等待完成)
```

---

## 4. I/O 完成路径

Controller 完成命令后，通过 MSI-X 中断通知 Driver，Driver 处理 CQE 并通知 Block Layer。

```mermaid
sequenceDiagram
    participant Ctrl as NVMe Controller
    participant CQ as Completion Queue
    participant IRQ as MSI-X 中断
    participant Drv as NVMe Driver (nvme_irq)
    participant BLK as Block Layer
    participant App as 应用程序

    Note over Ctrl,CQ: 命令执行完成

    Ctrl->>CQ: 写入 CQE (16 字节) 到 CQ[Tail]
    Note right of CQ: CQE.status: 成功/失败
    Note right of CQ: CQE.sq_head: 对应 SQ 位置
    Note right of CQ: CQE.cid: Command ID (匹配 SQE)
    Ctrl->>Ctrl: CQ Tail++

    Note over Ctrl,IRQ: 触发中断

    Ctrl->>IRQ: 发送 MSI-X (中断向量 = CQ 绑定的向量)
    IRQ->>Drv: CPU 调用 nvme_irq() handler
    Note right of Drv: 中断上下文中执行

    Note over Drv,CQ: 批量处理 CQE

    loop CQ Head != CQ Tail
        Drv->>CQ: 读取 CQE = &cq->cqes[head]
        Drv->>Drv: 检查 Phase Tag (P 位), 确认是新 CQE
        Drv->>Drv: cid = CQE.cid
        Drv->>Drv: 通过 cid 找到对应的 struct request

        alt Status = 成功 (SC=0x00)
            Drv->>BLK: blk_mq_complete_request(rq, BLK_STS_OK)
        else Status = 失败
            Drv->>Drv: 解析错误码 (SC, SCT, DNR)
            Drv->>Drv: 判断是否可重试

            alt 可重试 且 未超限
                Drv->>Drv: nvme_retry_req(rq)
                Note right of Drv: 重新提交到 SQ
            else DNR=1 或 重试超限
                Drv->>BLK: blk_mq_complete_request(rq, BLK_STS_IOERR)
                Drv->>Drv: 记录错误到 kernel log
            end
        end

        Drv->>Drv: CQ Head++
    end

    Note over Drv,Ctrl: 通知 Controller 回收资源

    Drv->>Ctrl: writel(cq_head, cq->q_db) (写 CQ Doorbell)
    Note right of Ctrl: Controller 确认完成, 回收 SQ 槽位
    Drv->>Drv: 更新 SQ Head = CQE.sq_head

    Note over BLK,App: 完成

    BLK->>BLK: blk_mq_end_request() -> bio_endio()
    BLK-->>App: 唤醒等待的进程, read()/write() 返回
```

---

## 5. Admin 命令处理

Admin 命令通过 SQ0/CQ0 发送，用于控制器管理和 Namespace 发现。

```mermaid
sequenceDiagram
    participant Core as NVMe Core (nvme-core.c)
    participant Drv as NVMe PCI Driver (nvme-pci.c)
    participant ASQ as Admin SQ (SQ0)
    participant Ctrl as NVMe Controller
    participant ACQ as Admin CQ (CQ0)

    Note over Core,Ctrl: Identify Controller

    Core->>Core: nvme_init_ctrl()
    Core->>Drv: nvme_submit_admin_cmd(nvme_admin_identify, CNS=1)
    Drv->>Drv: 构造 Admin SQE (opc=0x06, nsid=0, dptr=buf)
    Drv->>ASQ: 写入 SQE 到 Admin SQ
    Drv->>Ctrl: writel(db, admin_sq_db) (Doorbell)
    Ctrl->>Ctrl: 执行 Identify Controller
    Ctrl->>ACQ: 写 CQE + DMA 返回 4KB 数据
    Drv->>Drv: 等待 CQE 完成 (同步等待, admin 命令)
    Drv-->>Core: 返回 nvme_id_ctrl 数据结构

    Note over Core,Ctrl: Identify Namespace

    Core->>Drv: nvme_submit_admin_cmd(nvme_admin_identify, CNS=0, nsid=1)
    Drv->>ASQ: 写入 Identify NS SQE
    Drv->>Ctrl: Doorbell
    Ctrl-->>Drv: DMA 返回 nvme_id_ns (4KB)
    Drv-->>Core: 返回 NS 信息 (LBA size, capacity, etc.)

    Note over Core,Ctrl: Set Features: Number of Queues

    Core->>Drv: nvme_set_queue_count(ctrl, nr_queues)
    Drv->>ASQ: Set Features SQE (FID=0x07, cdw11=nr_queues)
    Drv->>Ctrl: Doorbell
    Ctrl-->>Drv: CQE (返回实际支持的队列对数)

    Note over Core,Ctrl: Get Log Page: Error Log / SMART

    Core->>Drv: nvme_get_log(ctrl, NVME_LOG_ERROR, buf)
    Drv->>ASQ: Get Log Page SQE (LID=0x01)
    Drv->>Ctrl: Doorbell
    Ctrl-->>Drv: DMA 返回 Error Log 数据
    Drv-->>Core: 返回日志

    Note over Core,Ctrl: Namespace Attach

    Core->>Drv: nvme_ns_attach(ctrl, ns_list)
    Drv->>ASQ: NS Attachment SQE (opc=0x15, sel=attach)
    Drv->>Ctrl: Doorbell
    Ctrl-->>Drv: CQE: 成功
```

---

## 6. 多队列与 CPU 亲和性

NVMe Driver 通过 `blk-mq` 框架实现多队列与 CPU 核的绑定，消除锁竞争。

```
┌─────────────────────────────────────────────────────────────────────┐
│                 blk-mq + NVMe 多队列映射                             │
│                                                                      │
│   blk-mq 硬件上下文 (hctx)        NVMe SQ/CQ                       │
│   ─────────────────────────       ──────────────                    │
│                                                                      │
│   hctx0 (CPU 0)  ──────────────▶ SQ1 / CQ1 ──▶ MSI-X Vector 1    │
│   hctx1 (CPU 1)  ──────────────▶ SQ2 / CQ2 ──▶ MSI-X Vector 2    │
│   hctx2 (CPU 2)  ──────────────▶ SQ3 / CQ3 ──▶ MSI-X Vector 3    │
│   hctx3 (CPU 3)  ──────────────▶ SQ4 / CQ4 ──▶ MSI-X Vector 4    │
│   ...                               ...                             │
│                                                                      │
│   每个 CPU 核的 I/O 请求:                                           │
│   task_queue → softirq → hctx[n] → SQ[n] → Controller              │
│                                                                      │
│   无共享队列 → 无锁竞争 → 线性扩展                                   │
└─────────────────────────────────────────────────────────────────────┘
```

### 6.1 队列分配策略

```mermaid
sequenceDiagram
    participant Drv as NVMe Driver
    participant Ctrl as NVMe Controller
    participant MQ as blk-mq
    participant IRQ as IRQ Affinity

    Note over Drv,IRQ: 队列创建与绑定

    Drv->>Ctrl: Admin: Identify Controller → 获取最大队列数
    Ctrl-->>Drv: 返回 maxq

    Drv->>Drv: nr_io_queues = min(online_cpus, maxq, nvme_max_queues)

    loop qid = 1 到 nr_io_queues
        Drv->>Ctrl: Admin: Create I/O CQ (qid, irq_vector = qid)
        Drv->>Ctrl: Admin: Create I/O SQ (qid, cqid)
    end

    Drv->>MQ: blk_mq_alloc_tag_set(nr_hw_queues = nr_io_queues)
    MQ->>MQ: 每个 hctx 映射到一个 SQ

    Note over Drv,IRQ: 中断亲和性

    Drv->>IRQ: irq_set_affinity_hint(vector[n], cpu_mask[n])
    Note right of IRQ: CQ1 中断 → CPU 0
    Note right of IRQ: CQ2 中断 → CPU 1
    Note right of IRQ: ...
```

---

## 7. 错误处理与控制器复位

当 Controller 出现异常（超时、错误率过高、PCIe 错误等），Driver 会执行复位流程恢复设备。

```mermaid
sequenceDiagram
    participant Drv as NVMe Driver
    participant SQ as Submission Queue
    participant Ctrl as NVMe Controller
    participant Timer as nvme_timeout (watchdog)
    participant Core as NVMe Core
    participant BLK as Block Layer

    Note over Drv,Ctrl: 正常 I/O 进行中...

    Drv->>SQ: 提交多个 I/O 请求
    Drv->>Ctrl: Doorbell

    Note over Ctrl,Timer: 异常发生: Controller 无响应

    Timer->>Timer: nvme_timeout() 定时器触发
    Note right of Timer: 请求超时 (默认 30s, 可配置)
    Timer->>Drv: nvme_timeout(rq)

    Note over Drv,Core: Phase 1: 尝试 Abort

    Drv->>Drv: nvme_abort_req(req)
    Drv->>Ctrl: Admin CMD: Abort (opc=0x08, cid=超时命令的 CID)

    alt Abort 成功
        Ctrl-->>Drv: CQE: Abort 成功
        Drv->>Drv: 标记请求完成 (错误)
        Note over Drv,Core: 恢复正常
    else Abort 也超时 (Controller 严重故障)
        Ctrl-->>Drv: 无响应
        Drv->>Drv: 进入 Reset 流程
    end

    Note over Drv,BLK: Phase 2: 控制器 Reset

    Drv->>BLK: blk_mq_quiesce_queue() (暂停新的 I/O 提交)
    BLK-->>Drv: 所有在飞请求冻结

    Drv->>Drv: nvme_disable_ctrl(ctrl)
    Note right of Drv: NVME_CC.EN = 0
    Drv->>Ctrl: 轮询 NVME_CSTS.RDY = 0

    Drv->>Ctrl: nvme_reset_ctrl()
    Note right of Drv: NVME_NSSR = 1 (NVM Subsystem Reset)

    alt Reset 成功
        Drv->>Ctrl: NVME_CC.EN = 1 (重新使能)
        Drv->>Ctrl: 轮询 NVME_CSTS.RDY = 1
        Drv->>Drv: nvme_configure_admin_queue() (重建 Admin Queue)
        Drv->>Drv: nvme_create_io_queues() (重建 I/O Queues)

        Note over Drv,BLK: Phase 3: 恢复 I/O

        Drv->>Drv: 重新提交之前未完成的请求
        Drv->>BLK: blk_mq_unquiesce_queue() (恢复 I/O 提交)
        Note over Drv,BLK: 恢复正常工作
    else Reset 失败 (硬件故障)
        Drv->>Core: nvme_remove_ctrl()
        Core->>BLK: del_gendisk() (移除 block device)
        Core->>Core: 打印 kernel error log
        Note right of Core: "nvme: I/O 2134 timeout, disabling controller"
        Note over Core: 需要人工干预 (更换 SSD)
    end
```

### 7.1 错误分级处理策略

```
┌─────────────────────────────────────────────────────────────────────┐
│                    NVMe Driver 错误分级策略                           │
│                                                                      │
│  Level 1: 请求级重试 (自动恢复)                                      │
│  ────────────────────────────                                       │
│  单个请求失败, SC != Generic Success                                 │
│  - 检查 DNR (Do Not Retry) 标志                                     │
│  - DNR=0: 最多重试 nvme_max_retries 次 (默认 3)                     │
│  - DNR=1: 直接上报错误, 不重试                                       │
│                                                                      │
│  Level 2: Abort 命令 (自动恢复)                                      │
│  ────────────────────────────                                       │
│  请求超时, Controller 可能卡住                                       │
│  - 发送 Admin Abort 命令 (opc=0x08)                                  │
│  - Abort 超时阈值: nvme_admin_timeout (默认 60s)                     │
│                                                                      │
│  Level 3: 控制器 Reset (自动恢复)                                    │
│  ────────────────────────────                                       │
│  Abort 失败 或 错误率超阈值                                         │
│  - nvme_reset_ctrl(): CC.EN=0 → NSSR=1 → CC.EN=1                   │
│  - 重建所有队列, 重新提交在飞请求                                     │
│  - 限制 Reset 频率 (防止 Reset 风暴)                                 │
│                                                                      │
│  Level 4: 移除设备 (需人工干预)                                      │
│  ────────────────────────────                                       │
│  Reset 失败 或 PCIe AER 严重错误                                     │
│  - del_gendisk(), 从系统中移除设备                                   │
│  - 内核日志告警, 触发硬件故障告警                                     │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 8. 健康监控

Driver 定期通过 Admin 命令采集 SMART 数据，监控 SSD 健康状态。

```mermaid
sequenceDiagram
    participant WD as nvme_wd_timer (内核定时器)
    participant Drv as NVMe Driver
    participant Ctrl as NVMe Controller
    participant Log as SMART / Health Log
    participant EVT as 内核事件

    Note over WD,EVT: 定期健康检查 (默认每 5 分钟)

    WD->>Drv: nvme_dev_ctrl_reboot_work() 定时触发

    Note over Drv,Log: 获取 SMART 日志

    Drv->>Ctrl: Admin CMD: Get Log Page (LID=0x02, SMART/Health)
    Ctrl->>Log: 从 Controller 内部采集 SMART 数据
    Log-->>Drv: DMA 返回 SMART Log (512 字节)

    Note over Drv,EVT: 解析关键指标

    Drv->>Drv: 检查 critical_warning 位:
    Note right of Drv: Bit 0: Available Spare 低于阈值
    Note right of Drv: Bit 1: Temperature 过高
    Note right of Drv: Bit 2: NVM 子系统可靠性降级
    Note right of Drv: Bit 3: ReadOnly 模式
    Note right of Drv: Bit 4: Volatile Memory Backup 失败

    alt 温度超阈值
        Drv->>EVT: dev_warn("nvme: temperature %dC above threshold", temp)
        Drv->>Drv: 触发温度管理 (降低性能 或 强制冷却)
    end

    alt Available Spare 不足
        Drv->>EVT: dev_warn("nvme: critical spare: %d%%", spare)
        Note right of EVT: SSD 寿命即将耗尽, 需要更换
    end

    alt Media Errors 增长异常
        Drv->>EVT: dev_warn("nvme: media errors: %llu", media_err)
        Note right of EVT: NAND 读写错误率升高, 可能预示盘故障
    end

    Note over Drv,Log: 获取 Error Log

    Drv->>Ctrl: Admin CMD: Get Log Page (LID=0x01, Error Log)
    Log-->>Drv: DMA 返回 Error Log Entries

    Drv->>Drv: 解析每个 Error Entry:
    Note right of Drv: Error Count, SQID, CID, Status, LBA, NSID
    Drv->>Drv: 更新 /sys/block/nvme0n1/device/error_log

    Note over Drv,EVT: 更新 sysfs 接口

    Drv->>EVT: 更新 sysfs 属性:
    Note right of EVT: /sys/class/nvme/nvme0/device/model
    Note right of EVT: /sys/class/nvme/nvme0/device/firmware_rev
    Note right of EVT: /sys/class/nvme/nvme0/device/temperature
    Note right of EVT: /sys/class/nvme/nvme0/device/SMART/...
```

### 8.1 sysfs 暴露的关键属性

```
/sys/class/nvme/nvme0/
├── device/
│   ├── model                    # 型号 (如 "Samsung SSD 980 PRO")
│   ├── serial                   # 序列号
│   ├── firmware_rev             # 固件版本
│   ├── temperature              # 当前温度 (毫摄氏度)
│   ├── SMART/
│   │   ├── critical_warning     # 关键告警位掩码
│   │   ├── available_spare      # 可用 spare 空间百分比
│   │   ├── percentage_used      # SSD 已使用寿命百分比
│   │   ├── data_units_read      # 读取数据量 (单位 512B)
│   │   ├── data_units_written   # 写入数据量
│   │   ├── host_reads           # 读取命令数
│   │   ├── host_writes          # 写入命令数
│   │   ├── power_cycles         # 电源开关次数
│   │   ├── power_on_hours       # 累计通电时间
│   │   ├── unsafe_shutdowns     # 非正常断电次数
│   │   └── media_errors         # 不可纠正的介质错误数
│   └── error_log/               # 最近的错误日志条目
└── nvme0n1/                     # Namespace (block device)
    └── ...
```

---

## 9. 固件更新流程

NVMe Driver 支持在不重启系统的情况下通过 Admin 命令在线更新 SSD 固件。

```mermaid
sequenceDiagram
    participant User as 用户空间 (nvme-cli)
    participant Drv as NVMe Driver
    participant Ctrl as NVMe Controller
    participant NAND as NAND Flash

    User->>Drv: ioctl(NVME_IOCTL_ADMIN_CMD)
    Note right of User: 或通过 /dev/nvme0 admin passthrough

    Note over Drv,Ctrl: Step 1: 下载固件镜像

    Drv->>Drv: 分配 DMA Buffer (固件大小)
    loop 分块下载 (每次最多 MDTS 大小)
        User->>Drv: Firmware Image Download (opc=0x10)
        Drv->>Drv: 构造 Admin SQE (NUMD=数据块数, OFST=偏移)
        Drv->>Ctrl: Admin CMD + DMA 传输固件数据
        Ctrl->>Ctrl: 将固件暂存到内部缓冲区
        Ctrl-->>Drv: CQE: 成功
    end

    Note over Drv,Ctrl: Step 2: 提交固件激活

    User->>Drv: Firmware Commit (opc=0x10)
    Drv->>Drv: 构造 Admin SQE:
    Note right of Drv: FS=0x02 (Slot 1 激活, 下次 Reset 后生效)

    alt 固件需要重启才能生效
        Drv->>Ctrl: Admin CMD: Firmware Commit
        Ctrl-->>Drv: CQE: 成功, 需要Reset
        Note right of Drv: 固件已下载, 等待下次控制器 Reset 激活
        Note right of Drv: 可手动: echo 1 > /sys/class/nvme/nvme0/device/reset
    else 固件支持即时激活
        Drv->>Ctrl: Admin CMD: Firmware Commit (FS=0x06)
        Note right of Drv: 激活并应用, Controller 自动 Reset
        Ctrl->>Ctrl: 应用新固件, 自动 Reset
        Ctrl-->>Drv: Reset 完成
        Drv->>Drv: nvme_reset_ctrl() (处理 Reset, 重建队列)
        Note right of Drv: 固件在线更新完成, 无需重启系统
    end
```

---

## 10. 移除与卸载流程

设备热拔或模块卸载时，Driver 需要安全清理所有资源。

```mermaid
sequenceDiagram
    participant EVT as 内核事件
    participant BLK as Block Layer
    participant Drv as NVMe Driver
    participant Ctrl as NVMe Controller
    participant PCI as PCI Subsystem

    Note over EVT,Ctrl: 触发: 热拔插 / rmmod / Reset 失败

    EVT->>Drv: nvme_remove(pci_dev)

    Note over Drv,BLK: Phase 1: 停止 I/O

    Drv->>BLK: blk_mq_quiesce_queue()
    BLK-->>Drv: 在飞请求完成
    Drv->>BLK: blk_sync_queue() (等待所有请求结束)
    Drv->>BLK: del_gendisk() (注销 /dev/nvme0n1)

    Note over Drv,Ctrl: Phase 2: 停止控制器

    Drv->>Ctrl: NVME_CC.EN = 0 (禁用控制器)
    Drv->>Ctrl: 轮询 NVME_CSTS.RDY = 0 (确认关闭)

    Note over Drv,Ctrl: Phase 3: 释放队列资源

    Drv->>Drv: 释放所有 I/O SQ/CQ DMA 内存
    Drv->>Drv: 释放 Admin SQ/CQ DMA 内存
    Drv->>Drv: 释放 Admin CMD DMA 缓冲区

    Note over Drv,PCI: Phase 4: 释放 IRQ 和 PCIe 资源

    Drv->>PCI: free_irq(所有 MSI-X 向量)
    Drv->>PCI: pci_free_irq_vectors()
    Drv->>PCI: iounmap(BAR 空间)
    Drv->>PCI: pci_release_mem_regions()
    Drv->>PCI: pci_disable_device()

    Note over Drv: Phase 5: 释放内核对象

    Drv->>Drv: kfree(nvme_ctrl)
    Drv->>Drv: kfree(nvme_queue[])

    Note over Drv: 移除完成
```

---

## 11. 功能总结

| 功能模块 | 核心函数 | 作用 |
|---------|---------|------|
| **PCIe 探测与绑定** | `nvme_probe()` | 枚举 NVMe 设备, 映射 BAR, 分配 MSI-X |
| **控制器初始化** | `nvme_init_ctrl()` | 使能 Controller, Identify, 创建队列 |
| **请求转换与提交** | `nvme_queue_rq()` | bio → SQE 转换, PRP 构建, 写 Doorbell |
| **I/O 完成处理** | `nvme_irq()` → `nvme_process_cq()` | 读 CQE, 匹配请求, 错误处理, 通知 blk-mq |
| **队列管理** | `nvme_create_queue()` / `nvme_delete_queue()` | Admin 命令创建/删除 I/O Queue, CPU 亲和性 |
| **Admin 命令** | `nvme_submit_admin_cmd()` | Identify, Set/Get Features, Get Log Page |
| **错误恢复** | `nvme_timeout()` → `nvme_reset_ctrl()` | Abort → Reset → 移除, 分级错误处理 |
| **健康监控** | `nvme_get_log()` (定时器触发) | SMART 日志采集, 温度/寿命/错误率监控 |
| **固件更新** | `nvme_fw_download()` + `nvme_fw_commit()` | 在线固件下载与激活 |
| **设备移除** | `nvme_remove()` | 停止 I/O, 释放队列/IRQ/PCIe 资源 |
