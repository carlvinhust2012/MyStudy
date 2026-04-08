# Lustre LDLM 架构定位与运行模式

---

## 目录

1. [LDLM 是独立进程吗？](#1-ldlm-是独立进程吗)
2. [为什么 LDLM 运行在内核态？](#2-为什么-ldlm-运行在内核态)
3. [Lustre 如何编译和加载到内核？](#3-lustre-如何编译和加载到内核)

---

## 1. LDLM 是独立进程吗？

**不是。** LDLM 是 Lustre 的内核模块代码，运行在 Linux 内核空间，不是独立进程。

```
LDLM 的运行位置:

  ┌─────────────────────────────────────────────────┐
  │           Linux 内核空间                         │
  │                                                  │
  │  ┌──────────┐  ┌──────────┐  ┌──────────────┐  │
  │  │ MDT 进程 │  │ OST 进程 │  │ Client 进程   │  │
  │  │ (内核线程)│  │ (内核线程)│  │ (内核线程)    │  │
  │  │          │  │          │  │              │  │
  │  │ LDLM    │  │ LDLM    │  │ LDLM Client  │  │
  │  │ Server  │  │ Server  │  │ (请求/持有锁)  │  │
  │  │ (授予锁) │  │ (授予锁) │  │              │  │
  │  └──────────┘  └──────────┘  └──────────────┘  │
  │                                                  │
  └─────────────────────────────────────────────────┘
         ↑              ↑              ↑
    共享同一个内核空间，通过函数调用交互

  没有独立的 ldldm 进程！
  没有独立的网络服务！
  没有独立的配置文件！
```

| 问题 | 答案 |
|------|------|
| 是独立进程吗？ | 不是，是 `lustre/ldlm/` 目录下的内核代码，编译进 `.ko` 内核模块 |
| 怎么通信？ | 服务端和客户端通过 **ptlrpc RPC** 通信（ENQUEUE/GRANT/BLOCKING AST 都是 RPC 消息） |
| 锁状态存在哪？ | 服务端：内核内存中的锁资源表（namespace hash table）；客户端：`ldlm_lock` 结构体也在内核内存 |
| 崩溃了怎么办？ | 随 MDT/OST 进程一起崩溃，重启时客户端通过 replay 恢复锁 |

---

## 2. 为什么 LDLM 运行在内核态？

**根本原因：Lustre 全栈运行在内核态。** LDLM 作为其核心组件，自然也在内核态。

### 2.1 Lustre 全栈内核架构

```
Lustre 的所有组件都运行在 Linux 内核空间:

  用户态:                    内核态:
  ┌─────────┐              ┌─────────────────────────────┐
  │         │              │  llite  (VFS 层, 行为像 ext4)  │
  │  应用程序 │ ── syscall ──│  LOV    (条带化管理)          │
  │  read() │              │  OSC    (对象存储客户端)       │
  │  write()│              │  LDLM   (分布式锁)             │
  │         │              │  ptlrpc (网络通信)            │
  └─────────┘              │  MDT/OST (服务端)             │
                           └─────────────────────────────┘
```

Lustre 不是像 NFS 那样"用户态服务端 + 内核态客户端"，而是**整个栈都在内核**，所以 LDLM 没有理由单独放在用户态。

### 2.2 为什么不放用户态

```
如果 LDLM 放在用户态:

  1. 性能灾难: 两次上下文切换
     内核(发起IO) → 用户态(LDLM判定锁) → 内核(执行IO)
     每次文件读写都多两次 context switch
     微秒级操作变成几十微秒

  2. 无法直接操作内核数据结构
     LDLM 需要和 OSC extent、VFS page cache 紧密交互:
       → osc_extent.oe_dlmlock 直接指向 ldlm_lock 指针
       → blocking_ast 直接触发 osc_lock_flush() 刷页
       → 放用户态必须通过 ioctl/netlink 桥接, 开销巨大

  3. RPC 已在内核
     Lustre 的网络层 (ptlrpc) 在内核态
     ENQUEUE/GRANT/BLOCKING AST 都是内核中的 RPC
     LDLM 放内核可以直接处理, 零拷贝

  4. 缓存一致性要求低延迟
     锁冲突检测和撤销必须在 IO 路径上完成
     用户态进程调度延迟不可控 (可能被 cfs 调度器抢占)
```

### 2.3 与其他文件系统的锁管理对比

| 文件系统 | 锁管理器位置 | 原因 |
|---------|------------|------|
| **Lustre** | 内核态 | 全栈内核，追求极致性能 |
| **NFS v4** | 内核态 | 同上，NFS 客户端和服务端都在内核 |
| **CephFS** | 内核态 | Capability 管理在 MDS/OSD 内核模块中 |
| **GFS2/OCFS2** | 内核态 | 本地集群文件系统，全栈内核 |
| **GlusterFS** | 用户态 | 全栈用户态 (FUSE)，性能换取灵活性 |
| **HDFS** | 用户态 | 全栈用户态，吞吐优先于延迟 |

总结：LDLM 在内核态是 Lustre **追求低延迟、零拷贝、与 IO 路径紧密耦合**的设计选择的必然结果，不是刻意为之的独立决策。

---

## 3. Lustre 如何编译和加载到内核？

### 3.1 编译产物：多个 ko 文件

Lustre 不是编译成一个单独的 `.ko`，而是十几个 `.ko` 文件：

```
源码编译:
  lustre 源码 → make → 生成多个 .ko 文件

  ┌─────────────────────────────────────────────────────┐
  │  Client 端 (客户端节点需要加载):                       │
  │    lnet.ko, ptlrpc.ko, obdclass.ko, ldlm.ko,      │
  │    mdc.ko, osc.ko, lov.ko, llite.ko                 │
  │                                                      │
  │  MDT 端 (元数据节点额外加载):                          │
  │    ldiskfs.ko, osd_ldiskfs.ko, mgs.ko, mdt.ko       │
  │                                                      │
  │  OST 端 (对象存储节点额外加载):                        │
  │    ofd.ko, ost.ko                                    │
  │                                                      │
  │  基础层 (所有节点都需要):                              │
  │    lnet.ko → ptlrpc.ko → obdclass.ko → ldlm.ko     │
  └─────────────────────────────────────────────────────┘
```

### 3.2 模块加载顺序

```
加载顺序 (modprobe, 有依赖关系):

  # 基础层
  modprobe lnet                ← 网络层 (最底层)
  modprobe ptlrpc              ← RPC 框架
  modprobe obdclass            ← OBD 基础类
  modprobe ldlm                ← 分布式锁管理器

  # 客户端
  modprobe mdc                 ← MDT 客户端 (元数据 RPC)
  modprobe osc                 ← OST 客户端 (数据 RPC)
  modprobe lov                 ← 条带化管理
  modprobe llite               ← VFS 层 (注册文件系统类型)

  # MDT 服务端 (额外)
  modprobe ldiskfs             ← ldiskfs 文件系统 (改造版 ext4)
  modprobe osd_ldiskfs         ← OSD 层 (对接 ldiskfs + jbd2)
  modprobe mgs                 ← 管理服务
  modprobe mdt                 ← 元数据服务

  # OST 服务端 (额外)
  modprobe ofd                 ← OST 过滤设备
  modprobe ost                 ← 对象存储服务

卸载顺序相反:
  rmmod llite → rmmod ost → rmmod mdt → ... → rmmod lnet
```

### 3.3 与 in-tree 内核模块的区别

| 维度 | Lustre | ext4 / btrfs |
|------|--------|-------------|
| **源码位置** | 外部代码（不在 Linux 内核树中） | 内核源码 `fs/ext4/`, `fs/btrfs/` |
| **编译方式** | `configure && make`，使用系统的内核头文件 | 随内核一起编译 |
| **兼容性** | 需要针对特定内核版本编译 | 内核自带，天然兼容 |
| **加载方式** | `modprobe lnet`, `modprobe llite` 等 | `modprobe ext4` |
| **运行方式** | 完全一样，都是内核模块 |
| **文件系统注册** | `llite.ko` 向 VFS 注册 `lustre` 文件系统类型 | `ext4.ko` 向 VFS 注册 `ext4` 文件系统类型 |
| **挂载方式** | `mount -t lustre mgs@mds:/fsname /mnt/lustre` | `mount -t ext4 /dev/sda1 /mnt/data` |

### 3.4 Lustre 各模块的依赖关系

```
  lnet
   └── ptlrpc
        └── obdclass
             ├── ldlm
             │    └── (被 mdc, osc, ofd, mdt, ost 依赖)
             ├── mdc ──────→ llite
             ├── osc ──→ lov → llite
             ├── ldiskfs
             │    └── osd_ldiskfs
             │         ├── mdt
             │         └── ofd → ost
             └── mgs
                  └── mdt

  箭头表示依赖方向: A → B 表示 B 依赖 A, A 必须先加载
```
