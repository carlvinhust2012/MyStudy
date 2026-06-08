# tmpfs 源码分析

## 1. 概述

tmpfs 是 **Linux 内核的一部分**，完全开源，采用 GPL v2 许可证。

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        tmpfs 源码概况                                    │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  许可证:    GPL v2                                                       │
│  主文件:    mm/shmem.c (~4000 行)                                        │
│  头文件:    include/linux/shmem_fs.h                                     │
│  首次引入:  Linux 2.4 (2001年)                                           │
│  基于实现:  ramfs                                                        │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 2. 源代码位置

### 2.1 内核源码树结构

```
linux/
├── mm/
│   └── shmem.c           ← tmpfs 核心实现 (主文件)
│
├── include/linux/
│   ├── shmem_fs.h        ← tmpfs 头文件
│   └── tmpfs.h           ← tmpfs 接口
│
└── fs/
    └── ramfs/
        └── inode.c       ← ramfs 实现 (tmpfs 基于此)
```

### 2.2 核心文件说明

| 文件 | 路径 | 行数 | 说明 |
|------|------|------|------|
| **shmem.c** | `mm/shmem.c` | ~4000 | tmpfs 核心实现 |
| **shmem_fs.h** | `include/linux/shmem_fs.h` | ~50 | 数据结构定义 |
| **tmpfs.h** | `include/linux/tmpfs.h` | ~20 | 接口声明 |

---

## 3. 在线查看源码

### 3.1 在线仓库

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        在线查看方式                                       │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  Linux 内核官方仓库:                                                     │
│  https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git     │
│                                                                          │
│  GitHub 镜像:                                                            │
│  https://github.com/torvalds/linux/blob/master/mm/shmem.c               │
│                                                                          │
│  在线代码浏览器 (推荐):                                                   │
│  https://elixir.bootlin.com/linux/latest/source/mm/shmem.c              │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

### 3.2 下载源码

```bash
# 方式 1: git clone
git clone https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git
cd linux

# 方式 2: GitHub 镜像 (国内更快)
git clone https://github.com/torvalds/linux.git
cd linux

# 方式 3: 下载特定版本
wget https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.6.tar.xz
tar -xf linux-6.6.tar.xz
cd linux-6.6
```

---

## 4. 核心数据结构

### 4.1 shmem_inode_info

```c
// include/linux/shmem_fs.h

/*
 * tmpfs 的 inode 信息结构
 * 每个 tmpfs 文件都有一个这样的结构
 */
struct shmem_inode_info {
    spinlock_t          lock;           // 自旋锁，保护结构
    unsigned int        seals;          // 文件密封 (sealing)
    unsigned long       flags;          // 状态标志
    unsigned long       alloced;        // 已分配的块数
    union {
        unsigned long   swapped;        // 换出到 Swap 的页数
        char            *symlink;       // 软链接目标路径
    };
    struct shared_policy policy;        // NUMA 内存策略
    struct list_head    swaplist;       // Swap 链表
    struct simple_xattrs xattrs;        // 扩展属性
    struct inode        vfs_inode;      // 嵌入的 VFS inode
};
```

**字段说明：**

| 字段 | 类型 | 说明 |
|------|------|------|
| `lock` | spinlock_t | 保护并发访问的自旋锁 |
| `seals` | unsigned int | 文件密封状态 (防止修改) |
| `alloced` | unsigned long | 已分配的内存块数 |
| `swapped` | unsigned long | 换出到 Swap 的页数 |
| `symlink` | char* | 软链接目标 (共用 union) |
| `vfs_inode` | struct inode | 嵌入的 VFS inode |

### 4.2 shmem_sb_info

```c
// mm/shmem.c

/*
 * tmpfs 超级块信息
 * 每个 tmpfs 挂载点有一个这样的结构
 */
struct shmem_sb_info {
    unsigned long       max_blocks;     // 最大块数 (限制大小)
    unsigned long       free_blocks;    // 空闲块数
    unsigned long       max_inodes;     // 最大 inode 数
    unsigned long       free_inodes;    // 空闲 inode 数
    spinlock_t          stat_lock;      // 统计信息锁
    struct kmem_cache   *inode_cache;   // inode 缓存
    umode_t             mode;           // 默认权限
    struct mempolicy    *mpol;          // NUMA 策略
};
```

**用于实现挂载选项：**

