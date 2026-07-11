# CubeFS 存储模型与模块架构分析

## 一、项目概述

**CubeFS**（"储宝"）是一个开源的云原生分布式文件和对象存储系统，由CNCF托管并已毕业。它支持多种访问协议（POSIX、HDFS、S3、REST API），提供高可扩展的元数据服务、强一致性、多租户隔离、混合云加速等特性。

### 关键特性
- **多协议支持**：POSIX、HDFS、S3、REST API
- **高可用元数据服务**：强一致性保证
- **灵活存储策略**：支持副本和纠删码
- **多租户支持**：资源利用和隔离
- **混合云I/O加速**：多级缓存
- **分布式设计**：支持大规模容器平台

---

## 二、CubeFS 存储模型

### 2.1 核心层级结构

CubeFS采用**三层存储模型**：

```
┌─────────────────────────────────────────────────────────┐
│                    应用层 (Clients)                     │
│  POSIX Client | HDFS Client | S3/Object Client        │
└────────────────────┬────────────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────────────┐
│                  管理/访问层                             │
│  Master Server | MetaNode | ObjectNode | AuthNode      │
└────────────────────┬────────────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────────────┐
│                  存储层 (DataNode)                      │
│  数据分片 | 副本管理 | 纠删码 | 本地存储                 │
└─────────────────────────────────────────────────────────┘
```

### 2.2 数据分片模型

CubeFS采用**分片存储**架构，将数据分割为多个数据分片（Data Partition）：

```
Volume (卷)
├── MetaPartition 1   [元数据分片]
│   ├── Inode 存储
│   ├── Dentry 存储
│   └── xattr 存储
│
├── MetaPartition 2
├── ...
│
├── DataPartition 1   [数据分片]
│   ├── Replica 1 (DataNode A)
│   ├── Replica 2 (DataNode B)
│   └── Replica 3 (DataNode C)
│
├── DataPartition 2
└── ...
```

### 2.3 副本与纠删码策略

```
副本模式（Replication）
- 默认3副本
- 同步副本保证强一致性
- 适用于热数据、小文件

纠删码模式（Erasure Coding）
- EC(k,m)编码：k个数据块，m个校验块
- 更高的存储效率
- 适用于冷数据、大文件
- 通过 BlobStore 子系统实现
```

---

## 三、模块架构

### 3.1 模块组成

```
cubefs/
├── master/              # 集群元数据管理
│   ├── server.go        # Master Server 主体
│   ├── cluster.go       # 集群管理
│   ├── topology.go      # 拓扑管理
│   └── metadata.go      # 元数据管理
│
├── metanode/            # 元数据节点
│   ├── server.go        # MetaNode Server
│   ├── metanode.go      # 核心逻辑
│   ├── partition.go     # 元数据分片
│   └── inode.go         # Inode 管理
│
├── datanode/            # 数据节点
│   ├── server.go        # DataNode Server
│   ├── datanode.go      # 核心逻辑
│   ├── partition.go     # 数据分片
│   └── storage.go       # 存储管理
│
├── objectnode/          # 对象存储节点（S3）
│   ├── server.go        # ObjectNode Server
│   ├── bucket.go        # 桶管理
│   ├── object.go        # 对象管理
│   └── api.go           # S3 API 实现
│
├── client/              # 客户端库
│   ├── fs.go            # 文件系统客户端
│   ├── api.go           # 核心 API
│   └── cache.go         # 缓存层
│
├── blobstore/           # Blob 存储子系统
│   ├── access/          # 访问接口
│   ├── storage/         # 存储引擎
│   └── ec/              # 纠删码实现
│
├── authnode/            # 认证节点
├── lcnode/              # 生命周期节点
├── raftstore/           # Raft 一致性存储
├── proto/               # 协议定义（Protobuf）
├── util/                # 工具库
├── sdk/                 # SDK 支持
└── test/                # 测试套件
```

### 3.2 主要模块详解

#### 3.2.1 Master Server 模块

**职责**：
- 集群拓扑管理
- 卷（Volume）生命周期管理
- 元数据分片和数据分片分配
- 节点健康检查
- 分布式锁和一致性协调

**核心组件**：
```go
type Server struct {
    id              uint64
    clusterName     string
    rocksDBStore    *raftstore_db.RocksDBStore  // 持久化存储
    raftStore       raftstore.RaftStore          // Raft 共识
    fsm             *MetadataFsm                 // 有限状态机
    partition       raftstore.Partition          // 分片信息
    cluster         *Cluster                     // 集群管理
}
```

