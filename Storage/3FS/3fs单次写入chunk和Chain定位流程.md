# 3FS 单次写入的 Chunk/Chain 定位流程

## 一、场景设定

```
文件路径:   /a/b/c
文件大小:   1 GB
写入请求:   buf[offset = 1MB, length = 500MB]

假设 Layout 参数:
  chunkSize  = 4 MB
  stripeSize = 4
```

## 二、总体流程

```
写入 500MB 到 offset=1MB 的完整路径:

  1. 路径解析: /a/b/c → InodeId
  2. 获取 Layout: InodeId → chunkSize, stripeSize, chains[]
  3. Chunk 拆分: 500MB 按 4MB chunkSize 切分为 126 个 Write IO
  4. Chain 映射: 每个 chunkIndex % stripeSize → 确定目标链
  5. 构造 ChunkId: (InodeId, track=0, chunkIndex)
  6. 发送到 Storage: 每个 WriteIO → 对应 Chain Head → 链式复制
```

---

## 三、逐步推导

### Step 1: 路径解析 → InodeId

```
/a/b/c 的路径解析:

  DENT + [root]     + "a"  → InodeId(100)
  DENT + [100]      + "b"  → InodeId(200)
  DENT + [200]      + "c"  → InodeId(300)

目标: InodeId = 300
```

### Step 2: 获取 Layout

```
从 Inode(300) 的 InodeData 中读取 Layout:

  Layout {
    chunkSize  = 4 MB (4194304)
    stripeSize = 4
    chains     = [Chain(0), Chain(1), Chain(2), Chain(3)]   // stripeSize=4 条链
  }

  Chain(0): [T_A → T_B → T_C]
  Chain(1): [T_D → T_E → T_F]
  Chain(2): [T_G → T_H → T_I]
  Chain(3): [T_J → T_K → T_L]
```

### Step 3: Chunk 拆分

```
写入范围: offset=1MB ~ 1MB+500MB = 525MB

按 chunkSize=4MB 切分:

  chunkIndex = floor(offset / chunkSize)
  chainIndex = chunkIndex % stripeSize
  ChunkId    = (InodeId=300, track=0, chunkIndex)

  ┌──────────────────────────────────────────────────────────┐
  │ Chunk 0:   1MB ~ 4MB    (3MB,  写入后部)                  │
  │ Chunk 1:   4MB ~ 8MB    (4MB,  全量覆盖)                  │
  │ Chunk 2:   8MB ~ 12MB   (4MB,  全量覆盖)                  │
  │ ...                                                  ...   │
  │ Chunk 124: 496MB ~ 500MB (4MB,  全量覆盖)                  │
  │ Chunk 125: 500MB ~ 504MB (1MB,  写入前部)                  │
  │                                                          │
  │ 共 126 个 Chunk                                          │
  └──────────────────────────────────────────────────────────┘

  特殊情况:
  ├── Chunk 0:  offset 不对齐 (1MB 而非 4MB 边界) → 只写后 3MB
  ├── Chunk 1~124: offset 对齐 → 每个 Chunk 写满 4MB
  └── Chunk 125: 写入不覆盖整个 Chunk → 只写前 1MB
```

### Step 4: Chain 映射

```
每个 Chunk → chainIndex = chunkIndex % stripeSize(4):

  Chunk 0:   0 % 4 = 0 → Chain(0): [T_A → T_B → T_C]
  Chunk 1:   1 % 4 = 1 → Chain(1): [T_D → T_E → T_F]
  Chunk 2:   2 % 4 = 2 → Chain(2): [T_G → T_H → T_I]
  Chunk 3:   3 % 4 = 3 → Chain(3): [T_J → T_K → T_L]
  Chunk 4:   4 % 4 = 0 → Chain(0): [T_A → T_B → T_C]  (循环)
  Chunk 5:   5 % 4 = 1 → Chain(1): [T_D → T_E → T_F]
  ...
  Chunk 125: 125 % 4 = 1 → Chain(1): [T_D → T_E → T_F]

分配统计:
  Chain(0): 32 个 Chunk  (0, 4, 8, ..., 124)
  Chain(1): 32 个 Chunk  (1, 5, 9, ..., 125)
  Chain(2): 31 个 Chunk  (2, 6, 10, ..., 122)
  Chain(3): 31 个 Chunk  (3, 7, 11, ..., 123)
```

