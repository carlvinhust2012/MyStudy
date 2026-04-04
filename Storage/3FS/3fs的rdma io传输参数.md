# 3FS RDMA I/O 传输关键参数

## 一、概览

3FS 使用 InfiniBand RDMA 进行数据传输，绕过 CPU 和内核实现零拷贝。RDMA 传输 I/O 数据需要 4 层关键参数：

```
┌─────────────────────────────────────────┐
│ 1. 连接层: QP, Port, LID, QPN          │ ← 建立通信通道
├─────────────────────────────────────────┤
│ 2. 内存层: MR, rkey, addr, length      │ ← 注册传输区域
├─────────────────────────────────────────┤
│ 3. 操作层: WR, SGE, Opcode             │ ← 描述一次传输
├─────────────────────────────────────────┤
│ 4. 通知层: CQ, WC, Signal, Solicited   │ ← 确认传输完成
└─────────────────────────────────────────┘
```

---

## 二、连接层参数

QP (Queue Pair) — RDMA 通信端点：

| 参数 | 说明 |
|------|------|
| **QPN** (Queue Pair Number) | 全局唯一标识 |
| **Transport** | 3FS 使用 RC (Reliable Connection) |
| **State** | INIT → RTR → RTS（建立握手过程） |
| **SQ** (Send Queue) | 发送队列，存放待发送的 WR |
| **RQ** (Receive Queue) | 接收队列，存放待接收的 buffer |

Port — 物理网卡端口：

| 参数 | 说明 |
|------|------|
| **LID** (Local Identifier) | InfiniBand 交换机分配 |
| **GID** (Global Identifier) | RDMA over Converged Ethernet 使用 |
| **多网卡支持** | 3FS 支持多张 IB/RoCE 网卡 (multi-rail) |

对端连接信息：

| 参数 | 说明 |
|------|------|
| `remote_lid` | 对端 LID |
| `remote_qpn` | 对端 QP Number |
| `remote_psn` | 初始 Packet Sequence Number |

---

## 三、内存层参数（最关键）

### 3.1 MR (Memory Region) — 注册到 RNIC 的内存区域

注册过程：

```c
struct ibv_mr *ibv_reg_mr(struct ibv_pd *pd,
                          void *addr,
                          size_t length,
                          int access);
```

| 参数 | 说明 |
|------|------|
| `addr` | 内存起始虚拟地址 |
| `length` | 注册区域大小 |
| `pd` | Protection Domain |

3FS 的 access flags：

| Flag | 说明 |
|------|------|
| `IBV_ACCESS_LOCAL_WRITE` | 本地可写 |
| `IBV_ACCESS_REMOTE_WRITE` | 远端可写（RDMA WRITE 操作） |
| `IBV_ACCESS_REMOTE_READ` | 远端可读（RDMA READ 操作） |
| `IBV_ACCESS_RELAXED_ORDERING` | 松散序，提高 CPU 侧性能 |

MR 返回的关键参数：

| 返回值 | 大小 | 说明 |
|--------|------|------|
| **rkey** (Remote Key) | 32 位 | 传给对端，对端凭此访问本端内存 |
| **lkey** (Local Key) | 32 位 | 本端 post WR 时使用 |
| `addr` | 64 位 | 注册区域起始虚拟地址 |
| `length` | 64 位 | 注册区域大小 |

### 3.2 为什么内存注册是必要的

```
RDMA 绕过 CPU 和内核:
  ┌──────────┐     DMA     ┌──────────┐
  │ 本端内存  │ ─────────→ │ 对端内存  │
  └──────────┘   (RNIC)    └──────────┘

  RNIC 需要知道:
  ├── 哪些虚拟地址可以被访问?     → addr + length
  ├── 虚拟地址对应的物理页在哪里?  → 通过 IOMMU/页表 (ibv_reg_mr 时建立)
  ├── 访问权限是什么?             → access flags
  └── 如何在 PCIe TLP 中标识?      → rkey/lkey

  未注册的内存无法被 RDMA 访问!
  ibv_reg_mr() 是代价较高的操作 (涉及页表锁定)
  → 3FS 使用预注册内存池, 启动时一次性注册
```

### 3.3 3FS 的预注册内存池

服务端 RDMABufPool：

| 类型 | 大小 | 数量 | 总量 | 用途 |
|------|------|------|------|------|
| 小 Buffer | 4 MB | 1024 | 4 GB | 常规读响应 |
| 大 Buffer | 64 MB | 64 | 4 GB | 超大读请求 |

