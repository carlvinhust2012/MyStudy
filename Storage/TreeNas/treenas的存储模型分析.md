# TrueNAS SCALE (Treenas) 存储模型分析

> 基于 `middleware-release-24.04.2` (Dragonfish) 源码分析

---

## 1. 项目概述

TrueNAS SCALE 是 iXsystems 基于 Linux (Debian) 的 NAS 操作系统，其中间件 (`middlewared`) 是一个纯 **Python 3** 守护进程，提供 REST/WebSocket API，管理底层 ZFS 存储系统。与 OpenNAS (PHP/FreeBSD) 不同，TrueNAS SCALE 采用**现代 Python 插件化架构**，使用 `libzfs` Python 绑定直接操作 ZFS。

### 技术栈

| 组件 | 技术 |
|------|------|
| 语言 | Python 3 |
| Web 框架 | aiohttp (异步 HTTP/WebSocket) |
| ORM | SQLAlchemy + Alembic |
| ZFS 绑定 | py-libzfs (libzfs Python 封装) |
| 存储后端 | ZFS (Linux native) |
| 数据库 | SQLite |
| 事件系统 | WebSocket (内置) |
| 定时任务 | 内置 Job Queue + cron |
| 复制引擎 | zettarepl |

### 核心插件目录结构

```
middlewared/plugins/
├── disk.py                  # 磁盘 CRUD (storage_disk 表)
├── disk_/                   # 磁盘子功能 (sync, format, smart, wipe, sed, etc.)
├── pool_/                   # 存储池管理 (storage_volume 表)
│   ├── pool.py              #   PoolService (高级 API)
│   ├── dataset.py           #   PoolDatasetService (数据集 CRUD)
│   ├── topology.py          #   vdev 拓扑管理
│   ├── format_disks.py      #   磁盘格式化
│   ├── expand.py            #   存储池扩容
│   ├── replace_disk.py      #   磁盘替换 (resilver)
│   ├── scrub.py             #   数据校验
│   ├── attach_disk.py       #   附加磁盘
│   └── ...                  #   加密、导入导出、快照计数等
├── zfs_/                    # ZFS 底层操作 (私有服务)
│   ├── pool.py              #   ZFSPoolService (libzfs 调用)
│   ├── dataset.py           #   ZFSDatasetService (libzfs 调用)
│   ├── snapshot.py          #   ZFSSnapshotService
│   ├── pool_utils.py        #   拓扑转换工具
│   ├── pool_status.py       #   存储池健康状态
│   └── ...
├── smb.py                   # SMB/CIFS 文件共享
├── nfs.py                   # NFS 文件共享
├── iscsi_/                  # iSCSI 块存储 (SCST 后端)
├── snapshot.py              # 定期快照任务
├── replication.py           # ZFS 复制 (zettarepl)
├── cloud_sync.py            # 云同步 (rclone)
├── boot.py / bootenv.py     # 引导管理 (zectl/beadm)
├── sysdataset.py            # 系统数据集
├── smart.py                 # SMART 磁盘监控
├── filesystem.py            # 文件系统操作 (ACL/stat)
└── alert/source/            # 存储告警 (pool, disk, nfs, iscsi, etc.)
```

---

## 2. 存储架构总览

TrueNAS SCALE 采用**纯 ZFS 存储模型**，所有存储功能基于 ZFS 实现，没有传统的 UFS/FAT 挂载系统：

```
┌─────────────────────────────────────────────────────────────────────┐
│                       TrueNAS SCALE 中间件                           │
│                                                                     │
│  ┌────────────┐  ┌────────────┐  ┌────────────┐  ┌────────────┐   │
│  │ Web UI /   │  │ WebSocket  │  │ CLI        │  │ REST API   │   │
│  │ TrueCommand │  │ Events     │  │ (midclt)   │  │ (OpenAPI)  │   │
│  └─────┬──────┘  └─────┬──────┘  └─────┬──────┘  └─────┬──────┘   │
│        └────────────────┼───────────────┼───────────────┘          │
│                         ▼                                           │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │              插件系统 (Plugin Framework)                       │   │
│  │  ┌─────────┐ ┌──────────┐ ┌──────────┐ ┌──────────────┐     │   │
│  │  │pool.*   │ │pool.dataset│ │disk.*   │ │smb/nfs/iscsi │     │   │
│  │  │(高级API)│ │(数据集API)│ │(磁盘API) │ │(共享服务)    │     │   │
│  │  └────┬────┘ └────┬─────┘ └────┬─────┘ └──────┬───────┘     │   │
│  │       │           │            │               │             │   │
│  │  ┌────▼────┐ ┌────▼─────┐ ┌───▼──────┐  ┌─────▼───────┐    │   │
│  │  │zfs.pool │ │zfs.dataset│ │disk._sub │  │service_.*   │    │   │
│  │  │(底层API)│ │(底层API) │ │(子功能)  │  │(服务管理)   │    │   │
│  │  └────┬────┘ └────┬─────┘ └──────────┘  └─────────────┘    │   │
│  └───────┼───────────┼──────────────────────────────────────────┘   │
│          │           │                                              │
│  ┌───────▼───────────▼──────────────────────────────────────────┐   │
│  │            基础设施层                                          │   │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌────────────┐  │   │
│  │  │ py-libzfs│  │ SQLAlchemy│  │ Job Queue│  │ zettarepl │  │   │
│  │  │ (ZFS绑定)│  │ (SQLite) │  │ (任务调度)│  │ (复制引擎)│  │   │
│  │  └────┬─────┘  └────┬─────┘  └──────────┘  └─────┬──────┘  │   │
│  └───────┼──────────────┼─────────────────────────────┼─────────┘   │
└──────────┼──────────────┼─────────────────────────────┼─────────────┘
           │              │                             │
           ▼              ▼                             ▼
┌──────────────┐  ┌──────────────┐  ┌──────────────────────────────┐
│   ZFS 内核   │  │    SQLite    │  │      外部服务                 │
│  ┌─────────┐ │  │              │  │  Samba | nfsd | SCST | rclone│
│  │  zpool  │ │  │ storage_disk │  │  smartd | sshd | fenced      │
│  │  dataset│ │  │ storage_volume│ │  mdnsresponder | snmp         │
│  │  zvol   │ │  │ storage_task │  │                              │
│  │  snapshot│ │  │ storage_replication │                         │
│  │  clone  │ │  │ sharing_nfs_share   │                         │
│  │  send/  │ │  │ sharing_cifs_share  │                         │
│  │  recv   │ │  │ services_iscsiextent│                         │
│  └─────────┘ │  └──────────────┘  └──────────────────────────────┘
└──────┬───────┘
       ▼
┌──────────────────────────────────────┐
│     物理存储设备                      │
│  /dev/sda  /dev/nvme0n1  (HDD/SSD)   │
│  /dev/sd[a-z]  (SAS/SATA)            │
│  /dev/nvme[0-9]n1  (NVMe)           │
└──────────────────────────────────────┘
```

