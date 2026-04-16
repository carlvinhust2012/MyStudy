# NCCL Proxy 层功能分析

## 1. Proxy 层的定位与作用

### 为什么需要 Proxy？

GPU Kernel 运行在 GPU 上，RDMA Verbs 运行在 CPU 用户态，两者是**不同的执行环境**，无法直接调用。需要一个 CPU 端的"中间层"来桥接：

```
┌────────────────────────────────────────────────────────────────────┐
│                         为什么需要 Proxy？                          │
│                                                                    │
│  GPU Kernel:     运行在 GPU 上，只能访问 GPU 显存，不能调用系统 API  │
│  RDMA Verbs:     运行在 CPU 上（libibverbs），操控网卡硬件           │
│                                                                    │
│  问题: GPU 无法直接调用 ibv_post_send / ibv_poll_cq                │
│  方案: Proxy 作为 CPU 端的"搬运工"，通过 FIFO 与 GPU Kernel 协同    │
└────────────────────────────────────────────────────────────────────┘
```

### Proxy 的三大职责

| 职责 | 说明 |
|------|------|
| **连接管理** | Service Thread 处理建连/拆连请求（创建 QP、内存注册等） |
| **数据搬运** | Progress Thread 轮询 FIFO，提交 RDMA 操作（ibv_post_send），轮询完成（ibv_poll_cq） |
| **状态同步** | 更新 FIFO head/tail，作为 GPU Kernel 与网络之间的信号桥梁 |

### Proxy 的整体位置

```
┌───────────────────────┐
│    GPU Kernel         │  CUDA Block 执行 send/recv Primitives
│    (ncclKernelMain)   │  直接读写 GPU 显存中的 FIFO
└───────────┬───────────┘
            │ FIFO (head/tail 在 GPU 显存)
┌───────────▼───────────┐
│    Proxy 层           │  CPU 用户态线程
│  ┌─────────────────┐  │
│  │ Service Thread  │  │  连接管理: 创建 QP、注册 MR、RDMA CM 握手
│  ├─────────────────┤  │
│  │Progress Thread  │  │  数据搬运: 轮询 FIFO → 提交 RDMA → 轮询 CQ
│  └────────┬────────┘  │
└───────────┼───────────┘
            │ ncclNet 接口 (isend/irecv/test/regMr)
┌───────────▼───────────┐
│    NET 传输层         │  RDMA Plugin (libibverbs)
│    (ibv_post_send,    │
│     ibv_poll_cq,      │
│     ibv_reg_mr)       │
└───────────┬───────────┘
            │ 硬件指令
┌───────────▼───────────┐
│    NIC (RNIC)         │  网卡硬件，DMA 直接搬运 GPU 显存
└───────────────────────┘
```

---

## 2. Proxy 的两个线程

### Service Thread（服务线程）

**职责：处理一次性/低频的控制面操作**

- 响应主线程的连接请求（`ncclTransportRingConnect` / `ncclTransportTreeConnect`）
- 创建 RDMA QP、CQ、PD
- 注册内存区域（MR）—— GPUDirect RDMA
- RDMA CM 地址解析与连接建立
- 拆连时的资源释放

```
Service Thread 生命周期:
  创建 ──► 等待请求 ──► 处理 connect ──► 返回结果 ──► 等待下一个请求 ...
                      处理 disconnect ──► 释放资源
  特点: 事件驱动，空闲时不消耗 CPU
```

### Progress Thread（进度线程）

**职责：处理连续的高频的数据面操作**

- **轮询发送端 FIFO**：检测 `tail` 变化，有新数据则提交 RDMA 发送
- **轮询接收端 CQ**：检测 RDMA 完成事件，更新 FIFO `head`
- **管理滑动窗口**：批量提交、追踪 inflight 请求数，避免 QP 溢出
- **处理所有 Channel**：一个 Progress Thread 管理一个 GPU 的所有 Channel

