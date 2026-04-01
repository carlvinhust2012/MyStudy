# 3FS 元数据布局详解

## 概述

3FS 将所有元数据存储在 **FoundationDB**（FDB）中，以键值对（Key-Value）形式组织。FDB 提供可串行化快照隔离（SSI）事务，为整个元数据子系统提供一致性保证。

本文档详细描述 3FS 在 FDB 中的完整键值布局，涵盖文件系统元数据（inode、目录项、会话、幂等记录）、集群管理元数据（节点表、链表、租约、配置）以及 ID 分配器等。

---

## 一、键前缀注册表

所有键以 4 字节前缀开头，由 4 个 ASCII 字符编码为 `uint32_t`（little-endian 排列）。

定义位置：`src/common/kv/KeyPrefix-def.h`

| 枚举名 | 字符串 | 线上字节 | 用途 |
|--------|--------|---------|------|
| `Inode` | `"INOD"` | `49 4E 4F 44` | Inode 记录 |
| `Dentry` | `"DENT"` | `44 45 4E 54` | 目录项 |
| `MetaDistributor` | `"META"` | `4D 45 54 41` | 元数据服务 Distributor |
| `Single` | `"SING"` | `53 49 4E 47` | 单例键（租约、版本号、ID 分配等） |
| `NodeTable` | `"NODE"` | `4E 4F 44 45` | 管理节点表 |
| `ChainTable` | `"CHIT"` | `43 48 49 54` | 链表定义 |
| `ChainInfo` | `"CHIF"` | `43 48 49 46` | 单条链信息 |
| `InodeSession` | `"INOS"` | `49 4E 4F 53` | 文件写会话 |
| `ClientSession` | `"CLIS"` | `43 4C 49 53` | 客户端会话（已废弃） |
| `MetaIdempotent` | `"IDEM"` | `49 44 45 4D` | 幂等性记录 |
| `UniversalTags` | `"UTGS"` | `55 54 47 53` | 通用标签 |
| `Config` | `"CONF"` | `43 4F 4E 46` | 配置信息 |
| `TargetInfo` | `"TGIF"` | `54 47 49 46` | 存储目标信息 |
| `User` | `"USER"` | `55 53 45 52` | 用户信息 |
| `KvTable` | `"KVTB"` | `4B 56 54 42` | KV 表元数据 |
| `KvNamespace` | `"KVNS"` | `4B 56 4E 53` | KV 命名空间 |
| `KvWorkerGroup` | `"KVWG"` | `4B 56 57 47` | KV Worker 组 |

### 前缀编码方式

`makePrefixValue` 函数将 4 个字符打包为 `uint32_t`：

```
s[0] | (s[1] << 8) | (s[2] << 16) | (s[3] << 24)
```

序列化时通过 `Serializer::put<uint32_t>()` 做 `memcpy`，线上字节即原始 ASCII 字符串顺序。

---

## 二、InodeId 编码

**定义位置**：`src/fbs/meta/Common.h`

```cpp
class InodeId {
  Key packKey() const {
    auto le = folly::Endian::little(val_);
    return folly::bit_cast<Key>(le);
  }
};
```

- `InodeId` 为 64 位无符号整数
- `packKey()` 转为 **8 字节 little-endian** 表示
- 选择 LE 编码是为了将顺序递增的 InodeId 分散到 FDB 的不同分片，避免热点

### 特殊 InodeId

| 常量 | 值 | 说明 |
|------|-----|------|
| `root()` | `0x00000000_00000000` | 文件系统根目录 |
| `gcRoot()` | `0x00000000_00000001` | GC 回收根目录 |
| `normalMax()` | `0x01FFFF_FFFFFFFF` | 普通文件最大 ID |
| `kNewChunkEngineMask` | `0x01000000_00000000` | 新 Chunk Engine 标志位 |
| `virt()` | `0xFFFFFFFF_FFFFFFFE` | 虚拟根 |
| `rmRf()` | `0xFFFFFFFF_FFFFFFFD` | 递归删除 |
| `getConf()` | `0xFFFFFFFF_00000000` | 获取配置 |
| `setConf()` | `0xFFFFFFFE_80000000` | 设置配置 |
| `iovDir()` | `0xFFFFFFFF_80000000` | IOV 目录 |