---

## 3. 存储对象层次结构

```
物理磁盘 (/dev/sda, /dev/nvme0n1)
  │
  │  [disk.format] ── parted 创建 GPT 分区表
  │     ├── ZFS 数据分区 (GPT type: 6a898cc3-1dd2-11b2-99a6-080020736631)
  │     └── Swap 分区 (可选)
  │
  ├── zpool (存储池)
  │   │
  │   │  由 vdev (虚拟设备) 组成:
  │   │  ├── MIRROR  (镜像, ≥2 disks)
  │   │  ├── STRIPE  (条带化, ≥1 disk)
  │   │  ├── RAIDZ1  (单校验, ≥3 disks)
  │   │  ├── RAIDZ2  (双校验, ≥4 disks)
  │   │  ├── RAIDZ3  (三校验, ≥5 disks)
  │   │  ├── DRAID   (声明式 RAID, 自带 spare)
  │   │  ├── CACHE   (L2ARC 读缓存)
  │   │  ├── LOG     (SLOG 写日志, 可镜像)
  │   │  ├── SPARE   (热备盘)
  │   │  ├── SPECIAL (元数据特殊分配)
  │   │  └── DEDUP   (去重 vdev)
  │   │
  │   ├── Dataset (数据集 / ZFS 文件系统)
  │   │   ├── 挂载点: /mnt/pool/dataset
  │   │   ├── 属性: compression, dedup, quota, atime, acltype, recordsize
  │   │   ├── 加密: encryption=on|off|aes-256-gcm, keyformat, keylocation
  │   │   ├── 用途: SMB/NFS 共享目录, Apps 数据, VM 磁盘镜像
  │   │   └── 快照 (snapshot)
  │   │       ├── @snap_name (即时快照, COW)
  │   │       ├── 定期快照 (PeriodicSnapshotTask → zettarepl)
  │   │       ├── 克隆 (clone) ── 从快照创建可写副本
  │   │       ├── 保留 (hold) ── 防止快照被删除
  │   │       └── 发送/接收 (send/recv) ── 复制到远端
  │   │
  │   └── Volume (zvol / 块设备)
  │       ├── 设备路径: /dev/zvol/pool/volume
  │       ├── 属性: volsize, volblocksize, compression
  │       └── 用途: iSCSI extent, VM 磁盘
  │
  └── [传统文件系统支持极少, 仅作为外部兼容]
```

---

## 4. 中间件插件系统架构

### 4.1 插件加载机制

```mermaid
sequenceDiagram
    participant Main as main.py | (启动入口)
    participant PluginLoader as LoadPluginsMixin | (插件加载器)
    participant Plugins as plugins/ 目录 | (~1600个.py文件)
    participant Registry as Service Registry | (服务注册表)
    participant Schema as Schema Resolver | (Schema解析器)
    participant Setup as setup() 函数 | (初始化钩子)

    Main->>PluginLoader: 加载所有插件
    PluginLoader->>Plugins: 递归扫描 plugins/ 目录 | (排除 *_freebsd.py)

    loop 每个 .py 模块
        PluginLoader->>Plugins: import 模块
        Plugins-->>PluginLoader: 返回 Service 子类列表
        PluginLoader->>Registry: 按命名空间注册服务
        Note right of Registry: DiskService → "disk" | PoolService → "pool" | ZFSPoolService → "zfs.pool" | (private, 不暴露给客户端)
    end

    PluginLoader->>Schema: resolve_methods()
    Schema->>Schema: 解析 @accepts/@returns 中的 | Patch, Ref, OROperator 引用
    Schema-->>PluginLoader: Schema 解析完成

    PluginLoader->>Setup: 按优先级执行 setup() 函数
    Note right of Setup: datastore → auth → service → pwenc | → boot → system → mail → alert | → account → replication → network | → (其他插件)
```