```
Progress Thread 主循环 (伪代码):

  while (running) {
      // 扫描所有 connection，检查是否有新数据要发
      for each connection in managed_connections {
          // 发送方向: GPU → Network
          tail = read(connection.fifo.tail)
          while (connection.sliding_window.posted < tail) {
              data = connection.fifo.buffs[posted % nFifo]
              ncclNet->isend(handle, data, size, tag)
              connection.sliding_window.posted++
          }

          // 接收方向: Network → GPU
          while (connection.sliding_window.done < connection.sliding_window.received) {
              if (ncclNet->test(request) == complete) {
                  write(connection.fifo.head, ++done)
              }
          }
      }
  }
```

---

## 3. Proxy 连接管理时序

```mermaid
sequenceDiagram
    participant Main as 主线程
    participant Svc as Service Thread
    participant Net as ncclNet (RDMA Plugin)
    participant IB as NIC (ibv_* verbs)
    participant R as 远端

    Note over Main: ncclTransportRingConnect()

    Main->>Main: selectTransport() → NET
    Main->>Main: bootstrapSend/Recv 交换连接元数据

    Note over Main: 发起 Proxy Connect 请求
    Main->>Svc: ncclProxyCallBlocking(CONNECT)

    rect rgb(230, 245, 230)
        Note over Svc,R: Service Thread 处理连接
        Svc->>Net: ncclNet->listen()
        Note over Net: rdma_create_id() + rdma_listen()

        Svc->>Net: ncclNet->connect()
        Note over Net: ibv_open_device()
        Svc->>IB: ibv_alloc_pd() — Protection Domain
        Svc->>IB: ibv_create_cq() — Completion Queue
        Svc->>IB: ibv_create_qp(RC) — Queue Pair

        Note over IB: RDMA CM 三次握手
        Svc->>IB: rdma_resolve_addr()
        Svc->>IB: rdma_resolve_route()
        Svc->>R: rdma_connect()
        R-->>Svc: rdma_accept()

        Note over IB: QP: RESET → INIT → RTR → RTS

        Svc->>IB: ibv_reg_mr(GPU 显存 via DMA-BUF)
        Note over IB: GPUDirect RDMA 内存注册
    end

    Svc-->>Main: 连接完成，返回 handle

    Main->>Main: cudaMemcpy H2D (连接信息写入 GPU 端)
    Note over Main: GPU Kernel 现在知道远端地址和 rkey
```

---

## 4. Proxy 数据搬运时序（单次 Send）

这是 Proxy 最核心的工作流程：**GPU Kernel 准备数据 → Proxy 提交 RDMA → Proxy 检测完成 → GPU Kernel 使用数据**。

```mermaid
sequenceDiagram
    participant GK as GPU Kernel<br/>(发送端)
    participant FIFO as FIFO Buffer<br/>(GPU 显存)
    participant PT as Progress Thread<br/>(发送端 Proxy)
    participant NIC as 本地 NIC
    participant RNIC as 远端 NIC
    participant RPT as Progress Thread<br/>(接收端 Proxy)
    participant RFIFO as FIFO Buffer<br/>(接收端 GPU 显存)
    participant RGK as GPU Kernel<br/>(接收端)

    Note over GK: Primitives send() 被调用

    rect rgb(255, 245, 220)
        Note over GK,FIFO: Step 1: GPU Kernel 准备数据
        GK->>FIFO: 1. 将数据写入 buffs[tail % nFifo]
        GK->>FIFO: 2. __threadfence_system() (内存屏障)
        GK->>FIFO: 3. 更新 tail (tail++)
        Note over FIFO: tail 变化对 CPU 可见
    end

    rect rgb(220, 240, 255)
        Note over PT,NIC: Step 2: Proxy 检测并提交发送
        PT->>FIFO: 轮询发现 tail > posted
        PT->>PT: 从 FIFO 取出数据地址、大小
        PT->>NIC: ncclNet->isend(handle, addr, size, tag)
        Note over NIC: ibv_post_send(<br/>  RDMA_WRITE_WITH_IMM,<br/>  local_addr = send_buf,<br/>  remote_addr = recv_buf,<br/>  rkey = remote_mr_key,<br/>  imm_data = tag)
        PT->>PT: 更新 sliding_window.posted
    end

    rect rgb(220, 255, 220)
        Note over NIC,RNIC: Step 3: 网卡硬件 DMA 传输
        NIC->>NIC: DMA 读取本地 GPU 显存
        NIC->>RNIC: RDMA 网络传输
        RNIC->>RNIC: DMA 写入远端 GPU 显存
        RNIC->>RNIC: 生成 CQE
    end

    rect rgb(255, 220, 240)
        Note over RPT,RFIFO: Step 4: 接收端 Proxy 检测完成
        RPT->>RNIC: ncclNet->test(request)
        Note over RPT: ibv_poll_cq → 返回 CQE
        RPT->>RFIFO: 更新 head (head++)
        Note over RFIFO: head 变化对 GPU 可见
    end

    rect rgb(255, 245, 220)
        Note over RGK,RFIFO: Step 5: 接收端 GPU Kernel 使用数据
        RGK->>RFIFO: 轮询发现 head 变化
        RGK->>RGK: 直接从 GPU 显存读取数据
        Note over RGK: 零拷贝！CPU 未触碰数据
    end
```

