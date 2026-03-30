# SPDK v23 读写 I/O 流程分析

## 一、总体架构

SPDK 采用**无锁轮询 (Lock-free Polling)** 架构，用户态直接驱动 NVMe 硬件，避免内核上下文切换和中断开销。

```
+====================================================================+
|                        Application Layer                             |
|  spdk_bdev_read() / spdk_bdev_write() / spdk_bdev_readv()          |
+====================================================================+
         |  bdev_io
         v
+====================================================================+
|                        BDEV Layer (块设备抽象)                     |
|  spdk_bdev_io -> bdev_io_submit() -> fn_table->submit_request()     |
|  (I/O 分片、QoS 排队、bounce buffer、锁检查)                         |
+====================================================================+
         |  bdev_io (driver_ctx = nvme_bdev_io)
         v
+====================================================================+
|                     bdev_nvme Module (NVMe 后端)                    |
|  bdev_nvme_readv() -> spdk_nvme_ns_cmd_readv()                     |
|  (多路径选择、SGL 回调、PI 校验、重试逻辑)                         |
+====================================================================+
         |  nvme_request
         v
+====================================================================+
|                     NVMe Transport Layer (传输抽象)                  |
|  nvme_qpair_submit_request() -> transport->qpair_submit_request()   |
|  (命令构建、PRP/SGL 建立、请求拆分)                                 |
+====================================================================+
         |  nvme_tracker
         v
+====================================================================+
|                     PCIe Transport (PCIe 实现)                       |
|  nvme_pcie_qpair_submit_tracker() -> SQ doorbell MMIO write         |
|  nvme_pcie_qpair_process_completions() -> CQ phase bit poll        |
|  (SQ/CQ 环形缓冲区、tracker 管理、shadow doorbell)                 |
+====================================================================+
         |  MMIO (PCIe BAR)
         v
+====================================================================+
|                     NVMe Controller (硬件)                            |
|  DMA 读取 SQ -> DMA 数据传输 -> DMA 写入 CQ                         |
+====================================================================+


+====================================================================+
|                     Event Framework (异步执行)                        |
|  Reactor (per lcore) -> SPDK Thread -> Poller (busy/timed)           |
|  spdk_app_start() -> reactor_run() -> spdk_thread_poll()            |
|  (无锁 ring、eventfd 通知、线程迁移)                                 |
+====================================================================+
```

## 二、完整读 I/O 时序流程图