### 4.2 双层服务架构

TrueNAS SCALE 的存储管理采用**高层业务服务 + 底层 ZFS 操作服务**的双层架构：

```
┌─────────────────────────────────────────────────────────────┐
│                   高层业务服务 (Public)                       │
│  命名空间: pool.*, pool.dataset.*, disk.*, smb.*, nfs.*      │
│  职责: 业务逻辑、输入验证、加密管理、ACL、HA、钩子、事件       │
│  数据: 读写 SQLite (storage_volume, storage_disk, sharing_*) │
│  ZFS:  通过 middleware.call() 调用底层服务                    │
├─────────────────────────────────────────────────────────────┤
│                   底层 ZFS 服务 (Private)                     │
│  命名空间: zfs.pool.*, zfs.dataset.*, zfs.snapshot.*         │
│  职责: 直接调用 py-libzfs 执行 ZFS 操作                       │
│  数据: 不直接操作数据库                                       │
│  ZFS:  直接使用 libzfs.ZFS() 上下文管理器                     │
│  进程:  运行在进程池 (process_pool) 中, 避免锁竞争             │
└─────────────────────────────────────────────────────────────┘
```

### 4.3 方法调用流程

```mermaid
sequenceDiagram
    participant Client as 客户端 | (Web UI / CLI)
    participant MW as Middleware Core | (aiohttp)
    participant Auth as Auth/RBAC | (认证/授权)
    participant Schema as Schema | (验证)
    participant High as 高层服务 | (pool.create)
    participant Low as 底层服务 | (zfs.pool.create)
    participant LibZFS as py-libzfs | (libzfs)
    participant ZFS as ZFS 内核
    participant DB as SQLite
    participant Event as WebSocket | 事件

    Client->>MW: call("pool.create", data)
    MW->>Auth: 认证 + 权限检查 (RBAC role)
    Auth-->>MW: 通过

    MW->>Schema: @accepts 验证输入
    Schema->>Schema: clean() → validate()
    Schema-->>MW: 验证通过

    MW->>High: pool.do_create(job, data)

    High->>High: 业务验证 | (名称、拓扑、加密)
    High->>High: _process_topology() | (验证磁盘可用性)

    High->>Low: middleware.call("zfs.pool.create", ...)
    Note right of MW: 转发到进程池执行

    Low->>LibZFS: libzfs.ZFS() context manager
    LibZFS->>ZFS: zpool create tank mirror /dev/sda /dev/sdb
    ZFS-->>LibZFS: 创建成功
    LibZFS-->>Low: 返回 pool 对象

    Low-->>High: 返回结果

    High->>High: zfs.dataset.mount(pool)
    High->>DB: datastore.insert(storage_volume)
    High->>DB: datastore.insert(storage_encrypteddataset)

    High->>Event: send_event("pool.query", "ADDED")
    High->>High: call_hook("pool.post_create")

    High-->>MW: 返回 pool 记录
    MW->>Schema: @returns 验证输出 (debug模式)
    MW-->>Client: JSON 响应
```

---

## 5. 数据库模型 (存储相关表)