---

## 三、值序列化格式

3FS 使用自定义的 **DownwardBytes** 二进制序列化（非 FlatBuffers、非 Protobuf）。

**核心特征**：
- **向下增长缓冲区**：数据从高地址向低地址写入
- **Varint 编码**：整数使用 `Varint32` / `Varint64` 编码
- **结构体**：字段按声明顺序序列化，带 `Varint32` 长度前缀
- **变体类型（variant）**：先写值，再写类型名字符串
- **可选字段**：值 + 哨兵字节（`HasValue` 或 `NullOpt`）
- **字符串**：长度前缀 + 原始字节
- **向量**：元素数量前缀 + 各元素
- **普通可拷贝类型**（uint8/32/64）：原生字节序直接写入

---

## 四、文件系统元数据布局

### 4.1 INOD — Inode 记录

**Key 格式**（12 字节固定长度）：

```
[INOD: 4B] [InodeId: 8B LE]
```

```cpp
// src/meta/store/Inode.cc
std::string Inode::packKey(InodeId id) {
  return Serializer::serRawArgs(KeyPrefix::Inode, id.packKey());
}
```

**Value 格式**：`serde::serialize(InodeData)` — DownwardBytes 二进制序列化

`InodeData` 结构体：

```
InodeData {
    type    : variant<File, Directory, Symlink>
    acl     : Acl { uid, gid, perm, iflags }
    nlink   : uint16
    atime   : UtcTime    // 微秒时间戳
    ctime   : UtcTime
    mtime   : UtcTime
}
```

**变体类型详情**：

| InodeType | 类型 | 额外字段 |
|-----------|------|---------|
| `File (0)` | 文件 | `length: uint64`, `truncateVer: uint64`, `layout: Layout`, `flags: uint32`, `dynStripe: uint32` |
| `Directory (1)` | 目录 | `parent: InodeId`, `layout: Layout`, `name: string`, `chainAllocCounter: uint32`, `lock: optional<Lock>` |
| `Symlink (2)` | 符号链接 | `target: string` |

**Layout 结构体**（嵌入 File 和 Directory）：

```
Layout {
    tableId      : ChainTableId (uint32)
    tableVersion : ChainTableVersion (uint32)
    chunkSize    : uint32          // 必须 2 的幂
    stripeSize   : uint32
    chains       : variant<Empty, ChainRange, ChainList>
}
```

- `ChainRange`：`{ baseIndex: uint32, shuffle: enum, seed: uint64 }` — 连续范围 + 随机种子
- `ChainList`：`{ vector<uint32> chainIndexes }` — 显式链索引列表

**访问模式**：通过 InodeId 做 `txn.get(packKey(id))` 点查。

---

### 4.2 DENT — 目录项

**Key 格式**（12 + N 字节，N 为文件名长度）：

```
[DENT: 4B] [Parent InodeId: 8B LE] [Name: N 字节原始 UTF-8]
```

```cpp
// src/meta/store/DirEntry.cc
std::string DirEntry::packKey(InodeId parent, std::string_view name) {
  String buf;
  Serializer ser{buf};
  ser.put(prefix);
  ser.put(parent.packKey());
  ser.putRaw(name.data(), name.size());
  return buf;
}
```

文件名直接追加为原始字节，无终止符、无长度前缀。

**Value 格式**：`serde::serialize(DirEntryData)` — 仅包含值部分，parent 和 name 已在 Key 中