```
  Application          BDEV Layer          bdev_nvme         NVMe Transport      PCIe Transport      NVMe HW
       |                    |                   |                    |                    |             |
       |  === I/O 提交 ===  |                   |                    |                    |             |
       |                    |                   |                    |                    |             |
       | spdk_bdev_read()  |                   |                    |                    |             |
       | buf, offset, len   |                   |                    |                    |             |
       |------------------->|                   |                    |                    |             |
       |                    | bdev_bytes_to_blocks()                   |                    |             |
       |                    | spdk_bdev_read_blocks()                  |                    |             |
       |                    | bdev_channel_get_io()                   |                    |             |
       |                    | 分配 bdev_io         |                    |                    |             |
       |                    | 填充: type=READ     |                    |                    |             |
       |                    | offset_blocks,     |                    |                    |             |
       |                    | num_blocks         |                    |                    |             |
       |                    |                   |                    |                    |             |
       |                    | bdev_io_submit()   |                    |                    |             |
       |                    | +-- 检查 LBA 锁     |                    |                    |             |
       |                    | +-- 检查是否需要拆分|                    |                    |             |
       |                    | +-- QoS 排队?      |                    |                    |             |
       |                    | +-- io_outstanding++                    |                    |             |
       |                    |                   |                    |                    |             |
       |                    | fn_table->submit_request()             |                    |             |
       |                    | (bdev_ch->channel) |                    |                    |             |
       |                    |------------------->|                    |                    |             |
       |                    |                   | bdev_nvme_submit   |                    |             |
       |                    |                   | _request()          |                    |             |
       |                    |                   |                    |                    |             |
       |                    |                   | [无缓冲区?]          |                    |             |
       |                    |                   | YES->spdk_bdev_io   |                    |             |
       |                    |                   | _get_buf()          |                    |             |
       |                    |                   | 异步分配缓冲区       |                    |             |
       |                    |                   | 有缓冲区后回调:      |                    |             |
       |                    |                   | bdev_nvme_get_buf_cb |                    |             |
       |                    |                   |                    |                    |             |
       |                    |                   | bdev_nvme_readv()   |                    |             |
       |                    |                   | +-- 多路径选择      |                    |             |
       |                    |                   | +-- ns, qpair       |                    |             |
       |                    |                   | +-- SGL 回调注册    |                    |             |
       |                    |                   |                    |                    |             |
       |                    |                   | spdk_nvme_ns_cmd   |                    |             |
       |                    |                   | _readv()           |                    |             |
       |                    |                   |------------------->|                    |             |
       |                    |                   |                    | _nvme_ns_cmd_rw()  |             |
       |                    |                   |                    | +-- 分配 request   |             |
       |                    |                   |                    | +-- 检查 max_xfer |             |
       |                    |                   |                    | +-- 拆分 (如需要)  |             |
       |                    |                   |                    | +-- 构建 NVMe 命令|             |
       |                    |                   |                    |   opc=READ        |             |
       |                    |                   |                    |   nsid, lba       |             |
       |                    |                   |                    |   lba_count       |             |
       |                    |                   |                    |                    |             |
       |                    |                   |                    | nvme_qpair_submit  |             |
       |                    |                   |                    | _request()        |             |
       |                    |                   |                    |------------------->|             |
       |                    |                   |                    |                   |             |
       |                    |                   |                    | PCIe transport:    |             |
       |                    |                   |                    | submit_tracker()  |             |
       |                    |                   |                    | +-- 获取 tracker   |             |
       |                    |                   |                    | +-- 分配 CID      |             |
       |                    |                   |                    | +-- 构建 PRP/SGL  |             |
       |                    |                   |                    |   spdk_vtophys()  |             |
       |                    |                   |                    |                    |             |
       |                    |                   |                    | copy cmd to SQ     |             |
       |                    |                   |                    | [SSE2 streaming]    |             |
       |                    |                   |                    | sq_tail++          |             |
       |                    |                   |                    |                    |             |
       |                    |                   |                    | sq_tdbl MMIO write |             |
       |                    |                   |                    | [doorbell]         |             |
       |                    |                   |                    |------------------>|             |
       |                    |                   |                    |                   | DMA 读 SQ   |
       |                    |                   |                    |                   | DMA 读数据  |
       |                    |                   |                    |                   | DMA 写 CQ   |
       |                    |                   |                    |                    |             |
       |  === I/O 完成 (Poller 轮询) ===                |             |
       |                    |                   |                    |                   |             |
       | reactor_run()      |                   |                    |                   |             |
       | +-- spdk_thread_poll()                  |                    |             |
       |     +-- NVMe poller (period=0)           |                    |             |
       |          |          |                   |                    |             |
       |          v          |                   |                    |             |
       | nvme_qpair_process_completions()         |                    |             |
       | <---------------------------------------|-------------------|--------------------|             |
       |                    |                   |                    | CQ phase bit 检查|
       |                    |                   |                    | cpl = cq[cq_head] |
       |                    |                   |                    | phase toggle     |
       |                    |                   |                    |                    |             |
       |                    |                   |                    | lookup tr[cid]    |             |
       |                    |                   |                    |                    |             |
       |                    |                   |                    | cq_hdbl MMIO write |             |
       |                    |                   |                    |-------------------->|             |
       |                    |                   |                    |                   |             |
       |                    |                   |                    | cb_fn(cb_arg, cpl) |             |
       |                    |                   | = bdev_nvme_readv   |             |
       |                    |                   |   _done(bio, cpl)    |             |
       |                    |                   |<-------------------|                    |             |
       |                    |                   |                    |                    |             |
       |                    |                   | [检查 PI 错误?]      |                    |             |
       |                    |                   | [需要重试?]         |                    |             |
       |                    |                   |                    |                    |             |
       |                    |                   | spdk_bdev_io_complete|                   |             |
       |                    |                   | _nvme_status()      |                    |             |
       |                    |                   |<-------------------|                    |             |
       |                    |                   |                    |                    |             |
       |                    | spdk_bdev_io_complete()                   |                    |             |
       |                    | +-- io_outstanding--|                    |                    |             |
       |                    | +-- 更新统计信息    |                    |                    |             |
       |                    |                   |                    |                    |             |
       |                    | bdev_io_complete() |                    |                    |             |
       |                    | [防递归: send_msg] |                    |                    |             |
       |                    |                   |                    |                    |             |
       | <-------------------|                   |                    |                    |             |
       |                    |                   |                    |                    |             |
       | APP cb(bdev_io,     |                   |                    |                    |             |
       |   success, ctx)    |                   |                    |                    |             |
```