---

## 5. Proxy 数据搬运时序（流水线批量）

实际生产中，Proxy 不会等单个请求完成再处理下一个，而是用**滑动窗口**批量提交：

```mermaid
sequenceDiagram
    participant GK as GPU Kernel
    participant PT as Progress Thread
    participant NIC as 本地 NIC
    participant RNIC as 远端 NIC
    participant RPT as 远端 Progress Thread
    participant RGK as 远端 GPU Kernel

    Note over PT: 滑动窗口大小 = 4 (最多 4 个 inflight 请求)

    rect rgb(255, 245, 220)
        Note over GK,PT: GPU 批量准备 3 个数据块
        GK->>GK: 写入 chunk 0, tail=1
        GK->>GK: 写入 chunk 1, tail=2
        GK->>GK: 写入 chunk 2, tail=3
    end

    rect rgb(220, 240, 255)
        Note over PT,NIC: Proxy 批量提交 (不逐个等待)
        PT->>NIC: isend(chunk 0) — posted=1
        PT->>NIC: isend(chunk 1) — posted=2
        PT->>NIC: isend(chunk 2) — posted=3
        Note over PT: inflight = 3, 窗口剩余 1
    end

    rect rgb(220, 255, 220)
        Note over NIC,RPT: 网卡并行传输
        NIC->>RNIC: chunk 0 传输中...
        NIC->>RNIC: chunk 1 传输中...
        NIC->>RNIC: chunk 2 传输中...
    end

    rect rgb(255, 220, 240)
        Note over RPT,RGK: 接收端逐个完成
        RPT->>RPT: test(chunk 0) ✓ → head=1
        RPT->>RPT: test(chunk 1) ✓ → head=2
        RPT->>RPT: test(chunk 2) ✓ → head=3
    end

    rect rgb(245, 230, 255)
        Note over GK,PT: GPU 继续写入 + Proxy 继续提交 (流水线重叠)
        GK->>GK: 写入 chunk 3, tail=4
        GK->>GK: 写入 chunk 4, tail=5
        PT->>NIC: isend(chunk 3) — posted=4 (窗口满)
        PT->>PT: test(chunk 0) ✓ → done=1, 窗口释放
        PT->>NIC: isend(chunk 4) — posted=5
    end
```

### 滑动窗口状态机

```
              GPU Kernel 写入 FIFO
                     │
                     ▼
              tail 递增
                     │
                     ▼
  ┌──────────────────────────────────────┐
  │  Sliding Window (Proxy 管理)         │
  │                                      │
  │  ┌─────┬──────────┬─────────┬─────┐  │
  │  │done │ received │ posted  │tail │  │
  │  │  ↑  │    ↑     │   ↑     │  ↑  │  │
  │  │ 已确认│ 已完成   │ 已提交   │已写入│  │
  │  └─────┴──────────┴─────────┴─────┘  │
  │       │              │                │
  │       │              │                │
  │  ncclNet->test()  ncclNet->isend()   │
  │  检测远端完成     提交到本地 NIC       │
  │       │              │                │
  │       ▼              ▼                │
  │  更新 FIFO head   从 FIFO 读数据      │
  └──────────────────────────────────────┘
                     │
                     ▼
              GPU Kernel 读 FIFO
              (检测 head 变化)
```