```
启动时:
  for (i = 0; i < 1024; i++)
    buf = malloc(4 MB)
    mr = ibv_reg_mr(buf, 4MB, ACCESS_FLAGS)
    pool.add(buf, mr->lkey, mr->rkey)

运行时:
  buf = pool.allocate()     ← 无 ibv_reg_mr, 直接取用
  pool.deallocate(buf)      ← 不 ibv_dereg_mr, 归还池
```

客户端 IOBuffer：

| 参数 | 说明 |
|------|------|
| `registerIOBuffer()` | 客户端注册 RDMA buffer |
| 每个注册 buffer 获得 | rkey + addr |
| 服务端读响应时 | RDMA WRITE 直接写入客户端 buffer |

---

## 四、操作层参数

### 4.1 WR (Work Request) — 描述一次 RDMA 操作

```c
struct ibv_send_wr {
    uint64_t        wr_id;        // 应用自定义标识
    enum ibv_wr_opcode opcode;    // 操作类型
    uint32_t        send_flags;   // 标志位
    union {
        struct {
            uint64_t remote_addr;  // 对端内存地址
            uint32_t rkey;         // 对端 MR Key
        } rdma;                    // RDMA WRITE/READ 使用
    };
    struct ibv_sge *sg_list;       // SGE 数组
    int              num_sge;      // SGE 数量
};
```

### 4.2 SGE (Scatter-Gather Element) — 描述内存段

```c
struct ibv_sge {
    uint64_t addr;    // 内存虚拟地址 (需在已注册 MR 范围内)
    uint32_t length;  // 数据长度
    uint32_t lkey;    // 本地 MR Key
};
```

### 4.3 Opcode — 操作类型

| Opcode | 说明 | 3FS 用途 |
|--------|------|---------|
| `IBV_WR_SEND` | 发送消息到对端 RQ | 传输 RPC 消息 (包含 rkey + addr) |
| `IBV_WR_RDMA_WRITE` | 直接写入对端内存 | **读响应**: 服务端写数据到客户端 buffer |
| `IBV_WR_RDMA_READ` | 直接从对端内存读取 | **写请求**: 服务端从客户端 buffer 拉取数据 |
| `IBV_WR_SEND_WITH_IMM` | 发送消息 + 32 位立即数据 | 带通知的消息发送 |

### 4.4 3FS 的批量操作参数

| 参数 | 值 | 说明 |
|------|-----|------|
| `max_sge` | 16 | 每个 WR 最多 16 个 SGE |
| `max_rdma_wr` | 128 | 单 QP 最多 128 个 pending WR |
| `max_rdma_wr_per_post` | 32 | 每次 ibv_post_send 最多 32 个 WR |
| `send_buf_cnt` | 32 | 每个 IBSocket 的发送 buffer 数量 |
| `buf_size` | 16 KB | 每个 SEND buffer 大小 |

---

## 五、通知层参数

### 5.1 CQ (Completion Queue) — 完成通知队列

```c
struct ibv_cq *ibv_create_cq(struct ibv_context *context,
                             int cqe,
                             void *cq_context,
                             struct ibv_comp_channel *channel,
                             int comp_vector);
```

### 5.2 CQE / WC (Work Completion) — 完成事件

```c
struct ibv_wc {
    uint64_t    wr_id;      // 对应的 WR ID (匹配用)
    enum ibv_wc_status status;  // 完成状态
    enum ibv_wc_opcode opcode;  // 完成的操作类型
    uint32_t    byte_len;   // 传输的字节数
    uint32_t    qp_num;     // 完成的 QP Number
};
```

### 5.3 Signal 机制

| Flag | 说明 |
|------|------|
| `IBV_SEND_SIGNALED` | 此 WR 完成后产生 CQE |
| `IBV_SEND_UNSIGNALED` | 不产生 CQE（用于批量操作） |

3FS 的批量通知策略：

| 参数 | 值 | 效果 |
|------|-----|------|
| `buf_signal_batch` | 8 | 每 8 个 SEND WR 产生 1 个 CQE |
| `buf_ack_batch` | 8 | 每 8 个 RECV 产生 1 个 ACK |
| `event_ack_batch` | 128 | 每 128 个事件批量处理 |

```
示例: 发送 32 个消息

  不批量:  32 个 WR → 32 个 CQE → 32 次 CPU 中断
  3FS:    32 个 WR → 4 个 CQE  → 4 次 CPU 中断  (减少 8x)
```

---

## 六、完整数据传输参数链路

### 6.1 读响应（RDMA WRITE）— 服务端向客户端发送数据

