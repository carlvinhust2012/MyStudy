# CubeFS 使用的 Raft 协议分析

## 一、结论

CubeFS 使用的是基于 **CoreOS etcd raft 库**（https://github.com/etcd-io/etcd）的 Raft 协议实现，具体由 **tiglabs** 团队在 etcd raft 基础上进行了二次开发（增加 multi-raft 等特性），并以内部依赖的形式放在 `depends/tiglabs/raft` 目录下，再由 `raftstore` 包封装对外提供分区（Partition）级别的多 raft 服务。

> **一句话总结**：CubeFS 的 Raft 实现 = `etcd raft`（CoreOS 原始库） + `tiglabs/raft`（二次开发增强层） + `raftstore`（CubeFS 业务封装层）。

---

## 二、背景

### 2.1 Raft 协议简介

Raft 是一种为了管理复制日志的一致性算法。它提供了和 Paxos 算法相同的功能和性能，但是它的算法结构和 Paxos 不同，使得它比 Paxos 更容易理解，也更容易构建实际的系统。

Raft 将一致性算法分解为三个子问题：
- **Leader 选举**：选出一个 Leader 负责处理所有客户端请求
- **日志复制**：Leader 将日志条目复制到其他服务器
- **安全性**：确保状态机安全性的约束条件

### 2.2 CubeFS 的一致性需求

CubeFS 是一个分布式文件系统，其元数据节点（MetaNode）和主节点（Master）需要强一致性保证。为了实现这一目标，CubeFS 采用 Raft 协议来保证元数据的强一致性复制，确保在节点故障时数据不丢失且服务可用。

---

## 三、证据来源

### 3.1 `raftstore` 包装层的导入证据

文件 `raftstore/raftstore.go` 中，导入并使用了 tiglabs 的 raft 实现：

```go
import (
    "fmt"
    syslog "log"
    "os"
    "path"
    "strconv"
    "time"

    "github.com/cubefs/cubefs/depends/tiglabs/raft"
    "github.com/cubefs/cubefs/depends/tiglabs/raft/logger"
    "github.com/cubefs/cubefs/depends/tiglabs/raft/proto"
    "github.com/cubefs/cubefs/depends/tiglabs/raft/storage/wal"
    raftlog "github.com/cubefs/cubefs/depends/tiglabs/raft/util/log"
    utilConfig "github.com/cubefs/cubefs/util/config"
)
```

并调用 `raft.NewRaftServer(rc)`、`raft.DefaultConfig()` 等接口创建 Raft 服务：

```go
func NewRaftStore(cfg *Config, extendCfg *utilConfig.Config) (mr RaftStore, err error) {
    // ...
    rc := raft.DefaultConfig()
    rc.NodeID = cfg.NodeID
    rc.LeaseCheck = true
    rc.PreVote = true
    // ...
    rs, err := raft.NewRaftServer(rc)
    // ...
}
```

### 3.2 tiglabs/raft 的 README 说明

文件 `depends/tiglabs/raft/README.md` 明确写道：