```
DirEntryData {
    id      : InodeId           // 指向的子 Inode
    type    : InodeType (uint8) // 0=File, 1=Directory, 2=Symlink
    dirAcl  : optional<Acl>     // 仅 Directory 类型存在
    uuid    : Uuid              // rename 幂等性用
    gcInfo  : optional<GcInfo>  // GC 条目时存在
}
```

`GcInfo` 结构体：`{ user: Uid, origPath: Path }`

**访问模式**：

| 操作 | 方法 | 说明 |
|------|------|------|
| 点查 | `txn.get(packKey(parent, name))` | 精确查找子项 |
| 列目录 | `txn.getRange(packKey(parent, ""), endKey)` | 范围扫描所有子项 |
| 判空 | `getRange(..., limit=1)` | 快速检查目录是否为空 |

**范围扫描技巧**（`prefixListEndKey`）：

```cpp
// 将前缀的最后一个非 0xFF 字节 +1，得到严格大于所有匹配键的终止键
String prefixListEndKey(std::string_view prefix) {
  String endKey(prefix);
  while (!endKey.empty()) {
    auto &c = endKey.back();
    if (c != '\xff') { ++c; break; }
    else endKey.pop_back();
  }
  return endKey;
}
```

**Key 排序特性**：同一目录下的所有子项在键空间中连续排列，按文件名的 UTF-8 字节序排序。

---

### 4.3 INOS — 文件写会话

**Key 格式**（28 字节固定长度）：

```
[INOS: 4B] [InodeId: 8B LE] [SessionId: 16B Uuid]
```

**Value 格式**：

```
FileSession {
    inodeId   : InodeId          // 与 Key 中的 InodeId 一致（冗余校验）
    clientId  : ClientId         // 打开文件的客户端
    sessionId : Uuid             // 会话 UUID
    timestamp : UtcTime          // 会话创建时间
    payload   : string           // 占位符（加载时清空）
}
```

**设计要点**：
- 仅追踪以**写模式打开**的文件（读模式不创建会话）
- 删除有活跃写会话的文件时，meta 服务延迟删除直到所有 fd 关闭
- 定期扫描并清理离线客户端的会话

**访问模式**：

| 操作 | 说明 |
|------|------|
| 列出某 Inode 的所有会话 | 范围扫描 `[INOS + InodeId]` 前缀 |
| 会话剪枝扫描 | 按 InodeId 空间分 256 个分片并发扫描 |
| 剪枝标记 | `InodeId(-1)` 下的会话作为待剪枝标记 |

---

### 4.4 IDEM — 幂等性记录

**Key 格式**（36 字节固定长度）：

```
[IDEM: 4B] [RequestId: 16B Uuid] [ClientId: 16B Uuid]
```

**RequestId 在前、ClientId 在后**，显式设计为避免 FDB 热点。

**Value 格式**：

```
Record<T> {
    clientId  : Uuid
    requestId : Uuid
    timestamp : UtcTime
    result    : Payload<T>    // 操作结果的序列化
}
```

**使用场景**：remove 和 rename 操作使用幂等性系统处理 FDB 的 `commit_unknown_result` 错误：
1. 执行前检查是否有历史结果，有则直接返回
2. 成功后将结果与元数据变更在同一事务中原子写入
3. 超时记录（默认 30 分钟）由后台任务清理

---

### 4.5 SING-inode-alloc — InodeId 分配器

**Key 格式**（可变长度，32 个分片）：

```
SING-inode-alloc-0
SING-inode-alloc-1
...
SING-inode-alloc-31
```

**Value 格式**：`uint64`（8 字节 LE）— 每个分片的计数器

**ID 生成算法**：

```
InodeId (64-bit) = [52 bits from FDB] [12 bits local]
```

- `kAllocatorShift = 12`，`kAllocateBatch = 4096`（1 << 12）
- FDB 端生成 52 位值：`allocated_id = val * numShard + shard`
- 本地从 `first_inode_id = allocated_id << 12` 开始分配 4096 个 ID
- 本地队列低于半数（2048）时触发新的 FDB 分配
- 32 个分片避免并发分配时的事务冲突

