# SCM 持久化指令分析: clwb 与 sfence

## 1. 一句话概括

CPU Cache 是易失性的（断电即丢），SCM 介质是非易失性的（断电不丢），clwb 把数据从 Cache 刷到 SCM，sfence 确保刷完才继续，两者配合才能保证掉电不丢数据。

## 2. 问题根源：CPU Cache 是易失性的

### 2.1 CPU 写数据的真实路径

```
正常写内存流程:

  ┌──────┐    ┌──────────┐    ┌──────────┐    ┌─────────┐
  │ CPU  │ → │ L1 Cache │ → │ L2 Cache │ → │ L3 Cache│ → 内存/SCM
  └──────┘    └──────────┘    └──────────┘    └─────────┘
                                              ^^^^^^^^
                                              数据可能在这里就停了
                                              Cache 是 SRAM（易失性，断电即丢）
```

CPU 写数据时，不会直接写到物理介质，而是写到 Cache 中（write-back 策略）。Cache 的材质是 SRAM，**断电数据就没了**。

### 2.2 三种存储介质的易失性

```
┌─────────────────────────────────────────────────────────┐
│                    断电安全性对比                         │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  CPU Cache（L1/L2/L3）                                   │
│  - 材质: SRAM                                           │
│  - 速度: 最快（1-10ns）                                   │
│  - 易失性: 断电立即丢失                                    │
│  - 数据状态: 可能停留在 cache line 中，尚未到达 SCM          │
│                                                         │
│  DRAM（普通内存）                                          │
│  - 材质: DRAM                                           │
│  - 速度: 快（~100ns）                                     │
│  - 易失性: 断电丢失（需要刷新电流维持）                      │
│                                                         │
│  SCM（Optane 等）                                        │
│  - 材质: 3D XPoint / 相变材料                             │
│  - 速度: 较快（~300ns）                                    │
│  - 易失性: 断电不丢（非易失性）                             │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

## 3. 三条关键持久化指令

### 3.1 clwb (Cache Line Write Back)

**作用：把指定 cache line 从 CPU Cache 刷回到物理介质。**

```
执行 clwb 之前:
  CPU 写了数据 → 数据停留在 L3 Cache 中 → 断电 → 数据丢失

执行 clwb 之后:
  L3 Cache 中的数据 → 被写回到 SCM 物理介质 → 断电 → 数据安全
```

### 3.2 sfence (Store Fence)

**作用：存储内存屏障，保证前面的所有写操作（包括 clwb）都完成后，才执行后面的指令。**

```
没有 sfence 的情况（CPU 指令重排序）:
  写数据到 cache → 发起 clwb → 通知其他线程"数据已持久化"
                                               → clwb 实际还没完成
                                               → 断电 → 丢数据

有 sfence 的情况:
  写数据到 cache → 发起 clwb → sfence（等待完成）
                                → 通知其他线程"数据已持久化"
                                → 断电 → 数据安全
```

### 3.3 clflushopt（旧方案，已不推荐）

Intel 早期的持久化指令，功能类似 clwb 但会 invalidate cache line（导致后续读取需要重新从 SCM 加载），clwb 保留了 cache line 的副本，性能更好。

```
对比:

  clflushopt [addr]:
    把 cache line 刷回 SCM + 使 cache line 失效
    → 后续读需要重新从 SCM 加载（~300ns 延迟）

  clwb [addr]:
    把 cache line 刷回 SCM + 保留 cache line 副本
    → 后续读直接命中 Cache（~1ns 延迟）
    → 性能更好，推荐使用
```

## 4. 完整持久化流程

### 4.1 三步持久化

```
步骤 1: 正常写入数据
  mov [addr], rax
  → 数据写到 L1/L2/L3 Cache，还没到 SCM

步骤 2: 刷回 cache line
  clwb [addr]
  → 把这个 cache line 从 Cache 刷到 SCM
  → 但 clwb 本身可能被 CPU 重排序，不保证立即完成

步骤 3: 确保刷回完成
  sfence
  → 内存屏障，保证 clwb 真正完成后才继续执行后续指令

此时: Cache 和 SCM 中都有数据副本 → 断电安全
```

### 4.2 断电安全性分析

```
不同写操作下的断电安全性:

  ┌──────────┐  ┌──────────┐  ┌───────────┐
  │ L1 Cache │  │ L3 Cache │  │ SCM 介质   │
  │ (易失)   │  │ (易失)   │  │ (非易失)   │
  └──────────┘  └──────────┘  └───────────┘

  只有 mov:                数据仅在 Cache 中       → 断电丢失
  mov + clwb（无 sfence）: 数据可能正在刷的过程中    → 不确定
  mov + clwb + sfence:    数据一定已到达 SCM        → 断电安全
