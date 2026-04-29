# SPDK Reactor 工作机制分析

## 一、Reactor 概览

SPDK Reactor 是整个 SPDK 应用的运行时核心——每个 DPDK lcore 上运行一个 Reactor，负责处理事件、轮询 I/O 完成、执行定时器和调度 SPDK Thread。

```
  DPDK EAL 初始化
  |
  |  lcore 0        lcore 1        lcore 2        lcore 3
  v
  +----------+   +----------+   +----------+   +----------+
  | Reactor 0|   | Reactor 1|   | Reactor 2|   | Reactor 3|
  |          |   |          |   |          |   |          |
  | threads: |   | threads: |   | threads: |   | threads: |
  |  app_thr |   |  io_thr  |   |  io_thr  |   |  nvmf_thr|
  |  rpc_thr |   |  vhost  |   |  iscsi   |   |  bdev_thr |
  |          |   |          |   |          |   |          |
  | polls:   |   | polls:   |   | polls:   |   | polls:   |
  |  nvme_qpr|   |  bdev_mod|   |  iscsi   |   |  nvmf_pg |
  |  bdev_mod|   |  vhost_pq|   |  login   |   |  qpair_pq |
  +----------+   +----------+   +----------+   +----------+
       |                |                |                |
       +----------------+----------------+----------------+
                         |
                    DPDK MP_SC Ring (跨核事件通信)
```

## 二、Reactor 核心数据结构

```c
// include/spdk_internal/event.h

struct spdk_reactor {                         // 每个 lcore 一个, 64 字节对齐
    TAILQ_HEAD(, spdk_lw_thread) threads;  // 该 reactor 上的所有 SPDK 线程
    uint32_t thread_count;
    uint32_t lcore;                        // 逻辑核编号

    struct spdk_ring *events;                // MP_SC ring, 容量 65536
    int events_fd;                           // eventfd (中断模式下唤醒 epoll)

    struct spdk_cpuset notify_cpuset;        // 需要通知的中断模式 reactor 集合
    bool in_interrupt;                       // 是否处于中断模式
    struct spdk_fd_group *fgrp;               // fd_group (epoll, 中断模式)
    int resched_fd;                          // eventfd (线程迁移通知)

    uint64_t busy_tsc, idle_tsc;             // 累计繁忙/空闲 TSC
    uint64_t tsc_last;                       // 上次时间戳

    struct spdk_lw_thread *thread_scratch;     // 单线程临时上下文
    uint64_t context_switch_monitor_tsc;       // 上下文切换监控时间
    struct rusage rusage;                     // getrusage 统计
};

struct spdk_lw_thread {                        // 桥接 SPDK Thread 和 Reactor
    TAILQ_ENTRY(spdk_lw_thread) link;
    uint64_t tsc_start;
    uint32_t lcore;                          // 当前所在 lcore
    bool resched;                            // 是否需要迁移到其他 reactor
    struct spdk_thread_stats total_stats;      // 全生命周期统计
    struct spdk_thread_stats current_stats;    // 调度周期统计 (差值)
};

struct spdk_event {                          // 一次性事件, 投递到指定 reactor
    uint32_t    lcore;                        // 目标 reactor
    spdk_event_fn   fn;                        // 回调函数
    void        *arg1, *arg2;                // 回调参数
};
```

## 三、Reactor 状态机

```
                    spdk_reactors_init()
                         |
                         v
  UNINITIALIZED -----> INITIALIZED
                         |
                    spdk_reactors_start()
                         |
                         v
                     RUNNING
                       /     \
          [正常退出]     [SIGINT/SIGTERM]
               |               |
               |    spdk_app_    |
               |    start_shutdown()
               |               |
               v               v
            EXITING <---------+
               |
          _reactors_stop() 设置状态
          + 通知所有 reactor 退出循环
               |
               v
           SHUTDOWN
          (所有 reactor 线程已退出)
```

## 四、Reactor 主循环

### 4.1 主循环入口