**关键功能**：
- 基于 RocksDB + Raft 实现高可用元数据
- 支持多主（Multi-Master）部署
- 自动分片均衡
- 故障转移和恢复

#### 3.2.2 MetaNode 模块

**职责**：
- 元数据存储和管理
- Inode 和 Dentry 操作
- 元数据分片的副本

**核心通信**：
```go
func (m *MetaNode) startServer() {
    // TCP 服务监听
    ln, err := net.Listen("tcp", addr)
    // 处理客户端连接
    go m.serveConn(conn)
}

func (m *MetaNode) handlePacket(conn net.Conn, p *Packet) {
    // 处理元数据操作
    err = m.metadataManager.HandleMetadataOperation(conn, p)
}
```

**支持协议**：
- 原生TCP协议
- SMUX（stream multiplexing）多路复用

#### 3.2.3 DataNode 模块

**职责**：
- 数据块存储
- 副本管理
- 心跳报告

**存储策略**：
- 副本模式：多个副本存储
- 纠删码模式：通过 BlobStore 存储

#### 3.2.4 ObjectNode 模块

**职责**：
- S3 兼容接口实现
- 对象到 Inode 的映射
- 访问控制和审计

**核心特性**：
```go
type ObjectNode struct {
    vm         *VolumeManager    // 卷管理
    mc         *master.MasterClient // Master 通信
    userStore  UserInfoStore     // 用户信息
    httpServer *http.Server      // HTTP 服务
}
```

- 支持 REST API
- S3 兼容 API
- 对象元数据缓存
- QoS 限流

#### 3.2.5 Client 模块

**职责**：
- POSIX 文件系统接口
- 数据读写

**核心机制**：
- 连接池
- 本地缓存
- 错误重试
- 元数据缓存

---

## 四、数据流与交互

### 4.1 写入流程

```
Client Write Request
    │
    ├─► 元数据操作
    │   └─► MetaNode: 创建 Inode，更新 Dentry
    │
    ├─► 数据写入
    │   ├─► 获取数据分片信息 (Master → MetaNode)
    │   ├─► 选择 DataNode
    │   └─► 写入数据副本 (Client → DataNode)
    │
    └─► 返回确认
```

### 4.2 读取流程

```
Client Read Request
    │
    ├─► 元数据查询
    │   └─► MetaNode: 获取 Inode 信息
    │
    ├─► 数据分片定位
    │   └─► Master: 查询分片副本位置
    │
    ├─► 数据读取
    │   └─► Client → DataNode (或本地缓存)
    │
    └─► 返回数据
```

### 4.3 元数据操作流程

```
Client Meta Op
    │
    └─► Master (协调)
        │
        ├─► Raft 一致性协议
        │   └─► 所有 Master 节点同步
        │
        ├─► 分配资源 (分片编号等)
        │
        └─► MetaNode 执行实际操作
            └─► RocksDB 持久化
```

---

## 五、架构设计模式

### 5.1 分离架构 (Separation of Concerns)

```
┌──────────────────────────────────────┐
│   Master (控制平面)                   │
│   - 集群管理                          │
│   - 资源分配                          │
│   - 调度策略                          │
└──────────────┬───────────────────────┘
               │ 管理指令
┌──────────────▼───────────────────────┐
│   MetaNode (元数据平面)               │
│   - 元数据存储                        │
│   - 一致性维护                        │
└──────────────┬───────────────────────┘
               │ 元数据索引
┌──────────────▼───────────────────────┐
│   DataNode (数据平面)                 │
│   - 数据存储                          │
│   - 副本和纠删码                      │
└─────────────────────────────────────┘
```

### 5.2 强一致性保证

```
Master Server
    ├─► Raft 协议
    │   ├─► 领导者选举
    │   ├─► 日志复制
    │   └─► 状态机应用
    │
    └─► RocksDB KV 存储
        └─► 持久化日志和快照
```

### 5.3 多副本容错

```
DataPartition = [Replica1, Replica2, Replica3]
    │
    ├─► 写入：同步到所有副本
    │   └─► 任意副本故障 → 自动恢复
    │
    └─► 读取：可从任意副本读取
        └─► 副本不可用 → 切换其他副本
```

---

## 六、关键技术点