---

## 6. 多 Channel 并行时序

一个 GPU 上所有 Channel 共享同一个 Progress Thread，Proxy 在主循环中轮询所有连接：

```mermaid
sequenceDiagram
    participant GK as GPU Kernel
    participant PT as Progress Thread
    participant NIC as NIC (多 QP)

    Note over GK: 每个 Channel 1 个 CUDA Block

    rect rgb(255, 245, 220)
        Note over GK: GPU 端并行写入
        GK->>GK: Channel 0: 写 FIFO, tail++
        GK->>GK: Channel 1: 写 FIFO, tail++
        GK->>GK: Channel 2: 写 FIFO, tail++
        GK->>GK: Channel 3: 写 FIFO, tail++
    end

    rect rgb(220, 240, 255)
        Note over PT,NIC: Progress Thread 轮询所有 Channel
        PT->>PT: for each connection { ... }
        PT->>NIC: isend(Channel 0, QP0)
        PT->>NIC: isend(Channel 1, QP1)
        PT->>NIC: isend(Channel 2, QP2)
        PT->>NIC: isend(Channel 3, QP3)
        Note over NIC: 4 个 QP 并发发送
    end

    rect rgb(220, 255, 220)
        Note over PT,NIC: 下一轮轮询: 检测完成 + 提交新数据
        PT->>NIC: test(Channel 0) ✓
        PT->>NIC: test(Channel 1) ✓
        PT->>NIC: isend(Channel 2, 下一片)
        PT->>NIC: isend(Channel 3, 下一片)
        Note over PT: 发送和接收交错处理
    end
```

---

## 7. Proxy 在不同传输模式下的差异

| 传输层 | Proxy 职责 | 数据路径 |
|--------|-----------|---------|
| **NET (RDMA)** | 轮询 FIFO → ibv_post_send(RDMA_WRITE) → ibv_poll_cq | GPU 显存 → NIC DMA → 网络 |
| **P2P (NVLink)** | 几乎不参与，GPU Kernel 通过 NVLink 直接读写对端显存 | GPU → NVLink → GPU |
| **SHM (共享内存)** | 轻量级，轮询 FIFO → memcpy 到共享内存 | GPU → D2H → /dev/shm → H2D → GPU |

```
                  Proxy 负载排序:

  P2P (NVLink)  ◄── 最轻，几乎不参与
       │
       ▼
  SHM (共享内存) ◄── 中等，做 CPU 端 memcpy
       │
       ▼
  NET (RDMA)    ◄── 最重，负责所有 RDMA 操作
                  但数据本身仍零拷贝 (GPUDirect)
```

---

## 8. 总结

```
┌─────────────────────────────────────────────────────────────────┐
│                    NCCL Proxy 层核心要点                        │
│                                                                 │
│  存在原因: GPU Kernel 无法直接调用 RDMA Verbs                   │
│                                                                 │
│  Service Thread:  事件驱动，处理建连/拆连（低频、一次性）          │
│  Progress Thread: 轮询驱动，搬运数据（高频、持续运行）             │
│                                                                 │
│  工作模式:                                                       │
│    GPU Kernel 写 FIFO tail  ←→  Progress Thread 检测并提交 RDMA  │
│    Progress Thread 检测完成  ←→  GPU Kernel 读 FIFO head        │
│                                                                 │
│  关键优化:                                                       │
│    滑动窗口批量提交 → 提高 NIC 利用率                             │
│    多 Channel 轮询 → 单线程管理所有连接                           │
│    GPUDirect RDMA → 数据零拷贝，Proxy 不触碰实际数据             │
│                                                                 │
│  性能特征:                                                       │
│    Proxy 不搬运数据本身，只提交和管理 RDMA 操作                   │
│    瓶颈在轮询延迟和网络带宽，不在 Proxy 的 CPU 开销              │
└─────────────────────────────────────────────────────────────────┘
```
