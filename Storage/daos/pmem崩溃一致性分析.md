# PMEM 崩溃一致性分析

## 1. 问题

PMEM 在运行过程中遇到突然掉电或进程崩溃，是否会出现数据不一致？各操作路径在崩溃恢复后的一致性如何？

## 2. 结论

**PMEM 的崩溃一致性整体安全。** 核心保障机制是「先数据、后元数据」的写入顺序（类似 WAL 思想），配合删除操作的幂等标记（status=2），确保恢复时只会索引到数据与元数据都完整持久化的 Slot。半写数据不会被当作有效缓存返回给上层。

但仍存在两个低概率的理论风险点：SlotMeta 256B 内部非原子更新、以及 ADR 硬件保护失效。

## 3. 核心安全机制

### 3.1 写入顺序：先数据，后元数据

所有写操作都遵循严格顺序：

```
① nvmem_memcpy(data_addr, ...)   → 持久化数据到 AEP 数据区
② nvmem_memcpy(meta_addr, ...)   → 持久化 SlotMeta 到 AEP 元数据区
③ 更新内存 IndexEntry            → 非持久化
```

- 步骤 ① 包含 sfence，保证数据全部落盘后才返回
- 步骤 ② 在 ① 完成后执行，保证元数据只在数据完整后才写入
- x86 TSO + sfence 保证 store 顺序，stream store 的 sfence 保证 AEP 写完成

### 3.2 删除标记：幂等 status=2

删除只在最后一个 Slot 的 SlotMeta 上标记 `status=2`，不擦除数据、不回写 status=0。恢复时通过 status=2 识别已删除的 Block，通知上层处理。该标记可重复执行，幂等安全。

### 3.3 恢复机制：从 AEP 元数据重建

Recovery 扫描所有 Slice 的 SlotMeta，根据 status 重建内存索引。所有内存状态（Index、MappingCache、free_slots 链表）均从 AEP 持久化数据重建，不依赖任何内存状态。

## 4. 崩溃场景逐一分析

### 4.1 场景一：写入过程中崩溃

**操作流程：**

```
① nvmem_memcpy(data_addr, buffer, 128KB)   → 写数据
② nvmem_memcpy(meta_addr, slot_meta, 256B)  → 写 meta（含 status=1）
③ 更新 IndexEntry                            → 更新内存索引
```

**各时刻崩溃的影响：**

| 崩溃时刻 | AEP 数据区 | AEP SlotMeta | 恢复行为 | 一致性 |
|---------|-----------|-------------|---------|--------|
| ① 之前 | 旧数据/全零 | status 旧值 | 按 status 旧值处理 | 一致 |
| ① 中途 | 部分写入 | status 旧值（0 或旧 in-use） | 按 status 旧值处理 | 安全，半写数据不会被索引 |
| ① 完成，② 之前 | 新数据完整 | status 旧值 | Slot 被当作 free 重用，新数据被覆盖 | 安全，浪费一次写入 |
| ② 完成 | 新数据完整 | status=1 | 重建索引，数据可用 | 一致 |
| ② 中途（256B 部分写入） | 新数据完整 | status 字段可能已为 1，但其他字段可能未更新 | 用不完整元数据建索引 | **有风险，见 5.1** |

**子场景：追加写（append）**

对已有 Slot 的追加写只更新 `length`。崩溃时 SlotMeta 的 length 保持旧值，恢复时按旧 length 读取，不会多读数据。安全。

**子场景：新 Slot 分配**

get_free_slot 是纯内存操作（CAS），崩溃后内存丢失，recovery 重新扫描 SlotMeta 重建 free_slots，不会丢失也不会多分配。

### 4.2 场景二：删除过程中崩溃

**操作流程：**

```
① 等待 entry.in_use == 0（无并发读取者）
② 标记内存 entry.status = DELETE
③ 设置最后一个 Slot 的 status=2，nvmem_memcpy 持久化
④ 从哈希表移除 IndexEntry
⑤ free_slot 释放所有 Slot（纯内存）
⑥ request.finish = true
```

**各时刻崩溃的影响：**

| 崩溃时刻 | AEP SlotMeta | 内存状态 | 恢复行为 | 一致性 |
|---------|-------------|---------|---------|--------|
| ③ 之前 | status=1 | 内存丢失 | 按 status=1 重建索引，Block 仍被缓存 | 安全，删除未生效，下次可重试 |
| ③ 完成，④ 之前 | 最后一个 Slot status=2 | 内存丢失 | 扫描发现 status=2，记入 del_blocks，通知上层 | 安全，幂等 |
| ④ 完成，⑤ 之前 | status=2 | Slot 已从索引移除但未加入 free_slots | status=2 的 Slot 被 del_blocks 处理，释放后加入 free_slots | 安全 |
| ⑤ 中途 | status=2 | 部分 Slot 已释放 | recovery 扫描所有 SlotMeta，重新统计 free/in-use/delete | 安全 |