### Step 5: ChunkId 构造

```
每个 WriteIO 的 ChunkId:

  ChunkId {
    tenant:     0x00  (1 byte,  固定)
    reserved:   0x00  (1 byte,  固定)
    inode:      300   (8 bytes, big-endian)
    track:      0     (2 bytes, big-endian)
    chunk:      chunkIndex (4 bytes, big-endian)
  }

  示例:
  Chunk 0:   ChunkId(0x00, 0x00, 300, 0, 0)
  Chunk 1:   ChunkId(0x00, 0x00, 300, 0, 1)
  Chunk 125: ChunkId(0x00, 0x00, 300, 0, 125)
```

### Step 6: 发送到 Storage

```
126 个 WriteIO → 按 chainIndex 分组 → 4 个 batchWrite RPC

  Batch 1 → Chain(0) Head T_A:  [Chunk 0, Chunk 4, ..., Chunk 124]  (32 个)
  Batch 2 → Chain(1) Head T_D:  [Chunk 1, Chunk 5, ..., Chunk 125]  (32 个)
  Batch 3 → Chain(2) Head T_G:  [Chunk 2, Chunk 6, ..., Chunk 122]  (31 个)
  Batch 4 → Chain(3) Head T_J:  [Chunk 3, Chunk 7, ..., Chunk 123]  (31 个)

  每个 Chain Head 内部: 链式复制到 Member2 → Tail
```

---

## 四、时序流程图

```mermaid
sequenceDiagram
    participant App as Application
    participant Fuse as FUSE Handler
    participant RC as RcInode (缓存)
    participant SC as StorageClient
    participant HA as Chain0 Head (T_A)
    participant HD as Chain1 Head (T_D)
    participant HG as Chain2 Head (T_G)
    participant HJ as Chain3 Head (T_J)

    Note over App,HJ: 写入 buf[offset=1MB, length=500MB] 到 /a/b/c

    App->>Fuse: write(fd, buf, 500MB, offset=1MB)
    Fuse->>RC: memcpy → InodeWriteBuf (缓冲)
    Note over RC: 500MB 数据缓冲在 RDMA 注册内存中

    Note over App,HJ: flushBuf() 触发

    RC->>RC: PioV.addWrite(offset=1MB, length=500MB)
    RC->>RC: 按 chunkSize=4MB 拆分为 126 个 WriteIO

    Note over App,HJ: batchWrite — 按 Chain 分组

    RC->>SC: batchWrite([
      Note over SC: Chunk0@(1MB,3MB), Chunk1@(4MB,4MB), ..., Chunk125@(500MB,1MB)
      Note over SC: 共 126 个 WriteIO
    ])

    SC->>SC: groupOpsByNodeId()
    Note over SC: Chain0: 32 IOs → T_A<br/>Chain1: 32 IOs → T_D<br/>Chain2: 31 IOs → T_G<br/>Chain3: 31 IOs → T_J

    par 并行发送 4 个 BatchWrite
      SC->>HA: batchWrite(32 IOs)
      SC->>HD: batchWrite(32 IOs)
      SC->>HG: batchWrite(31 IOs)
      SC->>HJ: batchWrite(31 IOs)
    end

    par 每条 Chain 内部: 链式复制
      HA->>HA: Chunk0: 锁 → 本地写入 → forward Member2 → Tail → commit
      HA->>HA: Chunk4: 锁 → 本地写入 → forward Member2 → Tail → commit
      Note over HA: ... 32 个 Chunk 依次处理

      HD->>HD: Chunk1: 锁 → 本地写入 → forward Member2 → Tail → commit
      HD->>HD: Chunk5: 锁 → 本地写入 → forward Member2 → Tail → commit
      Note over HD: ... 32 个 Chunk 依次处理

      HG->>HG: Chunk2: 锁 → 本地写入 → forward Member2 → Tail → commit
      HJ->>HJ: Chunk3: 锁 → 本地写入 → forward Member2 → Tail → commit
    end

    par 返回结果
      HA-->>SC: 32 IOs 完成
      HD-->>SC: 32 IOs 完成
      HG-->>SC: 31 IOs 完成
      HJ-->>SC: 31 IOs 完成
    end

    SC-->>RC: 全部 126 IOs 完成
    RC->>RC: finishWrite(): bump hintLength = 501MB

    RC->>RC: syncAndClose(): 上报 hintLength 给 Meta Server
    RC-->>Fuse: 写入完成
    Fuse-->>App: 返回 500MB
```