```c
// lib/event/reactor.c

static int reactor_run(void *arg)
{
    struct spdk_reactor *reactor = arg;
    reactor->tsc_last = spdk_get_ticks();

    while (1) {
        // ===== 检查退出条件 =====
        if (g_reactor_state != SPDK_REACTOR_STATE_RUNNING) {
            break;
        }

        // ===== 根据 interrupt 模式选择执行方式 =====
        if (reactor->in_interrupt) {
            reactor_interrupt_run(reactor);    // 阻塞在 epoll_wait
        } else {
            _reactor_run(reactor);            // 忙轮询 (不阻塞)
        }

        // ===== 上下文切换监控 (每 1 秒) =====
        if (g_framework_context_switch_monitor_enabled) {
            if ((reactor->last_rusage + g_rusage_period) < reactor->tsc_last) {
                get_rusage(reactor);       // getrusage(RUSAGE_THREAD)
                reactor->last_rusage = reactor->tsc_last;
            }
        }

        // ===== 定期调度 (负载均衡) =====
        if (g_scheduler_period > 0 &&
            (reactor->tsc_last - last_sched) > g_scheduler_period &&
            reactor == g_scheduling_reactor &&
            !g_scheduling_in_progress) {
            last_sched = reactor->tsc_last;
            g_scheduling_in_progress = true;
            _reactors_scheduler_gather_metrics(NULL, NULL);
        }
    }

    // ===== 退出处理 =====
    // 1. 强制退出 reactor 上的所有 SPDK Thread
    // 2. 轮询直到所有 Thread 已退出和销毁
    return 0;
}
```

### 4.2 Poll 模式 (`_reactor_run`)

```
  _reactor_run(reactor)
  |
  +-- event_queue_run_batch(reactor)         <<<< 处理事件 ring
  |     |
  |     +-- SPDK_RING_TYPE_MP_SC ring_dequeue(events, batch[8])
  |     +-- 对每个 event: event->fn(arg1, arg2)
  |     +-- event 返回 mempool
  |
  +-- TAILQ_FOREACH lw_thread in reactor->threads:
        |
        +-- spdk_thread_poll(thread, 0, tsc_last)
        |     |
        |     +-- [1] 执行 critical_msg (如果有, 原子交换取走)
        |     |
        |     +-- [2] msg_queue_run_batch()
        |     |     +-- ring_dequeue(thread->messages, msgs[32])
        |     |     +-- 对每个 msg: msg->fn(msg->arg)
        |     |     +-- msg 返回 cache 或 mempool
        |     |
        |     +-- [3] TAILQ_FOREACH_REVERSE active_pollers:
        |     |     +-- poller->fn(poller->arg)   <<<< 周期=0 的忙轮询
        |     |     +-- poller 移到尾部 (轮转)
        |     |
        |     +-- [4] Walk timed_pollers RB-Tree:
        |           +-- next_run_tick <= now ?
        |           +     YES -> 执行, 重新插入 (新 next_run_tick)
        |           +     NO  -> 跳过
        |
        +-- reactor_post_process_lw_thread(reactor, lw_thread)
              |
              +-- [resched?] -> YES:
              |     lw_thread->resched = false
              |     从 reactor 移除线程
              |     _reactor_schedule_thread(thread)  -> 重新调度到新 reactor
              |
              +-- [thread 状态 == EXITING?]
                    强制退出
```

### 4.3 中断模式 (`reactor_interrupt_run`)

```
  reactor_interrupt_run(reactor)
  |
  +-- spdk_fd_group_wait(reactor->fgrp, -1)   <<<< 阻塞在 epoll_wait
        |
        |     [events_fd 可读]
        |     -> event_queue_run_batch()
        |        (ACK eventfd -> ring_dequeue -> 执行 events)
        |        (如果有更多 event -> re-trigger 通知)
        |
        |     [resched_fd 可读]
        |     -> reactor_schedule_thread_event()
        |        (读取 resched, 处理线程迁移)
        |
        |     [thread[i].interruptfd 可读]  (每个 SPDK Thread 各有一个)
        |     -> thread_process_interrupts()
        |        (更新 idle_tsc -> spdk_thread_poll(thread))
```

### 4.4 Poll 模式 vs 中断模式 对比