```
┌─────────────────────────────────────────────────────────────────────┐
│                        SQLite 数据库                                 │
│                                                                     │
│  ┌─────────────────────┐     ┌──────────────────────────┐          │
│  │    storage_disk      │     │     storage_volume        │          │
│  ├─────────────────────┤     ├──────────────────────────┤          │
│  │ PK: disk_identifier  │     │ PK: id (AUTO)            │          │
│  │    disk_name         │     │ UQ: vol_name             │          │
│  │    disk_serial       │     │    vol_guid              │          │
│  │    disk_size         │     │    vol_encrypt            │          │
│  │    disk_model        │     │    vol_encryptkey         │          │
│  │    disk_type         │     └──────────┬───────────────┘          │
│  │    disk_rotationrate │                │                          │
│  │    disk_zfs_guid ────┼──→ (指向 pool)  │                          │
│  │    disk_enclosure_slot│               │                          │
│  │    disk_togglesmart   │  ┌────────────▼──────────────┐          │
│  │    disk_smartoptions   │  │ storage_encrypteddataset  │          │
│  │    disk_passwd (SED)   │  ├──────────────────────────┤          │
│  │    disk_expiretime     │  │ PK: id                   │          │
│  └─────────────────────┘  │    name (dataset path)     │          │
│                           │    encryption_key          │          │
│                           │    kmip_uid                │          │
│                           └───────────────────────────┘          │
│                                                                     │
│  ┌─────────────────────┐  ┌───────────────────────────┐           │
│  │   storage_scrub      │  │    storage_resilver       │           │
│  ├─────────────────────┤  ├───────────────────────────┤           │
│  │ FK: scrub_volume_id  │  │    enabled                │           │
│  │    scrub_threshold   │  │    begin/end (time)       │           │
│  │    scrub_cron_fields │  │    weekday                │           │
│  │    scrub_enabled     │  └───────────────────────────┘           │
│  └─────────────────────┘                                          │
│                                                                     │
│  ┌─────────────────────┐  ┌───────────────────────────┐           │
│  │   storage_task       │  │   storage_replication     │           │
│  │ (定期快照任务)        │  │   (ZFS 复制配置)          │           │
│  ├─────────────────────┤  ├───────────────────────────┤           │
│  │    task_dataset      │  │    repl_target_dataset    │           │
│  │    task_recursive    │  │    repl_direction         │           │
│  │    task_lifetime_*   │  │    repl_transport         │           │
│  │    task_naming_schema│  │    repl_source_datasets   │           │
│  │    task_cron_fields  │  │    repl_compression       │           │
│  │    task_exclude      │  │    repl_encryption_*      │           │
│  │    task_enabled      │  │    repl_state             │           │
│  └────────┬────────────┘  └────────┬──────────────────┘           │
│           │        ┌───────────────┘                              │
│           ▼        ▼                                               │
│  ┌─────────────────────────────┐                                  │
│  │ storage_replication_        │                                  │
│  │ repl_periodic_snapshot_tasks│                                  │
│  │ (快照任务↔复制任务 多对多)   │                                  │
│  └─────────────────────────────┘                                  │
│                                                                     │
│  ┌──────────────────┐  ┌──────────────────┐  ┌─────────────────┐  │
│  │ sharing_nfs_share │  │ sharing_cifs_share│  │services_iscsi   │  │
│  ├──────────────────┤  ├──────────────────┤  │targetextent     │  │
│  │ nfs_path         │  │ cifs_path         │  ├─────────────────┤  │
│  │ nfs_network      │  │ cifs_name         │  │ extent_name     │  │
│  │ nfs_ro           │  │ cifs_ro           │  │ extent_type     │  │
│  │ nfs_maproot_*    │  │ cifs_browsable    │  │ extent_path     │  │
│  │ nfs_security     │  │ cifs_timemachine  │  │ extent_blocksize│  │
│  │ nfs_enabled      │  │ cifs_acl          │  │ extent_filesize │  │
│  └──────────────────┘  │ cifs_enabled      │  └─────────────────┘  │
│                        └──────────────────┘                        │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 6. 存储池创建时序图

```mermaid
sequenceDiagram
    participant UI as Web UI / API
    participant Pool as PoolService | pool.create
    participant Valid as 验证器
    participant DiskOp as disk.format
    participant ZFS as ZFSPoolService | zfs.pool.create
    participant LibZFS as py-libzfs
    participant Kernel as ZFS 内核
    participant DB as SQLite
    participant Hook as Hook/Event

    UI->>Pool: pool.create({name, topology, encryption})

    Pool->>Valid: validate_name(name)
    Valid-->>Pool: 名称合法

    Pool->>Pool: _validate_topology(topology)
    Note right of Pool: 验证 vdev 类型和磁盘数量: | STRIPE≥1, MIRROR≥2, RAIDZ1≥3 | RAIDZ2≥4, RAIDZ3≥5 | DRAID 特殊约束 | 同类 vdev 组内类型一致

    Pool->>Pool: 检查磁盘可用性 | (不在其他 pool, 未过期)

    Pool->>Pool: validate_encryption_data()
    Note right of Pool: 验证加密选项: | 加密不可放在非加密父级下 | passphrase 父级不可有 key 子级

    Pool->>DiskOp: pool.format_disks(disk_list)
    loop 每块磁盘
        DiskOp->>DiskOp: disk.wipe() (QUICK 清分区表)
        DiskOp->>Kernel: parted 创建 GPT 分区表
        DiskOp->>Kernel: 创建 ZFS 数据分区 (磁盘末尾)
        DiskOp->>Kernel: 创建 Swap 分区 (磁盘开头, ≥1GiB)
        DiskOp->>Kernel: 调整分区顺序 (逻辑=物理)
    end

    Pool->>Pool: 构建 options: | lz4_compress, altroot=/mnt, | cachefile, failmode=continue, | autoexpand=on, ashift=12
    Pool->>Pool: 构建 fsoptions: | atime=off, acltype=posix, | aclinherit=passthrough, | compression=lz4, encryption=...

    Pool->>ZFS: zfs.pool.create(name, vdevs, options, fsoptions)

    ZFS->>ZFS: convert_topology() | 转换 vdev 规格为 libzfs 格式

    ZFS->>LibZFS: libzfs.ZFS().create(name, topology, opts, fsopts)
    LibZFS->>Kernel: zpool create -o ... -O ... tank mirror /dev/sda /dev/sdb
    Kernel-->>LibZFS: zpool 创建完成
    LibZFS-->>ZFS: 返回 pool 对象

    ZFS-->>Pool: 创建成功

    Pool->>ZFS: zfs.dataset.update(mountpoint=INHERIT)
    Pool->>ZFS: zfs.dataset.mount(pool_name)

    Pool->>DB: INSERT INTO storage_volume | (name, guid)
    Pool->>DB: INSERT INTO storage_encrypteddataset | (如启用加密)
    Pool->>DB: INSERT INTO storage_scrub

    Pool->>Hook: call_hook("pool.post_create")
    Pool->>Hook: call_hook("dataset.post_create")
    Pool->>Hook: send_event("pool.query", "ADDED")

    alt 创建失败
        Pool->>LibZFS: zpool destroy (回滚)
        Pool->>DB: DELETE (回滚)
    end

    Pool-->>UI: 返回 pool 记录