```bash
# size=1G 对应 max_blocks
mount -t tmpfs -o size=1G tmpfs /mnt/tmpfs

# nr_inodes=100k 对应 max_inodes
mount -t tmpfs -o nr_inodes=100k tmpfs /mnt/tmpfs
```

---

## 5. 核心函数

### 5.1 函数列表

```c
// mm/shmem.c

// ==================== 初始化 ====================

// 文件系统初始化
static int __init shmem_init(void)

// inode 缓存初始化
static int __init shmem_init_inodecache(void)

// ==================== 文件系统操作 ====================

// 挂载 tmpfs
static struct dentry *shmem_mount(struct file_system_type *fs_type,
                                   int flags, const char *dev_name,
                                   void *data)

// 获取超级块
static int shmem_fill_super(struct super_block *sb,
                            struct fs_context *fc)

// ==================== 文件操作 ====================

// 创建文件
static int shmem_create(struct mnt_idmap *idmap,
                        struct inode *dir,
                        struct dentry *dentry,
                        umode_t mode, bool excl)

// 打开文件
static int shmem_file_open(struct inode *inode, struct file *file)

// 读取文件
static ssize_t shmem_file_read_iter(struct kiocb *iocb,
                                     struct iov_iter *to)

// 写入文件
static ssize_t shmem_file_write_iter(struct kiocb *iocb,
                                      struct iov_iter *from)

// 内存映射 (关键!)
static int shmem_mmap(struct file *file, struct vm_area_struct *vma)

// ==================== 页面管理 ====================

// 分配页面
static int shmem_getpage_gfp(struct inode *inode, pgoff_t index,
                             struct page **pagep, enum sgp_type sgp,
                             gfp_t gfp, struct vm_fault *vmf)

// 换入页面
static int shmem_swapin(swp_entry_t swap, gfp_t gfp,
                        struct shmem_inode_info *info,
                        pgoff_t index)

// 换出页面
static int shmem_writepage(struct page *page,
                           struct writeback_control *wbc)
```

### 5.2 关键特性实现位置

| 特性 | 函数 | 大致行号 |
|------|------|----------|
| 页面分配 | `shmem_getpage_gfp()` | ~1700-1900 |
| Swap 换入 | `shmem_swapin()` | ~1600-1700 |
| Swap 换出 | `shmem_writepage()` | ~1400-1500 |
| mmap 实现 | `shmem_mmap()` | ~2200-2300 |
| 文件写入 | `shmem_file_write_iter()` | ~2100-2200 |
| 文件读取 | `shmem_file_read_iter()` | ~2000-2100 |
| 大小限制 | `shmem_statfs()` | ~2900-3000 |

---

## 6. 文件系统注册

### 6.1 文件系统类型定义

```c
// mm/shmem.c

static struct file_system_type shmem_fs_type = {
    .owner          = THIS_MODULE,
    .name           = "tmpfs",          // 文件系统名称
    .init_fs_context = shmem_init_fs_context,
    .parameters     = shmem_fs_parameters,
    .kill_sb        = kill_litter_super,
    .fs_flags       = FS_USERNS_MOUNT | FS_ALLOW_IDMAP,
};
```

### 6.2 初始化流程

```c
// mm/shmem.c

static int __init shmem_init(void)
{
    int error;

    // 1. 初始化 inode 缓存
    error = shmem_init_inodecache();
    if (error)
        goto out2;

    // 2. 注册 tmpfs 文件系统
    error = register_filesystem(&shmem_fs_type);
    if (error)
        goto out1;

    // 3. 创建内核内部挂载点
    shm_mnt = kern_mount(&shmem_fs_type);
    if (IS_ERR(shm_mnt)) {
        error = PTR_ERR(shm_mnt);
        goto out1;
    }

    // 4. 初始化 SysV 共享内存
    error = init_tmpfs();

    return 0;

out1:
    unregister_filesystem(&shmem_fs_type);
out2:
    return error;
}

// 内核启动时调用
fs_initcall(shmem_init);
```

**初始化时序：**

```
内核启动
    │
    ▼
fs_initcall(shmem_init)
    │
    ├──▶ shmem_init_inodecache()  ─ 创建 inode 缓存
    │
    ├──▶ register_filesystem()    ─ 注册 tmpfs 文件系统
    │
    ├──▶ kern_mount()             ─ 内核内部挂载
    │
    └──▶ init_tmpfs()             ─ 初始化 /dev/shm 等
```

---

## 7. 文件操作结构

### 7.1 file_operations