---

## 五、GC（垃圾回收）元数据布局

GC 不使用独立的 FDB 前缀，而是以**特殊目录项**的形式存储在文件系统命名空间中。

### GC 根目录

```
InodeId::gcRoot() = InodeId(1)
```

### GC 目录

每个 meta 服务在 GC 根下创建最多 5 个 GC 目录：

```
DENT + InodeId(gcRoot=1).packKey() + "GC-Node-1"
DENT + InodeId(gcRoot=1).packKey() + "GC-Node-1.1"
DENT + InodeId(gcRoot=1).packKey() + "GC-Node-1.2"
DENT + InodeId(gcRoot=1).packKey() + "GC-Node-1.3"
DENT + InodeId(gcRoot=1).packKey() + "GC-Node-1.4"
```

### GC 条目

GC 条目是 GC 目录下的目录项，**名称编码了 GC 类型、时间戳和 InodeId**：

```
格式: <类型字符>-<20位微秒时间戳>-<InodeId十六进制>
示例: f-00171234567890123456-0x0000000000005678
```

| GC 类型 | 字符 | 优先级 | 说明 |
|---------|------|--------|------|
| `DIRECTORY` | `d` | 中 | 待删除目录 |
| `FILE_MEDIUM` | `f` | 中 | 普通文件（32 <= chunks < 128） |
| `FILE_LARGE` | `L` | 高 | 大文件（chunks >= 128） |
| `FILE_SMALL` | `S` | 低 | 小文件（chunks < 32） |

### GC 孤儿目录

权限错误导致的 GC 失败项被移至：

```
trash/gc-orphans/<username>-<YYYYMMDD>/
```

以标准目录/符号链接形式存储，符号链接 `_hf3fs_original_path` 记录原始路径。

---

## 六、集群管理元数据布局

### 6.1 SING — 单例键

#### 6.1.1 MgmtdLease（主节点租约）

**Key**：`SING` + `MgmtdLease`（10 字节）

**Value**：`MgmtdLeaseInfo` 序列化

```
MgmtdLeaseInfo {
    primary        : PersistentNodeInfo
    leaseStart     : UtcTime    // 微秒时间戳
    leaseEnd       : UtcTime
    releaseVersion : ReleaseVersion
}
```

- 全局唯一键，用于主节点选举
- 若租约已过期，任何节点可抢占；若属于自己且未过期，可续约
- 每次写事务前通过 `ensureLeaseValid()` 验证

#### 6.1.2 RoutingInfoVersion（路由信息版本）

**Key**：`SING` + `RoutingInfoVersion`（19 字节）

**Value**：`uint64` 原始字节（8 字节）— 应用层版本号（非 FDB Versionstamp）

- 每次路由信息持久化变更时递增
- 初始值为 1（0 表示无数据）
- 默认每 5 秒强制递增一次，确保客户端刷新

### 6.2 NODE — 节点表

**Key 格式**（12 字节固定）：

```
[NODE: 4B] [NodeId: 8B native endian]
```

**Value**：`PersistentNodeInfo` 序列化

```
PersistentNodeInfo {
    nodeId       : NodeId (uint64)
    type         : NodeType (uint8)   // MGMTD=0, META=1, STORAGE=2, CLIENT=3, FUSE=4
    serviceGroups: vector<ServiceGroupInfo>
    tags         : vector<TagPair>
    hostname     : string
}
```

通过前缀扫描 `"NODE"` 加载所有节点，加载时校验 Key 中的 nodeId 与 Value 中的一致。

### 6.3 CHIT — 链表

**Key 格式**（12 字节固定）：

```
[CHIT: 4B] [ChainTableId: 4B native] [ChainTableVersion: 4B native]
```

**Value**：`ChainTable` 序列化

```
ChainTable {
    chainTableId      : ChainTableId (uint32)
    chainTableVersion : ChainTableVersion (uint32)
    chains            : vector<ChainId>   // 链 ID 列表
    desc              : string
}
```