```

---

## 7. 数据集 (Dataset) 创建时序图

```mermaid
sequenceDiagram
    participant UI as Web UI / API
    participant DS as PoolDatasetService | pool.dataset.create
    participant ZFS as ZFSDatasetService | zfs.dataset.create
    participant LibZFS as py-libzfs
    participant FS as FilesystemService | (ACL)
    participant DB as SQLite
    participant Event as WebSocket Event

    UI->>DS: pool.dataset.create({name, pool, share_type, compression, quota, ...})

    DS->>DS: 验证名称格式/长度

    DS->>DS: 确定 share_type 默认值
    Note right of DS: SMB → case_insensitive, NFSv4 ACL | NFS → case_sensitive, NFSv4 passthrough | APPS → case_sensitive | MULTIPROTOCOL → case_sensitive

    DS->>DS: __common_validation()
    Note right of DS: 父级存在性检查 | ACL 交叉验证 | POSIX+DISCARD 必需, NFSV4+DISCARD 禁止 | recordsize 必须对 pool 有效 | dRAID pool 默认 recordsize=1M | 加密规则检查

    DS->>DS: 检查挂载点不冲突

    DS->>DS: 检查父级未锁定 | (加密数据集必须已解锁)

    alt share_type = SMB
        DS->>DS: 准备 SMB ACL
    end

    DS->>DS: 构建属性映射 (~30个 API 字段 → ZFS 属性)
    Note right of DS: compression → compression | dedup → dedup | quota → quota | atime → atime | recordsize → recordsize | acltype → acltype | encryption → encryption | xattr=sa (默认, 文件系统类型)

    DS->>ZFS: zfs.dataset.create(name, type, properties)

    ZFS->>LibZFS: pool.create(name, params, fstype)
    LibZFS->>LibZFS: zfs create -o compression=lz4 | -o quota=10G -o acltype=posix | tank/data
    LibZFS-->>ZFS: 创建完成

    ZFS-->>DS: 返回 dataset 信息

    DS->>DB: INSERT INTO storage_encrypteddataset | (如启用加密)

    DS->>Event: call_hook("dataset.post_create")

    DS->>ZFS: zfs.dataset.mount(dataset)
    Note right of ZFS: mount -t zfs tank/data /mnt/tank/data

    alt 需要设置 ACL
        DS->>FS: filesystem.setacl(dataset, acl)
    end

    DS->>Event: send_event("pool.dataset.query", "ADDED")

    DS-->>UI: 返回 dataset 记录
```

---

## 8. ZFS 快照管理时序图

```mermaid
sequenceDiagram
    participant Cron as Cron 调度器
    participant Task as PeriodicSnapshotTask | snapshot.py
    participant Zetta as zettarepl | (快照引擎)
    participant Snap as ZFSSnapshotService | zfs.snapshot
    participant LibZFS as py-libzfs
    participant VM as VMware/VM 管理
    participant Event as WebSocket Event

    Note over Cron,Event: === 定期快照 (自动化) ===

    Cron->>Task: 触发 snapshot task (cron 调度)
    Task->>Zetta: zettarepl.run_periodic_snapshot_task(id)

    Zetta->>Zetta: 解析 naming_schema | (如 "auto-%Y-%m-%d_%H-%M")
    Zetta->>Zetta: 生成快照名 | 如 "auto-2026-04-17_14-00"

    alt exclude 列表存在
        Zetta->>Zetta: zettarepl.create_recursive_snapshot | _with_exclude()
    else 无 exclude
        Zetta->>Snap: zfs.snapshot.create()
    end

    Snap->>LibZFS: dataset.snapshot(name, recursive=True)
    LibZFS-->>Snap: 快照创建完成
    Snap-->>Zetta: 成功

    Zetta->>Event: send_event("zfs.snapshot.query", "ADDED")

    Note over Cron,Event: === 手动快照 ===

    participant UI as Web UI
    UI->>Snap: zfs.snapshot.create({dataset, name, recursive})

    Snap->>Snap: 验证 name 和 naming_schema 互斥
    Snap->>Snap: validate_snapshot_name()

    alt vmware_sync = true
        Snap->>VM: vmware.snapshot_begin()
    end
    alt suspend_vms = true
        Snap->>VM: vm.suspend_vms()
    end

    Snap->>LibZFS: dataset.snapshot(name, recursive)
    LibZFS-->>Snap: 完成

    Snap->>Event: send_event("ADDED")

    Note over Snap,VM: finally: 恢复 VM, 结束 VMware 同步

    Note over Cron,Event: === 快照删除 ===

    UI->>Snap: zfs.snapshot.delete(id, {defer, recursive})

    Snap->>LibZFS: snap.delete(defer, recursive)

    alt 有依赖的 clone
        Snap-->>UI: ValidationErrors | "快照有依赖的克隆, 使用 defer"
    else 删除成功
        Snap->>Event: send_event("REMOVED")
    end

    Note over Cron,Event: === 快照克隆 ===

    UI->>Snap: zfs.snapshot.clone(snapshot, target)
    Snap->>LibZFS: zfs clone tank/data@snap tank/clone1
    LibZFS-->>Snap: 克隆完成 (COW, 不立即占空间)