**关键特性：删除是幂等的。** status=2 标记一旦落盘，无论后续步骤是否完成，恢复时都能正确识别并通知上层。多次删除同一 Block 不会产生副作用。

### 4.3 场景三：Slot 释放后重新分配

**时序：**

```
1. Block A 写入 Slot 100（数据 + meta status=1 落盘）
2. Block A 被删除（Slot 100 最后一个 slot status=2 落盘）
3. Slot 100 被 free_slot 加入 free_slots（纯内存）
4. Block B 分配到 Slot 100，开始写入
5. ← 崩溃
```

**分析：**

| 步骤 5 崩溃时 Slot 100 的状态 | AEP 上 SlotMeta status | 恢复行为 | 一致性 |
|-------------------------------|----------------------|---------|--------|
| get_free_slot 之后，数据写入之前 | 2（delete 标记仍在） | 记入 del_blocks | 安全，Slot 不会误用 |
| 数据部分写入 | 2（delete 标记仍在） | 记入 del_blocks | 安全，半写数据不会被索引 |
| 数据写入完成，meta 未写 | 2（delete 标记仍在） | 记入 del_blocks | 安全 |
| meta 写入完成（status=1 for Block B） | 1 | 按 Block B 重建索引 | 一致，前提是 256B meta 完整写入 |

**结论：** 只要 delete 标记（status=2）已落盘，即使 Slot 被重新分配后崩溃，也不会将旧 Block A 的数据误当作 Block B 返回。因为新写入遵循「先数据后 meta」规则，在 meta 写入完成前 Slot 的 status 始终为 2。

### 4.4 场景四：Slice 状态转换中崩溃

Slice 状态机：`slice_empty → in_use → slice_full → slice_free → in_use`

SliceMeta.state 写入 AEP，但 recovery 不依赖 SliceMeta.state 做精确判断——而是遍历所有 4088 个 SlotMeta 的实际 status 统计：

- free_count == 0 → slice_full
- free_count == 4088 → slice_empty
- 其他 → slice_free

SliceMeta.state 仅作为快速路径提示。崩溃后 recovery 从实际 Slot 状态重建，不受 SliceMeta.state 不一致影响。**安全。**

### 4.5 场景五：Recovery 过程中再次崩溃

Recovery 期间如果再次崩溃：

- Recovery 是只读扫描 AEP 元数据 + 构建内存结构
- 不修改 AEP 上的任何 SlotMeta / SliceMeta / CacheObjMeta
- 再次启动时重新执行 recovery，幂等且结果相同

**安全。**

### 4.6 场景六：Prewarm 过程中崩溃

初始化时 prewarmer 每 2MB 写 256B 零到 AEP，用于建立页表映射（touch 页）。

- 崩溃后部分页已 touch，部分未 touch
- 再次启动时 prewarm 重新执行，对所有页写零
- 重复写零不会破坏已有数据（只写每个 2MB 对齐区域的前 256B，不覆盖有效 Slot 数据区域）
- 即使覆盖了 SliceMeta（SliceMeta 就在 Slice 的前 2KB），recovery 不依赖 SliceMeta 的精确状态（见场景四）

**安全。**

### 4.7 场景七：并发多线程写入时崩溃

多个 WriteThread 可能同时写入同一 Slice 的不同 Slot。

- 每个 Slot 独立（128KB 数据区 + 256B 元数据区），互不影响
- 每个 Slot 的写入都是独立的「先数据后 meta」序列
- 崩溃时各 Slot 独立处于各自的中间状态，按场景一分析

**安全，Slot 间无交叉依赖。**

### 4.8 场景八：并发读写竞争时崩溃

ReadThread 通过 `++in_use` 引用计数访问 IndexEntry，DeleteThread 等待 `in_use==0` 后执行删除。

- 崩溃时内存状态全部丢失，引用计数归零
- Recovery 重建索引，不存在残留的锁或引用
- 上层未完成的读请求由上层超时机制处理（PMEM 是同步 API，上层自旋等待 `request.finish`）

**安全。**

### 4.9 场景九：MappingCache resize 时崩溃

MappingCache 是内存中的分段动态数组，按需 2 倍扩容。

- 纯内存结构，崩溃后丢失
- Recovery 从 AEP SlotMeta 重建 MappingCache
- resize 过程中的中间状态不影响持久化数据

**安全。**

### 4.10 场景十：del_blocks 通知上层时崩溃

Recovery 完成后将 del_blocks 列表通知上层（如「Block X 已被标记删除」）。

- 如果崩溃发生在通知之前：下次 recovery 重新发现 status=2 的 Slot，重新生成 del_blocks，重新通知
- 如果上层收到通知后、处理前崩溃：下次 recovery 再次通知，上层需保证处理逻辑幂等
- 上层重复收到同一 Block 的删除通知不应产生副作用