---

## 五、定位公式汇总

```
给定: offset, length, InodeId(300), Layout{chunkSize=4MB, stripeSize=4}

1. Chunk 拆分:
   first_chunk = floor(offset / chunkSize)
   last_chunk  = floor((offset + length - 1) / chunkSize)

   本例: first=0, last=125, 共 126 个 Chunk

2. Chain 映射:
   chainIndex = chunkIndex % stripeSize

   Chunk 0   → chain 0
   Chunk 1   → chain 1
   Chunk 4   → chain 0 (循环)
   Chunk 125 → chain 1

3. Chain 选择:
   chain = chains[chainIndex]

   chain 0 → [T_A → T_B → T_C]
   chain 1 → [T_D → T_E → T_F]
   chain 2 → [T_G → T_H → T_I]
   chain 3 → [T_J → T_K → T_L]

4. ChunkId:
   ChunkId(inodeId=300, track=0, chunk=chunkIndex)

5. 每个 Chunk 内的偏移:
   chunk_offset = offset % chunkSize     (仅 first chunk 非零)
   write_length = min(chunkSize, end - chunk_start)

   Chunk 0:   chunk_offset=1MB, write_length=3MB
   Chunk 1:   chunk_offset=0,    write_length=4MB
   Chunk 125: chunk_offset=0,    write_length=1MB
```

---

## 六、完整数据流图

```
Application buf[500MB], offset=1MB
        │
        ▼
  FUSE InodeWriteBuf (RDMA 注册内存缓冲)
        │
        │ PioV.addWrite() — 按 4MB 切分为 126 个 WriteIO
        ▼
  StorageClient.batchWrite(126 IOs)
        │
        │ groupOpsByNodeId() — 按目标 Node 分组
        ▼
  ┌─────────────────────────────────────────────────────────────┐
  │                       4 个 BatchWrite RPC                   │
  │                                                             │
  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌────┐│
  │  │ Chain 0      │  │ Chain 1      │  │ Chain 2      │  │ C3 ││
  │  │ Head T_A     │  │ Head T_D     │  │ Head T_G     │  │T_J ││
  │  │              │  │              │  │              │  │    ││
  │  │ Chunk 0  (3M)│  │ Chunk 1  (4M)│  │ Chunk 2  (4M)│  │C3 ││
  │  │ Chunk 4  (4M)│  │ Chunk 5  (4M)│  │ Chunk 6  (4M)│  │C7 ││
  │  │ Chunk 8  (4M)│  │ Chunk 9  (4M)│  │ Chunk 10 (4M)│  │C11││
  │  │ ...         │  │ ...         │  │ ...         │  │...││
  │  │ Chunk 124(4M)│  │ Chunk 125(1M)│  │ Chunk 122(4M)│  │C23││
  │  │              │  │              │  │              │  │    ││
  │  │ 32 chunks    │  │ 32 chunks    │  │ 31 chunks    │  │ 31││
  │  │ 128 MB       │  │ 127 MB       │  │ 124 MB       │  │ 93││
  │  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘  └─┬──┘│
  └─────────┼─────────────────┼─────────────────┼────────────┼───┘
            │                 │                 │            │
            ▼                 ▼                 ▼            ▼
       ┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐
       │T_A→T_B→  │    │T_D→T_E→  │    │T_G→T_H→  │    │T_J→T_K→  │
       │  T_C      │    │  T_F      │    │  T_I      │    │  T_L      │
       │(3副本)   │    │(3副本)   │    │(3副本)   │    │(3副本)   │
       └──────────┘    └──────────┘    └──────────┘    └──────────┘

  总写入量: 500MB × 3 副本 = 1.5 GB (4 条链 × 3 副本)
  实际网络: 500MB × 4 条链 = 2 GB RDMA 传输
  并行度: 4 条链同时写入
```

---