## 三、完整写 I/O 时序流程图

写 I/O 的提交路径与读基本相同，主要区别：

```
  Application          BDEV Layer          bdev_nvme         NVMe Transport      PCIe Transport
       |                    |                   |                    |                    |
       | spdk_bdev_write() |                   |                    |                    |
       | buf, offset, len   |                   |                    |                    |
       |------------------->|                   |                    |                    |
       |                    | spdk_bdev_write_blocks()                  |                    |
       |                    | bdev_io_submit()   |                    |                    |
       |                    | fn_table->submit  |                    |                    |
       |                    |------------------->|                    |                    |
       |                    |                   | bdev_nvme_submit   |                    |
       |                    |                   | _request()          |                    |
       |                    |                   | bdev_nvme_writev()  |                    |
       |                    |                   |                    |                    |
       |                    |                   | spdk_nvme_ns_cmd   |                    |
       |                    |                   | _writev() /         |                    |
       |                    |                   |  _write()           |                    |
       |                    |                   |------------------->|                    |
       |                    |                   |                    | _nvme_ns_cmd_rw()  |             |
       |                    |                   |                    | opc=WRITE         |             |
       |                    |                   |                    |------------------->|             |
       |                    |                   |                    | 构建 PRP/SGL      |             |
       |                    |                   |                    | 写入 SQ + doorbell |             |
       |                    |                   |                    |------------------->| HW DMA Write|
       |                    |                   |                    |                    | HW 写 CQ   |
       |                    |                   |                    |                    |             |
       |                    |                   | [完成路径与读相同]     |                    |             |
       |                    |                   | bdev_nvme_writev    |                    |             |
       |                    |                   | _done(bio, cpl)      |                    |             |
       |                    |                   |                    |                    |             |
       |                    | spdk_bdev_io_complete()                   |                    |
       | <-------------------|                   |                    |                    |
       | APP cb(bdev_io, success, ctx)                                          |             |
```

**读写关键差异**:

| 维度 | 读 (READ) | 写 (WRITE) |
|------|----------|-----------|
| 缓冲区 | 可能需要延迟分配 (`spdk_bdev_io_get_buf`) | 应用提供，直接使用 |
| NVMe 命令 | `SPDK_NVME_OPC_READ` | `SPDK_NVME_OPC_WRITE` |
| 数据方向 | HW → 内存 (DMA Read) | 内存 → HW (DMA Write) |
| SGL 回调 | `reset_sgl_fn` + `next_sge_fn` 描述缓冲区 | 同读 |
| PI 校验 | 可检测 PI 错误后重新读取验证 | 可检测 PI 错误 |

## 四、PCIe 传输层详解

### 4.1 命令提交到硬件

```
  PCIe Transport: nvme_pcie_qpair_submit_request()
  |
  +-- 1. 获取 tracker (从 free_tr TAILQ)
  |     tr->cid = tracker 索引 (作为 Command ID)
  |     tr->req = nvme_request
  |
  +-- 2. 构建 PRP/SGL (如果 payload_size > 0)
  |     +-- PRP1: 数据缓冲区的第一页物理地址 (可能页内不对齐)
  |     +-- PRP2 或 PRP List:
  |     |     数据 <= 2 页: PRP2 直接指向第二页
  |     |     数据 > 2 页:  PRP2 指向 tracker 内嵌的 prp[503] 数组
  |     |     (每个 PRP 描述一页, 最多 503 页 = ~2MB)
  |     |
  |     +-- SGL (控制器支持时优先):
  |           按物理连续段构建 SGL 描述符
  |           可跨页连续, 比 PRP 更灵活
  |           描述符直接嵌入 cmd->dptr.sgl1 (单段) 或
  |           指向 tracker 内嵌的 sgl[250] 数组 (多段)
  |
  +-- 3. 复制 64 字节 NVMe 命令到 SQ
  |     nvme_pcie_copy_command(&sq[sq_tail], &cmd)
  |     使用 SSE2 _mm_stream_si128 (非时序存储, 避免缓存污染)
  |
  +-- 4. 推进 SQ tail
  |     sq_tail = (sq_tail + 1) % num_entries
  |
  +-- 5. 写 SQ Doorbell (MMIO)
        spdk_wmb()                    // 写内存屏障
        spdk_mmio_write_4(sq_tdbl, sq_tail)  // MMIO 写, 通知 HW
```

