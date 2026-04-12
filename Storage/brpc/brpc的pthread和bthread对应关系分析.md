# brpc pthread 与 bthread 对应关系及同步机制分析

## 目录

1. [pthread 与 bthread 的 M:N 映射](#1-pthread-与-bthread-的-mn-映射)
2. [pthread 与 TaskGroup 的 1:1 绑定](#2-pthread-与-taskgroup-的-11-绑定)
3. [bthread 为什么需要 Butex](#3-bthread-为什么需要-butex)
4. [bthread 的协作式调度与 Butex 的配合](#4-bthread-的协作式调度与-butex-的配合)
5. [bthread 同步原语全景](#5-bthread-同步原语全景)
6. [典型场景中的 Butex 使用](#6-典型场景中的-butex-使用)
7. [为什么不用 pthread 互斥锁](#7-为什么不用-pthread-互斥锁)
8. [完整时序分析](#8-完整时序分析)

---

## 1. pthread 与 bthread 的 M:N 映射

### 1.1 映射关系

```
默认配置: bthread_concurrency = 8 → 8 个 Worker pthread + 1 个 idle pthread = 9 个 pthread

pthread 0  ←→  TaskGroup 0  ←→  bthread A, B, C, D, ...
pthread 1  ←→  TaskGroup 1  ←→  bthread E, F, G, ...
pthread 2  ←→  TaskGroup 2  ←→  bthread H, I, J, ...
...
pthread 7  ←→  TaskGroup 7  ←→  bthread K, L, M, ...
pthread 8  ←→  TaskGroup 8  ←→  (idle / steal only)
```

### 1.2 关键特征

| 特征 | 说明 |
|---|---|
| 绑定关系 | 1 个 pthread 绑定 1 个 TaskGroup（生命周期一致） |
| 调度关系 | N 个 bthread 共享 M 个 pthread（M 远小于 N） |
| 栈 | 每个 pthread 有自己的系统栈（8MB）；每个 bthread 有独立的用户态栈（默认 1MB） |
| 切换 | bthread 之间的切换是用户态上下文切换（~10-20ns），不涉及内核 |
| 同一时间 | 每个 pthread 只执行 1 个 bthread，其他 bthread 挂起 |

---

## 2. pthread 与 TaskGroup 的 1:1 绑定

### 2.1 创建流程

```mermaid
sequenceDiagram
    participant MAIN as 主线程
    participant TC as TaskControl
    participant P as Worker pthread
    participant TG as TaskGroup
    participant WSQ as WorkStealingQueue
    participant PL as ParkingLot

    MAIN->>TC: TaskControl.init(concurrency=8)

    loop 创建 9 个 Worker pthread
        MAIN->>P: pthread_create(worker_main, tg_index)
        P->>TG: TaskGroup 初始化
        TG->>WSQ: init(capacity=4096)
        TG->>TG: 创建 epoll fd
        TG->>TG: 初始化 steal_seed, steal_offset
        TG->>PL: 绑定 ParkingLot (hash 分片)
        TG->>TG: 注册到全局 _tls_task_groups[]
        P->>TG: run_main_task()
    end
```

### 2.2 绑定数据结构

```c
// 每个 pthread 有一个 TLS 变量指向自己的 TaskGroup
__thread TaskGroup* tls_task_group = NULL;

// 全局数组: 所有 TaskGroup 按索引存储
// TaskControl::_groups[tag][index]
// 用于 steal 时遍历其他 TaskGroup
```

### 2.3 生命周期一致性

```
pthread 创建  →  TaskGroup 创建    →  pthread 运行调度循环
pthread 退出   →  TaskGroup 销毁    →  所有 bthread 必须完成
```

**pthread 不会销毁直到它上面所有的 bthread 执行完毕**。`TaskControl::stop_and_join()` 会等待所有 bthread 完成。

---

## 3. bthread 为什么需要 Butex

### 3.1 核心问题

虽然任务已经分派给 bthread 执行，但 bthread 之间存在**协作关系**，需要同步：

```mermaid
graph LR
    subgraph "pthread 0"
        BT_A["bthread A (发送 RPC)"]
        BT_B["bthread B (等待响应)"]
    end

    BT_A -->|"1. 发出请求"| NET["网络"]
    NET -->|"2. 响应到达"| BT_A
    BT_A -->|"3. butex_wake"| BT_B
```

**问题**：bthread B 需要等待 bthread A（或其他异步事件）的结果。在同一个 pthread 上，bthread B 不能 `sleep()` 或 `pthread_cond_wait()`，因为那会阻塞整个 pthread，导致其他 bthread 都无法执行。

### 3.2 三种等待场景

| 场景 | 需要等待的对象 | 不能用的方式 | 正确方式 |
|---|---|---|---|
| 等待 RPC 响应 | 其他 bthread 或 I/O 事件 | `sleep()`（阻塞 pthread） | `butex_wait()` + yield |
| 等待定时器 | 时间到期 | `nanosleep()`（阻塞 pthread） | `bthread_usleep()` + yield |
| 等待 I/O | fd 可读/可写 | `epoll_wait()`（阻塞 pthread） | `bthread_fd_wait()` + yield |
| 等待写缓冲区 | Socket 可写 | 忙等轮询（浪费 CPU） | yield + EPOLLOUT |

### 3.3 Butex 的本质

**Butex = bthread mutex**，但它不是互斥锁，而是一个**条件等待原语**（类似 futex）：

```c
butex_wait(butex, expected_val):
    if butex.value == expected_val:
        将当前 bthread 加入 butex 等待队列
        yield（让出 pthread，调度其他 bthread）
    // 被唤醒后从这里恢复

butex_wake(butex):
    从 butex 等待队列取出 bthread
    推入该 bthread 所属 TaskGroup 的 WSQ 或远程 RQ
    signal_task() 唤醒 idle worker
```

**关键区别**：

| 特性 | pthread mutex/cond | butex |
|---|---|---|
| 阻塞对象 | pthread（内核线程） | bthread（用户态协程） |
| 阻塞代价 | 内核上下文切换（~1-10μs） | 用户态上下文切换（~10-20ns） |
| 唤醒目标 | 特定 pthread | 特定 bthread（可能在不同 pthread 上恢复执行） |
| 与 M:N 的兼容性 | 不兼容（阻塞 pthread 会饿死其他 bthread） | 完全兼容（yield 让出 pthread） |

---

## 4. bthread 的协作式调度与 Butex 的配合

### 4.1 yield 是一切的基础

```mermaid
sequenceDiagram
    participant BT_A as bthread A
    participant BT_B as bthread B
    participant P as pthread 0

    Note over BT_A,P: BT_A 正在执行

    P->>BT_A: sched_to(bthread A)
    BT_A->>BT_A: 发送 RPC 请求
    BT_A->>BT_A: butex_wait(my_butex, 0)
    Note over BT_A: yield! A 保存上下文

    P->>BT_B: sched_to(bthread B) ← 同一个 pthread
    BT_B->>BT_B: 执行其他工作
    Note over BT_B: pthread 没有被阻塞

    Note over BT_A,P: I/O 事件到达

    P->>BT_A: sched_to(bthread A)
    BT_A->>BT_A: 从 butex_wait 返回
    BT_A->>BT_A: 继续处理响应
```

**要点**：`butex_wait()` 内部调用 `yield`，将 pthread 让给其他 bthread。这与 pthread 的 `pthread_cond_wait()` 有本质不同——后者让出的是内核线程。

### 4.2 同一 pthread 上两个 bthread 通过 Butex 同步

```mermaid
sequenceDiagram
    participant P as pthread 0
    participant PROD as 生产者 bthread
    participant CONsumer as 消费者 bthread
    participant BX as Butex

    Note over P,BX: 两个 bthread 在同一 pthread 上交替执行

    P->>CONsumer: sched_to(consumer)
    CONsumer->>BX: butex_wait(butex, 0)
    Note over CONsumer: yield → 挂起

    P->>PROD: sched_to(producer)
    PROD->>PROD: 生产数据
    PROD->>BX: butex_wake(butex)
    Note over PROD: consumer 被推入 WSQ

    PROD->>PROD: 继续执行其他工作...

    P->>CONsumer: sched_to(consumer)
    Note over CONsumer: 从 butex_wait 恢复
    CONsumer->>CONsumer: 消费数据
```

### 4.3 跨 pthread 上两个 bthread 通过 Butex 同步

```mermaid
sequenceDiagram
    participant P0 as pthread 0
    participant P1 as pthread 1
    participant PROD as 生产者 bthread（pthread 0）
    participant CONSUMER as 消费者 bthread（pthread 1）
    participant RQ as pthread 1 远程 RQ
    participant BX as Butex

    P1->>CONSUMER: sched_to(consumer)
    CONSUMER->>BX: butex_wait(butex, 0)
    Note over CONSUMER: yield → pthread 1 调度其他 bthread

    P0->>PROD: sched_to(producer)
    PROD->>PROD: 生产数据
    PROD->>BX: butex_wake(butex)
    Note over BX: consumer 属于 TG1<br/>当前线程是 TG0

    BX->>RQ: push(consumer_tid) 到 TG1 远程 RQ
    BX->>P1: signal_task(1)

    Note over P1: pthread 1 被唤醒

    P1->>CONSUMER: steal_task() 或 pop(remote_rq)
    Note over CONSUMER: 从 butex_wait 恢复
    CONSUMER->>CONSUMER: 消费数据
```

**关键点**：`butex_wake` 知道目标 bthread 属于哪个 TaskGroup，自动选择投递到本地 WSQ（同 pthread）或远程 RQ（跨 pthread）。

---

## 5. bthread 同步原语全景

### 5.1 同步原语层次

```mermaid
graph TB
    subgraph "bthread 同步原语"
        BTX["Butex / bthread_mutex / bthread_cond / countdown_event"]
    end

    subgraph "基于 Butex 的高层原语"
        USLEEP["bthread_usleep()"]
        FD_W["bthread_fd_wait()"]
        IDLE["ParkingLot.wait()"]
        SOCK_W["Socket.Write() (EAGAIN yield)"]
    end

    USLEEP -->|"TimerThread + butex_wake"| BTX
    FD_W -->|"EpollThread + butex_wake"| BTX
    IDLE -->|"futex_wait/wake"| BTX
    SOCK_W -->|"yield + EPOLLOUT"| BTX

    subgraph "bthread 互斥原语"
        BM["bthread_mutex_t"]
        BC["bthread_cond_t"]
    end

    BM -->|"基于 Butex"| BTX
    BC -->|"基于 Butex"| BTX
```

### 5.2 各原语说明

| 原语 | 用途 | 阻塞时行为 |
|---|---|---|
| `butex_wait/butex_wake` | 基础等待/唤醒 | yield，push 到 WSQ/RQ |
| `bthread_mutex_lock` | 互斥锁 | 如锁已被持有，yield |
| `bthread_mutex_trylock` | 非阻塞互斥锁 | 不阻塞，返回 EBUSY |
| `bthread_cond_wait/signal` | 条件变量 | yield |
| `bthread_countdown_event` | 倒计时事件 | yield |
| `bthread_usleep` | 定时睡眠 | yield，TimerThread 唤醒 |
| `bthread_fd_wait` | 等待 fd 事件 | yield，EpollThread 唤醒 |
| `bthread_join` | 等待 bthread 结束 | yield，结束时唤醒 |

---

## 6. 典型场景中的 Butex 使用

### 6.1 同步 RPC 调用

```mermaid
sequenceDiagram
    participant USR as 用户代码 bthread
    participant CNTL as Controller
    participant BX as done_butex
    participant IO_B as I/O bthread
    participant SOCK as Socket

    USR->>CNTL: IssueRPC(request)
    USR->>SOCK: Socket.Write(request)

    Note over USR: 同步等待响应

    USR->>BX: butex_wait(done_butex, 0)
    Note over USR: yield! 当前 bthread 挂起<br/>pthread 调度其他 bthread

    SOCK->>IO_B: EPOLLIN（响应到达）
    IO_B->>IO_B: Parse response
    IO_B->>CNTL: ProcessResponse

    IO_B->>BX: butex_wake(done_butex)
    Note over IO_B: USR bthread 被推入 WSQ

    Note over USR: USR bthread 被重新调度

    USR->>USR: 从 butex_wait 返回
    USR->>USR: 使用 response
```

### 6.2 bthread 互斥锁

```mermaid
sequenceDiagram
    participant BT_A as bthread A
    participant BT_B as bthread B
    participant MTX as bthread_mutex_t
    participant CRIT as 临界区

    BT_A->>MTX: bthread_mutex_lock()

    alt 锁空闲
        MTX-->>BT_A: 获取成功
        BT_A->>CRIT: 执行临界区代码
        BT_A->>MTX: bthread_mutex_unlock()
    else 锁被 bthread B 持有
        MTX-->>BT_A: 获取失败
        BT_A->>MTX: butex_wait(mutex_butex)
        Note over BT_A: yield! 让出 pthread

        BT_B->>CRIT: 执行临界区代码
        BT_B->>MTX: bthread_mutex_unlock()
        BT_B->>MTX: butex_wake(mutex_butex)

        MTX-->>BT_A: 被唤醒
        BT_A->>CRIT: 执行临界区代码
        BT_A->>MTX: bthread_mutex_unlock()
    end
```

### 6.3 定时器等待

```mermaid
sequenceDiagram
    participant BT as bthread
    participant TM as TimerThread
    participant BX as timer_butex

    BT->>TM: bthread_timer_add(expire_us, wake_cb, timer_butex)
    BT->>BX: butex_wait(timer_butex, 0)
    Note over BT: yield! 挂起

    Note over TM: ... 时间流逝 ...

    TM->>BX: 定时器到期, wake_cb
    TM->>BX: butex_wake(timer_butex)

    Note over BT: BT 被重新调度
    BT->>BT: 从 butex_wait 返回
```

---

## 7. 为什么不用 pthread 互斥锁

### 7.1 致命问题：阻塞整个 pthread

```mermaid
sequenceDiagram
    participant P as pthread 0
    participant BT_A as bthread A
    participant BT_B as bthread C
    participant BT_C as bthread D

    Note over P,BT_C: 错误示范: bthread 使用 pthread_mutex_lock

    P->>BT_A: 执行 bthread A
    BT_A->>BT_A: pthread_mutex_lock(&mutex)
    Note over P: pthread 阻塞! bthread C 和 D 都无法执行
    Note over BT_B: 无法调度（pthread 被阻塞）
    Note over BT_C: 无法调度（pthread 被阻塞）

    Note over P,BT_C: 等待锁释放后才能继续

    Note over P: pthread_mutex_unlock 被调用
    P->>BT_A: 恢复执行
```

### 7.2 对比

| 特性 | pthread_mutex | bthread_mutex (butex) |
|---|---|---|
| 阻塞单位 | pthread（内核线程） | bthread（用户态协程） |
| 阻塞影响 | 同一 pthread 上所有 bthread 全部饿死 | 仅当前 bthread 让出，其他 bthread 正常执行 |
| 唤醒成本 | 内核调度（~1-10μs） | 用户态调度（~10-20ns） |
| 在 M:N 模型中 | **禁止使用** | **必须使用** |
| 最大并发 | 受限于 pthread 数量 | 仅受内存限制（百万级 bthread） |

---

## 8. 完整时序分析

### 8.1 一个 pthread 上的典型调度周期

```mermaid
sequenceDiagram
    participant P as pthread 0 / TaskGroup 0
    participant WSQ as 本地 WSQ
    participant BT as 当前 bthread
    participant BT2 as 下一个 bthread

    Note over P,BT2: 1. 从 WSQ pop 获取任务

    P->>WSQ: pop(tid)
    WSQ-->>P: bthread X
    P->>BT: sched_to(X)
    BT->>BT: 执行任务...

    Note over P,BT2: 2. 任务遇到 butex_wait, yield

    BT->>BT: butex_wait(butex, val)
    Note over BT: 保存上下文, 推入 butex 等待队列

    P->>WSQ: pop(tid)
    WSQ-->>P: bthread Y
    P->>BT2: sched_to(Y)
    BT2->>BT2: 执行其他任务...

    Note over P,BT2: 3. 或 steal 其他 TG 的任务

    P->>P: steal_task() → 从 TG1 的 WSQ steal
    P->>BT2: sched_to(stealed_task)
    BT2->>BT2: 执行窃取的任务...

    Note over P,BT2: 4. 或所有队列空, 进入 idle

    P->>P: wait_task() → ParkingLot.futex_wait()
    Note over P: pthread 挂起, 零 CPU 开销

    Note over P,BT2: 5. signal_task 唤醒

    P->>WSQ: pop(tid) 或 steal_task()
    P->>BT2: sched_to(task)
```

### 8.2 bthread 在不同 pthread 上恢复执行

```mermaid
sequenceDiagram
    participant P0 as pthread 0
    participant P1 as pthread 1
    participant BT as bthread X

    Note over P0,P1: 阶段 1: bthread X 在 pthread 0 上执行

    P0->>BT: sched_to(X)
    BT->>BT: butex_wait(butex)
    Note over BT: yield, 挂起在 pthread 0

    Note over P0,P1: 阶段 2: pthread 0 执行其他 bthread

    P0->>P0: 执行 bthread Y, Z, ...

    Note over P0,P1: 阶段 3: 但ex_wake 从任意线程触发

    P0->>BT: butex_wake(butex)
    Note over BT: push 到 TG0 的 WSQ

    Note over P0,P1: 阶段 4: 可能被不同 pthread 调度执行!

    alt pthread 0 可用
        P0->>BT: pop(X) → 在 pthread 0 恢复
    else pthread 0 忙, pthread 1 空闲
        P1->>P1: steal_task() → steal TG0 的 WSQ → 获取 X
        P1->>BT: sched_to(X) → 在 pthread 1 恢复!
    end

    Note over BT: bthread 可能在不同 pthread 上恢复<br/>对 bthread 完全透明
```

**这就是 bthread 的核心优势**：bthread 不绑定特定 pthread，任何空闲的 pthread 都可以执行它。Butex 确保了无论在哪个 pthread 上执行，语义都是正确的。