> A multi-raft implementation built on top of the [CoreOS etcd raft library](https://github.com/etcd-io/etcd).

并说明在 CoreOS etcd/raft 基础上做了修改，新增了多项特性。

### 3.3 NOTICE 版权声明

文件 `depends/tiglabs/raft/NOTICE`：

```
CoreOS Project
Copyright 2014 CoreOS, Inc

This product includes software developed at CoreOS, Inc.
(http://www.coreos.com/).

Modified work Copyright 2018 The tiglabs Authors.
```

子目录 `depends/tiglabs/raft/etcd/NOTICE` 同样标注了 CoreOS 版权：

```
CoreOS Project
Copyright 2014 CoreOS, Inc

This product includes software developed at CoreOS, Inc.
(http://www.coreos.com/).
```

---

## 四、tiglabs/raft 在 etcd raft 基础上的增强特性

根据 `depends/tiglabs/raft/README.md`，tiglabs 在 CoreOS etcd/raft 实现基础上增加了以下特性：

| 特性 | 说明 |
|------|------|
| **multi-raft 支持** | 支持单个节点上运行多个 raft 实例，CubeFS 据此实现分区级别的多 raft |
| **snapshot manager** | 快照管理器，优化快照的创建与传输 |
| **合并压缩的心跳消息** | 减少心跳消息的网络开销，提升大规模集群的扩展性 |
| **下线副本检测** | 能够检测不健康的副本 |
| **单 raft panic 隔离可检测** | 单个 raft 实例 panic 不会影响其他实例，且可被检测 |
| **新的 WAL 实现** | 重新实现的 Write-Ahead Log，替换 etcd 原始 WAL |
| **导出更多运行状态** | 提供更丰富的运行时状态信息，便于监控与运维 |
| **batch commit 实现** | 批量提交，提升写入吞吐量 |

---

## 五、架构层次关系

CubeFS 的 Raft 实现采用三层封装架构：

```
┌─────────────────────────────────────────────────────┐
│  CubeFS 业务层（Master / MetaNode / AuthNode）       │
│  通过 raftstore.RaftStore 接口使用 Raft 服务         │
└──────────────────────┬──────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────┐
│  raftstore 包（cubefs/raftstore/）                   │
│  - 封装 Partition 概念                               │
│  - 管理 WAL 路径与配置                               │
│  - 提供 RaftStore 接口                               │
│  - 文件：raftstore.go, partition.go, config.go 等   │
└──────────────────────┬──────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────┐
│  tiglabs/raft（cubefs/depends/tiglabs/raft/）        │
│  - 基于 etcd raft 的 multi-raft 实现                 │
│  - 增加 snapshot manager、batch commit 等特性        │
│  - 文件：raft.go, server.go, raft_fsm_*.go 等       │
└──────────────────────┬──────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────┐
│  etcd raft（CoreOS 原始库）                          │
│  - 经典 Raft 协议的 Go 实现                          │
│  - https://github.com/etcd-io/etcd                  │
│  - Apache License 2.0                               │
└─────────────────────────────────────────────────────┘
```

### 关键接口说明

`raftstore` 包对外暴露的核心接口：

```go
type RaftStore interface {
    CreatePartition(cfg *PartitionConfig) (Partition, error)
    Stop()
    RaftConfig() *raft.Config
    RaftStatus(raftID uint64) (raftStatus *raft.Status)
    NodeManager
    RaftServer() *raft.RaftServer
    RemoveBackup(id uint64) error
    GetPeers(id uint64) (nodes []uint64)
}
```

通过 `CreatePartition` 创建一个 raft 分区，每个分区对应一个独立的 raft 实例，从而实现 multi-raft 架构。

---

## 六、配置参数

CubeFS 的 raft 相关配置（见 `raftstore/config.go`）主要包括：

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| `HeartbeatPort` | `DefaultHeartbeatPort` | 心跳端口 |
| `ReplicaPort` | `DefaultReplicaPort` | 副本复制端口 |
| `NumOfLogsToRetain` | `DefaultNumOfLogsToRetain` | 保留的日志数量 |
| `ElectionTick` | `DefaultElectionTick` | 选举超时 tick 数 |
| `TickInterval` | `DefaultTickInterval` | tick 间隔（毫秒） |
| `RecvBufSize` | `2048` | 接收缓冲区大小 |
| `LeaseCheck` | `true` | 租约检查 |
| `PreVote` | `true` | 预投票（PreVote）优化 |

其中 `PreVote` 是 etcd raft 的经典优化，避免网络分区恢复后由于旧 term 扰乱集群。

---

## 七、结论

CubeFS **没有**使用 `hashicorp/raft`，而是使用了 **etcd 的 raft 库**（CoreOS 维护，Apache 2.0 协议）。具体路径为：

1. **底层**：CoreOS etcd raft 库 — 经典 Raft 协议的 Go 实现
2. **中间层**：tiglabs/raft — 在 etcd raft 基础上增加 multi-raft、snapshot manager、batch commit 等特性
3. **上层**：raftstore — CubeFS 业务封装层，提供 Partition 级别的多 raft 服务

这种分层设计使 CubeFS 既能复用 etcd raft 成熟稳定的协议实现，又能通过 tiglabs 的增强满足大规模分布式存储场景对多分区、高性能、可运维性的需求。

---

## 参考文件

- `cubefs/raftstore/raftstore.go` — RaftStore 接口与实现
- `cubefs/raftstore/config.go` — Raft 配置定义
- `cubefs/raftstore/partition.go` — Partition（raft 分区）实现
- `cubefs/depends/tiglabs/raft/README.md` — tiglabs/raft 说明文档
- `cubefs/depends/tiglabs/raft/NOTICE` — 版权声明
- `cubefs/depends/tiglabs/raft/etcd/NOTICE` — etcd 版权声明
- https://github.com/etcd-io/etcd — etcd raft 原始仓库