```c
// mm/shmem.c

static const struct file_operations shmem_file_operations = {
    .mmap           = shmem_mmap,              // 内存映射
    .get_unmapped_area = shmem_get_unmapped_area,
    .read_iter      = shmem_file_read_iter,    // 读取
    .write_iter     = shmem_file_write_iter,   // 写入
    .fsync          = shmem_file_fsync,        // 同步
    .splice_read    = generic_file_splice_read,
    .splice_write   = iter_file_splice_write,
    .fallocate      = shmem_fallocate,         // 预分配空间
    .fop_flags      = FOP_MMAP_SYNC,
    .llseek         = generic_file_llseek,
};
```

### 7.2 inode_operations

```c
// mm/shmem.c

static const struct inode_operations shmem_inode_operations = {
    .getattr        = shmem_getattr,
    .setattr        = shmem_setattr,
    .tmpfile        = shmem_tmpfile,
    .atomic_open    = shmem_atomic_open,
};

// 目录操作
static const struct inode_operations shmem_dir_inode_operations = {
    .create         = shmem_create,
    .lookup         = simple_lookup,
    .link           = shmem_link,
    .unlink         = shmem_unlink,
    .symlink        = shmem_symlink,
    .mkdir          = shmem_mkdir,
    .rmdir          = shmem_rmdir,
    .mknod          = shmem_mknod,
    .rename         = shmem_rename2,
    .tmpfile        = shmem_tmpfile,
    .getattr        = shmem_getattr,
    .setattr        = shmem_setattr,
};
```

### 7.3 address_space_operations

```c
// mm/shmem.c

static const struct address_space_operations shmem_aops = {
    .writepage      = shmem_writepage,         // 写回页面 (换出)
    .dirty_folio    = noop_dirty_folio,
    .migrate_folio  = migrate_folio,
    .error_remove_folio = shmem_error_remove_folio,
};
```

---

## 8. 关键函数详解

### 8.1 shmem_mmap - 内存映射

```c
// mm/shmem.c

static int shmem_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct inode *inode = file_inode(file);

    // 1. 检查文件密封
    if (shmem_file_seals(file) & F_SEAL_WRITE) {
        if (vma->vm_flags & VM_SHARED)
            return -EPERM;
        if (vma->vm_flags & VM_WRITE)
            return -EPERM;
    }

    // 2. 设置文件访问
    if (!inode->i_mapping->a_ops->read_folio)
        return -ENOEXEC;

    // 3. 更新访问时间
    file_accessed(file);

    // 4. 设置 VM 操作
    vma->vm_ops = &shmem_vm_ops;

    return 0;
}

// VM 操作
static const struct vm_operations_struct shmem_vm_ops = {
    .fault          = shmem_fault,      // 缺页处理
    .map_pages      = filemap_map_pages,
    .pagesize       = shmem_vm_pagesize,
};
```

### 8.2 shmem_getpage_gfp - 页面分配

```c
// mm/shmem.c (简化版)

static int shmem_getpage_gfp(struct inode *inode, pgoff_t index,
                             struct page **pagep, enum sgp_type sgp,
                             gfp_t gfp, struct vm_fault *vmf)
{
    struct shmem_inode_info *info = SHMEM_I(inode);
    struct shmem_sb_info *sbinfo = SHMEM_SB(inode->i_sb);

    // 1. 检查是否已有页面
    page = find_get_entry(inode->i_mapping, index);
    if (xa_is_value(page)) {
        // 页面在 Swap 中，需要换入
        error = shmem_swapin(swap, gfp, info, index);
        goto repeat;
    }

    // 2. 检查空间限制
    if (sbinfo->max_blocks) {
        if (percpu_counter_compare(&sbinfo->used_blocks,
                                    sbinfo->max_blocks) >= 0)
            return -ENOSPC;  // 空间不足
    }

    // 3. 分配新页面
    page = shmem_alloc_page(gfp, info, index);
    if (!page)
        return -ENOMEM;

    // 4. 加入页缓存
    error = shmem_add_to_page_cache(page, inode->i_mapping,
                                     index, gfp);

    // 5. 更新统计
    info->alloced++;

    *pagep = page;
    return 0;
}
```

### 8.3 shmem_writepage - 换出到 Swap