```

---

## 9. 磁盘同步与管理时序图

```mermaid
sequenceDiagram
    participant Boot as 系统启动 | (system.ready 事件)
    participant Sync as DiskService.sync | disk_/sync.py
    participant Sys as Linux sysfs/udev
    participant DB as SQLite (storage_disk)
    participant Alert as Alert 系统
    participant SMART as smartd

    Boot->>Sync: disk.sync_all() (Job, 带锁)

    Sync->>Sys: device.get_disks() | (枚举系统块设备)

    Sync->>DB: SELECT * FROM storage_disk | ORDER BY expiretime

    loop 每个数据库中的磁盘
        Sync->>Sys: ident_to_dev(identifier)
        alt 磁盘仍存在
            Sync->>Sync: _map_device_disk_to_db() | (映射 serial, size, type, model, etc.)
            Sync->>DB: UPDATE storage_disk | (如有变化)
        else 磁盘不存在
            alt 首次缺失
                Sync->>DB: UPDATE expiretime = now + 7天
            else 已过期 (>7天)
                Sync->>DB: DELETE FROM storage_disk
                Note right of Sync: 重置 KMIP 密钥
            end
        end
    end

    loop 每个系统磁盘
        alt 不在数据库中
            Sync->>Sys: dev_to_ident(name)
            Sync->>DB: INSERT INTO storage_disk
            Sync->>Alert: create DIF-disk alert | (如 applicable)
        end
    end

    Sync->>DB: 同步 ZFS GUID → disk_zfs_guid
    Sync->>SMART: 重启 smartd (如有变化)
    Sync->>Alert: 发送 CHANGED/REMOVED/ADDED 事件

    Note over Sync,Alert: disk_expiretime 机制: | 磁盘消失后给 7 天宽限期 | 在此期间重新插入可恢复配置 | 过期后彻底删除记录
```

---

## 10. 文件共享服务时序图

### 10.1 SMB/CIFS 共享

```mermaid
sequenceDiagram
    participant UI as Web UI
    participant SMB as SharingSMBService | smb.py
    participant Samba as Samba | (net/pdbedit/smbcontrol)
    participant DB as SQLite | (sharing_cifs_share)
    participant FS as 文件系统

    UI->>SMB: sharing.smb.create({path, name, comment, ...})

    SMB->>SMB: 应用预设 (SMBSharePreset)
    Note right of SMB: DEFAULT_SHARE / TIMEMACHINE | ENHANCED_TIMEMACHINE | MULTI_PROTOCOL_NFS | WORM_DROPBOX / PRIVATE_DATASETS

    SMB->>SMB: validate_smb() | (guest account, bindip, masks, etc.)
    SMB->>SMB: 生成 vuid (UUID per share)
    SMB->>SMB: 压缩数据

    SMB->>DB: INSERT INTO sharing_cifs_share

    SMB->>Samba: 添加到 Samba registry | (net conf addshare)
    SMB->>Samba: 设置 sharesec | (sharesec/set)

    alt 新建共享
        SMB->>Samba: restart cifs service
    else 更新共享
        SMB->>Samba: reload cifs service | (smbcontrol reload-config)
    end

    SMB-->>UI: 返回 share 记录

    Note over SMB,Samba: SMB 高级特性支持: | - Time Machine (苹果备份) | - VFS 模块: fruit, streams_xattr | - SMB 多通道 (multichannel) | - NFSv4 ACL passthrough | - ShadowCopy (之前的版本快照) | - FSRVP (VSS 快照提供者) | - 持久化句柄 (durable handle) | - ADS 审计 (audit)
```

### 10.2 NFS 共享

```mermaid
sequenceDiagram
    participant UI as Web UI
    participant NFS as SharingNFSService | nfs.py
    participant NFSd as nfsd | (内核 NFS 服务)
    participant DB as SQLite | (sharing_nfs_share)
    participant DNS as DNS 解析器
    participant Krb as Kerberos

    UI->>NFS: sharing.nfs.create({path, network, hosts, security, ...})

    NFS->>NFS: validate_share_path()
    Note right of NFS: 检查符号链接 | k8s 数据集限制 | 导出路径冲突检测

    NFS->>NFS: validate_hosts_and_networks()
    DNS->>DNS: 并行 DNS 解析 (8路并发)
    NFS->>NFS: test_for_overlapped_networks()
    Note right of NFS: ipaddress 库检测子网重叠

    NFS->>NFS: 验证 Kerberos 兼容性
    Note right of NFS: sec=SYS (无需 Kerberos) | sec=KRB5 (Kerberos 认证) | sec=KRB5I (Kerberos + 完整性校验) | sec=KRB5P (Kerberos + 加密)

    NFS->>DB: INSERT INTO sharing_nfs_share

    NFS->>NFSd: 生成 /etc/exports 配置
    NFS->>NFSd: exportfs -ra (重载导出表)
    NFS->>NFSd: reload nfsd service

    NFS-->>UI: 返回 share 记录

    Note over NFS,Krb: NFS 协议支持: | - NFSv3 + NFSv4 (默认双启) | - Kerberos 安全性 | - 按主机/网络授权 | - maproot/mapall 用户映射 | - 只读模式 | - HA 集成 (仅主节点导出)