```
  Poll 模式 (默认)                     中断模式 (需显式启用)
  ================                     =======================

  主循环: 忙轮询, 不阻塞               主循环: epoll_wait 阻塞

  CPU 占用: 100% (busy spin)           CPU 占用: ~0% (空闲时睡眠)

  延迟: 极低 (~ns 级)                延迟: 较高 (~us 级, 取决于 epoll)

  吞吐: 极高                         吞吐: 取决于 I/O 频率
  适用: 高性能存储, 低延迟场景           适用: CPU 敏感, 低 IOPS 场景

  Poller 执行: 直接函数调用             Poller 触发:
    - busy poller: 每次循环调用             - timerfd: 定时器到期触发
    - timed poller: tick 检查执行           - eventfd: 水平触发 (一直 ready)

  通知机制: 不需要                       通知机制: eventfd + epoll
```

## 五、SPDK Thread 调度

### 5.1 Thread 创建与调度

```
  spdk_thread_create("io_thread", &cpumask)
  |
  +-- 分配 spdk_thread 结构
  +-- thread->messages = ring_create(MP_SC, 65536)
  +-- thread->msg_cache: 预分配 1024 个 msg 对象
  |
  +-- SPDK_THREAD_OP_NEW:
        |
        +-- reactor_thread_op() 设置 lw_thread->lcore = LCORE_ID_ANY
        +-- _reactor_schedule_thread(lw_thread)
              |
              +-- [指定 lcore?] -> 使用该 lcore
              |
              +-- [LCORE_ID_ANY?] -> 轮询调度:
                    |
                    +-- pthread_mutex_lock(&g_scheduler_mtx)
                    +-- g_next_core 从当前 core 开始轮询
                    +-- core = g_next_core
                    +-- g_next_core = next_core
                    +-- 检查 cpuset 是否允许该 core
                    +-- pthread_mutex_unlock
                    |
                    +-- spdk_event_allocate(core, _schedule_thread, lw_thread)
                    +-- spdk_event_call(event)
                          |
                          +-- 目标 reactor 的 events ring 入队
                          +-- [中断模式] write(events_fd) 唤醒 epoll
```

### 5.2 线程迁移 (`resched` 机制)

```
  调度器 balance() 决定将 Thread A 从 Reactor 0 迁移到 Reactor 2
       |
       v
  _threads_reschedule_thread(thread_info)
  |
  +-- lw_thread->resched = true
  +-- lw_thread->lcore = 2  (目标 reactor)
  |
  v
  下一次 Reactor 0 轮询 Thread A 后:
  reactor_post_process_lw_thread(reactor, lw_thread)
  |
  +-- 检测 resched == true
  +-- lw_thread->resched = false
  +-- TAILQ_REMOVE 从 Reactor 0 的 threads 链表
  +-- _reactor_schedule_thread(thread)
        +-- spdk_event_allocate(2, _schedule_thread, lw_thread)
        +-- spdk_event_call(event)
              +-- 目标 Reactor 2 的 events ring 入队
              +-- [中断模式] 唤醒 Reactor 2
              |
              v
  Reactor 2 下一次循环:
  event_queue_run_batch() -> _schedule_thread()
  +-- TAILQ_INSERT 到 Reactor 2 的 threads 链表
  +-- Thread A 开始在 Reactor 2 上执行 poll
```

### 5.3 负载均衡 (三阶段)

```
  g_scheduler_period > 0 时 (默认=0, 静态调度器不启用)
  由 g_scheduling_reactor (init 时的 main core) 定期触发

  Phase 1: 收集指标 (_reactors_scheduler_gather_metrics)
  =============================================
  g_scheduling_reactor
    |
    +-- 分发 event 到 Reactor 1: _reactors_scheduler_gather_metrics()
    |     -> 快照 Reactor 1 的 idle/busy TSC 和线程统计
    |     -> 分发 event 到 Reactor 2...
    |     -> 分发 event 到 Reactor N
    |
    +-- 回到 g_scheduling_reactor:
          _reactors_scheduler_balance()

  Phase 2: 均衡决策 (_reactors_scheduler_balance)
  ===========================================
  scheduler->balance(g_core_infos, g_reactor_count)
  |
  +-- 默认静态调度器: balance() 为空操作 (不迁移)
  +-- 自定义调度器可覆盖此函数:
        - 分析各 reactor 的 busy/idle 比例
        - 将繁忙 reactor 的线程迁移到空闲 reactor
        - 修改 thread_infos[i].lcore 字段

  Phase 3: 执行迁移 (_threads_reschedule)
  ===========================================
  _reactors_scheduler_fini()
    -> _threads_reschedule()
       -> 遍历所有 thread_infos:
            如果 lcore != 当前 lcore:
                 _threads_reschedule_thread()
                   +-- 设置 resched=true, lcore=新值

  Phase 4: 更新中断模式 (_reactors_scheduler_update_core_mode)
  ======================================================
  如果调度器修改了 g_core_infos[i].interrupt_mode:
    -> spdk_reactor_set_interrupt_mode() 切换 reactor 模式
```