- 同一链表的多个版本并存，按 Version 排序
- 更新链表内容时创建新版本，旧版本保留
- 特殊情况：`tableId==0 && tableVersion==0` 时，`chainIndex` 直接作为 `ChainId` 使用

### 6.4 CHIF — 链信息

**Key 格式**（8 字节固定）：

```
[CHIF: 4B] [ChainId: 4B native endian]
```

**Value**：`ChainInfo` 序列化

```
ChainInfo {
    chainId              : ChainId (uint32)
    chainVersion         : ChainVersion (uint32)    // 链重构时递增
    targets              : vector<ChainTargetInfo>
    preferredTargetOrder : vector<TargetId>
}

ChainTargetInfo {
    targetId     : TargetId (uint64)
    publicState  : PublicTargetState (uint8)
                  // INVALID=0, SERVING=1, LASTSRV=2, SYNCING=4, WAITING=8, OFFLINE=16
}
```

### 6.5 TGIF — 存储目标信息

**Key 格式**（12 字节固定）：

```
[TGIF: 4B] [TargetId: 8B BIG ENDIAN]
```

> 注意：TargetId 使用 **big-endian** 编码（其他键均使用 native/little-endian），使范围扫描按 ID 升序返回。

**Value**：`TargetInfo` 序列化

```
TargetInfo {
    targetId     : TargetId (uint64)
    publicState  : PublicTargetState (uint8)
    localState   : LocalTargetState (uint8)   // INVALID=0, UPTODATE=1, ONLINE=2, OFFLINE=4
    chainId      : ChainId (uint32)
    nodeId       : optional<NodeId>
    diskIndex    : optional<uint32>
    usedSize     : uint64
}
```

- 由后台任务 `MgmtdTargetInfoPersister` 周期性持久化（默认 1 秒）
- 不参与路由信息版本号机制

### 6.6 CONF — 配置信息

**Key 格式**（可变长度）：

```
[CONF: 4B] [NodeType 字符串长度: 1B] [NodeType 字符串] [max-version: 8B BIG ENDIAN]
```

版本编码为 `UINT64_MAX - version`，使**最新版本排在最前**，可通过 `limit=1` 前缀扫描获取最新配置。

**Value**：`ConfigInfo` 序列化

```
ConfigInfo {
    configVersion : ConfigVersion (uint64)
    content       : string    // 配置内容（如 TOML）
    desc          : string
}
```

### 6.7 UTGS — 通用标签

**Key 格式**（可变长度）：

```
[UTGS: 4B] [id: 可变字符串]
```

**Value**：`vector<TagPair>` 序列化

```
TagPair { key: string, value: string }
```

- 无版本控制，不参与路由信息版本机制
- 通过前缀扫描加载

### 6.8 META — Distributor（元数据服务分发器）

#### Server Map

**Key**：`META`（仅前缀，4 字节）

**Value**：`ServerMap` 序列化 — `{ vector<NodeId> active }` — 活跃 meta 服务节点列表

#### Per-Server 心跳

**Key**：`META-XXXXXXXX`（如 `META-00000001`，13 字节）

**Value**：10 字节 FDB Versionstamp（通过 `setVersionstampedValue` 原子写入）

#### 版本键

**Key**：`"\xff/metadataVersion"`（FDB 内置键）

**Value**：10 字节 FDB Versionstamp

---

## 七、ChunkId 格式（存储层）

ChunkId 虽不直接存储在 FDB 中，但作为文件数据分片的核心标识，与元数据中的 InodeId 紧密关联。

**格式**（16 字节固定）：

```
[0]     : tenant     (1 byte, 固定 0x00)
[1]     : reserved   (1 byte, 固定 0x00)
[2..9]  : inode      (8 bytes, big-endian InodeId)
[10..11]: track      (2 bytes, big-endian track_id)
[12..15]: chunk      (4 bytes, big-endian chunk_num)
```