**PMEM 侧安全，但要求上层删除处理逻辑幂等。**

## 5. 残留风险点

### 5.1 SlotMeta 256B 内部非原子更新（低概率）

SlotMeta 共 256B，通过 stream store 128-bit 分组写入。如果掉电恰好发生在 256B 写入中途：

- 写入从 SlotMeta 头部（offset 0）开始，`status` 字段在 offset 40B
- 可能出现 `status=1` 已落盘，但 `block_id`（offset 8B）、`length`（offset 32B）等字段仍为旧值或不完整
- Recovery 会用不完整的元数据建索引，可能导致：
  - 将数据归属到错误的 Block（block_id 错误）
  - 读取长度不匹配（length 错误，读出脏数据或截断有效数据）

**发生概率极低**（256B stream store 窗口约微秒级），但理论上存在。

**修复建议：** 在 SlotMeta 中增加 8B 的 `checksum` 或 `version` 字段，写入前计算所有字段的校验和，恢复时校验通过才认可该 Slot。也可将 `status` 放到 SlotMeta 的**最后 8B**，使其成为最后一个被写入的字段，进一步缩小风险窗口。

### 5.2 ADR/ADR3 硬件保护失效（极低概率）

AEP 设备依赖 ADR（Asynchronous DRAM Refresh）机制——设备自带超级电容，在掉电后将内部 Write Pending Queue（WPQ）中的数据刷写到持久化介质。

sfence 保证 CPU 已发出所有 store 指令，但数据此时可能在 AEP 控制器的 WPQ 中，尚未到达持久化介质。正常情况下 ADR 机制保证 WPQ 被排空。如果超级电容硬件故障：

- 已通过 sfence 的数据可能仍留在 WPQ 中，掉电后丢失
- 此时「数据完整写入但 meta 未写入」的假设可能不成立
- 最严重场景：meta 写入到达了介质但数据未到达，导致 status=1 对应空/旧数据

**发生概率极低**（硬件故障级别），Intel AEP 的 ADR 机制有较高可靠性保障。

### 5.3 同一 Block 跨 Slot 写入的部分可见

一个 Block 可能占用多个 Slot（按 128KB 分片）。如果写入中途崩溃：

- 部分 Slot 的 meta 已更新（status=1），部分未更新
- Recovery 会索引到已完成写入的部分 Slot
- 上层通过 IndexEntry 的 `start/end` 范围查找，可能发现 Block 只有部分数据在缓存中
- 这不是一致性问题（缓存允许部分命中），但上层需能处理「缓存中只有 Block 头部/部分数据」的情况

**安全，但上层需正确处理部分缓存命中。**

## 6. 全场景总结

| # | 崩溃场景 | 一致性 | 说明 |
|---|---------|--------|------|
| 1 | 写入过程中 | 安全 | 先数据后 meta，半写数据不被索引 |
| 2 | 删除过程中 | 安全 | status=2 幂等标记，可重试 |
| 3 | Slot 释放后重分配 | 安全 | delete 标记持续保护直到新 meta 落盘 |
| 4 | Slice 状态转换 | 安全 | recovery 从实际 Slot 状态重建 |
| 5 | Recovery 过程中 | 安全 | 只读扫描，幂等可重入 |
| 6 | Prewarm 过程中 | 安全 | 重复写零，不破坏有效数据 |
| 7 | 并发多线程写入 | 安全 | Slot 间独立，无交叉依赖 |
| 8 | 并发读写竞争 | 安全 | 内存状态全部重建，无残留锁 |
| 9 | MappingCache resize | 安全 | 纯内存结构，从 AEP 重建 |
| 10 | del_blocks 通知上层 | PMEM 安全 | 要求上层删除处理幂等 |

| 风险项 | 概率 | 影响 | 建议 |
|--------|------|------|------|
| SlotMeta 256B 非原子更新 | 极低 | 可能索引到错误 Block 或错误 length | 增加 checksum/version 校验字段 |
| ADR 硬件保护失效 | 极低 | meta 写入但数据丢失 | 硬件层面保障，无需软件处理 |
| Block 跨 Slot 部分可见 | 正常 | 缓存部分命中 | 上层需处理部分缓存场景 |

## 7. 改进建议

1. **SlotMeta 增加 checksum 字段**：在 256B 末尾增加 8B 校验和（覆盖 block_id、offset、length、status），恢复时校验不通过则将该 Slot 视为 free
2. **将 status 移至 SlotMeta 末尾**：使 status 成为 stream store 最后写入的字段，最小化「status 已更新但其他字段未更新」的窗口
3. **文档化上层幂等要求**：明确要求上层对 del_blocks 的重复通知做幂等处理