### 4.2 硬件完成处理

```
  PCIe Transport: nvme_pcie_qpair_process_completions()
  |
  +-- 循环检查 CQ:
  |     |
  |     +-- cpl = cq[cq_head]
  |     |
  |     +-- Phase Bit 检查:
  |     |     cpl.status.p == pqpair->flags.phase ?
  |     |       YES -> 新完成项, 继续处理
  |     |       NO  -> 无新完成, 退出循环
  |     |
  |     +-- 预取优化:
  |           __builtin_prefetch(&tr[cpl.cid])      // 预取 tracker
  |           __builtin_prefetch(&tr[cid].req->stailq) // 预取请求链
  |
  |     +-- 推进 CQ head:
  |           cq_head = (cq_head + 1) % num_entries
  |           if (cq_head == 0) phase = !phase  // 环绕时翻转
  |
  |     +-- 查找 tracker:
  |           tr = &pqpair->tr[cpl.cid]
  |           更新 sq_head = cpl.sqhd (控制器报告的 SQ 头)
  |
  |     +-- 完成 tracker:
  |           nvme_pcie_qpair_complete_tracker(qpair, tr, cpl)
  |           回收 tracker 到 free_tr
  |           调用 req->cb_fn(cb_arg, cpl)
  |
  +-- 写 CQ Doorbell (如果有完成):
        spdk_mmio_write_4(cq_hdbl, cq_head)

  +-- delay_cmd_submit 优化:
        如果有延迟的 SQ 提交, 此时一起写入 SQ doorbell
```

### 4.3 Shadow Doorbell 优化

```
  标准 Doorbell 流程:
  ===============
  每次提交/完成命令都写一次 MMIO (PCIe 写, ~1us)
  高 IOPS 时成为瓶颈

  Shadow Doorbell 优化:
  ======================
  Controller 支持 Shadow Doorbell (在 CMB 中分配):

  提交命令时:
    1. 更新内存中的 shadow_sq_tdbl (无 MMIO)
    2. 检查 controller 的 sq_eventidx
    3. 如果 eventidx 足够远 (控制器已处理旧命令)
           -> 才执行 MMIO 写 doorbell
       否则
           -> 跳过 MMIO 写 (控制器会主动读 shadow)

  效果: 大幅减少 MMIO 写次数, 高 IOPS 场景显著提升性能
```

### 4.4 Tracker 结构 (4KB 对齐)

```
  struct nvme_tracker (恰好 4096 字节 = 1 页)
  {
      struct nvme_request *req;         // 关联的请求
      uint16_t cid;                    // Command ID = tracker 索引
      uint64_t prp_sgl_bus_addr;        // 内嵌 PRP/SGL 数组的物理地址

      union {
          uint64_t prp[503];           // PRP 列表 (最多 503 页 = ~2MB)
          spdk_nvme_sgl_descriptor sgl[250]; // SGL 列表
      }
  }

  4KB 对齐保证: 内嵌的 PRP/SGL 数组不会跨页边界 (DMA 正确性要求)
```

## 五、Reactor 事件循环与 Poller

### 5.1 初始化流程