ChunkId 使用 **big-endian** 编码以保持排序性，支持范围扫描：
- 扫描某 Inode 的所有 chunk：`ChunkId(inode, 0, begin)` 到 `ChunkId(inode, 1, 0)`

---

## 八、版本控制方案总览

| 版本类型 | 数据类型 | 作用域 | 递增时机 |
|---------|---------|--------|---------|
| `RoutingInfoVersion` | uint64 | 全局路由信息 | 每次路由变更持久化时 |
| `ChainTableVersion` | uint32 | 每个链表 | 链表内容变化时 |
| `ChainVersion` | uint32 | 每条链 | 目标/状态变化时 |
| `ConfigVersion` | uint64 | 每种 NodeType | 配置更新时 |
| `HeartbeatVersion` | uint64 | 每个节点心跳 | 客户端单调递增上报 |
| FDB Versionstamp | 10B | Distributor 心跳/元数据版本 | FDB 事务提交时原子生成 |

---

## 九、Key 空间全景图

```
FDB Key Space
├── INOD + <InodeId LE 8B>                     → InodeData
│   ├── 0x0000000000000000                      → 文件系统根 inode
│   ├── 0x0000000000000001                      → GC 根 inode
│   └── ...                                     → 其他 inode
│
├── DENT + <ParentInodeId LE 8B> + <name>       → DirEntryData
│   ├── DENT + [root] + "dir1"                  → 根目录下的 dir1
│   ├── DENT + [root] + "dir1/file.txt"          → dir1 下的 file.txt（多级目录需逐级查找）
│   └── DENT + [gcRoot] + "GC-Node-1"           → GC 目录
│
├── INOS + <InodeId LE 8B> + <SessionUuid 16B>  → FileSession
│   └── INOS + 0xFFFF...FFFF + <uuid>           → 待剪枝标记
│
├── IDEM + <RequestUuid 16B> + <ClientUuid 16B>  → Idempotent::Record
│
├── SING + "MgmtdLease"                          → MgmtdLeaseInfo
├── SING + "RoutingInfoVersion"                  → uint64
├── SING + "inode-alloc-N" (N=0..31)            → uint64 counter
│
├── NODE + <NodeId LE 8B>                       → PersistentNodeInfo
│
├── CHIT + <TableId LE 4B> + <Version LE 4B>    → ChainTable
├── CHIF + <ChainId LE 4B>                      → ChainInfo
│
├── TGIF + <TargetId BE 8B>                     → TargetInfo
│
├── CONF + <NodeType> + <max-version BE 8B>     → ConfigInfo
│
├── META                                       → ServerMap (active meta servers)
├── META-XXXXXXXX                               → Versionstamp (heartbeat)
│
└── \xff/metadataVersion                        → Versionstamp
```

---

## 十、持久化 vs 派生数据

| 数据 | 存储位置 | 说明 |
|------|---------|------|
| Inode | FDB（INOD） | 持久化 |
| 目录项 | FDB（DENT） | 持久化 |
| 文件会话 | FDB（INOS） | 持久化 |
| 幂等记录 | FDB（IDEM） | 持久化，定期清理 |
| GC 队列 | FDB（DENT under gcRoot） | 持久化 |
| 节点信息 | FDB（NODE） | 持久化 |
| 链表/链信息 | FDB（CHIT/CHIF） | 持久化 |
| Target 信息 | FDB（TGIF） | 持久化（后台周期写入） |
| 配置 | FDB（CONF） | 持久化（多版本） |
| 租约 | FDB（SING） | 持久化 |
| NodeMap | 仅内存 | 从 PersistentNodeInfo + 心跳重建 |
| TargetMap | 仅内存 | 从 ChainInfo + 心跳状态派生 |
| NewBornChains | 仅内存 | 重启后临时标记 |
| ClientSessionMap | 仅内存 | 重启后清空 |

---