### 5.4 `spdk_for_each_reactor()` — 跨 Reactor 事件跳转

```
  spdk_for_each_reactor(nop_fn, arg1, arg2, cpl_fn)
  |
  +-- 分发 event 到 Reactor 0: on_reactor(core=0, fn=nop_fn)
  |     Reactor 0: nop_fn() + 分发 event 到 Reactor 1
  |     Reactor 1: nop_fn() + 分发 event 到 Reactor 2
  |     ...
  |     Reactor N: nop_fn() + 分发 event 回 Reactor 0
  |
  +-- Reactor 0: cpl_fn() 完成回调
        -> 设置 g_reactor_state = EXITING (用于 shutdown)
```

```
  事件跳转示意图:
  Reactor 0 ----event----> Reactor 1 ----event----> Reactor 2 ----event----> ...
       |                                                                   |
       +--------------------------event------------------------------+
                                                    |
  cpl_fn() 在 Reactor 0 执行 (回到原始 reactor)
```

## 六、Event Ring 机制

### 6.1 Ring 类型

```
  SPDK_RING_TYPE_MP_SC (Multi-Producer, Single-Consumer)
  ================================================

  Producer: 任意线程 (任何 reactor, 任何 SPDK thread)
  Consumer: 仅 Ring 所属的 Reactor

  特性:
    +-- 无锁入队 (CAS 操作)
    +-- 批量出队 (每次最多 8 个 event)
    +-- 容量 65536 个 event
    +-- 每个事件 24 字节 (lcore + fn + arg1 + arg2)

  内存池: g_spdk_event_mempool (262143 个 event 对象)
    从 DPDK hugepage 分配, cache-line 对齐, 无锁分配/释放
```

### 6.2 事件生命周期

```
  事件分配:
  ===========
  event = spdk_event_allocate(target_lcore, fn, arg1, arg2)
    +-- spdk_mempool_get(g_spdk_event_mempool)
    +-- event->lcore = target_lcore
    +-- event->fn = fn
    +-- event->arg1 = arg1
    +-- event->arg2 = arg2

  事件提交:
  ===========
  spdk_event_call(event)
    +-- reactor = g_reactors[event->lcore]
    +-- spdk_ring_enqueue(reactor->events, &event, 1, NULL)
    +-- [如果调用者不在 reactor 上, 或目标 reactor 在中断模式]
    |     write(reactor->events_fd, &notify, 8)  // eventfd 唤醒
    |
  +-- [Poll 模式] 下次 _reactor_run() 自动处理
  +-- [中断模式] epoll_wait 被 events_fd 唤醒后处理

  事件执行:
  ===========
  event_queue_run_batch()
    +-- [中断模式] read(events_fd) ACK 防丢失
    +-- spdk_ring_dequeue(reactor->events, events[8], 8)
    +-- for each event:
          +-- spdk_set_thread(thread)  // 设置线程上下文
          +-- event->fn(event->arg1, event->arg2)
          +-- spdk_set_thread(NULL)
    +-- spdk_mempool_put_bulk(g_spdk_event_mempool, events, count)

  事件返回:
  ===========
    回到 g_spdk_event_mempool (释放)
```

## 七、CPU Core 锁定机制

### 7.1 锁定文件