```
  main()
    |
    +-- spdk_app_start(&opts, start_fn, arg)
          |
          +-- app_setup_env()
          |     +-- spdk_env_init() -> DPDK EAL 初始化
          |     |     +-- hugepage 分配 (锁定所有可用大页)
          |     |     +-- PCI 设备枚举 (vfio-pci / uio_pci_generic)
          |     |     +-- lcore 绑定
          |
          +-- spdk_reactors_init()
          |     +-- g_reactors[lcore] = 每个 lcore 一个 reactor
          |     +-- 每个 reactor: events ring (65536)
          |     +-- g_spdk_event_mempool (262143 个 event 对象)
          |     +-- g_spdk_msg_mempool (262143 个 msg 对象)
          |
          +-- spdk_thread_create("app_thread")
          |     +-- 创建 app_thread, 绑定到 main core
          |
          +-- spdk_thread_send_msg(app_thread, bootstrap_fn)
          |
          +-- spdk_reactors_start()  <<<< 阻塞, 进入事件循环
                |
                +-- for each lcore (非 main):
                |     spdk_env_thread_launch_pinned(lcore, reactor_run)
                |     (POSIX thread 绑定到 lcore)
                |
                +-- reactor_run() on main core (主线程直接运行)
                      |
                      +-- [循环]
                      |     +-- event_queue_run_batch()
                      |     |     处理 reactor->events ring
                      |     |
                      |     +-- FOR_EACH lw_thread on reactor:
                      |     |     spdk_thread_poll(thread)
                      |     |       +-- 处理消息 (msg ring)
                      |     |       +-- 执行 active pollers (period=0)
                      |     |       +-- 执行 timed pollers
                      |     |
                      |     +-- [检查 g_reactor_state == RUNNING]
                      |
                      +-- [退出时]:
                            清理所有线程和资源

  bootstrap_fn() (在 app_thread 上):
    +-- spdk_subsystem_init()  -> bdev, nvmf, iscsi, vhost 子系统
    +-- app_start_rpc()
    +-- app_start_application()
          +-- g_start_fn(g_start_arg)  <<<< 用户的入口函数
```

### 5.2 NVMe Poller 工作方式

```
  Reactor 事件循环
  |
  +-- spdk_thread_poll(app_thread)
        |
        +-- 1. 执行 critical_msg (如果有)
        |
        +-- 2. 处理消息 (msg ring, 最多 32 条/批)
        |
        +-- 3. TAILQ_FOREACH_REVERSE active_pollers:
              |
              +-- [NVMe Admin Poller] (timed, 1s)
              |     spdk_nvme_ctrlr_process_admin_completions(ctrlr)
              |     -> 处理 admin qpair 完成 (keep-alive, 异步事件)
              |
              +-- [NVMe I/O Poller] (busy, period=0)  <<<< 关键 Poller
              |     bdev_nvme_qpair_poller()
              |     -> 遍历所有 I/O qpairs:
              |           spdk_nvme_qpair_process_completions(qpair, max)
              |           -> 检查 CQ phase bit
              |           -> 完成 I/O -> 回调链 -> 应用 callback
              |
              +-- [bdev Module Poller] (timed, 10us)
              |     bdev_module_poller()
              |     -> 处理 bdev 子系统定期任务
              |
              +-- [RPC Poller] (timed)
              |     spdk_rpc_server_poll()
              |     -> 处理 JSON-RPC 请求
```

### 5.3 SPDK Thread vs OS Thread

```
  OS Thread (POSIX)                 SPDK Thread (协程式)
  ===================                 ==========================

  每个 lcore 一个 OS thread         一个 OS thread 上多个 SPDK thread
  内核抢占调度                      协作式调度 (poll 模式)
  有独立栈                        无栈 (stackless)
  真正并行                        同一 reactor 上串行
  上下文切换代价高 (~us)            轮询切换代价极低 (~ns)

  示例:
  +-- lcore 0 (POSIX thread)
        +-- reactor_run()
              +-- SPDK Thread: "app_thread"
              |     +-- Poller: bdev_nvme_qpair_poller
              |     +-- Poller: rpc_poller
              |
              +-- SPDK Thread: "io_thread"
                    +-- Poller: nvmf_poll_group_poll
                    +-- Poller: vhost_poller
```

## 六、I/O Channel 层级关系

