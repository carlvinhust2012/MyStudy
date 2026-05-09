# QEMU 事件处理与中断时序分析

## 目录
1. [QEMU 架构概述](#一qemu-架构概述)
2. [主事件循环时序](#二主事件循环时序)
3. [中断注入与处理时序](#三中断注入与处理时序)
4. [KVM 中断虚拟化时序](#四kvm-中断虚拟化时序)
5. [IOThread 处理时序](#五iothread-处理时序)
6. [设备 IO 处理时序](#六设备-io-处理时序)
7. [Timer 与异步事件时序](#七timer-与异步事件时序)
8. [信号处理时序](#八信号处理时序)

---

## 一、QEMU 架构概述

### 1.1 核心组件架构

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              QEMU 进程                                       │
│                                                                             │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                        Main Thread (主线程)                          │   │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────────┐  │   │
│  │  │   vCPU 0     │  │   vCPU 1     │  │      Main Loop           │  │   │
│  │  │  (KVM/TCG)   │  │  (KVM/TCG)   │  │  ┌────────────────────┐  │  │   │
│  │  │              │  │              │  │  │ GMainLoop          │  │  │   │
│  │  │ ┌──────────┐ │  │ ┌──────────┐ │  │  │ ├─ iohandlers      │  │  │   │
│  │  │ │ KVM Run  │ │  │ │ KVM Run  │ │  │  │ ├─ file descriptors│  │  │   │
│  │  │ │ (VMX/SVM)│ │  │ │ (VMX/SVM)│ │  │  │ ├─ timers          │  │  │   │
│  │  │ └──────────┘ │  │ └──────────┘ │  │  │ └─ bottom halves   │  │  │   │
│  │  └──────────────┘  └──────────────┘  │  └────────────────────┘  │  │   │
│  │           ↑                             ↑                        │  │   │
│  │           │                             │                        │  │   │
│  │  ┌────────┴─────────────────────────────┴────────────────────┐   │  │   │
│  │  │              Interrupt Controller (APIC/GIC)               │   │  │   │
│  │  │  ├─ 接收设备中断请求                                       │   │  │   │
│  │  │  ├─ 中断路由和分发                                          │   │  │   │
│  │  │  └─ 注入到 vCPU                                             │   │  │   │
│  │  └───────────────────────────────────────────────────────────┘   │  │   │
│  └──────────────────────────────────────────────────────────────────┘   │   │
│                                                                             │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                        IOThread (I/O 线程) × N                       │   │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────────┐  │   │
│  │  │  AioContext  │  │  AioContext  │  │      Event Loop           │  │   │
│  │  │   (块设备)    │  │   (网络设备)  │  │  ┌────────────────────┐  │  │   │
│  │  │              │  │              │  │  │ ├─ fd watchers      │  │  │   │
│  │  │ ┌──────────┐ │  │ ┌──────────┐ │  │  │ ├─ coroutines       │  │  │   │
│  │  │ │ Block I/O│ │  │ │ Net I/O  │ │  │  │ └─ task queues      │  │  │   │
│  │  │ │ (iothread)│ │  │ │ (iothread)│ │  │  └────────────────────┘  │  │   │
│  │  │ └──────────┘ │  │ └──────────┘ │  │                          │  │   │
│  │  └──────────────┘  └──────────────┘  └──────────────────────────┘  │   │
│  └───────────────────────────────────────────────────────────────────┘   │
│                                                                             │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                        设备模型 (Device Models)                       │   │
│  │  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐  │   │
│  │  │  virtio  │ │  e1000   │ │  NVMe    │ │  USB     │ │  VGA     │  │   │
│  │  │  -net    │ │  -pci    │ │  -pci    │ │  -uhci   │ │  -std    │  │   │
│  │  │  -blk    │ │          │ │          │ │  -xhci   │ │          │  │   │
│  │  └────┬─────┘ └────┬─────┘ └────┬─────┘ └────┬─────┘ └────┬─────┘  │   │
│  │       └─────────────┴─────────────┴─────────────┴──────────────────  │   │
│  │                            │                                         │   │
│  │                            ↓                                         │   │
│  │                   ┌─────────────────┐                                │   │
│  │                   │  Memory Region  │                                │   │
│  │                   │  (MMIO/PMIO)    │                                │   │
│  │                   └─────────────────┘                                │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 1.2 关键概念

| 概念 | 说明 |
|------|------|
| **Main Loop** | QEMU 主事件循环，基于 GMainLoop，处理非 vCPU 事件 |
| **AioContext** | 异步 I/O 上下文，每个 IOThread 有自己的 AioContext |
| **Bottom Half (BH)** | 延迟执行的回调，在主循环中处理 |
| **IOHandler** | 文件描述符事件处理器 (可读/可写) |
| **KVM IRQFD** | KVM 直接注入中断到 vCPU，绕过 QEMU |
| **IRQ Routing** | 中断路由表，决定中断分发到哪个 vCPU |

---

## 二、主事件循环时序

### 2.1 Main Loop 标准流程

```
时间 ─────────────────────────────────────────────────────────────────────────>

QEMU 启动:
├─ [T0]   初始化 GMainLoop
├─ [T1]   注册默认 io_handlers (fd 监视器)
├─ [T2]   初始化 timer_list (多个时钟源)
├─ [T3]   加载机器模型和 CPU
├─ [T4]   初始化设备模型
└─ [T5]   进入 main_loop_wait()

主循环迭代 (每次循环):
─────────────────────────────────────────

[Loop Start]
    │
    ├─ [T6]   main_loop_should_exit()?
    │         ├─ YES → 退出循环，清理资源
    │         └─ NO  → 继续
    │
    ├─ [T7]   qemu_clock_run_all_timers()
    │         ├─ 检查 virtual clock timers
    │         ├─ 检查 host clock timers
    │         ├─ 检查 realtime clock timers
    │         └─ 执行到期的 timer callbacks
    │
    ├─ [T8]   aio_poll(qemu_aio_context, blocking=true/false)
    │         ├─ 非阻塞: 立即返回
    │         └─ 阻塞: 等待 fd 事件或 timer
    │
    ├─ [T9]   qemu_run_watchers()
    │         └─ 执行 fd 对应的 io_handlers
    │
    ├─ [T10]  qemu_run_bh()
    │         └─ 执行所有的 bottom halves
    │
    └─ [T11]  → [Loop Start]

时序示例 (Timer + IO 事件):
─────────────────────────────────────────

    │
    ├─ [T0]   Timer A 已调度，到期时间 T+50ms
    ├─ [T1]   fd=10 (网络设备) 注册到 poll
    ├─ [T2]   fd=12 (块设备) 注册到 poll
    │
    ├─ [T3]   进入 poll，超时时间 50ms (Timer A)
    │
    ├─ [T25]  fd=10 有数据到达 ← 中断等待
    │         ├─ poll 返回
    │         ├─ 执行 fd=10 的 handler
    │         └─ 处理网络包
    │
    ├─ [T30]  执行 BH: 发送响应包
    │
    ├─ [T31]  重新进入 poll，剩余 20ms
    │
    ├─ [T50]  Timer A 到期
    │         ├─ poll 返回 (超时)
    │         ├─ 执行 Timer A callback
    │         └─ 调度下一次 Timer A
    │
    └─ [T51]  下一循环迭代
```

### 2.2 Main Loop 详细时序图

```
QEMU Main Thread
    │
    │  [初始化阶段]
    │
    ├─ qemu_init_main_loop()
    │   ├─ g_main_loop_new()
    │   ├─ qemu_aio_context = aio_context_new()
    │   ├─ timerlistgroup_init()
    │   └─ qemu_thread_atexit_add()
    │
    ├─ [配置阶段]
    │
    ├─ qemu_init()
    │   ├─ cpu_exec_init_all()
    │   ├─ memory_map_init()
    │   ├─ machine_init()
    │   └─ device_init()
    │
    └─ [运行阶段]
        │
        ├─ main_loop_start()
        │   │
        │   ├─ while (!main_loop_should_exit) {
        │   │
        │   │   ├─ qemu_run_timers()
        │   │   │   ├─ process virtual_clock timers
        │   │   │   ├─ process host_clock timers
        │   │   │   └─ process realtime_clock timers
        │   │   │
        │   │   ├─ qemu_poll_ns()
        │   │   │   └─ poll(fds, timeout_ns)
        │   │   │       ├─ 等待 fd 事件 (网络/块设备)
        │   │   │       └─ 或超时 (timer 到期)
        │   │   │
        │   │   ├─ qemu_run_watchers()
        │   │   │   └─ 对每个就绪 fd:
        │   │   │       ├─ 调用注册的 handler
        │   │   │       └─ handler 可能:
        │   │   │           ├─ 读取设备数据
        │   │   │           ├─ 注入中断到 vCPU
        │   │   │           └─ 调度 BH 延迟处理
        │   │   │
        │   │   └─ qemu_run_bh()
        │   │       └─ 执行所有 pending 的 bottom halves
        │   │           ├─ BH 用于延迟处理
        │   │           └─ 避免在 handler 中阻塞
        │   │
        │   └─ }
        │
        └─ main_loop_quit()
            └─ 清理资源

Bottom Half (BH) 机制详解:
─────────────────────────────────────────

场景: 网卡接收数据包

网卡设备 (e1000)                           Main Loop
    │                                           │
    │  1. 收到数据包 (来自宿主机网卡)             │
    │                                           │
    ├─ 2. 读取数据到 buffer                      │
    │                                           │
    ├─ 3. 不能直接注入中断!                      │
    │   (避免在中断上下文阻塞)                    │
    │                                           │
    ├─ 4. 创建 BH (qemu_bh_new)                 │
    │   bh->cb = e1000_bh_handler               │
    │                                           │
    ├─ 5. 调度 BH (qemu_bh_schedule)            │
    │──────────────────────────────────────────>│
    │                                           │
    │            6. Main Loop 检测到 pending BH   │
    │            7. qemu_run_bh() 执行           │
    │            8. 调用 e1000_bh_handler        │
    │                                           │
    │<──────────────────────────────────────────┤
    │                                           │
    ├─ 9. BH handler 中:                         │
    │   ├─ 处理接收的数据包                       │
    │   ├─ 更新设备状态寄存器                     │
    │   └─ 注入中断到 vCPU                        │
    │      (apic_deliver_interrupt)              │
    │                                           │
    │  10. vCPU 收到中断，进入 ISR                │
```

---

## 三、中断注入与处理时序

### 3.1 中断注入完整流程 (PIC/IOAPIC)

```
设备模型                                    Interrupt Controller              vCPU
    │                                               │                         │
    │  1. 设备触发中断                              │                         │
    │  (如: 网卡收到数据包)                         │                         │
    │                                               │                         │
    ├─ 2. 调用 qemu_irq_raise()                     │                         │
    │   (设置 IRQ 线的电平)                         │                         │
    │                                               │                         │
    │──────────────────────────────────────────────>│                         │
    │                                               │                         │
    │            3. IOAPIC 接收中断请求             │                         │
    │            4. 查 Redirect Table               │                         │
    │            5. 确定 Delivery Mode              │                         │
    │            (Fixed/NMI/SMI/ExtINT)             │                         │
    │                                               │                         │
    │            6. 发送中断到 LAPIC                │                         │
    │            (通过 APIC Bus)                    │                         │
    │                                               │                         │
    │            ───────────────────────────────────┼────────────────────────>│
    │                                               │                         │
    │                                               │            7. Local APIC
    │                                               │               接收 IPI
    │                                               │                         │
    │                                               │            8. 设置 IRR
    │                                               │               (中断请求)
    │                                               │                         │
    │                                               │            9. 评估优先级
    │                                               │               vs TPR
    │                                               │                         │
    │                                               │           10. 设置 ISR
    │                                               │               (服务中)
    │                                               │                         │
    │                                               │           11. vCPU 退出
    │                                               │               (KVM_EXIT)
    │                                               │               或 TCG 模拟
    │                                               │                         │
    │                                               │           12. 跳转 ISR
    │                                               │               处理中断
    │                                               │                         │
    │<──────────────────────────────────────────────┼─────────────────────────│
    │  13. 设备可能继续触发或清除中断                │                         │
    │      (电平触发 vs 边沿触发)                   │                         │

中断完成 (EOI):
─────────────────────────────────────────

vCPU                                         Local APIC            IOAPIC
  │                                               │                    │
  │  执行 EOI 指令 (wrmsr/指令)                    │                    │
  ├──────────────────────────────────────────────>│                    │
  │                                               │                    │
  │            2. 清除 ISR 对应位                  │                    │
  │            3. 检查 TMR (触发模式)              │                    │
  │                                               │                    │
  │            4. 如为电平触发，发送 EOI 广播       │                    │
  │            ───────────────────────────────────┼───────────────────>│
  │                                               │                    │
  │                                               │            5. 清除
  │                                               │               Remote IRR
  │                                               │                    │
  │<──────────────────────────────────────────────┼────────────────────│
  │            6. 可接收新中断                     │                    │

关键数据结构:
─────────────────────────────────────────

IOAPIC Redirect Table Entry (RTE):
├─ Vector: 0-255 (中断向量号)
├─ Delivery Mode: 000=Fixed, 010=SMI, 100=NMI, 111=ExtINT
├─ Dest Mode: 0=Physical, 1=Logical
├─ Delivery Status: 0=Idle, 1=Send Pending
├─ Interrupt Input: 0=Edge, 1=Level
├─ Trigger Mode: 0=Edge, 1=Level
├─ Mask: 0=Enabled, 1=Masked
└─ Destination: APIC ID 或 MDA
```

### 3.2 MSI/MSI-X 中断时序

```
设备 (PCIe)                                 PCI Bus              vCPU (LAPIC)
    │                                            │                      │
    │  1. 设备需要发送中断                        │                      │
    │                                            │                      │
    ├─ 2. 读取 MSI/MSI-X Capability              │                      │
    │   ├─ Message Address (LAPIC 地址)          │                      │
    │   ├─ Message Data (Vector + 控制位)        │                      │
    │   └─ Mask Bit                              │                      │
    │                                            │                      │
    ├─ 3. 执行 Memory Write TLP                  │                      │
    │   地址: 0xFEEXXXXX (LAPIC 基址)            │                      │
    │   数据: Vector + 其他控制位                 │                      │
    │                                            │                      │
    │───────────────────────────────────────────>│                      │
    │                                            │                      │
    │            4. 内存写到达 LAPIC              │                      │
    │            (通过系统总线)                   │                      │
    │            ──────────────────────────────────────────────────────>│
    │                                            │                      │
    │                                            │            5. LAPIC 接收
    │                                            │               Memory Write
    │                                            │                      │
    │                                            │            6. 提取 Vector
    │                                            │               设置 IRR
    │                                            │                      │
    │                                            │            7. 中断注入
    │                                            │               (KVM/TCG)
    │                                            │                      │
    │                                            │            8. vCPU 执行 ISR

MSI vs MSI-X 区别:
─────────────────────────────────────────

MSI:
├─ 最多 32 个中断向量
├─ 所有向量共享相同的 Message Address/Data
├─ 通过 Data 的低 3 位区分不同中断
└─ 配置较简单

MSI-X:
├─ 最多 2048 个中断向量
├─ 每个向量独立的 Message Address/Data
├─ 每个向量独立的 Mask/Pending 位
├─ 支持更灵活的中断分发
└─ 需要 Table BIR + PBA BIR
```

---

## 四、KVM 中断虚拟化时序

### 4.1 KVM 中断注入流程

```
QEMU (Userspace)              KVM Kernel Module               Hardware
      │                              │                           │
      │  1. 设备触发中断              │                           │
      │  (如: virtio-net)             │                           │
      │                              │                           │
      ├─ 2. 检查是否可用 IRQFD       │                           │
      │   ├─ 是 → 直接通过 eventfd   │                           │
      │   └─ 否 → 使用 ioctl(KVM_IRQ_LINE)                      │
      │                              │                           │
      │  场景 A: IRQFD (优化路径)     │                           │
      ├─ [提前注册了 eventfd → MSI 映射]                         │
      ├─ 3. 写 eventfd               │                           │
      │─────────────────────────────>│                           │
      │                              │ 4. 内核 eventfd 回调        │
      │                              │ 5. 查找对应的 MSI route     │
      │                              │ 6. 直接注入到 vCPU          │
      │                              │    (VMCS/APICv)            │
      │                              │──────────────────────────>│
      │                              │                           │
      │                              │                           │ 7. 硬件直接
      │                              │                           │    注入中断
      │                              │                           │    无需 VM Exit!
      │                              │                           │
      │  场景 B: 传统路径 (无 IRQFD)  │                           │
      ├─ 3. ioctl(KVM_SET_IRQ_LINE)  │                           │
      │─────────────────────────────>│                           │
      │                              │ 4. 设置 IRQ line 状态       │
      │                              │ 5. 如需要，发送 IPI 到      │
      │                              │    host CPU 运行 vCPU       │
      │                              │                           │
      │  4. vCPU 线程从 KVM_RUN 返回  │                           │
      │<─────────────────────────────│                           │
      │  exit_reason = KVM_EXIT_IRQ_WINDOW_OPEN                  │
      │                              │                           │
      ├─ 5. QEMU 检查 pending 中断   │                           │
      ├─ 6. ioctl(KVM_SET_IRQ_LINE)  │                           │
      │   或 KVM_INTERRUPT             │                           │
      │─────────────────────────────>│                           │
      │                              │ 7. 注入中断到 vCPU          │
      │                              │    (通过 LAPIC)            │
      │                              │                           │
      ├─ 8. 重新 KVM_RUN             │                           │
      │─────────────────────────────>│                           │

APICv (APIC Virtualization) 优化:
─────────────────────────────────────────

支持 APICv 的 CPU (Intel VT-x):
├─ 硬件直接处理大部分 APIC 操作
├─ 无需 VM Exit 即可接收中断
├─ IRR/ISR/PPR 自动更新
└─ 性能接近物理机

不支持 APICv 的 CPU:
├─ 每次 APIC 访问触发 VM Exit
├─ 内核模拟 APIC 操作
├─ 每次中断需要 VM Exit
└─ 性能较低
```

### 4.2 IRQFD 与 ResampleFD 时序

```
QEMU                              KVM Kernel                    vCPU
  │                                    │                         │
  │  初始化阶段 (设备配置)               │                         │
  │                                    │                         │
  ├─ 1. 创建 eventfd (irqfd)           │                         │
  ├─ 2. 配置 MSI 路由表                │                         │
  │   ├─ guest_addr: 0xFEE00000        │                         │
  │   ├─ host_irqfd: eventfd           │                         │
  │   └─ gsi: 0 (MSI 不使用)           │                         │
  │                                    │                         │
  ├─ 3. ioctl(KVM_IRQFD)               │                         │
  │───────────────────────────────────>│                         │
  │                                    │ 4. 注册到 KVM 内部表      │
  │                                    │ 5. 关联 eventfd → vCPU    │
  │                                    │    (通过 workqueue)       │
  │                                    │                         │
  │  运行时阶段                        │                         │
  │                                    │                         │
  ├─ 6. 设备触发中断                   │                         │
  ├─ 7. write(irqfd)                   │                         │
  │───────────────────────────────────>│                         │
  │                                    │ 8. irqfd 唤醒             │
  │                                    │ 9. 查表找到目标 vCPU      │
  │                                    │ 10. 调用 irq_bypass       │
  │                                    │    或 kvm_set_msi         │
  │                                    │                         │
  │                                    │────────────────────────>│
  │                                    │                         │
  │                                    │                         │ 11. APICv 直接
  │                                    │                         │     注入中断
  │                                    │                         │
  │  ResampleFD (电平触发优化)          │                         │
  │  ─────────────────────────────────                          │
  │                                    │                         │
  │  12. 创建 resamplefd               │                         │
  │  13. ioctl(KVM_IRQFD with resample)│                         │
  │───────────────────────────────────>│                         │
  │                                    │ 14. 当 guest EOI 后       │
  │                                    │    自动 write(resamplefd) │
  │                                    │─────────────────────────>│
  │<───────────────────────────────────│                         │
  │  15. QEMU 收到 resample 通知        │                         │
  │  16. 清除设备中断状态               │                         │

性能对比:
─────────────────────────────────────────

传统路径 (无 IRQFD):
├─ 每次中断: VM Exit → QEMU → KVM → VM Entry
├─ 延迟: ~3-5 μs
└─ CPU 开销: 高

IRQFD 路径:
├─ 中断: 内核直接注入 (无 VM Exit)
├─ 延迟: ~0.5-1 μs
└─ CPU 开销: 极低

适用场景:
├─ 高 I/O 吞吐量设备 (NVMe, virtio-net)
├─ 延迟敏感应用
└─ 需要 kernel 支持 kvm_irqfd
```

---

## 五、IOThread 处理时序

### 5.1 IOThread 启动与事件循环

```
QEMU Main Thread                    IOThread
      │                                 │
      │  1. 创建 IOThread               │
      ├─ object_new(TYPE_IOTHREAD)      │
      │                                 │
      ├─ 2. 初始化 AioContext           │
      │   iothread->ctx = aio_context_new()
      │                                 │
      ├─ 3. 启动线程                    │
      │   qemu_thread_create()          │
      │────────────────────────────────>│
      │                                 │
      │            4. 线程入口: iothread_run()
      │                                 │
      │            5. 获取 AioContext    │
      │            aio_context_acquire() │
      │                                 │
      │            6. 设置线程亲和性     │
      │            qemu_thread_set_affinity()
      │                                 │
      │            7. 注册到 GMainLoop   │
      │            (如果支持)            │
      │                                 │
      │            8. 进入事件循环       │
      │            while (!stopped) {    │
      │                                 │
      │               ├─ 8.1 aio_poll() │
      │               │   ├─ 处理 fd 事件│
      │               │   ├─ 执行 timers │
      │               │   └─ 执行 BHs    │
      │               │                 │
      │               ├─ 8.2 处理任务队列 │
      │               │   └─ iothread_process_requests()
      │               │                 │
      │               └─ 8.3 如 blocking │
      │                   等待下一个事件 │
      │            }                    │
      │                                 │
      │            9. 清理               │
      │            aio_context_release() │
      │                                 │
      │<────────────────────────────────│
      │  线程结束                        │

IOThread 与主线程通信:
─────────────────────────────────────────

场景: 块设备完成 I/O，需要通知主线程

IOThread (Block Backend)             Main Thread
      │                                    │
      │  1. I/O 完成回调                   │
      ├─ blk_aio_complete()                │
      │                                    │
      ├─ 2. 获取 BH                        │
      │   aio_bh_schedule_oneshot()        │
      │   (在 qemu_aio_context)            │
      │                                    │
      ├─ 3. 写入 eventfd                   │
      │   (通知主线程)                     │
      │───────────────────────────────────>│
      │                                    │
      │            4. Main Loop 收到通知    │
      │            从 poll 返回            │
      │                                    │
      │            5. qemu_run_bh()        │
      │            执行 block 回调         │
      │                                    │
      │            6. 回调中可能:           │
      │               ├─ 更新 vCPU 状态    │
      │               ├─ 注入中断          │
      │               └─ 唤醒 vCPU         │
```

### 5.2 virtio-blk 与 IOThread 协作时序

```
Guest vCPU (KVM)                      QEMU Main                        IOThread
      │                                  │                                 │
      │  1. 提交 I/O 请求                │                                 │
      │  (写入 virtqueue)                │                                 │
      │                                  │                                 │
      │  2. 写通知 (kick)                │                                 │
      │  VP_MEMORY_W/OUT                 │                                 │
      │─────────────────────────────────>│                                 │
      │                                  │                                 │
      │            3. KVM_EXIT_IO         │                                 │
      │            (virtio_pci_notify)    │                                 │
      │                                  │                                 │
      │            4. 检查是否 offloaded  │                                 │
      │            到 IOThread            │                                 │
      │                                  │                                 │
      │            5. 提交到 IOThread     │                                 │
      │            (virtio_queue_aio_*)   │                                 │
      │─────────────────────────────────>│                                 │
      │                                  │                                 │
      │                                  │            6. IOThread 收到任务   │
      │                                  │            (aio_context)        │
      │                                  │                                 │
      │                                  │            7. 执行磁盘 I/O       │
      │                                  │            (pread/pwrite/aio)   │
      │                                  │                                 │
      │                                  │            8. I/O 完成回调       │
      │                                  │            (virtio_blk_complete)│
      │                                  │                                 │
      │                                  │            9. 写入 resamplefd    │
      │                                  │            或 eventfd           │
      │                                  │<────────────────────────────────│
      │                                  │                                 │
      │            10. 处理完成           │                                 │
      │            ├─ 更新 virtqueue     │                                 │
      │            ├─ 如需要，注入中断   │                                 │
      │            └─ KVM_RUN            │                                 │
      │<─────────────────────────────────│                                 │
      │                                  │                                 │
      │  11. Guest 收到中断              │                                 │
      │  执行 ISR 处理完成               │                                 │

配置对比:
─────────────────────────────────────────

无 IOThread (默认):
├─ I/O 在主线程处理
├─ 阻塞 I/O 会暂停 VM
├─ 简单但性能受限
└─ 配置: 无需 iothread 参数

有 IOThread:
├─ I/O 在独立线程处理
├─ 不阻塞主线程和 VM
├─ 更好的并行性
└─ 配置: -object iothread,id=iothread1
         -device virtio-blk-pci,iothread=iothread1
```

---

## 六、设备 IO 处理时序

### 6.1 Port IO (PIO) 处理时序

```
Guest vCPU                            KVM                             QEMU
      │                                 │                               │
      │  1. 执行 IN/OUT 指令            │                               │
      │  (如: inb 0x3f8)                │                               │
      │                                 │                               │
      ├─ 2. 触发 VM Exit                │                               │
      │────────────────────────────────>│                               │
      │                                 │                               │
      │            3. exit_reason =     │                               │
      │               KVM_EXIT_IO       │                               │
      │                                 │                               │
      │            4. 填充 io 结构体    │                               │
      │               ├─ direction      │                               │
      │               ├─ size           │                               │
      │               ├─ port           │                               │
      │               └─ data           │                               │
      │                                 │                               │
      │<────────────────────────────────│                               │
      │                                 │                               │
      │  5. KVM_RUN 返回                │                               │
      │                                 │                               │
      ├─ 6. 调用 IO handler             │                               │
      │   kvm_handle_io()               │                               │
      │                                 │                               │
      │────────────────────────────────────────────────────────────────>│
      │                                 │                               │
      │                                 │            7. 查找 port handler
      │                                 │            memory_region_find()
      │                                 │                               │
      │                                 │            8. 调用注册的 handler
      │                                 │            (如: serial_ioport_*)
      │                                 │                               │
      │                                 │            9. 执行设备逻辑
      │                                 │            ├─ 读取寄存器状态
      │                                 │            ├─ 更新设备状态
      │                                 │            └─ 可能触发其他操作
      │                                 │                               │
      │<────────────────────────────────────────────────────────────────│
      │                                 │                               │
      ├─ 10. 如为 IN 指令               │                               │
      │    写回数据到 vCPU 寄存器       │                               │
      │                                 │                               │
      ├─ 11. 重新 KVM_RUN              │                               │
      │────────────────────────────────>│                               │
      │                                 │                               │
      │            12. VM Entry         │                               │
      │            恢复 vCPU 执行       │                               │

PIO 性能优化 (iport):)
─────────────────────────────────────────

传统 PIO (每次访问都 VM Exit):
├─ 延迟: ~1-3 μs 每次访问
├─ 适合: 低频设备访问
└─ 如: CMOS, PIC

PIO 优化 (内核处理):
├─ 设置 in-kernel PIC/PIT/APIC
├─ 无需 VM Exit
└─ 如: 8259 PIC, 8254 PIT
```

### 6.2 MMIO 处理时序

```
Guest vCPU                            KVM                             QEMU
      │                                 │                               │
      │  1. 执行内存访问指令            │                               │
      │  (mov to [0xfeb00000])          │                               │
      │                                 │                               │
      ├─ 2. EPT/NPT 缺页或权限检查失败  │                               │
      │────────────────────────────────>│                               │
      │                                 │                               │
      │            3. exit_reason =     │                               │
      │               KVM_EXIT_MMIO     │                               │
      │                                 │                               │
      │            4. 填充 mmio 结构体  │                               │
      │               ├─ phys_addr      │                               │
      │               ├─ data           │                               │
      │               ├─ len            │                               │
      │               └─ is_write       │                               │
      │                                 │                               │
      │<────────────────────────────────│                               │
      │                                 │                               │
      ├─ 5. 调用 MMIO handler           │                               │
      │   address_space_rw()            │                               │
      │                                 │                               │
      │────────────────────────────────────────────────────────────────>│
      │                                 │                               │
      │                                 │            6. 查找 MemoryRegion
      │                                 │            address_space_lookup()
      │                                 │                               │
      │                                 │            7. 找到匹配的 MR
      │                                 │            (如: virtio-mmio)  │
      │                                 │                               │
      │                                 │            8. 调用 .read/.write
      │                                 │            (如: virtio_mmio_*)│
      │                                 │                               │
      │                                 │            9. 处理 MMIO 访问
      │                                 │            ├─ 读取设备寄存器
      │                                 │            ├─ 写入 virtqueue
      │                                 │            ├─ 触发中断注入
      │                                 │            └─ 更新设备状态
      │                                 │                               │
      │<────────────────────────────────────────────────────────────────│
      │                                 │                               │
      ├─ 10. 如为读操作                 │                               │
      │    数据写回 vCPU                │                               │
      │                                 │                               │
      ├─ 11. KVM_SET_REGS/KVM_RUN       │                               │
      │────────────────────────────────>│                               │
      │                                 │                               │
      │            12. 更新 EPT/NPT     │                               │
      │            (如需要)             │                               │
      │                                 │                               │
      │            13. VM Entry         │                               │

MMIO 优化技术:
─────────────────────────────────────────

1. Memory Coalescing:
├─ 合并多次 MMIO 访问
├─ 减少 VM Exit 次数
└─ 如: virtio 批量处理

2. Shadow Page Tables:
├─ 在 shadow 页表中标记 MMIO 区域
├─ 访问时直接触发 VM Exit
└─ 避免 EPT walk 开销

3. Device Assignment (VFIO):
├─ 设备直通给 Guest
├─ Guest 直接访问设备 MMIO
├─ DMA 通过 IOMMU
└─ 性能接近物理机
```

---

## 七、Timer 与异步事件时序

### 7.1 QEMU Timer 系统时序

```
时间 ─────────────────────────────────────────────────────────────────────────>

QEMU Timer 层级:
─────────────────────────────────────────

┌─────────────────────────────────────────┐
│     Virtual Clock (vm_clock)            │
│     - 与 VM 运行时间同步                 │
│     - 暂停 VM 时停止                     │
│     - 用于: 设备模拟, vCPU 定时器         │
├─────────────────────────────────────────┤
│     Host Clock (host_clock)             │
│     - 基于主机 MONOTONIC 时钟            │
│     - VM 暂停时继续运行                  │
│     - 用于: I/O 超时, 内部定时            │
├─────────────────────────────────────────┤
│     Realtime Clock (rt_clock)           │
│     - 基于主机 REALTIME 时钟             │
│     - 受 NTP 调整影响                    │
│     - 用于: wall-clock 相关操作           │
└─────────────────────────────────────────┘

Timer 处理时序:
─────────────────────────────────────────

Main Loop
    │
    ├─ qemu_clock_run_all_timers()
    │   │
    │   ├─ 1. 获取当前时间
    │   │   ├─ qemu_clock_get_ns(vm_clock)
    │   │   ├─ qemu_clock_get_ns(host_clock)
    │   │   └─ qemu_clock_get_ns(rt_clock)
    │   │
    │   ├─ 2. 检查每个 timer list
    │   │   timerlist_run_timers(vm_clock_timers)
    │   │   timerlist_run_timers(host_clock_timers)
    │   │   timerlist_run_timers(rt_clock_timers)
    │   │
    │   ├─ 3. 遍历到期的 timers
    │   │   while (timer->expire_time <= now) {
    │   │       timer->callback(timer->opaque)
    │   │       // 执行回调
    │   │       // 回调中可能重新调度 timer
    │   │   }
    │   │
    │   └─ 4. 计算下一次超时
    │       next_timeout = earliest(timer->expire_time)
    │
    ├─ 5. 使用 next_timeout 计算 poll 超时
    │   poll_timeout = MIN(io_timeout, timer_timeout)
    │
    └─ 6. 进入 poll 等待

Timer 使用场景示例:
─────────────────────────────────────────

场景 1: vCPU 定时器 (ICOUNT)
    │
    ├─ [T0]  vCPU 执行 TB
    ├─ [T+1] TB 边界检查 icount_decr
    ├─ [T+2] 如 decremented 到 0
    │        触发虚拟时钟前进
    ├─ [T+3] 检查虚拟时钟 timers
    └─ [T+4] 如 timer 到期，注入中断

场景 2: 设备 watchdog
    │
    ├─ [T0]  启动 watchdog timer (5秒)
    ├─ [T+1] VM 正常运行
    ├─ ...
    ├─ [T+5] 如 VM 无响应
    │        timer 到期
    └─ [T+6] 执行 reset 或 panic

场景 3: 网络重传定时器
    │
    ├─ [T0]  发送数据包
    ├─ [T+1] 启动重传 timer (200ms)
    ├─ [T+2] 收到 ACK，取消 timer
    │   或
    ├─ [T+200] 未收到 ACK
    │          重传 timer 到期
    └─ [T+201] 重新发送数据包
```

### 7.2 异步事件处理 (BH + Aio)

```
异步事件链:
─────────────────────────────────────────

硬件/内核事件                          QEMU 处理
      │                                    │
      │  1. 硬件中断/信号/IO 就绪           │
      ├─ 2. 内核回调                       │
      │                                    │
      ├─ 3. 写入 eventfd/signalfd          │
      │                                    │
      ├─ 4. GMainLoop/AioContext 收到通知  │
      │   (通过 poll/epoll)                │
      │                                    │
      ├─ 5. 调用注册的处理函数             │
      │   ├─ fd handler                    │
      │   ├─ signal handler                │
      │   └─ timer callback                │
      │                                    │
      ├─ 6. handler 中可能:                 │
      │   ├─ 立即处理简单任务               │
      │   ├─ 调度 BH 延迟处理               │
      │   └─ 提交 AIO 请求                  │
      │                                    │
      └─ 7. 异步完成后再次回调              │

BH (Bottom Half) 时序详解:
─────────────────────────────────────────

设备驱动 (网卡)
    │
    ├─ 1. 网卡收到数据包 (来自宿主机 tap)
    │
    ├─ 2. fd handler 被调用
    │   └─ 从 fd 读取数据
    │
    ├─ 3. 检查是否可立即处理
    │   ├─ 是 → 直接处理，注入中断
    │   └─ 否 → 调度 BH
    │
    ├─ 4. qemu_bh_schedule(bh)
    │   └─ 将 BH 加入 pending 队列
    │
    └─ 5. handler 返回 (不阻塞)

Main Loop (下一轮迭代)
    │
    ├─ 6. qemu_run_bh()
    │   └─ 执行所有 pending BH
    │
    ├─ 7. 调用 BH callback
    │   └─ 如: e1000_receive_bh()
    │
    ├─ 8. BH 中执行:
    │   ├─ 处理数据包
    │   ├─ 更新接收描述符
    │   └─ 注入中断到 vCPU
    │
    └─ 9. vCPU 收到中断，处理数据包

AIO (Asynchronous I/O) 时序:
─────────────────────────────────────────

IOThread
    │
    ├─ 1. 收到块设备请求
    ├─ 2. 准备 AIO 请求 (Linux AIO/IOCB)
    ├─ 3. io_submit() 提交到内核
    │   └─ 立即返回 (非阻塞)
    │
    ├─ 4. 继续处理其他事件
    │   (不等待 I/O 完成)
    │
    ├─ 5. 内核完成 I/O
    │   (DMA 完成，中断触发)
    │
    ├─ 6. eventfd 收到通知
    ├─ 7. AioContext 回调触发
    │
    ├─ 8. 调用完成回调
    │   └─ 如: blk_aio_complete()
    │
    ├─ 9. 回调中:
    │   ├─ 更新块设备状态
    │   ├─ 如需要，通过 eventfd 通知 Main Thread
    │   └─ 调度 BH 在主线程执行
    │
    └─ 10. 继续处理下一个 AIO

Linux AIO vs io_uring 对比:
─────────────────────────────────────────

Linux AIO (传统):
├─ 仅支持 O_DIRECT 文件
├─ 需要 eventfd 通知完成
├─ 系统调用开销较高
└─ 成熟但功能有限

io_uring (现代):
├─ 支持任何文件类型
├─ 共享 ring buffer，减少 syscall
├─ 支持 polling mode (无中断)
├─ 支持 linked operations
└─ 性能更好，Linux 5.1+
```

---

## 八、信号处理时序

### 8.1 标准信号处理流程

```
信号源                                处理流程
    │                                    │
    ├─ SIGINT (Ctrl+C)                   │
    ├─ SIGTERM (kill)                    │
    ├─ SIGUSR1 (用户自定义)               │
    ├─ SIGIO (异步 IO)                   │
    └─ SIGALRM (定时器)                  │
                                         │
    1. 内核发送信号到 QEMU 进程          │
                                         │
    2. signal handler 被调用             │
       ├─ 设置信号标志位                 │
       ├─ 写入 signalfd (如使用)         │
       └─ 快速返回 (不阻塞)               │
                                         │
    3. Main Loop 检测到信号              │
       ├─ 通过 signalfd                  │
       └─ 或通过信号标志检查              │
                                         │
    4. 执行信号处理逻辑                  │
       ├─ SIGINT/SIGTERM: 优雅退出       │
       ├─ SIGUSR1: 创建快照              │
       ├─ SIGIO: 处理异步 IO             │
       └─ SIGALRM: 定时任务              │
```

### 8.2 信号处理详细时序

```
用户/系统                          内核                          QEMU
    │                               │                             │
    │  按 Ctrl+C                    │                             │
    ├──────────────────────────────>│                             │
    │                               │                             │
    │                               │ 1. 生成 SIGINT               │
    │                               │ 2. 找到目标进程 (QEMU)       │
    │                               │                             │
    │                               │ 3. 检查信号处理方式          │
    │                               │   ├─ SIG_DFL: 终止进程       │
    │                               │   └─ 自定义 handler: 调用    │
    │                               │                             │
    │                               │────────────────────────────>│
    │                               │                             │
    │                               │            4. signal_handler()
    │                               │            (在信号上下文)    │
    │                               │                             │
    │                               │            5. 设置标志位     │
    │                               │            exit_request = 1  │
    │                               │                             │
    │                               │            6. 写入 signalfd  │
    │                               │            (如启用)          │
    │                               │<────────────────────────────│
    │                               │                             │
    │                               │ 7. 返回用户空间              │
    │                               │                             │
    │                               │ 8. 如信号可中断系统调用      │
    │                               │    如 poll/read 返回 EINTR   │
    │                               │                             │
    │                               │────────────────────────────>│
    │                               │                             │
    │                               │            9. Main Loop      │
    │                               │            检查 exit_request │
    │                               │                             │
    │                               │            10. 或 signalfd   │
    │                               │            返回可读          │
    │                               │                             │
    │                               │            11. 执行退出逻辑   │
    │                               │            qemu_cleanup()    │
    │                               │                             │
    │                               │            12. 保存状态      │
    │                               │            关闭设备          │
    │                               │            释放资源          │
    │                               │                             │
    │                               │            13. exit(0)       │
    │                               │<────────────────────────────│
    │                               │                             │
    │                               │ 14. 进程终止                 │
    │                               │                             │

特殊信号: SIGUSR1 (快照)
─────────────────────────────────────────

QEMU 运行时
    │
    ├─ 用户发送 SIGUSR1
    ├─ signal handler 设置 snapshot_request = 1
    │
    ├─ Main Loop 检测
    ├─ 调用 do_savevm()
    │
    ├─ 保存步骤:
    │   ├─ 暂停 vCPU
    │   ├─ 保存 RAM (增量/完整)
    │   ├─ 保存设备状态
    │   ├─ 保存磁盘状态 (如 quiesce)
    │   ├─ 写入快照文件
    │   └─ 恢复 vCPU 运行
    │
    └─ 快照完成，记录日志

注意事项:
─────────────────────────────────────────

信号安全函数:
├─ 信号 handler 只能调用 async-signal-safe 函数
├─ 不能调用 malloc, printf 等
├─ 通常只设置标志位，由主循环处理
└─ 或使用 signalfd 完全避免信号 handler

实时信号 vs 标准信号:
├─ 标准信号: 可能丢失 (不排队)
├─ 实时信号: 排队，不会丢失
└─ QEMU 通常使用 signalfd 统一处理

多线程信号:
├─ 信号发送到进程，具体哪个线程处理不确定
├─ 使用 pthread_sigmask 控制线程信号屏蔽
└─ QEMU 通常在主线程处理所有信号
```

---

## 附录: 关键数据结构

### QEMUFile (迁移/快照)

```c
struct QEMUFile {
    const QEMUFileOps *ops;     // 文件操作函数表
    void *opaque;                // 私有数据

    // 写入缓冲区
    uint8_t *buf;
    size_t buf_size;
    size_t buf_index;

    // 错误状态
    int last_error;

    // 钩子函数
    qemu_file_put_notify *put_notify;
};
```

### AioContext

```c
struct AioContext {
    GSource source;              // GMainLoop 源

    // 文件描述符监视
    struct io_handler *first_io_handler;

    // 定时器
    QEMUTimerList *timer_lists[QEMU_CLOCK_MAX];

    // Bottom Halves
    struct QEMUBH *first_bh;

    // 锁
    QemuRecMutex lock;

    // 通知机制
    EventNotifier notifier;
};
```

---