```
  /var/tmp/spdk_cpu_lock_0    (lcore 0)
  /var/tmp/spdk_cpu_lock_1    (lcore 1)
  /var/tmp/spdk_cpu_lock_2    (lcore 2)
  /var/tmp/spdk_cpu_lock_3    (lcore 3)
  ...
  /var/tmp/spdk_cpu_lock_N    (最多 128 个)
```

### 7.2 锁定流程

```
  spdk_app_start()
    |
    +-- g_core_locks[0..N] = -1  (初始化为未锁定)
    |
    +-- claim_cpu_cores()
          |
          +-- for each core in DPDK core mask:
                |
                +-- open("/var/tmp/spdk_cpu_lock_N", O_RDWR | O_CREAT)
                +-- mmap(fd, sizeof(int))    (共享映射)
                |
                +-- fcntl(fd, F_SETLK, F_WRLCK)
                |     |
                |     [成功]
                |     +-- *core_map = getpid()  (写入当前进程 PID)
                |     +-- g_core_locks[core] = fd  (保持 fd 打开以持锁)
                |     |
                |     [失败]
                |     +-- read core_map -> 获取 PID
                |     +-- unclaim_cpu_cores()  (释放已获取的锁)
                |     +-- return -1
                |     +-- 打印: "Core N is locked by PID P"
```

### 7.3 锁定用途

```
  防止多个 SPDK 实例使用相同的 CPU core:
  +-- 实例 A 启动, 锁定 lcore 0-3
  +-- 实例 B 尝试启动, lcore 0-3 锁定失败 -> 退出
  +-- 避免两个进程同时操作同一 PCIe 设备 (通过同一个 lcore)

  可通过 -L / --disable-cpumask-locks 禁用
  (适用于容器环境或受控测试环境)
```

## 八、中断模式详解

### 8.1 启用方式

```
  必须在 spdk_thread_lib_init() 之前调用:
    spdk_interrupt_mode_enable();

  仅支持 Linux (使用 eventfd + timerfd + epoll)
```

### 8.2 Poller 在中断模式下的行为变化

```
  注册 Poller 时:
  ==============================

  spdk_poller_register(fn, arg, period_us)
       |
       +-- [poll 模式]
       |     period=0 -> 插入 active_pollers (每次循环执行)
       |     period>0 -> 插入 timed_pollers RB-Tree (tick 检查)
       |
       +-- [中断模式]
             period=0 -> 创建 eventfd
             +-- eventfd 水平触发 (write一次, 永久 ready)
             +-- 注册到 thread->fgrp
             +-- epoll_wait 触发 -> fn(arg)
                    |
             period>0 -> 创建 timerfd
             +-- timerfd_settime() 设置重复到期
             +-- 注册到 thread->fgrp
             +-- epoll_wait 触发 -> fn(arg)
                    +-- 重新设置下次到期时间
```

### 8.3 Reactor 级中断模式切换

```
  spdk_reactor_set_interrupt_mode(reactor, enable)
  |
  +-- [切换到中断模式]
        1. 在所有 reactor 的 notify_cpuset 中设置 reactor 位
        2. reactor->in_interrupt = true
        3. write(events_fd) + write(resched_fd) 唤醒 reactor
        |
  +-- [切换到 Poll 模式]
        1. reactor->in_interrupt = false
        2. 从所有 reactor 的 notify_cpuset 中清除 reactor 位
        (保证切换后不会有其他 reactor 向其发通知)
```

## 九、关闭流程完整时序图