| 技术点 | 实现 | 说明 |
|------|------|------|
| **一致性协议** | Raft (etcd/raft v3) | Master 之间的一致性 |
| **存储引擎** | RocksDB | 高性能 KV 存储 |
| **网络通信** | TCP + Protocol Buffers | 高效的二进制协议 |
| **多路复用** | SMUX | 连接复用 |
| **纠删码** | Reed-Solomon (klauspost/reedsolomon) | EC 编码 |
| **缓存** | 多级缓存（内存/本地磁盘） | 性能优化 |
| **监控** | Prometheus + Consul | 指标收集和服务发现 |
| **副本管理** | 3副本 (可配) | 高可用 |

---

## 七、部署架构

```
生产环境部署示例：

┌─────────────────────────────────────────────────────────┐
│              Client Layer (POSIX/S3)                     │
└────────────────┬────────────────────────────────────────┘
                 │
        ┌────────┼────────┐
        │        │        │
    ┌───▼─┐ ┌───▼─┐ ┌───▼─┐
    │M1   │ │M2   │ │M3   │  Master Cluster (3节点)
    └──┬──┘ └──┬──┘ └──┬──┘
       │       │       │
    ┌──▼───────▼───────▼──┐
    │  Raft Consensus      │  (支持故障转移)
    └──┬───────────────────┘
       │
   ┌───┼───────────────────────────────┐
   │   │                               │
┌──▼─┐┌▼──┐ ... ┌─────┐              │
│MN1 ││MN2│     │MNn  │  MetaNode集群
└──┬─┘└─┬─┘     └────┬┘
   │    │            │
┌──▼────▼────────────▼──┐
│   Metadata Distribution  │
└──┬───────────────────────┘
   │
   ├─────────────────────────┬──────────────┐
   │                         │              │
┌──▼─┐┌────┐ ... ┌────┐ ┌──▼─┐┌────┐ ┌──▼─┐
│DN1 ││DN2 │     │DNn │ │ON1 ││ON2 │ │ONm │
└────┘└────┘     └────┘ └────┘└────┘ └────┘
DataNode Cluster        ObjectNode Cluster
```

---

## 八、性能优化策略

### 8.1 小文件优化
- 小文件聚合存储
- 批量元数据操作
- 连接复用

### 8.2 大文件优化
- 数据分片并行传输
- 后台异步操作
- 纠删码加速

### 8.3 缓存策略
- 客户端缓存
- 元数据缓存
- ObjectNode 对象缓存

### 8.4 并发优化
- Raft 日志批处理
- 数据写入管道化
- 连接复用 (SMUX)

---

## 九、总结

CubeFS采用**经典的分布式存储架构**：

1. **控制平面** (Master)：基于 Raft 的高可用元数据管理
2. **元数据平面** (MetaNode)：分布式元数据存储
3. **数据平面** (DataNode/ObjectNode)：可扩展的数据存储
4. **访问层** (Client)：多协议支持

其核心优势在于：
- **强一致性**：通过 Raft 协议保证
- **高可用性**：多副本容错机制
- **可扩展性**：分片和分布式设计
- **灵活性**：支持多种存储策略和访问协议
- **云原生**：CNCF 认可的生产级系统

---

一个文件是被切成多个extent分片，保存到datanode节点的吗？

让我查看一下CubeFS中关于extent分片的实现细节。

## **是的，文件会被切成多个 Extent 分片存储到 DataNode**

根据CubeFS官方文档和源代码分析，这里是完整的机制：

### 📋 **Extent 存储结构**

```
DataPartition 目录结构（磁盘上的实际存储）：

datapartition_2181_128849018880/
├── 1              # ExtentID=1 (extent文件)
├── 10             # ExtentID=10 (extent文件)
├── 1024           # ExtentID=1024 (extent文件)
├── ...
├── EXTENT_CRC     # CRC校验文件
├── EXTENT_META    # Extent元数据
├── META           # 分片元数据
└── wal_2181/      # Raft日志目录
```

### 🔍 **两种 Extent 类型**

CubeFS根据文件大小自动选择不同的存储方式：

#### **1. TinyExtent（小文件）**
```
用途：存储小文件或大文件的尾部数据
- 最大容量：4 TiB
- ExtentID：1 ~ 64
- 数据对齐：4 KiB 对齐
- 特点：多个小文件聚合存储在一个 Extent

例如：
File1.txt (10KB) ┐
File2.txt (8KB)  ├─→ TinyExtent#1
File3.txt (5KB)  │
                 └─ TINYEXTENT_DELETE 中记录删除的记录
```