```c
// mm/shmem.c (简化版)

static int shmem_writepage(struct page *page,
                           struct writeback_control *wbc)
{
    struct shmem_inode_info *info;
    struct inode *inode;

    // 1. 获取 inode
    inode = page->mapping->host;
    info = SHMEM_I(inode);

    // 2. 检查是否能换出
    if (info->flags & VM_LOCKED)
        goto redirty;

    // 3. 分配 Swap 空间
    swap = get_swap_page(page);
    if (!swap.val)
        goto redirty;

    // 4. 将页面写入 Swap
    if (add_to_swap_cache(page, swap, ...) != 0)
        goto redirty;

    // 5. 更新统计
    info->swapped++;

    // 6. 设置 Swap 入口
    shmem_remember_swap(inode, page, swap);

    // 7. 从页缓存移除
    delete_from_page_cache(page);

    // 8. 写入 Swap 设备
    swap_writepage(page, wbc);

    return 0;

redirty:
    redirty_page_for_writepage(wbc, page);
    return 0;
}
```

---

## 9. tmpfs 与 shmem 的关系

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    tmpfs 与 shmem 架构关系                                │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  用户态接口                                                               │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │                                                                  │   │
│  │   tmpfs 挂载          POSIX shm          SysV shm                │   │
│  │   mount -t tmpfs      shm_open()         shmget()               │   │
│  │                                                                  │   │
│  └─────────────────────────────────────────────────────────────────┘   │
│                              │                                          │
│                              ▼                                          │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │                      VFS 层                                      │   │
│  │                                                                  │   │
│  │   文件系统接口 (file_operations, inode_operations)               │   │
│  │                                                                  │   │
│  └─────────────────────────────────────────────────────────────────┘   │
│                              │                                          │
│                              ▼                                          │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │                    shmem 核心                                    │   │
│  │                       mm/shmem.c                                 │   │
│  │                                                                  │   │
│  │   ├── 内存分配 (shmem_getpage)                                   │   │
│  │   ├── Swap 换入/换出                                             │   │
│  │   ├── 页缓存管理                                                 │   │
│  │   └── NUMA 策略支持                                              │   │
│  │                                                                  │   │
│  └─────────────────────────────────────────────────────────────────┘   │
│                              │                                          │
│                              ▼                                          │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │                     内存管理                                      │   │
│  │                                                                  │   │
│  │   页分配器 (Buddy)    页缓存     Swap 子系统                     │   │
│  │                                                                  │   │
│  └─────────────────────────────────────────────────────────────────┘   │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

**关系说明：**

| 组件 | 说明 |
|------|------|
| **shmem** | 内核子系统，提供基于内存的文件存储能力 |
| **tmpfs** | shmem 的用户态接口，挂载为文件系统 |
| **POSIX shm** | 基于 shmem 的 POSIX 共享内存，挂载在 `/dev/shm` |
| **SysV shm** | 基于 shmem 的 System V 共享内存 |

---

## 10. 源码阅读建议

### 10.1 阅读顺序

```
1. 数据结构
   └── include/linux/shmem_fs.h

2. 初始化流程
   └── shmem_init()

3. 文件创建
   └── shmem_create(), shmem_mknod()

4. 页面分配 (核心)
   └── shmem_getpage_gfp()

5. 读写操作
   └── shmem_file_read_iter(), shmem_file_write_iter()

6. 内存映射
   └── shmem_mmap(), shmem_fault()

7. Swap 支持
   └── shmem_swapin(), shmem_writepage()
```

### 10.2 调试技巧

```bash
# 查看 tmpfs 内核日志
dmesg | grep -i tmpfs
dmesg | grep -i shmem

# 查看内核统计
cat /proc/meminfo | grep -i shmem

# 追踪内核函数
echo 'shmem_getpage_gfp' > /sys/kernel/debug/tracing/set_ftrace_filter
echo function > /sys/kernel/debug/tracing/current_tracer
cat /sys/kernel/debug/tracing/trace
```

---

## 11. 总结

| 问题 | 答案 |
|------|------|
| **是否开源** | ✅ 是，GPL v2 许可证 |
| **主文件** | `mm/shmem.c` (~4000 行) |
| **头文件** | `include/linux/shmem_fs.h` |
| **在线查看** | https://elixir.bootlin.com/linux/latest/source/mm/shmem.c |
| **核心结构** | `shmem_inode_info`, `shmem_sb_info` |
| **核心函数** | `shmem_getpage_gfp()`, `shmem_mmap()`, `shmem_init()` |
| **首次引入** | Linux 2.4 (2001年) |
| **基于实现** | ramfs |

---