```

## 5. 为什么普通内存不需要这些指令

```
DRAM 场景:
  - DRAM 本身就是易失的，断电数据本来就会丢
  - 持久化由上层软件负责（数据库 WAL、文件系统 journal）
  - 操作系统定期把脏页从 Page Cache 刷到 SSD/HDD
  - 不需要 clwb/sfence

SCM 场景:
  - SCM 介质本身是非易失的，但 CPU Cache 是易失的
  - 数据可能在 Cache 中就停了
  - 不刷 Cache 就断电 = SCM 当普通内存用了，白买了
  - 所以必须用 clwb + sfence 显式持久化
  - 这是使用 SCM 的开发者必须承担的额外责任
```

## 6. PMDK 如何封装这些指令

PMDK 把底层汇编指令封装成了简单的高级 API，开发者不需要直接写汇编:

```c
// PMDK 内部实现（简化版）
void pmem_persist(const void *addr, size_t len) {
    for (each cache line in [addr, addr+len)) {
        _mm_clwb(addr);    // 逐 cache line 刷回
    }
    _mm_sfence();          // 等待全部完成
}

// 半持久化（数据已到持久化域，但可能在 CPU Write Combining Buffer 中）
void pmem_flush(const void *addr, size_t len) {
    for (each cache line in [addr, addr+len)) {
        _mm_clwb(addr);    // 只刷回，不等待
    }
}

// 等待之前的 flush 完成
void pmem_drain(void) {
    _mm_sfence();          // 单独的屏障
}

// 开发者只需要调用:
pmem_persist(ptr, size);   // 一步到位
// 或者分两步:
pmem_flush(ptr, size);     // 先刷（可批量）
pmem_drain();              // 再等（一次屏障）
```

## 7. 指令重排序问题：为什么必须用 sfence

现代 CPU 为了性能会对指令进行重排序，可能导致持久化语义被破坏:

```
场景: 写两个数据 A 和 B，都要求持久化

错误写法（可能丢数据）:
  mov [A], value_A
  clwb [A]
  mov [B], value_B          ← CPU 可能把 clwb[A] 排到写 B 后面
  clwb [B]
  sfence                     ← 只保证 clwb[B] 完成

正确写法:
  mov [A], value_A
  clwb [A]
  sfence                     ← 保证 A 刷完
  mov [B], value_B
  clwb [B]
  sfence                     ← 保证 B 刷完

性能优化写法（批量刷）:
  mov [A], value_A
  mov [B], value_B
  clwb [A]
  clwb [B]
  sfence                     ← 一个屏障等两个 clwb
```

## 8. 持久化域（Persistence Domain）

Intel 定义了三个持久化域，理解域的层次很重要:

```
┌─────────────────────────────────────────────────────┐
│               持久化域层次                            │
├─────────────────────────────────────────────────────┤
│                                                     │
│  域 1: CPU 寄存器                                    │
│  - 数据还没到 Cache                                   │
│  - 断电必丢                                         │
│  - 需要至少执行 store 指令                            │
│                                                     │
│  域 2: CPU Cache (含 Write Combining Buffer)          │
│  - 数据在 Cache 中                                    │
│  - 断电会丢                                         │
│  - 需要执行 clwb/clflushopt                          │
│                                                     │
│  域 3: 持久化介质 (SCM)                               │
│  - 数据已到达物理介质                                  │
│  - 断电安全                                         │
│  - 需要 clwb + sfence 确保到达                        │
│                                                     │
└─────────────────────────────────────────────────────┘

目标: 确保数据从域 1 到达域 3
路径:  store → clwb → sfence → 域 3（安全）
```

## 9. 总结

```
问题: CPU Cache 是易失的，SCM 是非易失的，中间有断层
解决: clwb 把数据从 Cache 搬到 SCM，sfence 确保搬完了才继续

本质上是在弥补 CPU Cache 架构和 SCM 介质之间的"持久化语义鸿沟"。
开发者必须显式地跨越这条鸿沟，否则数据可能在 Cache 中断电丢失。
PMDK 的价值就是把这些底层指令封装成简单的 API，降低使用门槛。
```
