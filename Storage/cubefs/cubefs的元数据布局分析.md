# CubeFS 元数据布局与时序流程分析

> 基于 `cubefs/metanode` 源码分析，梳理 MetaPartition 的内存布局、持久化结构以及关键操作的 Raft 共识时序。

---

## 目录

1. [总体架构](#1-总体架构)
2. [MetaPartition 内存布局](#2-metapartition-内存布局)
3. [核心数据结构](#3-核心数据结构)
4. [持久化文件布局](#4-持久化文件布局)
5. [Raft 共识与 FSM 机制](#5-raft-共识与-fsm-机制)
6. [关键时序流程图](#6-关键时序流程图)
7. [快照与恢复机制](#7-快照与恢复机制)
8. [总结](#8-总结)

---

## 1. 总体架构

CubeFS 的元数据子系统由 **MetaNode** 进程组成，每个 MetaNode 上运行多个 **MetaPartition** 实例。每个 MetaPartition 通过 **Raft** 协议在多个副本间实现强一致性，负责管理一段连续的 Inode ID 范围 `[Start, End)`。

```
┌─────────────────────────────────────────────────┐
│                   MetaNode                       │
│  ┌─────────────┐ ┌─────────────┐ ┌───────────┐ │
│  │ MetaPartition│ │ MetaPartition│ │   ...     │ │
│  │   (Raft     │ │   (Raft     │ │           │ │
│  │   Group 1)  │ │   Group 2)  │ │           │ │
│  └─────────────┘ └─────────────┘ └───────────┘ │
│         │              │                         │
│  ┌──────┴──────────────┴──────────────────┐     │
│  │         metadataManager                 │     │
│  └─────────────────────────────────────────┘     │
└─────────────────────────────────────────────────┘
```

### 核心组件关系

```mermaid
graph TB
    subgraph MetaNode
        MM[metadataManager]
        MM --> MP1[MetaPartition 1]
        MM --> MP2[MetaPartition 2]
        MM --> MPN[MetaPartition N]
    end

    subgraph "MetaPartition 内部"
        MP1 --> Config[MetaPartitionConfig]
        MP1 --> RaftPart[raftPartition]
        MP1 --> InodeTree[inodeTree BTree]
        MP1 --> DentryTree[dentryTree BTree]
        MP1 --> ExtendTree[extendTree BTree]
        MP1 --> MultipartTree[multipartTree BTree]
        MP1 --> TxProc[TransactionProcessor]
        MP1 --> FreeList[freeList]
        MP1 --> UidMgr[UidManager]
        MP1 --> QuotaMgr[MetaQuotaManager]
    end

    TxProc --> TxMgr[txManager]
    TxProc --> TxRes[txResource]
    TxMgr --> TxTree[txTree BTree]
    TxRes --> TxRbIno[txRbInodeTree BTree]
    TxRes --> TxRbDen[txRbDentryTree BTree]
```

---

## 2. MetaPartition 内存布局

`metaPartition` 结构体（`partition.go`）维护了以下核心内存数据结构：

```mermaid
graph LR
    subgraph "metaPartition 内存结构"
        IT[inodeTree<br/>BTree - Inode 集合]
        DT[dentryTree<br/>BTree - Dentry 集合]
        ET[extendTree<br/>BTree - XAttr 集合]
        MT[multipartTree<br/>BTree - Multipart 集合]
        
        TT[txTree<br/>BTree - 活跃事务]
        TRIT[txRbInodeTree<br/>BTree - 回滚Inode]
        TRDT[txRbDentryTree<br/>BTree - 回滚Dentry]
        
        FL[freeList<br/>延迟删除Inode列表]
        FHL[freeHybridList<br/>混合云迁移删除]
        
        UV[verSeq / multiVersionList<br/>多版本快照]
        
        UC[uniqChecker<br/>幂等去重检查器]
    end
```

### 状态字段

| 字段 | 类型 | 说明 |
|------|------|------|
| `applyID` | `uint64` | 已应用的 Raft 日志索引（Inode/Dentry 最大 applyID） |
| `storedApplyId` | `uint64` | 已持久化到磁盘快照的 applyID |
| `size` | `uint64` | 分区内所有文件总大小 |
| `verSeq` | `uint64` | 当前版本序列号（快照功能） |
| `state` | `uint32` | 分区状态：Standby → Start → Running → Shutdown → Stopped |

### 状态机流转

```mermaid
stateDiagram-v2
    [*] --> New
    New --> StateStandby: 创建
    StateStandby --> StateStart: Start() CAS
    StateStart --> StateRunning: onStart() 成功
    StateStart --> StateStandby: onStart() 失败
    StateRunning --> StateShutdown: Stop() CAS
    StateShutdown --> StateStopped: onStop() 完成
    StateStopped --> [*]
```

---

## 3. 核心数据结构

### 3.1 Inode（索引节点）

`Inode` 结构（`inode.go`）表示文件系统中的文件/目录元数据。

#### 内存结构

```
Inode {
    // 8-byte 字段
    Inode           uint64   // Inode ID
    Size            uint64   // 文件大小
    Generation      uint64   // 世代号
    CreateTime      int64    // 创建时间
    AccessTime      int64    // 访问时间
    ModifyTime      int64    // 修改时间
    Reserved        uint64   // 保留位（含标志位）
    LeaseExpireTime uint64   // 租约过期时间

    // 4-byte 字段
    Type            uint32   // 文件类型
    Uid             uint32   // 用户 ID
    Gid             uint32   // 组 ID
    NLink           uint32   // 硬链接计数
    Flag            int32    // 标志位
    StorageClass    uint32   // 存储类别
    ClientID        uint32   // 客户端 ID

    // 指针字段
    LinkTarget      []byte   // 符号链接目标
    multiSnap       *InodeMultiSnap          // 多版本快照
    HybridCloudExtents          *SortedHybridCloudExtents          // 混合云 Extents
    HybridCloudExtentsMigration *SortedHybridCloudExtentsMigration // 迁移 Extents
}
```

#### 序列化格式

```
Marshal Key:
  +-------+-------+
  | item  | Inode |
  +-------+-------+
  | bytes |   8   |
  +-------+-------+

Marshal Value:
  +-------+------+------+-----+----+----+----+--------+------------------+
  | item  | Type | Size | Gen | CT | AT | MT | ExtLen | MarshaledExtents |
  +-------+------+------+-----+----+----+----+--------+------------------+
  | bytes |  4   |  8   |  8  | 8  | 8  | 8  |   4    |      ExtLen      |
  +-------+------+------+-----+----+----+----+--------+------------------+

Marshal Entity (KV 对):
  +-------+-----------+--------------+-----------+--------------+
  | item  | KeyLength | MarshaledKey | ValLength | MarshaledVal |
  +-------+-----------+--------------+-----------+--------------+
  | bytes |     4     |   KeyLength  |     4     |   ValLength  |
  +-------+-----------+--------------+-----------+--------------+
```

#### Reserved 标志位

| 标志 | 值 | 说明 |
|------|------|------|
| `V2EnableEbsFlag` | `0x02` | 启用 EBS（BlobStore）对象 Extents |
| `V3EnableSnapInodeFlag` | `0x04` | 启用 Inode 快照 |
| `V4EnableHybridCloud` | `0x08` | 启用混合云 |
| `V4MigrationExtentsFlag` | `0x40` | 启用迁移 Extents |

### 3.2 Dentry（目录项）

`Dentry` 结构（`dentry.go`）表示目录中的条目，关联父目录与子 Inode。

#### 内存结构

```
Dentry {
    ParentId  uint64            // 父目录 Inode ID
    Inode     uint64            // 当前 Inode ID
    Name      string            // 名称
    Type      uint32            // 类型
    multiSnap *DentryMultiSnap  // 多版本快照
}
```

#### 序列化格式

```
Marshal Key:
  +-------+----------+------+
  | item  | ParentId | Name |
  +-------+----------+------+
  | bytes |    8     | rest |
  +-------+----------+------+

Marshal Value:
  +-------+-------+------+
  | item  | Inode | Type |
  +-------+-------+------+
  | bytes |   8   |   4  |
  +-------+-------+------+
```

#### BTree 排序规则

```go
func (d *Dentry) Less(than BtreeItem) bool {
    // 先按 ParentId 排序，相同则按 Name 排序
    return (d.ParentId < dentry.ParentId) ||
           (d.ParentId == dentry.ParentId && d.Name < dentry.Name)
}
```

### 3.3 其他数据结构

| 结构 | BTree | 说明 |
|------|-------|------|
| `Extend` | `extendTree` | 扩展属性（XAttr），Key 为 Inode ID |
| `Multipart` | `multipartTree` | Multipart 上传管理 |
| `TransactionInfo` | `txTree` | 活跃事务信息 |
| `TxRollbackInode` | `txRbInodeTree` | 事务回滚 Inode 记录 |
| `TxRollbackDentry` | `txRbDentryTree` | 事务回滚 Dentry 记录 |

---

## 4. 持久化文件布局

MetaPartition 的持久化数据存储在 `config.RootDir` 目录下，分为 **元数据文件** 和 **快照文件** 两部分。

### 4.1 目录结构

```
{RootDir}/
├── meta                          # MetaPartitionConfig (JSON)
├── snapshot/                     # 快照目录
│   ├── inode                     # Inode 快照
│   ├── dentry                    # Dentry 快照
│   ├── extend                    # Extend (XAttr) 快照
│   ├── multipart                 # Multipart 快照
│   ├── tx_info                   # 事务信息快照
│   ├── tx_rb_inode               # 事务回滚 Inode 快照
│   ├── tx_rb_dentry              # 事务回滚 Dentry 快照
│   ├── apply                     # ApplyID 文件
│   ├── transactionID             # 事务 ID 分配器
│   ├── uniqID                    # 唯一 ID 分配器
│   ├── uniqChecker               # 幂等检查器快照
│   ├── multiVer                  # 多版本列表
│   └── .sign                     # CRC 签名
└── .snapshot/                    # 临时快照写入目录
```

### 4.2 快照文件格式

#### Inode/Dentry/Tx 文件（长度前缀格式）

```
┌──────────┬───────────────────┬──────────┬───────────────────┬───┐
│ Len (4B) │ MarshaledData     │ Len (4B) │ MarshaledData     │...│
│  uint32  │    (Len bytes)    │  uint32  │    (Len bytes)    │   │
└──────────┴───────────────────┴──────────┴───────────────────┴───┘
```

每个记录由 4 字节长度头 + 变长数据体组成，整个文件末尾通过 CRC32 校验。

#### Extend/Multipart 文件（Varint 长度格式）

```
┌─────────────┬──────────────┬──────────────┬───────────────────┬───┐
│ Count (var) │ Len1 (var)   │ Data1 (Len1) │ Len2 (var)        │...│
│  记录总数   │ 第1条长度    │ 第1条数据    │ 第2条长度        │   │
└─────────────┴──────────────┴──────────────┴───────────────────┴───┘
```

#### ApplyID / TxID / UniqID 文件（纯文本格式）

```
# apply 文件
{applyID}|{cursor}

# transactionID 文件
{txID}

# uniqID 文件
{uniqId}
```

#### 多版本文件（multiVer）

```
{applyID}|{verListJSON}
```

### 4.3 快照文件常量定义

```go
const (
    snapshotDir         = "snapshot"
    snapshotDirTmp      = ".snapshot"
    inodeFile           = "inode"
    dentryFile          = "dentry"
    extendFile          = "extend"
    multipartFile       = "multipart"
    txInfoFile          = "tx_info"
    txRbInodeFile       = "tx_rb_inode"
    txRbDentryFile      = "tx_rb_dentry"
    applyIDFile         = "apply"
    TxIDFile            = "transactionID"
    uniqIDFile          = "uniqID"
    uniqCheckerFile     = "uniqChecker"
    verdataFile         = "multiVer"
    SnapshotSign        = ".sign"
    metadataFile        = "meta"
)
```

---

## 5. Raft 共识与 FSM 机制

### 5.1 写操作流程

所有写操作（CreateInode、UnlinkInode、CreateDentry 等）都必须通过 Raft 共识：

```mermaid
sequenceDiagram
    participant Client as Client
    participant Leader as MetaPartition Leader
    participant Raft as Raft Engine
    participant Follower as Follower MetaPartition
    participant FSM as FSM (Apply)

    Client->>Leader: 发起写请求 (e.g. CreateInode)
    Leader->>Leader: 构造 MetaItem{Op, V}
    Leader->>Raft: submit(op, data) - 提案到 Raft
    
    par 并行复制
        Raft->>Leader: 本地持久化日志
        and
        Raft->>Follower: AppendEntries 复制日志
        Follower->>Follower: 持久化日志
        Follower-->>Raft: ACK
    end

    Raft-->>Leader: 日志已提交 (Committed)
    Leader->>FSM: Apply(command, index)
    FSM->>FSM: Unmarshal MetaItem
    FSM->>FSM: switch msg.Op - 路由到对应 fsmXXX 方法
    FSM->>FSM: 更新 inodeTree/dentryTree 等 BTree
    FSM->>FSM: updateApplyID(index)
    FSM-->>Leader: 返回结果
    Leader-->>Client: 响应
```

### 5.2 FSM 操作类型

`partition_fsm.go` 中的 `Apply` 方法支持以下操作类型：

#### Inode 操作

| Op | 方法 | 说明 |
|----|------|------|
| `opFSMCreateInode` | `fsmCreateInode` | 创建 Inode |
| `opFSMCreateInodeQuota` | `fsmCreateInode` + `setInodeQuota` | 创建带配额的 Inode |
| `opFSMUnlinkInode` | `fsmUnlinkInode` | 删除 Inode 链接 |
| `opFSMUnlinkInodeOnce` | `fsmUnlinkInode` (带 UniqID) | 幂等删除 Inode |
| `opFSMCreateLinkInode` | `fsmCreateLinkInode` | 创建硬链接 |
| `opFSMEvictInode` | `fsmEvictInode` | 驱逐 Inode |
| `opFSMSetAttr` | `fsmSetAttr` | 设置属性 |
| `opFSMExtentTruncate` | `fsmExtentsTruncate` | 截断 Extents |
| `opFSMExtentsAdd` | `fsmAppendExtents` | 追加 Extents |
| `opFSMObjExtentsAdd` | `fsmAppendObjExtents` | 追加对象 Extents |

#### Dentry 操作

| Op | 方法 | 说明 |
|----|------|------|
| `opFSMCreateDentry` | `fsmCreateDentry` | 创建目录项 |
| `opFSMDeleteDentry` | `fsmDeleteDentry` | 删除目录项 |
| `opFSMUpdateDentry` | `fsmUpdateDentry` | 更新目录项 (rename) |

#### 事务操作

| Op | 方法 | 说明 |
|----|------|------|
| `opFSMTxInit` | `fsmTxInit` | 初始化事务 |
| `opFSMTxCreateInode` | `fsmTxCreateInode` | 事务内创建 Inode |
| `opFSMTxCreateDentry` | `fsmTxCreateDentry` | 事务内创建 Dentry |
| `opFSMTxCommit` | `fsmTxCommit` | 提交事务 |
| `opFSMTxRollback` | `fsmTxRollback` | 回滚事务 |
| `opFSMTxCommitRM` | `fsmTxCommitRM` | RM 阶段提交 |
| `opFSMTxRollbackRM` | `fsmTxRollbackRM` | RM 阶段回滚 |

#### 其他操作

| Op | 方法 | 说明 |
|----|------|------|
| `opFSMStoreTick` | 发送到 storeChan | 触发快照持久化 |
| `opFSMSetXAttr` | `fsmSetXAttr` | 设置扩展属性 |
| `opFSMCreateMultipart` | `fsmCreateMultipart` | 创建 Multipart |
| `opFSMSyncCursor` | 更新 Cursor | 同步 Cursor |
| `opFSMVersionOp` | `fsmVersionOp` | 多版本操作 |
| `opFSMUniqID` | `fsmUniqID` | 分配唯一 ID |

---

## 6. 关键时序流程图

### 6.1 MetaPartition 启动流程

```mermaid
sequenceDiagram
    participant MN as MetaNode
    participant MP as MetaPartition
    participant Load as load()
    participant Raft as RaftStore

    MN->>MP: Start(isCreate)
    MP->>MP: CAS state: Standby → Start
    
    alt isCreate && clusterEnableSnapshot
        MP->>MP: versionInit(isCreate)
        MP->>MN: masterClient.GetVerList(volName)
        MN-->>MP: VolVersionInfoList
    end

    MP->>Load: load(isCreate)
    
    par 并行加载
        Load->>Load: loadMetadata() - 读取 meta 配置
        Load->>Load: loadInode() - 加载 Inode 到 inodeTree
        Load->>Load: loadDentry() - 加载 Dentry 到 dentryTree
        Load->>Load: loadExtend() - 加载 XAttr 到 extendTree
        Load->>Load: loadMultipart() - 加载 Multipart
        Load->>Load: loadApplyID() - 加载 applyID
        Load->>Load: loadTxInfo() - 加载事务信息
        Load->>Load: loadTxRbInode() - 加载回滚 Inode
        Load->>Load: loadTxRbDentry() - 加载回滚 Dentry
        Load->>Load: loadTxID() - 加载 TxID
        Load->>Load: loadUniqID() - 加载 UniqID
        Load->>Load: loadUniqChecker() - 加载幂等检查器
        Load->>Load: loadMultiVer() - 加载多版本列表
    end

    Load-->>MP: 加载完成
    MP->>MP: startScheduleTask()
    MP->>MN: forceUpdateVolumeView(mp)
    MN-->>MP: VolumeView 更新
    
    alt 冷卷或支持 BlobStore
        MP->>MP: NewBlobStoreClientWrapper()
    end

    MP->>MP: startFreeList() - 启动延迟删除
    MP->>MP: startCheckerEvict() - 启动检查器驱逐
    MP->>Raft: startRaft(isCreate)
    Raft->>Raft: CreatePartition(PartitionConfig)
    Raft-->>MP: raftPartition 创建成功
    MP->>MP: ForceSetMetaPartitionToFininshLoad()
    MP->>MP: updateSize() - 启动定时统计
    MP->>MP: CAS state: Start → Running
```

### 6.2 创建文件（CreateInode）完整时序

```mermaid
sequenceDiagram
    participant C as Client
    participant SDK as MetaSDK
    participant MP as MetaPartition (Leader)
    participant Raft as Raft Engine
    participant FSM as Apply/FSM
    participant DP as DataPartition

    C->>SDK: create(path, mode)
    SDK->>SDK: 计算父目录所属 MetaPartition
    SDK->>MP: CreateInode(req)
    
    MP->>MP: 分配 InodeID (nextInodeID)
    MP->>MP: 构造 Inode 对象
    MP->>MP: 构造 MetaItem{opFSMCreateInode, inode.Marshal()}
    
    MP->>Raft: submit(op, data)
    
    par Raft 共识
        Raft->>Raft: Leader 写日志
        Raft->>Raft: 复制到 Followers
    end
    
    Raft-->>MP: 日志提交
    MP->>FSM: Apply(command, index)
    
    FSM->>FSM: msg.UnmarshalJson(command)
    FSM->>FSM: ino.Unmarshal(msg.V)
    FSM->>FSM: 更新 Cursor if ino.Inode > Cursor
    FSM->>FSM: fsmCreateInode(ino)
    FSM->>FSM: inodeTree.ReplaceOrInsert(ino)
    FSM->>FSM: updateApplyID(index)
    FSM-->>MP: 返回 InodeResponse{Status: OpOk}
    
    MP-->>SDK: 响应 (InodeID)
    
    Note over SDK,DP: 后续数据写入流程
    SDK->>DP: 数据写入 DataPartition
    SDK->>MP: ExtentAppend(ExtentKey)
    MP->>Raft: submit(opFSMExtentsAdd, ...)
    Raft-->>FSM: Apply
    FSM->>FSM: fsmAppendExtents(ino)
    FSM->>FSM: 更新 Inode 的 HybridCloudExtents
```

### 6.3 创建目录项（CreateDentry）时序

```mermaid
sequenceDiagram
    participant C as Client
    participant MP as MetaPartition (Leader)
    participant Raft as Raft Engine
    participant FSM as Apply/FSM

    C->>MP: CreateDentry(parentId, name, inode, type)
    MP->>MP: 构造 Dentry{ParentId, Name, Inode, Type}
    MP->>MP: 构造 MetaItem{opFSMCreateDentry, dentry.Marshal()}
    
    MP->>Raft: submit(op, data)
    
    par Raft 共识
        Raft->>Raft: Leader 持久化 + 复制到 Followers
    end
    
    Raft-->>MP: 日志提交
    MP->>FSM: Apply(command, index)
    
    FSM->>FSM: den.Unmarshal(msg.V)
    FSM->>FSM: dentryInTx(den.ParentId, den.Name) - 检查事务冲突
    FSM->>FSM: fsmCreateDentry(den, false)
    FSM->>FSM: dentryTree.ReplaceOrInsert(den)
    FSM->>FSM: 更新父目录 Inode 的 NLink
    FSM->>FSM: updateApplyID(index)
    FSM-->>MP: 返回状态
    MP-->>C: 响应
```

### 6.4 快照持久化时序（StoreTick）

```mermaid
sequenceDiagram
    participant T as 定时器
    participant FSM as FSM Apply
    participant SC as storeChan
    participant Store as store goroutine
    participant Disk as 磁盘

    T->>FSM: opFSMStoreTick (定时触发)
    FSM->>FSM: 收集各 BTree 快照
    Note over FSM: inodeTree, dentryTree, extendTree,<br/>multipartTree, txTree, txRbInodeTree,<br/>txRbDentryTree, uniqChecker, multiVerList
    FSM->>FSM: 构建 storeMsg{applyIndex, trees...}
    FSM->>SC: storeChan <- msg

    Store->>SC: 读取 storeMsg
    SC-->>Store: msg

    Store->>Disk: 创建 .snapshot 临时目录
    
    par 并行写入
        Store->>Disk: storeInode() - 写 inode 文件
        Store->>Disk: storeDentry() - 写 dentry 文件
        Store->>Disk: storeExtend() - 写 extend 文件
        Store->>Disk: storeMultipart() - 写 multipart 文件
        Store->>Disk: storeTxInfo() - 写 tx_info 文件
        Store->>Disk: storeTxRbInode() - 写 tx_rb_inode 文件
        Store->>Disk: storeTxRbDentry() - 写 tx_rb_dentry 文件
    end

    Store->>Disk: storeApplyID() - 写 apply 文件
    Store->>Disk: storeTxID() - 写 transactionID
    Store->>Disk: storeUniqID() - 写 uniqID
    Store->>Disk: storeUniqChecker() - 写 uniqChecker
    Store->>Disk: storeMultiVersion() - 写 multiVer
    Store->>Disk: 写 .sign CRC 签名
    
    Store->>Disk: .snapshot → snapshot (原子重命名)
    Store->>Store: 更新 storedApplyId
    Store->>Disk: 删除旧快照目录
```

### 6.5 Raft 快照传输与恢复时序

```mermaid
sequenceDiagram
    participant F as Follower (落后节点)
    participant L as Leader
    participant FSM_F as Follower FSM
    participant FSM_L as Leader FSM

    Note over F,L: Follower 日志落后过多，触发快照传输

    L->>FSM_L: Snapshot()
    FSM_L-->>L: newMetaItemIterator(mp) - 创建快照迭代器
    
    L->>L: 迭代器遍历所有 BTree
    
    loop 逐条发送快照数据
        L->>F: 发送快照数据流 (MetaItem 序列化)
        F->>FSM_F: ApplySnapshot(peers, iter)
        FSM_F->>FSM_F: snap.UnmarshalBinary(data)
        
        alt snap.Op == opFSMSnapFormatVersion
            FSM_F->>FSM_F: 检查快照格式版本
        else snap.Op == opFSMApplyId
            FSM_F->>FSM_F: 记录 appIndexID
        else snap.Op == opFSMTxId
            FSM_F->>FSM_F: 记录 txID
        else snap.Op == opFSMCursor
            FSM_F->>FSM_F: 记录 cursor
        else snap.Op == opFSMCreateInode
            FSM_F->>FSM_F: inodeTree.ReplaceOrInsert(ino)
        else snap.Op == opFSMCreateDentry
            FSM_F->>FSM_F: dentryTree.ReplaceOrInsert(den)
        else snap.Op == opFSMSetXAttr
            FSM_F->>FSM_F: extendTree.ReplaceOrInsert(extend)
        else snap.Op == opFSMCreateMultipart
            FSM_F->>FSM_F: multipartTree.ReplaceOrInsert(multipart)
        else snap.Op == opFSMTxSnapshot
            FSM_F->>FSM_F: txTree.ReplaceOrInsert(txInfo)
        else snap.Op == opFSMVerListSnapShot
            FSM_F->>FSM_F: 解析 verList
        end
    end

    L-->>F: 快照传输完成 (io.EOF)
    
    F->>FSM_F: defer 执行
    FSM_F->>FSM_F: 替换所有内存 BTree
    FSM_F->>FSM_F: 更新 applyID, txID, cursor, verSeq
    FSM_F->>F: 发送 storeMsg 到 storeChan
    F->>F: extReset <- struct{}{} 
    F->>F: blockUntilStoreSnapshot() - 等待落盘完成
```

### 6.6 事务（Transaction）流程

```mermaid
sequenceDiagram
    participant C as Client
    participant MP1 as MetaPartition A (Leader)
    participant MP2 as MetaPartition B (Leader)
    participant Master as Master

    Note over C,Master: 跨分区事务：在 MP1 创建文件，在 MP2 创建目录项

    C->>Master: 创建事务 TxCreate(txID, txType)
    Master-->>C: TransactionInfo

    C->>MP1: TxCreateInode(txInfo)
    MP1->>MP1: submit(opFSMTxCreateInode)
    MP1->>MP1: fsmTxCreateInode - 写入 txRbInodeTree
    MP1-->>C: InodeID

    C->>MP2: TxCreateDentry(txInfo)
    MP2->>MP2: submit(opFSMTxCreateDentry)
    MP2->>MP2: fsmTxCreateDentry - 写入 txRbDentryTree
    MP2-->>C: 成功

    alt 提交事务
        C->>Master: TxCommit(txID)
        Master->>MP1: TxCommitRM(txInfo)
        MP1->>MP1: submit(opFSMTxCommitRM)
        MP1->>MP1: fsmTxCommitRM - 将 txInode 转为正式 Inode
        MP1->>MP1: 从 txRbInodeTree 删除
        
        Master->>MP2: TxCommitRM(txInfo)
        MP2->>MP2: submit(opFSMTxCommitRM)
        MP2->>MP2: fsmTxCommitRM - 将 txDentry 转为正式 Dentry
        MP2->>MP2: 从 txRbDentryTree 删除
    else 回滚事务
        C->>Master: TxRollback(txID)
        Master->>MP1: TxRollbackRM(txInfo)
        MP1->>MP1: fsmTxRollbackRM - 删除预创建的 Inode
        Master->>MP2: TxRollbackRM(txInfo)
        MP2->>MP2: fsmTxRollbackRM - 删除预创建的 Dentry
    end
```

### 6.7 Leader 选举与初始化

```mermaid
sequenceDiagram
    participant Raft as Raft Engine
    participant MP as MetaPartition
    participant HC as HandleLeaderChange

    Raft->>HC: HandleLeaderChange(leaderID)
    
    alt 本节点成为 Leader
        HC->>HC: 关闭旧连接 (触发客户端重连)
        HC->>MP: storeChan <- {startStoreTick}
        
        alt Start==0 && Cursor==0 (新分区)
            HC->>MP: nextInodeID()
            MP-->>HC: rootInodeID
            HC->>MP: initInode(NewInode(rootInodeID, ModeDir))
            MP->>Raft: submit(opFSMCreateInode, rootInode)
        end
    else 本节点不再是 Leader
        HC->>MP: storeChan <- {stopStoreTick}
    end
```

### 6.8 多版本快照操作时序

```mermaid
sequenceDiagram
    participant Master as Master
    participant MP as MetaPartition
    participant FSM as FSM
    participant Chan as verUpdateChan

    Note over Master,MP: 创建版本（Prepare → Commit）

    Master->>MP: HandleVersionOp(CreateVersionPrepare, verSeq)
    MP->>Chan: verUpdateChan <- opData
    Chan->>FSM: submit(opFSMVersionOp, reqData)
    FSM->>FSM: fsmVersionOp(reqData)
    FSM->>FSM: VerList.append({Ver: verSeq, Status: VersionPrepare})
    FSM->>FSM: mp.verSeq = verSeq

    Master->>MP: HandleVersionOp(CreateVersionCommit, verSeq)
    MP->>Chan: verUpdateChan <- opData
    Chan->>FSM: submit(opFSMVersionOp, reqData)
    FSM->>FSM: fsmVersionOp(reqData)
    FSM->>FSM: VerList[last].Status = VersionNormal

    Note over Master,MP: 删除版本

    Master->>MP: HandleVersionOp(DeleteVersion, verSeq)
    MP->>Chan: verUpdateChan <- opData
    Chan->>FSM: submit(opFSMVersionOp, reqData)
    FSM->>FSM: fsmVersionOp(reqData)
    FSM->>FSM: VerList 删除指定版本
    FSM->>FSM: 清理 Inode/Dentry 中的历史版本数据
```

---

## 7. 快照与恢复机制

### 7.1 快照触发条件

1. **定时触发**：`opFSMStoreTick` 由 leader 定期发起
2. **Leader 切换**：`HandleLeaderChange` 发送 `startStoreTick`
3. **ApplySnapshot 后**：follower 接收 raft 快照后触发一次落盘

### 7.2 快照一致性保证

```mermaid
graph TB
    A[FSM Apply 更新内存] --> B[更新 applyID]
    B --> C{定时触发 opFSMStoreTick}
    C --> D[快照 BTree 引用 - 写时复制]
    D --> E[并行写入 .snapshot 临时目录]
    E --> F[所有文件写入完成]
    F --> G[计算 CRC 签名]
    G --> H[.snapshot → snapshot 原子重命名]
    H --> I[更新 storedApplyId]
    I --> J[删除旧快照目录]
```

### 7.3 CRC 校验

每个快照文件在加载时都会进行 CRC32 校验：

```go
// 加载时校验
if res := crcCheck.Sum32(); res != crc {
    return ErrSnapshotCrcMismatch
}
```

| 文件 | CRC 计算方式 |
|------|-------------|
| `inode` | 遍历每条记录的 Length + Data |
| `dentry` | 遍历每条记录的 Length + Data |
| `extend` | Count + 每条记录的 Length + Data |
| `multipart` | Count + 每条记录的 Length + Data |
| `tx_info` / `tx_rb_inode` / `tx_rb_dentry` | 遍历每条记录 |
| `uniqChecker` | 整个文件内容 |
| `multiVer` | JSON 序列化后的 verList |

---

## 8. 总结

### 8.1 元数据布局特点

| 特性 | 说明 |
|------|------|
| **分区化** | Inode ID 范围 `[Start, End)` 划分到不同 MetaPartition |
| **BTree 内存索引** | 所有元数据使用 BTree 在内存中维护，支持高效范围查询 |
| **Raft 强一致** | 所有写操作通过 Raft 共识，保证多副本一致 |
| **快照持久化** | 定时将内存 BTree 序列化到磁盘文件，带 CRC 校验 |
| **多版本支持** | Inode/Dentry 支持快照版本链，实现快照回滚 |
| **事务支持** | 跨分区事务通过 TxInfo + Rollback 记录实现 |
| **混合云** | 支持 Replica 和 BlobStore 两种存储类别 |

### 8.2 数据流总结

```mermaid
graph LR
    subgraph 写入路径
        W1[Client 写请求] --> W2[MetaPartition Leader]
        W2 --> W3[Raft 共识]
        W3 --> W4[FSM Apply 更新 BTree]
        W4 --> W5[定时快照落盘]
    end

    subgraph 读取路径
        R1[Client 读请求] --> R2{Leader 或 Follower?}
        R2 -->|Leader| R3[直接读 BTree]
        R2 -->|Follower<br/>FollowerRead=true| R4[读 BTree 副本]
    end

    subgraph 恢复路径
        L1[启动] --> L2[load 快照文件]
        L2 --> L3[重建 BTree]
        L3 --> L4[启动 Raft]
        L4 --> L5{需要补全日志?}
        L5 -->|是| L6[ApplySnapshot]
        L5 -->|否| L7[正常服务]
    end
```

### 8.3 关键设计要点

1. **写时复制快照**：`opFSMStoreTick` 通过获取 BTree 的 `GetTree()` 引用，实现写时复制，不阻塞 FSM 继续处理写入
2. **幂等性保证**：`nonIdempotent` 互斥锁 + `uniqChecker` 确保同一操作不会被重复应用
3. **延迟删除**：`freeList` 机制实现 Inode 的延迟删除，避免删除大文件时阻塞
4. **快照格式版本兼容**：`opFSMSnapFormatVersion` 支持快照格式的版本演进
5. **混合云迁移**：`HybridCloudExtentsMigration` 支持数据在不同存储类别间的迁移

---