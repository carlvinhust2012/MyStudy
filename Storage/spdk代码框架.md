# SPDK 代码结构

SPDK（Storage Performance Development Kit）采用模块化分层架构。

## 目录结构

```
spdk/
├── include/spdk/          # 头文件（对外API）
├── lib/                   # 核心库
│   ├── env/               # 环境抽象层（内存、线程、PCIe）
│   │   ├── pci/
│   │   └── dpdk/          # 基于DPDK的env实现
│   ├── event/             # 事件框架（reactor模型）
│   ├── thread/            # 线程/轮询模型（spdk_thread）
│   ├── log/               # 日志
│   ├── json/              # JSON解析
│   ├── jsonrpc/           # JSON-RPC框架
│   ├── notify/            # 通知机制
│   ├── sock/              # Socket抽象（POSIX/vsock）
│   ├── trace/             # Trace点
│   └── util/              # 工具库（ring、bitmap、io_channel等）
│
├── module/                # 可插拔模块
│   ├── bdev/              # Block Device层（统一块设备抽象）
│   │   ├── nvme/          #   NVMe bdev
│   │   ├── rpc/           #   bdev JSON-RPC
│   │   └── ...
│   └── event/             # 事件子系统模块（app/rpc/net等）
│
├── driver/                # 驱动（旧代码）
├── rpc/                   # 通用JSON-RPC
│
├── app/                   # 应用程序
│   ├── spdk_tgt/          # SPDK target（NVMe/SCSI/vhost统一target）
│   ├── nvme_cli/          # NVMe CLI工具
│   ├── spdk_top/          # 性能监控工具
│   └── ...
│
└── scripts/               # 构建和辅助脚本
    └── rpc.py             # Python RPC客户端
```

## 核心架构分层（从上到下）

```
┌─────────────────────────────────────┐
│  Applications (spdk_tgt / your app) │
├─────────────────────────────────────┤
│  Protocols (vhost/nbd/iscsi/nvme)  │  ← 网络/存储协议
├─────────────────────────────────────┤
│  Bdev Layer (block device抽象)      │  ← 统一块设备接口
├─────────────────────────────────────┤
│  NVMe/IOAT/Virtio Drivers           │  ← 具体驱动
├─────────────────────────────────────┤
│  Event Framework (reactor/poller)   │  ← 事件驱动核心
├─────────────────────────────────────┤
│  Env (DPDK:内存/PCIe/定时器/锁)     │  ← 底层环境抽象
└─────────────────────────────────────┘
```

## 关键概念

| 概念 | 说明 |
|---|---|
| **reactor** | 每个CPU核心一个reactor线程，轮询处理事件 |
| **spdk_thread** | 轻量级线程，绑定到reactor上运行 |
| **poller** | 注册到reactor的轮询回调，替代中断 |
| **bdev** | 统一块设备抽象，上层协议不直接操作驱动 |
| **io_channel** | bdev的I/O通道，per-thread独立，无锁 |
| **vhost** | virtio后端，支持blk/net/scsi target |

## DPU 场景 I/O 路径

```
Guest NVMe Driver
    ↓ (PCIe MMIO/doorbell)
DPU NVMe Emulation (sdpcie 层)
    ↓
SPDK vhost-user target
    ↓
SPDK bdev layer
    ↓
SPDK NVMe driver (bdev_nvme)
    ↓
物理 NVMe SSD / 远端存储
```