```
  User: Ctrl+C / SIGINT
       |
       v
  Signal Handler: __shutdown_signal()
       |
       +-- spdk_app_start_shutdown()
             |
             +-- spdk_thread_send_critical_msg(g_app_thread, app_start_shutdown)
                   (绕过 ring, 原子设置 critical_msg)
                   |
                   v
  app_thread: app_start_shutdown()
       |
       +-- [有 shutdown callback?]
             YES -> g_spdk_app.shutdown_cb(rc)  (用户自定义关闭逻辑)
             NO  -> spdk_app_stop(0)
                   |
                   v
  app_thread: app_stop(rc)
       |
       +-- spdk_thread_send_msg(g_app_thread, app_stop)
             (通过 msg ring, 入队到 app_thread 消息队列)
             |
             v
  app_thread: app_stop()
       |
       +-- spdk_rpc_finish()
       +-- g_spdk_app.stopped = true
       +-- _start_subsystem_fini()
             |
             +-- [有调度在进行中?]
             |     YES -> 重新发送 msg 给自己 (spin-wait)
             |
             +-- spdk_subsystem_fini(spdk_reactors_stop, NULL)
                   |
                   v
  所有子系统 fini (bdev, nvmf, iscsi, vhost, ...)
       |
       +-- [fini 完成, 调用回调]
             spdk_reactors_stop(NULL)
             |
             v
  spdk_reactors_stop():
       |
       +-- g_stopping_reactors = true (阻止新的跨 reactor 操作)
       +-- spdk_for_each_reactor(nop, NULL, NULL, _reactors_stop)
             |
             +-- 跳转到每个 reactor: _reactors_stop()
                   +-- g_reactor_state = EXITING
                   +-- write(events_fd) 唤醒每个 reactor (含中断模式)
             |
             +-- 回到调用者 reactor: g_reactor_state = EXITING
                   |
                   v
  所有 Reactor 退出主循环:
  reactor_run():
       |
       +-- while (1):
       |     +-- g_reactor_state != RUNNING -> break
       |     +-- [Poll/中断模式执行]
       |
       +-- break 后:
             +-- 强制 spdk_thread_exit() 所有非 app_thread 线程
             +-- 轮询直到所有 Thread 状态 = EXITED + 已销毁
             |
             v
       return 0  (reactor_run 返回)

  +-- spdk_env_thread_wait_all()
  |     (等待所有非 main core 的 reactor 线程退出)
  |
  +-- g_reactor_state = SHUTDOWN
  |
  v
  spdk_app_start() 返回 g_spdk_app.rc
```

## 十、统计监控

### 10.1 指标层次

```
  +-- Per-Reactor 统计 (reactor->busy_tsc, reactor->idle_tsc)
  |     每次 reactor_run() 迭代中更新
  |     Poll 模式: 直接根据 spdk_thread_poll() 返回值累加
  |     中断模式: 根据 thread_process_interrupts() 返回值累加
  |
  +-- Per-Thread 统计 (thread->stats.busy_tsc, thread->stats.idle_tsc)
  |     全生命周期累加
  |
  +-- Per-LW-Thread 统计 (lw_thread->current_stats)
  |     调度周期差值 (当前周期 - 上次快照)
  |     用于负载均衡决策
  |
  +-- 上下文切换监控 (getrusage)
        每 1 秒采样 RUSAGE_THREAD
        记录 voluntary/involuntary context switch 次数
```

### 10.2 指标采集时序

```
  _reactor_run() 每次迭代:
  |
  +-- TAILQ_FOREACH lw_thread:
        |
        +-- start_tsc = reactor->tsc_last
        +-- rc = spdk_thread_poll(thread)
        +-- end_tsc = thread.last_tsc
        |
        +-- rc == 0 (空闲):
        |     reactor->idle_tsc += (end_tsc - start_tsc)
        |
        +-- rc > 0 (繁忙):
        |     reactor->busy_tsc += (end_tsc - start_tsc)
        |
        +-- reactor->tsc_last = end_tsc
```

## 十一、关键源文件

| 文件 | 说明 |
|------|------|
| `lib/event/reactor.c` | Reactor 核心: 主循环、调度、事件处理、中断模式、关闭 |
| `lib/event/app.c` | 应用生命周期: start/stop/shutdown、CPU 锁定、子系统初始化 |
| `lib/thread/thread.c` | 线程库: poll/poller 注册/执行、中断模式、统计 |
| `lib/event/scheduler_static.c` | 默认调度器 (no-op) |
| `include/spdk_internal/event.h` | Reactor/LWThread/Event 内部结构定义 |
| `include/spdk/event.h` | 公共 API: spdk_app_start, spdk_event |
| `include/spdk/thread.h` | 公共 API: spdk_thread, spdk_poller, spdk_io_channel |
| `include/spdk/scheduler.h` | 调度器接口和数据结构 |
