# CubeFS 线程模型分析

> 本文档分析 CubeFS 各组件的线程(Goroutine)模型，并使用 Mermaid 时序图展示关键交互流程。

---

## 目录

1. [总体架构](#1-总体架构)
2. [Master 线程模型](#2-master-线程模型)
3. [MetaNode 线程模型](#3-metanode-线程模型)
4. [DataNode 线程模型](#4-datanode-线程模型)
5. [SDK/Client 线程模型](#5-sdkclient-线程模型)
6. [Raft 一致性线程模型](#6-raft-一致性线程模型)
7. [典型 IO 流程时序图](#7-典型-io-流程时序图)
8. [心跳与任务调度时序图](#8-心跳与任务调度时序图)
9. [总结](#9-总结)

---

## 1. 总体架构

CubeFS 采用 Go 语言开发，其"线程模型"本质上是 **Goroutine + Channel** 的 CSP 并发模型。各组件之间通过 TCP/HTTP 通信，内部通过 Goroutine 实现并发。

### 组件关系总览

```mermaid
graph TB
    subgraph Client
        SDK[SDK / FUSE Mount]
    end

    subgraph Master Cluster
        M1[Master Leader]
        M2[Master Follower]
        M1 <-. Raft .-> M2
    end

    subgraph MetaNode Cluster
        MN1[MetaNode 1]
        MN2[MetaNode 2]
        MN3[MetaNode 3]
        MN1 <-. Raft .-> MN2
        MN2 <-. Raft .-> MN3
    end

    subgraph DataNode Cluster
        DN1[DataNode 1]
        DN2[DataNode 2]
        DN3[DataNode 3]
        DN1 <-. Raft .-> DN2
        DN2 <-. Raft .-> DN3
    end

    SDK -->|元数据操作| MN1
    SDK -->|数据读写| DN1
    SDK -->|卷管理/分区查询| M1
    M1 -->|AdminTask 下发| MN1
    M1 -->|AdminTask 下发| DN1
    MN1 -->|心跳上报| M1
    DN1 -->|心跳上报| M1
```

### Goroutine 模型分类

| 类型 | 描述 | 示例 |
|------|------|------|
| **Accept Loop Goroutine** | 监听 TCP 连接，每连接派生 Goroutine | `serveConn`, `serveSmuxConn` |
| **Ticker/Timer Goroutine** | 定时执行周期任务 | `AdminTaskManager.process`, `scheduleTask` |
| **Worker Goroutine** | 处理具体业务逻辑 | Raft apply, packet handling |
| **Background Goroutine** | 长期运行的后台任务 | `startUpdateNodeInfo`, `startCpuSample`, `startGcTimer` |

---

## 2. Master 线程模型

### 2.1 核心组件

Master 节点是集群的管理中心，其核心并发组件包括：

- **AdminTaskManager**: 负责向 MetaNode / DataNode 下发管理任务
- **Raft Store**: 保证 Master 集群一致性
- **HTTP Server**: 处理客户端和节点请求
- **定时调度器**: 心跳检测、分区管理、负载均衡

### 2.2 AdminTaskManager 线程模型

`AdminTaskManager` 是 Master 向节点下发管理命令的核心组件。每个 MetaNode/DataNode 对应一个 `AdminTaskManager` 实例。

**关键代码** (`master/admin_task_manager.go`):

```go
func newAdminTaskManager(targetAddr, clusterID string) (sender *AdminTaskManager) {
    sender = &AdminTaskManager{
        targetAddr: targetAddr,
        TaskMap:    make(map[string]*proto.AdminTask),
        exitCh:     make(chan struct{}, 1),
        connPool:   util.NewConnectPoolWithTimeout(...),
    }
    go sender.process()  // 启动后台 Goroutine
    return
}

func (sender *AdminTaskManager) process() {
    ticker := time.NewTicker(TaskWorkerInterval) // 2秒
    for {
        select {
        case <-sender.exitCh:
            return
        case <-ticker.C:
            sender.doDeleteTasks()  // 清理超时任务
            sender.doSendTasks()    // 发送待发送任务
        }
    }
}
```

### 2.3 Master AdminTask 调度时序图

```mermaid
sequenceDiagram
    participant Master as Master (主节点)
    participant ATM as AdminTaskManager
    participant TaskMap as TaskMap (共享Map)
    participant Node as MetaNode/DataNode

    Note over Master: 业务逻辑调用 AddTask
    Master->>TaskMap: AddTask(task) (加写锁)
    TaskMap-->>Master: OK

    Note over ATM: 每2秒 Ticker 触发
    ATM->>ATM: process() select <-ticker.C
    
    rect rgb(255, 230, 230)
    Note over ATM: 第一步: 清理超时任务
    ATM->>TaskMap: getToBeDeletedTasks() (加读锁)
    TaskMap-->>ATM: delTasks[]
    loop 每个超时任务
        ATM->>TaskMap: DelTask(t) (加写锁)
    end
    end

    rect rgb(230, 255, 230)
    Note over ATM: 第二步: 发送待发送任务
    ATM->>TaskMap: getToDoTasks() (加读锁)
    Note right of ATM: 优先级:<br/>1. 心跳任务<br/>2. 紧急任务<br/>3. 普通任务<br/>限制: MaxTaskNum=30
    TaskMap-->>ATM: tasks[]
    
    loop 每个任务 (最多30个)
        ATM->>Node: getConn() 从连接池获取连接
        ATM->>Node: sendAdminTask(packet) WriteToConn
        Node-->>ATM: ReadFromConnWithVer 响应
        alt 发送成功
            ATM->>ATM: updateTaskInfo(task, true)<br/>Status=TaskRunning
        else 发送失败
            ATM->>ATM: updateTaskInfo(task, false)<br/>SendCount++
        end
        ATM->>Node: putConn() 归还连接
    end
    end
```

### 2.4 Master 主要 Goroutine 清单

| Goroutine | 触发方式 | 功能 |
|-----------|---------|------|
| `AdminTaskManager.process` | Ticker (2s) | 周期性发送/清理管理任务 |
| Raft `tick` | 定时 | Raft 心跳与选举 |
| Raft `apply` | Commit 事件 | 应用已提交的日志 |
| HTTP Handler | 请求驱动 | 处理 API 请求 |
| `scheduleTask` | Ticker | 分区创建、迁移、负载均衡 |
| `checkNodeHeartbeat` | Ticker | 检测节点存活状态 |

---

## 3. MetaNode 线程模型

### 3.1 核心组件

MetaNode 管理元数据分片(MetaPartition)，通过 Raft 保证一致性。其启动流程 (`doStart`) 按顺序启动多个服务：

```go
func doStart(s common.Server, cfg *config.Config) (err error) {
    m.parseConfig(cfg)
    m.register()             // 向 Master 注册
    m.startRaftServer(cfg)   // 启动 Raft
    m.newMetaManager(cfg)    // 创建元数据管理器
    m.startServer()          // 启动 TCP 服务
    m.startSmuxServer()      // 启动 Smux 服务
    m.startMetaManager()     // 启动元数据管理器
    m.registerAPIHandler()   // 注册 HTTP API
    go m.startUpdateNodeInfo() // 后台: 更新节点信息
    m.startStat()            // 启动统计
}
```

### 3.2 TCP 连接处理模型

MetaNode 采用 **"一个连接一个 Goroutine"** 的模型处理 TCP 请求：

```mermaid
sequenceDiagram
    participant Client as Client/SDK
    participant Listener as TCP Listener
    participant ServeConn as serveConn Goroutine
    participant Handler as handlePacket
    participant Mgr as MetadataManager

    Client->>Listener: TCP Connect
    Listener->>Listener: ln.Accept()
    
    par 新建 Goroutine
        Listener->>ServeConn: go m.serveConn(conn, stopC)
    end
    
    ServeConn->>ServeConn: AddConnection() (atomic++)
    ServeConn->>ServeConn: SetKeepAlive(true), SetNoDelay(true)
    
    loop 读循环
        ServeConn->>Client: ReadFromConnWithVer(packet)
        Client-->>ServeConn: Packet 数据
        
        ServeConn->>Handler: handlePacket(conn, p, remoteAddr)
        Handler->>Mgr: HandleMetadataOperation(conn, p, remoteAddr)
        
        alt 同步操作 (如创建/删除)
            Mgr->>Mgr: Raft.Propose()
            Mgr->>Mgr: 等待 Raft Apply
            Mgr-->>Handler: 结果
        else 读操作 (如 lookup/getattr)
            Mgr->>Mgr: 直接读取本地内存
            Mgr-->>Handler: 结果
        end
        
        Handler-->>ServeConn: err
        ServeConn->>Client: 写回响应 Packet
    end
    
    Note over ServeConn: 连接断开或 stopC 关闭
    ServeConn->>ServeConn: RemoveConnection() (atomic--)
    ServeConn->>ServeConn: conn.Close()
```

### 3.3 Smux 多路复用模型

为减少连接数开销，MetaNode 支持 Smux (多路复用) 协议，在单条 TCP 连接上创建多个逻辑流：

```mermaid
sequenceDiagram
    participant SDK as SDK (Smux Client)
    participant SListener as Smux Listener
    participant SSess as serveSmuxConn Goroutine
    participant SStream as serveSmuxStream Goroutine
    participant Mgr as MetadataManager

    SDK->>SListener: TCP Connect (Smux端口)
    SListener->>SListener: Accept()
    SListener->>SSess: go serveSmuxConn(conn, stopC)
    
    SSess->>SSess: smux.Server(conn, config)
    SSess->>SSess: AddConnection()
    
    loop 接受新 Stream
        SSess->>SSess: sess.AcceptStream()
        
        par 新建 Stream Goroutine
            SSess->>SStream: go serveSmuxStream(stream, ...)
        end
    end
    
    Note over SStream: 每个 Stream 独立处理请求
    loop Stream 读循环
        SStream->>SDK: ReadFromConnWithVer(stream)
        SDK-->>SStream: Packet
        SStream->>Mgr: HandleMetadataOperation(stream, p, ...)
        Mgr-->>SStream: 结果
        SStream->>SDK: 写回响应
    end
```

### 3.4 MetaNode 主要 Goroutine 清单

| Goroutine | 触发方式 | 功能 |
|-----------|---------|------|
| TCP Accept Loop | `startServer` | 接受 TCP 连接 |
| `serveConn` | 每连接 | 处理 TCP 请求 |
| Smux Accept Loop | `startSmuxServer` | 接受 Smux 连接 |
| `serveSmuxConn` | 每 Smux Session | 管理 Smux 流 |
| `serveSmuxStream` | 每 Smux Stream | 处理流上请求 |
| Raft goroutines | RaftStore | Raft 协议 |
| `startUpdateNodeInfo` | 后台定时 | 向 Master 上报节点信息 |
| `startStat` | 定时 | 统计信息收集 |

---

## 4. DataNode 线程模型

### 4.1 核心组件

DataNode 管理数据分片(DataPartition)，同样通过 Raft 保证副本一致性。其启动流程最为复杂：

```go
func doStart(server common.Server, cfg *config.Config) (err error) {
    s.parseConfig(cfg)
    s.parseRaftConfig(cfg)
    s.registerMetrics()
    s.register(cfg)           // 向 Master 注册
    s.parseSmuxConfig(cfg)
    s.startStat(cfg)
    s.initConnPool()
    initRepairLimit()
    s.startRaftServer(cfg)
    s.newSpaceManager(cfg)
    s.startTCPService()       // 启动 TCP 服务
    s.startSmuxService(cfg)   // 启动 Smux 服务
    s.startSpaceManager(cfg)  // 并发加载磁盘
    go s.registerHandler()    // 注册 HTTP Handler
    s.scheduleTask()          // 启动定时任务
    s.startMetrics()
    s.startCpuSample()        // CPU 采样
    s.startGcTimer()          // GC 定时器
}
```

### 4.2 磁盘并发加载模型

`startSpaceManager` 使用 `sync.WaitGroup` 并发加载多块磁盘：

```mermaid
sequenceDiagram
    participant Main as Main Goroutine
    participant WG as sync.WaitGroup
    participant Disk1 as Disk Loader 1
    participant Disk2 as Disk Loader 2
    participant DiskN as Disk Loader N
    participant Space as SpaceManager

    Main->>Main: 遍历磁盘路径列表
    Main->>WG: wg.Add(N)
    
    par 并发加载磁盘
        Main->>Disk1: go LoadDisk(path1, ...)
        Main->>Disk2: go LoadDisk(path2, ...)
        Main->>DiskN: go LoadDisk(pathN, ...)
    end

    Disk1->>Space: LoadDisk(path1) - 加载分区
    Disk2->>Space: LoadDisk(path2) - 加载分区
    DiskN->>Space: LoadDisk(pathN) - 加载分区
    
    Disk1->>WG: wg.Done()
    Disk2->>WG: wg.Done()
    DiskN->>WG: wg.Done()
    
    Main->>WG: wg.Wait() (等待所有磁盘加载完成)
    
    Main->>Space: StartCheckDiskLost()
    Main->>Space: StartDiskSample()
    Main->>Space: StartEvictExtentCache() (go)
```

### 4.3 DataNode TCP 服务模型

DataNode 的 TCP 服务与 MetaNode 类似，采用 **每连接一 Goroutine** 模型：

```mermaid
sequenceDiagram
    participant Client as Client/SDK
    participant Listener as TCP Listener
    participant ServeConn as serveConn Goroutine
    participant Repl as repl Package
    participant DP as DataPartition

    Client->>Listener: TCP Connect
    Listener->>Listener: Accept()
    Listener->>ServeConn: go s.serveConn(conn)
    
    loop 请求循环
        ServeConn->>Client: 读取 Packet
        
        alt 写操作 (Create/Write/Delete)
            ServeConn->>Repl: 处理复制请求
            Repl->>DP: Raft.Propose
            DP->>DP: Raft 复制到 follower
            DP-->>Repl: Apply 结果
            Repl-->>ServeConn: 响应
        else 读操作 (Read/StreamRead)
            ServeConn->>DP: 直接读取磁盘
            DP-->>ServeConn: 数据
        else 修复操作 (Repair)
            ServeConn->>Repl: repl.RepairExtent
            Repl->>DP: 从其他副本拉取
            DP-->>ServeConn: 修复结果
        end
        
        ServeConn->>Client: 写回响应
    end
```

### 4.4 DataNode 后台任务模型

```mermaid
sequenceDiagram
    participant Ticker as Various Tickers
    participant DN as DataNode
    participant Master as Master
    participant Disk as Disk Manager

    rect rgb(240, 240, 255)
    Note over Ticker,Disk: 1. scheduleTask - 定时调度
    Ticker->>DN: 触发 (周期性)
    DN->>Disk: 检查磁盘状态
    DN->>Disk: 执行磁盘维护
    end

    rect rgb(255, 240, 240)
    Note over Ticker,Disk: 2. startMetrics - 指标收集
    Ticker->>DN: 触发 (周期性)
    DN->>Disk: 收集 LackDpCount 等指标
    DN->>Master: 上报指标
    end

    rect rgb(240, 255, 240)
    Note over Ticker,Disk: 3. startCpuSample - CPU 采样
    Ticker->>DN: 触发 (每1秒)
    DN->>DN: 采样 CPU 使用率
    DN->>DN: 更新 cpuUtil (atomic)
    end

    rect rgb(255, 255, 240)
    Note over Ticker,Disk: 4. startGcTimer - GC 定时器
    Ticker->>DN: 触发 (每10秒)
    DN->>Disk: gcRecyclePercent 检查
    DN->>DN: runtime.GC() (如果需要)
    end

    rect rgb(255, 240, 255)
    Note over Ticker,Disk: 5. registerHandler - HTTP 服务
    Note over DN: 独立 Goroutine 注册 HTTP 路由
    DN->>DN: http.HandleFunc(...)
    end
```

### 4.5 DataNode 主要 Goroutine 清单

| Goroutine | 触发方式 | 功能 |
|-----------|---------|------|
| TCP Accept Loop | `startTCPService` | 接受 TCP 连接 |
| `serveConn` | 每连接 | 处理数据读写请求 |
| Smux Accept Loop | `startSmuxService` | 接受 Smux 连接 |
| `serveSmuxConn` | 每 Smux Session | 处理 Smux 流 |
| 磁盘加载 | `startSpaceManager` | 并发加载磁盘 (WaitGroup) |
| `registerHandler` | 后台 | 注册 HTTP API |
| `scheduleTask` | Ticker | 定时调度任务 |
| `startMetrics` | Ticker | 指标收集与上报 |
| `startCpuSample` | Ticker (1s) | CPU 使用率采样 |
| `startGcTimer` | Ticker (10s) | GC 触发 |
| `StartEvictExtentCache` | 后台 | Extent 缓存驱逐 |
| `StartDiskSample` | 后台 | 磁盘采样 |
| `StartCheckDiskLost` | 后台 | 检查磁盘丢失 |

---

## 5. SDK/Client 线程模型

### 5.1 核心组件

CubeFS SDK 是客户端的核心，包含：
- **MetaClient**: 元数据操作客户端
- **ExtentClient**: 数据操作客户端
- **MasterClient**: 与 Master 交互
- **连接池**: TCP/Smux 连接复用

### 5.2 SDK 读写流程时序图

```mermaid
sequenceDiagram
    participant App as 应用程序
    participant SDK as CubeFS SDK
    participant MC as MasterClient
    participant MetaC as MetaClient
    participant ExtC as ExtentClient
    participant MN as MetaNode
    participant DN as DataNode

    Note over App,DN: === 写流程 ===
    
    App->>SDK: Write(path, data)
    SDK->>MetaC: 获取 MetaPartition 信息
    
    alt 缓存未命中
        MetaC->>MC: GetDataPartition
        MC-->>MetaC: 分区信息
        MetaC->>MetaC: 缓存分区信息
    end
    
    SDK->>MetaC: CreateInode / 空间分配
    MetaC->>MN: 发送元数据请求 (TCP/Smux)
    MN-->>MetaC: Inode + Extent 信息
    
    SDK->>ExtC: WriteExtent(inode, data)
    
    par 并发写入多个 Extent
        ExtC->>DN: Write Packet (TCP)
        DN-->>ExtC: 写入成功
    end
    
    ExtC->>MetaC: 更新 Inode 的 Extent 列表
    MetaC->>MN: AppendExtents (Raft)
    MN-->>MetaC: 确认
    
    SDK-->>App: 写入完成

    Note over App,DN: === 读流程 ===
    
    App->>SDK: Read(path, offset, size)
    SDK->>MetaC: GetExtents(inode)
    MetaC->>MN: 查询 Extent 列表
    MN-->>MetaC: Extent 列表
    
    SDK->>ExtC: ReadExtent(extents)
    
    par 并发读取多个 Extent
        ExtC->>DN: Read Packet (TCP)
        DN-->>ExtC: 数据
    end
    
    SDK-->>App: 数据返回
```

### 5.3 SDK 连接池模型

```mermaid
graph TB
    subgraph SDK 连接管理
        MCP[MasterConnPool<br/>HTTP 连接池]
        TCP[TCP ConnPool<br/>普通TCP连接]
        SMC[Smux ConnPool<br/>多路复用连接]
    end

    subgraph 目标节点
        M[Master]
        MN[MetaNode]
        DN[DataNode]
    end

    MCP -->|HTTP/REST| M
    TCP -->|TCP Packet| MN
    TCP -->|TCP Packet| DN
    SMC -->|Smux Stream| MN
    SMC -->|Smux Stream| DN

    style MCP fill:#e1f5fe
    style TCP fill:#fff3e0
    style SMC fill:#e8f5e9
```

---

## 6. Raft 一致性线程模型

### 6.1 Raft 在 CubeFS 中的角色

CubeFS 在三个层面使用 Raft：
1. **Master 集群**: 元数据管理(卷、节点信息)
2. **MetaPartition**: 元数据分片一致性
3. **DataPartition**: 数据分片副本一致性

### 6.2 Raft 写入流程时序图

```mermaid
sequenceDiagram
    participant Client as Client Request
    participant Leader as Leader Node
    participant Raft as Raft Module
    participant FSM as Finite State Machine
    participant F1 as Follower 1
    participant F2 as Follower 2

    Client->>Leader: 业务请求
    Leader->>Raft: Raft.Propose(data)
    
    rect rgb(230, 245, 255)
    Note over Leader,Raft: 1. Leader 处理
    Raft->>Raft: 追加到本地日志
    Raft->>Raft: 更新 lastLogIndex
    end
    
    par 并行复制
        Raft->>F1: AppendEntries RPC (heartbeat)
        and
        Raft->>F2: AppendEntries RPC (heartbeat)
    end
    
    F1->>F1: 追加日志
    F1-->>Raft: AppendEntriesResponse (success)
    F2->>F2: 追加日志
    F2-->>Raft: AppendEntriesResponse (success)
    
    Raft->>Raft: 检查 majority (2/3)
    Raft->>Raft: commitIndex++
    
    rect rgb(255, 245, 230)
    Note over Raft,FSM: 2. Apply 到 FSM
    Raft->>FSM: Apply(committedLog)
    FSM->>FSM: 更新内存状态 (inode/dentry/extent)
    FSM-->>Raft: Apply 结果
    end
    
    Raft-->>Leader: Propose 完成
    Leader-->>Client: 操作成功
    
    par 异步同步
        Raft->>F1: commitIndex 通知 (下次 heartbeat)
        and
        Raft->>F2: commitIndex 通知 (下次 heartbeat)
    end
    
    F1->>F1: FSM.Apply (本地)
    F2->>F2: FSM.Apply (本地)
```

### 6.3 Raft 主要 Goroutine

| Goroutine | 功能 |
|-----------|------|
| `tick` goroutine | 定时触发心跳和选举超时 |
| `step` goroutine | 处理 Raft 消息 |
| `propose` channel | 接收写提案 |
| `apply` goroutine | 应用已提交日志到 FSM |
| `snap` goroutine | 快照创建与传输 |

---

## 7. 典型 IO 流程时序图

### 7.1 完整的文件写入流程

```mermaid
sequenceDiagram
    participant App as 应用
    participant FUSE as FUSE/Mount
    participant SDK as SDK
    participant MC as MasterClient
    participant MN as MetaNode (Leader)
    participant MN2 as MetaNode (Follower)
    participant DN1 as DataNode 1 (Leader)
    participant DN2 as DataNode 2 (Follower)
    participant DN3 as DataNode 3 (Follower)

    App->>FUSE: write(fd, data)
    FUSE->>SDK: WriteInode(inode, data)
    
    rect rgb(245, 245, 245)
    Note over SDK,MC: 步骤1: 获取元数据信息
    SDK->>MC: GetInode/GetExtents
    MC-->>SDK: MetaPartition 信息
    end
    
    rect rgb(230, 255, 230)
    Note over SDK,DN3: 步骤2: 写数据到 DataNode
    SDK->>DN1: Packet (OpWrite)
    DN1->>DN1: Raft Propose
    par 并行复制
        DN1->>DN2: AppendEntries
        and
        DN1->>DN3: AppendEntries
    end
    DN2-->>DN1: ACK
    DN3-->>DN1: ACK
    DN1->>DN1: Raft Commit & Apply
    DN1-->>SDK: 写入成功 + ExtentID
    end
    
    rect rgb(255, 245, 230)
    Note over SDK,MN2: 步骤3: 更新元数据
    SDK->>MN: AppendExtents(inode, extents)
    MN->>MN: Raft Propose
    par 并行复制
        MN->>MN2: AppendEntries
    end
    MN2-->>MN: ACK
    MN->>MN: Raft Commit & Apply
    MN-->>SDK: 元数据更新成功
    end
    
    SDK-->>FUSE: 写入完成
    FUSE-->>App: bytes written
```

### 7.2 完整的文件读取流程

```mermaid
sequenceDiagram
    participant App as 应用
    participant FUSE as FUSE/Mount
    participant SDK as SDK
    participant MC as MasterClient
    participant MN as MetaNode
    participant DN1 as DataNode 1
    participant DN2 as DataNode 2

    App->>FUSE: read(fd, size)
    FUSE->>SDK: ReadInode(inode)
    
    SDK->>MN: GetExtents(inode)
    MN-->>SDK: Extent 列表 [e1, e2, e3]
    
    par 并发读取多个 Extent
        SDK->>DN1: OpRead (extent1)
        Note right of DN1: 直接读磁盘 (无需 Raft)
        DN1-->>SDK: data1
    and
        SDK->>DN2: OpRead (extent2)
        DN2-->>SDK: data2
    and
        SDK->>DN1: OpStreamRead (extent3)
        DN1-->>SDK: data3 (流式)
    end
    
    SDK->>SDK: 合并数据
    SDK-->>FUSE: 完整数据
    FUSE-->>App: data
```

---

## 8. 心跳与任务调度时序图

### 8.1 Master-Node 心跳与任务下发

```mermaid
sequenceDiagram
    participant MN as MetaNode/DataNode
    participant M as Master
    participant ATM as AdminTaskManager
    participant Ticker as Ticker

    rect rgb(255, 240, 240)
    Note over MN,M: 节点主动上报心跳
    loop 每个心跳间隔
        MN->>M: HTTP Heartbeat (节点状态、分区信息)
        M->>M: 更新节点状态
        M-->>MN: HeartbeatResponse (包含任务列表/配置变更)
    end
    end

    rect rgb(240, 255, 240)
    Note over M,ATM: Master 主动下发任务
    M->>ATM: AddTask(adminTask)
    Note right of ATM: 任务类型:<br/>- 创建分区<br/>- 删除分区<br/>- 迁移分区<br/>- 配置更新
    
    Ticker->>ATM: 每2秒触发
    ATM->>ATM: getToDoTasks (优先级排序)
    ATM->>MN: TCP 发送任务 Packet
    MN->>MN: 执行管理操作
    MN-->>ATM: 响应结果
    ATM->>ATM: 更新任务状态
    end

    rect rgb(240, 240, 255)
    Note over MN,M: 任务结果汇报
    MN->>M: 任务完成汇报 (下次心跳)
    M->>M: 更新集群状态
    M->>ATM: DelTask (清理已完成任务)
    end
```

### 8.2 MetaNode 启动与注册流程

```mermaid
sequenceDiagram
    participant MN as MetaNode
    participant MC as MasterClient
    participant M as Master
    participant Raft as RaftStore
    participant TCP as TCP Server

    rect rgb(245, 245, 245)
    Note over MN: 1. 配置解析
    MN->>MN: parseConfig()
    MN->>MC: NewMasterCLientWithResolver()
    MC->>MC: Start() (启动解析器)
    end

    rect rgb(255, 240, 240)
    Note over MN,M: 2. 向 Master 注册
    loop 重试直到成功
        MN->>MC: getClusterInfo()
        MC->>M: GetClusterInfo HTTP
        M-->>MC: ClusterInfo
        MC-->>MN: ClusterInfo
        
        MN->>MC: AddMetaNodeWithAuthNode()
        MC->>M: 注册节点
        M-->>MC: NodeID
        MC-->>MN: NodeID
    end
    end

    rect rgb(240, 255, 240)
    Note over MN: 3. 启动服务 (顺序执行)
    MN->>Raft: startRaftServer()
    MN->>MN: newMetaManager()
    MN->>TCP: startServer() (TCP Accept Loop)
    MN->>TCP: startSmuxServer() (Smux Accept Loop)
    MN->>MN: startMetaManager()
    MN->>MN: registerAPIHandler()
    end

    rect rgb(240, 240, 255)
    Note over MN: 4. 后台 Goroutine
    par 并行启动
        MN->>MN: go startUpdateNodeInfo()
    and
        MN->>MN: startStat()
    end
    end

    rect rgb(255, 245, 230)
    Note over MN: 5. 健康检查
    MN->>MC: checkLocalPartitionMatchWithMaster()
    MC->>M: GetMetaNodeInfo
    M-->>MC: 分区列表
    MC-->>MN: 对比本地分区
    end
```

---

## 9. 总结

### 9.1 并发模型特点

| 特点 | 说明 |
|------|------|
| **CSP 模型** | Go 语言原生 Goroutine + Channel，而非 OS 线程 |
| **每连接一 Goroutine** | TCP/Smux 连接由独立 Goroutine 处理 |
| **Ticker 驱动** | 大量后台任务通过 `time.Ticker` 定时触发 |
| **连接池复用** | TCP/Smux 连接池减少连接建立开销 |
| **Raft 复制** | 写操作通过 Raft 在多副本间同步 |
| **并发加载** | DataNode 磁盘加载使用 `sync.WaitGroup` 并发 |

### 9.2 线程模型架构总览

```mermaid
graph TB
    subgraph Master
        M_HTTP[HTTP Server Goroutine]
        M_ATM[AdminTaskManager Ticker Goroutine]
        M_Raft[Raft Goroutines]
        M_Sched[Schedule Task Goroutine]
    end

    subgraph MetaNode
        MN_TCP[TCP Accept Loop]
        MN_Conn[serveConn Goroutines]
        MN_Smux[Smux Accept Loop]
        MN_Stream[serveSmuxStream Goroutines]
        MN_Raft[Raft Goroutines]
        MN_Update[UpdateNodeInfo Goroutine]
    end

    subgraph DataNode
        DN_TCP[TCP Accept Loop]
        DN_Conn[serveConn Goroutines]
        DN_Smux[Smux Accept Loop]
        DN_Disk[Disk Sample Goroutine]
        DN_GC[GC Timer Goroutine]
        DN_CPU[CPU Sample Goroutine]
        DN_Raft[Raft Goroutines]
    end

    subgraph SDK
        S_Meta[MetaClient Goroutines]
        S_Ext[ExtentClient Goroutines]
        S_Pool[Connection Pool]
    end

    M_ATM -->|AdminTask| MN_Conn
    M_ATM -->|AdminTask| DN_Conn
    S_Meta -->|元数据请求| MN_Conn
    S_Ext -->|数据请求| DN_Conn
    MN_Conn -->|Raft Propose| MN_Raft
    DN_Conn -->|Raft Propose| DN_Raft
```

### 9.3 关键设计决策

1. **Goroutine 而非线程池**: CubeFS 大量使用 Goroutine 处理并发，Go 运行时自动调度，无需手动管理线程池。

2. **连接级隔离**: 每个 TCP/Smux 连接由独立 Goroutine 处理，避免了连接间相互阻塞。

3. **AdminTaskManager 解耦**: Master 通过 `AdminTaskManager` 异步下发管理任务，业务逻辑与网络通信解耦。

4. **Raft 同步写 + 直接读**: 写操作通过 Raft 保证一致性，读操作直接读本地内存/磁盘，兼顾一致性与性能。

5. **Smux 多路复用**: 在单条 TCP 连接上复用多个逻辑流，减少连接数同时保持并发能力。

6. **定时器集群**: 各组件通过多个独立 Ticker Goroutine 执行周期性任务(心跳、GC、采样、统计)，互不干扰。

---

> **注**: 本文档基于 CubeFS 源码分析，主要参考文件:
> - `master/admin_task_manager.go` - AdminTask 管理器
> - `metanode/server.go`, `metanode/metanode.go` - MetaNode 服务
> - `datanode/server.go` - DataNode 服务
> - `sdk/data/stream/extent_client.go` - SDK 数据客户端