```
  spdk_bdev_get_io_channel(desc)
  |
  v
  spdk_bdev_channel
  +-- bdev = spdk_bdev
  +-- channel = spdk_io_channel (nvme_bdev io_device)
  +-- io_submitted: TAILQ (追踪所有在途 I/O)
  +-- io_outstanding: 计数
  |
  v
  nvme_bdev_channel (spdk_io_channel 的 ctx)
  +-- current_io_path: 当前活跃 I/O 路径
  +-- io_path_list: STAILQ (所有可用路径, 多路径)
  +-- retry_io_list: TAILQ (待重试 I/O)
  +-- retry_io_poller: 定期重试 Poller
  |
  v
  nvme_io_path (每个 namespace 一条)
  +-- nvme_ns -> spdk_nvme_ns
  +-- qpair -> nvme_qpair
  |
  v
  nvme_qpair
  +-- ctrlr -> nvme_ctrlr
  +-- qpair -> spdk_nvme_qpair  <<<< 实际的传输层 qpair
  +-- group -> nvme_poll_group
  |
  v
  spdk_nvme_qpair (PCIe transport)
  +-- cmd[]: Submission Queue (环形缓冲区)
  +-- cpl[]: Completion Queue (环形缓冲区)
  +-- tr[]: Tracker 数组
  +-- sq_tdbl, cq_hdbl: Doorbell MMIO 寄存器指针
  +-- phase: CQ Phase bit
```

## 七、数据结构关系图

```
  spdk_bdev_io
  +-- bdev: spdk_bdev *
  +-- type: SPDK_BDEV_IO_TYPE_READ/WRITE
  +-- u.bdev.iovs: iovec [] (scatter-gather)
  +-- u.bdev.num_blocks, offset_blocks
  +-- internal.cb: 应用回调函数
  +-- internal.caller_ctx: 应用上下文
  +-- internal.buf: bdev 分配的缓冲区
  +-- internal.status: I/O 状态
  +-- driver_ctx[0]: ----+
                                  |
                                  v
  nvme_bdev_io (40 bytes)
  +-- iovs: iovec *, iovcnt, iovpos, iov_offset
  +-- io_path: nvme_io_path * (多路径选择)
  +-- retry_count, retry_ticks
  |
  v
  nvme_request
  +-- cmd: spdk_nvme_cmd (64 字节 NVMe 命令)
  +-- payload_type: CONTIG / SGL
  +-- payload: union { contig_buf, sgl }
  +-- cb_fn, cb_arg: 回调
  +-- children: 分片子请求 (如果拆分)
  +-- parent: 父请求
  |
  v
  nvme_tracker (4096 bytes, 每个 qpair 有 num_entries-1 个)
  +-- req: nvme_request *
  +-- cid: uint16_t (Command ID)
  +-- prp[503] 或 sgl[250]: 内嵌 DMA 描述符
```

## 八、内存与 DMA

### 8.1 内存管理

```
  spdk_env_init() -> DPDK EAL
  |
  +-- hugepage: 锁定所有可用 2MB 大页
  +-- spdk_dma_malloc():
  |     从 hugepage 分配, 物理连续, 锁定不换出, DMA 安全
  +-- spdk_mempool_create():
        从 hugepage 预分配, 无锁 per-core cache, 快速分配/释放

  spdk_vtophys(buf):
  +-- DPDK memseg 查找 (2MB 粒度)
  +-- paddr = memseg->phys_addr + (vaddr & 0x1FFFFF)
  +-- 有 IOMMU 时: 使用 IOVA 映射
  +-- BAR 地址: 直接从 BAR base + offset 计算
```

### 8.2 PRP vs SGL 选择

```
  条件判断: 控制器是否支持 SGL?
  |
  +-- [SGL 支持 && 非 Admin && 非 DSM Quirk]
  |     YES -> SGL
  |       +-- 连续缓冲区: 单个 SGL 描述符 (直接嵌入 cmd->dptr.sgl1)
  |       +-- 散列缓冲区: 多个 SGL 描述符 (指向 tracker 内嵌 sgl[250])
  |       +-- 优势: 可跨页连续, 描述符更少
  |
  +-- [PRP (默认)]
        +-- PRP1: 第一页物理地址 (可页内不对齐)
        +-- PRP2: 第二页物理地址 (<=2 页时)
        +-- PRP List: tracker 内嵌 prp[503] (最多 503 页 ≈ 2MB)
        +-- 限制: 每个 PRP 恰好描述一页 (4KB)
```

## 九、初始化与 NVMe 连接