```

---

## 11. iSCSI 块存储时序图

```mermaid
sequenceDiagram
    participant UI as Web UI
    participant Ext as iSCSITargetExtentService | iscsi_/extents.py
    participant SCST as SCST | (iscsitarget)
    participant ZFS as ZFSDatasetService
    participant LibZFS as py-libzfs
    participant DB as SQLite | (services_iscsitargetextent)
    participant Init as iSCSI Initiator | (客户端)

    Note over UI,Init: === 创建 iSCSI Extent ===

    UI->>Ext: iscsi.targetextent.create({name, type, path, blocksize})

    alt type = "DISK" (zvol)
        Ext->>Ext: 验证 path 以 "zvol/" 开头
        Ext->>ZFS: 验证 zvol 存在且已解锁
        Ext->>ZFS: zfs.dataset.update(volthreading=off)
        Note right of Ext: 关闭 zvol 线程以提升 iSCSI 性能
    else type = "FILE"
        Ext->>Ext: 验证文件路径在 ZFS volume 内
        Ext->>Ext: 验证 filesize 是 blocksize 的整数倍
        Ext->>Ext: truncate -s <size> <path> | (创建稀疏文件)
    end

    Ext->>Ext: 生成唯一 serial (随机 hex) 和 NAA (SHA256)
    Ext->>DB: INSERT INTO services_iscsitargetextent
    Ext->>SCST: reload iscsitarget service

    Ext-->>UI: 返回 extent 记录

    Note over UI,Init: === Initiator 连接 ===

    Init->>SCST: iSCSI Login (Discovery)
    SCST-->>Init: 返回 Target 列表

    Init->>SCST: iSCSI Connect (LUN 登录)
    SCST->>SCST: 映射 Extent → LUN

    alt type = DISK
        SCST->>LibZFS: 读写 /dev/zvol/tank/vol1
    else type = FILE
        SCST->>SCST: 读写稀疏文件
    end

    SCST-->>Init: SCSI Response (Block I/O)

    Note over UI,Init: === 删除 Extent ===

    UI->>Ext: iscsi.targetextent.delete(id)

    Ext->>Ext: 检查活跃会话
    Ext->>Ext: 移除 target-to-extent 关联
    Ext->>ZFS: zfs.dataset.update(volthreading=on) | (恢复 zvol 线程)
    Ext->>SCST: reload iscsitarget
    Ext->>DB: DELETE FROM services_iscsitargetextent
```

---

## 12. ZFS 复制时序图

```mermaid
sequenceDiagram
    participant UI as Web UI
    participant Repl as ReplicationService | replication.py
    participant Zetta as zettarepl | (复制引擎)
    participant SSH as SSH 连接
    participant Local as 本地 ZFS
    participant Remote as 远端 NAS | (middleware)
    participant DB as SQLite
    participant Task as PeriodicSnapshotTask
    participant Event as WebSocket Event

    Note over UI,Event: === 创建复制任务 ===

    UI->>Repl: replication.create({
        name, direction: "PUSH",
        transport: "SSH",
        source_datasets: ["tank/data"],
        target_dataset: "backup/data",
        periodic_snapshot_tasks: [1,2],
        encryption_key, ...
    })

    Repl->>Repl: _validate()
    Note right of Repl: PUSH: 需要 snapshot task 或 naming_schema | SSH: 验证凭证 | 加密: 验证密钥格式/位置 | 全量复制: 需递归+属性同步

    Repl->>DB: INSERT INTO storage_replication
    Repl->>DB: INSERT INTO storage_replication_ | repl_periodic_snapshot_tasks | (多对多关联)
    Repl->>Zetta: zettarepl.update_tasks()

    Note over UI,Event: === 执行复制 (自动/手动) ===

    Repl->>Zetta: zettarepl.run_replication_task(id)

    Zetta->>Task: 获取关联的快照命名规则
    Zetta->>Local: zfs list -t snapshot -s creation | 找到匹配的源快照

    Zetta->>Zetta: 确定增量发送起点

    alt 首次复制
        Zetta->>Local: zfs send -R tank/data@snap_latest
    else 增量复制
        Zetta->>Local: zfs send -R -I | tank/data@snap_last tank/data@snap_new
    end

    Local-->>Zetta: 增量数据流

    alt transport = SSH
        Zetta->>SSH: ssh remote_nas | "zfs recv -F backup/data"
    else transport = SSH+NETCAT
        Zetta->>SSH: ssh → 启动 netcat 服务端
        Zetta->>Zetta: 本地 netcat → 发送数据
    else transport = LOCAL
        Zetta->>Remote: zfs recv (本地 pool)
    end

    Remote-->>Zetta: 接收完成

    Zetta->>Remote: 应用保留策略 | (retention_policy)
    Zetta->>Zetta: 更新任务状态
    Zetta->>Event: send_event("replication.query", "CHANGED")
    Zetta-->>Repl: 完成

    Note over UI,Event: === 双向复制 (PULL) ===
    Note right of Zetta: PULL 模式: 远端触发 | 需要 REPLICATION_TASK_WRITE_PULL 权限 | 不可 hold pending snapshots