```
Step 1: 客户端注册内存 (一次性)
  buf = malloc(4 MB)
  mr = ibv_reg_mr(buf, 4MB, IBV_ACCESS_REMOTE_WRITE)
  client_rkey = mr->rkey          ← 传给服务端
  client_addr = mr->addr          ← 传给服务端

Step 2: 客户端发送读请求 (SEND WR)
  SGE: RPC 消息体 (ChunkId, offset, length, client_rkey, client_addr)
  Opcode: IBV_WR_SEND
  → 消息到达服务端 RQ

Step 3: 服务端读取数据到本地 buffer
  AIO Worker: pread(chunk_file) → server_rdmabuf (4MB 预注册)

Step 4: 服务端 RDMA WRITE (零拷贝发送数据)
  WR:
    wr_id      = request_id          ← 匹配 CQE 用
    opcode     = IBV_WR_RDMA_WRITE
    send_flags = IBV_SEND_SIGNALED   ← 最后 1 个 WR 设信号

  WR.wr.rdma:
    remote_addr = client_addr         ← 客户端 buffer 虚拟地址
    rkey        = client_rkey         ← 客户端 MR Remote Key

  SGE[0]:
    addr   = server_rdmabuf_ptr       ← 服务端本地数据地址
    length = 4 MB                     ← 传输长度
    lkey   = server_rdmabuf_lkey      ← 服务端 MR Local Key

  执行: RNIC 直接从服务端内存 → 网络 → 客户端内存 (CPU 不参与)

Step 5: 客户端收到 CQE
  CQE.wr_id    = request_id
  CQE.status   = IBV_WC_SUCCESS
  CQE.byte_len = 4 MB

  CPU 全程不触碰数据!
```

### 6.2 写请求（RDMA READ）— 服务端从客户端拉取数据

```
Step 1: 客户端准备数据在注册 buffer 中
  memcpy(client_buf, user_data, 4 MB)

Step 2: 客户端发送写请求 (SEND WR)
  SGE: RPC 消息体 (ChunkId, offset, length, client_rkey, client_addr)
  Opcode: IBV_WR_SEND
  → 消息到达服务端 RQ

Step 3: 服务端 RDMA READ (零拷贝拉取数据)
  WR:
    opcode = IBV_WR_RDMA_READ
    wr_id  = request_id

  WR.wr.rdma:
    remote_addr = client_addr         ← 客户端数据源地址
    rkey        = client_rkey         ← 客户端 MR Key

  SGE[0]:
    addr   = server_rdmabuf_ptr       ← 服务端目标 buffer
    length = 4 MB
    lkey   = server_rdmabuf_lkey

  执行: RNIC 直接从客户端内存 → 网络 → 服务端内存 (CPU 不参与)

Step 4: 服务端收到 CQE → 数据已在 server_rdmabuf 中
Step 5: 服务端写入磁盘 (AIO pwrite)
Step 6: 服务端 SEND 回复客户端 (ACK)
```

---

## 七、两种数据传输模式对比

| 维度 | RDMA WRITE (读响应) | RDMA READ (写请求) |
|------|-------------------|-------------------|
| **方向** | 服务端 → 客户端 | 客户端 → 服务端 |
| **发起者** | 服务端 | 服务端 |
| **数据源** | 服务端本地 buffer | 客户端注册 buffer |
| **数据目标** | 客户端注册 buffer | 服务端本地 buffer |
| **rkey 来源** | 客户端传给服务端 | 客户端传给服务端 |
| **CPU 拷贝** | 0 次 | 0 次 |
| **使用场景** | batchRead 响应 | batchWrite 数据传输 |

---

## 八、关键参数汇总

| 参数 | 层级 | 类型 | 说明 | 3FS 值 |
|------|------|------|------|-------|
| `rkey` | 内存 | 32-bit | 远端内存访问密钥 | 预注册, 运行时不变 |
| `lkey` | 内存 | 32-bit | 本地 MR 密钥 | 预注册, 运行时不变 |
| `addr` | 内存 | 64-bit | 虚拟地址 | 预注册 buffer 地址 |
| `opcode` | 操作 | enum | WR 操作类型 | RDMA_WRITE / READ / SEND |
| `wr_id` | 操作 | 64-bit | 请求标识 | 匹配 CQE 用 |
| `num_sge` | 操作 | int | 每个 WR 的 SGE 数量 | 最大 16 |
| `remote_addr` | 操作 | 64-bit | 对端内存地址 | 客户端注册 buffer 的 addr |
| `SIGNALED` | 通知 | flag | 是否产生 CQE | 每 8 个 WR 设 1 个 |
| `max_rdma_wr` | 流控 | int | 单 QP 最大 pending WR | 128 |
| `rdmabuf_size` | 资源 | size | 服务端 buffer 大小 | 4 MB (小) / 64 MB (大) |
| `rdmabuf_count` | 资源 | int | 服务端 buffer 数量 | 1024 (小) / 64 (大) |

---