```
  spdk_app_start() 中的 NVMe 初始化:
  ========================================

  bootstrap_fn()
    |
    +-- spdk_subsystem_init()
    |     +-- bdev 子系统初始化
    |     +-- 加载 JSON 配置
    |     +-- 解析 subsystems 段:
    |           [Bdev]
    |           [NVMe]
    |
    +-- 用户 start_fn()
          |
          +-- spdk_nvme_connect(trid)
                |
                +-- 查找 PCIe transport
                +-- nvme_transport_ctrlr_construct()
                |     +-- spdk_pci_device_claim()
                |     +-- map BAR 0 (MMIO 寄存器)
                |     +-- 启用 busmaster (PCI CMD |= 0x404)
                |     +-- 读 CAP 寄存器 (DSTRD, MQES, etc.)
                |
                +-- nvme_ctrlr_connect()
                |     +-- 创建 Admin Qpair (qid=0, depth=32)
                |     +-- 发送 SET_FEATURES, IDENTIFY 等命令
                |     +-- 发现 Namespace
                |
                +-- 创建 I/O Qpair (通过 Admin 命令)
                |     +-- CREATE_IO_CQ (Admin 命令, 提供 CQ 物理地址)
                |     +-- CREATE_IO_SQ (Admin 命令, 提供 SQ 物理地址, 关联 CQ)
                |     +-- 配置 shadow doorbell (如果支持)
                |
                +-- 返回 spdk_nvme_ctrlr
```

## 十、关键源文件

### BDEV 层

| 文件 | 说明 |
|------|------|
| `include/spdk/bdev.h` | 公共 API: spdk_bdev_read/write/readv/writev |
| `include/spdk/bdev_module.h` | 模块接口: spdk_bdev_fn_table, spdk_bdev_io |
| `lib/bdev/bdev.c` | BDEV 核心: I/O 提交/完成/拆分/QoS/buffer 管理 |

### bdev_nvme 模块

| 文件 | 说明 |
|------|------|
| `module/bdev/nvme/bdev_nvme.c` | NVMe bdev 后端: submit_request, readv/writev, 完成处理 |
| `module/bdev/nvme/bdev_nvme.h` | 内部结构: nvme_bdev_channel, nvme_qpair, nvme_io_path |

### NVMe 核心层

| 文件 | 说明 |
|------|------|
| `include/spdk/nvme.h` | 公共 API: ctrlr, qpair, ns, transport 接口 |
| `lib/nvme/nvme_ns_cmd.c` | NVMe 命令构建: read/write, 拆分, PRP/SGL |
| `lib/nvme/nvme_qpair.c` | Qpair 提交/完成: submit_request, manual_complete |
| `lib/nvme/nvme_ctrlr.c` | Controller 管理: admin completions, keep-alive |
| `lib/nvme/nvme_transport.c` | 传输抽象: 注册/分派到具体 transport |

### PCIe 传输层

| 文件 | 说明 |
|------|------|
| `lib/nvme/nvme_pcie.c` | PCIe controller: BAR 映射, doorbell 计算, 热插拔 |
| `lib/nvme/nvme_pcie_common.c` | PCIe qpair: 命令提交, 完成处理, PRP/SGL 构建 |
| `lib/nvme/nvme_pcie_internal.h` | 内部结构: nvme_pcie_ctrlr, nvme_pcie_qpair, nvme_tracker |

### 事件/线程框架

| 文件 | 说明 |
|------|------|
| `include/spdk/event.h` | 公共 API: spdk_app_start, spdk_event |
| `include/spdk_internal/event.h` | 内部: spdk_reactor, spdk_lw_thread |
| `include/spdk/thread.h` | 线程 API: spdk_thread, spdk_poller |
| `lib/event/app.c` | 应用启动: spdk_app_start 初始化流程 |
| `lib/event/reactor.c` | Reactor: reactor_run 事件循环, 调度, 迁移 |
| `lib/thread/thread.c` | 线程: spdk_thread_poll, send_msg, poller 注册/执行 |

### 环境/内存

| 文件 | 说明 |
|------|------|
| `include/spdk/env.h` | 环境: DMA 分配, memzone, mempool, vtophys, ring |
| `lib/env_dpdk/memory.c` | vtophys 实现: 2MB memseg 查找, IOMMU 映射 |
| `lib/env_dpdk/pci.c` | PCI 设备: DPDK PCI 驱动, BAR 映射, 配置空间 |