```

---

## 13. 与 OpenNAS 的对比

| 维度 | OpenNAS (PHP/FreeBSD) | TrueNAS SCALE (Python/Linux) |
|------|----------------------|------------------------------|
| 语言 | PHP + Shell | Python 3 |
| 内核 | FreeBSD 10.2 | Debian Linux |
| ZFS 绑定 | Shell 命令 (`zpool`/`zfs` CLI) | py-libzfs (Python C 绑定) |
| 存储模型 | 双轨 (ZFS + 传统 FS) | 纯 ZFS |
| 配置存储 | 单一 XML 文件 (config.xml) | SQLite + SQLAlchemy ORM |
| 架构 | 直接脚本调用 | 插件化 + 进程池 + 异步事件循环 |
| 数据持久化 | 文件级滚动备份 (6 级) | 数据库 + Alembic 迁移 |
| 磁盘管理 | GEOM 框架 (gmirror, gstripe, gconcat) | ZFS vdev (mirror, raidz, draid) |
| 加密 | GELI (FreeBSD) | ZFS 原生加密 + LUKS (Linux) |
| 文件共享 | NFS, CIFS, AFP, FTP | NFSv3/v4, SMB (Samba 4), iSCSI (SCST) |
| 复制 | shell 脚本 (zfs send/recv) | zettarepl (Python 引擎) |
| 高可用 | HAST + CARP | fenced + Active/Standby |
| 告警 | 邮件通知 | 19+ 种存储告警源 (WebUI/邮件/PagerDuty) |
| 云集成 | 无 | rclone (S3/Azure/GCS/B2/Swift) |
| 虚拟化 | 无 | KVM VM + OpenEBS (Kubernetes 存储) |
| API | XML-RPC | REST + WebSocket + OpenAPI |

---

## 14. 关键 API 汇总

### 14.1 存储池 (pool.*)

| 方法 | 调用路径 | 说明 |
|------|---------|------|
| 创建池 | `pool.create` | 验证拓扑→格式化磁盘→zfs.pool.create→写DB→钩子 |
| 扩容池 | `pool.update` | 添加 vdev→zfs.pool.extend |
| 导入池 | `pool.import_pool` | 导入已有 zpool |
| 导出池 | `pool.export` | 导出 zpool |
| 删除池 | `pool.delete` | zfs.pool.delete→删DB |
| 查询池 | `pool.query` | zfs.pool.query→拓扑解析 |
| 数据校验 | `pool.scrub` | zpool scrub 定时任务 |
| 替换磁盘 | `pool.replace_disk` | zpool replace (resilver) |

### 14.2 数据集 (pool.dataset.*)

| 方法 | 调用路径 | 说明 |
|------|---------|------|
| 创建数据集 | `pool.dataset.create` | 验证→zfs.dataset.create→mount→ACL |
| 更新属性 | `pool.dataset.update` | zfs.dataset.update (支持 INHERIT) |
| 删除数据集 | `pool.dataset.delete` | 清理共享→zfs.dataset.delete |
| 查询数据集 | `pool.dataset.query` | zfs.dataset.query→变换→过滤 |
| 锁定/解锁 | `pool.dataset.{lock,unlock}` | 加密数据集密钥操作 |
| 推广克隆 | `pool.dataset.promote` | zfs promote |

### 14.3 快照 (zfs.snapshot.*)

| 方法 | 说明 |
|------|------|
| `zfs.snapshot.create` | 创建快照 (支持递归、命名模板、VM 挂起、VMware 同步) |
| `zfs.snapshot.delete` | 删除快照 (支持 defer、递归) |
| `zfs.snapshot.query` | 查询快照 (快速/完整双路径) |

### 14.4 磁盘 (disk.*)

| 方法 | 说明 |
|------|------|
| `disk.sync_all` | 全量同步系统磁盘到数据库 |
| `disk.format` | 格式化磁盘 (GPT + ZFS分区 + Swap) |
| `disk.wipe` | 擦除磁盘 (QUICK/FULL) |
| `disk.update` | 更新磁盘配置 (SMART/电源管理) |
| `disk.sed_unlock` | SED 自加密盘解锁 (OPAL/ATA) |

### 14.5 共享 (sharing.*)

| 方法 | 说明 |
|------|------|
| `sharing.smb.create` | 创建 SMB 共享 (支持预设模板) |
| `sharing.nfs.create` | 创建 NFS 导出 (支持 Kerberos) |
| `iscsi.targetextent.create` | 创建 iSCSI extent (zvol/文件) |
| `iscsi.target.create` | 创建 iSCSI target |
| `iscsi.targettoextent.create` | 绑定 target ↔ extent |

---

## 15. 总结

### 设计特点

1. **纯 ZFS 存储模型**: 所有存储功能基于 ZFS 实现，不依赖 LVM/RAID 等外部工具，ZFS 内建 RAIDZ/dRAID/Mirror 提供数据保护
2. **现代 Python 插件架构**: 1600+ 插件文件，通过元类驱动的服务注册，支持 CRUDService/ConfigService/Service 三级基类
3. **双层服务隔离**: 高层服务处理业务逻辑 (验证/加密/ACL/HA)，底层服务直接调用 libzfs，运行在独立进程池避免锁竞争
4. **SQLite 持久化 + ORM**: 使用 SQLAlchemy 管理配置状态，Alembic 处理数据库迁移，对比 OpenNAS 的 XML 文件更灵活
5. **异步事件驱动**: aiohttp 异步框架 + WebSocket 实时事件推送，支持 Job 队列进行长任务管理
6. **丰富的共享协议**: NFSv3/v4 (Kerberos)、SMB (Time Machine/多通道/ACL)、iSCSI (SCST)，通过 FSAttachmentDelegate 实现共享与数据集的生命周期联动
7. **完整的复制生态**: zettarepl 引擎支持 SSH/NETCAT/LOCAL 传输、增量发送、加密复制、保留策略
8. **全面的监控告警**: 19+ 种存储告警源，SMART 温度监控、磁盘过期机制 (7天宽限期)