#### **2. NormalExtent（大文件）**
```
用途：存储大文件
- 最大容量：128 MiB (= 1024块 × 128KiB)
- ExtentID：从 1024 开始
- 块大小：128 KiB
- 特点：每个 Extent 只存储一个文件的一部分

例如：
LargeFile.bin (500MB)
  ├─→ DataPartition#1 - NormalExtent#1024 (0MB ~ 128MB)
  ├─→ DataPartition#1 - NormalExtent#1025 (128MB ~ 256MB)
  ├─→ DataPartition#2 - NormalExtent#1026 (256MB ~ 384MB)
  └─→ DataPartition#2 - NormalExtent#1027 (384MB ~ 500MB)
```

### 💾 **文件到 Extent 的映射过程**

```go
// SDK 客户端判断存储模式
func (s *Streamer) GetStoreMod(offset int, size int) (storeMode int) {
    // 小文件通常在第一次写入时使用 TinyExtent
    if offset+size > s.tinySizeLimit() {
        storeMode = proto.NormalExtentType  // 大文件
    } else {
        storeMode = proto.TinyExtentType    // 小文件
    }
    return
}
```

### 📊 **元数据记录**

| 文件 | 说明 |
|-----|------|
| **EXTENT_META** | 记录最大 ExtentID、已分配 ExtentID |
| **EXTENT_CRC** | 每个 Extent 的 CRC 校验值（4096字节/条） |
| **TINYEXTENT_DELETE** | 记录已删除的小 Extent，格式：`ExtentID\|offset\|size` |
| **NORMALEXTENT_DELETE** | 记录已删除的大 Extent，只记录 ExtentID |

### 🔄 **写入流程**

```
Client Write Request
    │
    ├─► 确定存储类型
    │   ├─ 文件 < 阈值？ → TinyExtent
    │   └─ 文件 ≥ 阈值？ → NormalExtent
    │
    ├─► 申请 Extent
    │   ├─► Master 分配 DataPartition
    │   └─► DataNode 分配 ExtentID
    │
    ├─► 写入数据
    │   ├─► 写入数据到 Extent 文件
    │   ├─► 更新 EXTENT_META（偏移量）
    │   └─► 更新 CRC 校验
    │
    └─► 元数据更新
        └─► MetaNode 记录：Inode 中包含 ExtentKey[ExtentID, Offset, Size]
```

### 🎯 **关键参数**

```go
// 源代码中的常量
type ExtentKey struct {
    PartitionId  uint64  // 数据分片ID
    ExtentId     uint64  // Extent文件ID
    ExtentOffset int64   // 在 Extent 中的偏移
    Size         int64   // 数据大小
}

// Inode 中包含的 ExtentKey 数组
type Inode struct {
    ExtentKeys []ExtentKey  // 每个 Extent 的引用
    Size       int64        // 文件总大小
}
```

### 📈 **存储示例**

假设有一个 500MB 的大文件，在3副本集群中的存储：

```
File: LargeFile.bin (500 MB)
│
├─ 副本1: DataNode-A
│   ├─ DataPartition#1 → NormalExtent#1024 (128MB)
│   ├─ DataPartition#1 → NormalExtent#1025 (128MB)
│   ├─ DataPartition#2 → NormalExtent#1026 (128MB)
│   └─ DataPartition#2 → NormalExtent#1027 (16MB)
│
├─ 副本2: DataNode-B  (相同的 Extent 分布)
│   └─ ...
│
└─ 副本3: DataNode-C  (相同的 Extent 分布)
    └─ ...
```

### ✅ **总结**

1. **是的，文件被切成多个 Extent**
2. **小文件**：多个小文件聚合到一个 TinyExtent（ID: 1-64）
3. **大文件**：一个大文件分成多个 NormalExtent（ID: 1024+），每个最大 128MB
4. **Extent 分布**：可以跨越多个 DataPartition，分布在不同的 DataNode 上
5. **元数据**：Inode 中记录 `ExtentKey` 数组，包含每个 Extent 的位置和大小

## 参考

- 官方文档：https://cubefs.io/docs/master/overview/introduction.html
- 研究论文：CFS: A Distributed File System for Large Scale Container Platforms (SIGMOD'19)
- GitHub 仓库：https://github.com/cubefs/cubefs

```
