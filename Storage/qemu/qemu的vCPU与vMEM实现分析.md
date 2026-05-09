# QEMU vCPU 与虚拟内存实现分析

本文深入分析 QEMU/KVM 中 vCPU 和虚拟内存的实现原理，涵盖软件模拟（TCG）和硬件辅助虚拟化（KVM）两种方案。

---

## 目录

1. [vCPU 实现架构](#一vcpu-实现架构)
2. [软件模拟：TCG](#二软件模拟tcg)
3. [硬件虚拟化：KVM](#三硬件虚拟化kvm)
4. [虚拟内存架构](#四虚拟内存架构)
5. [内存虚拟化技术](#五内存虚拟化技术)
6. [DMA 与设备访问](#六dma-与设备访问)
7. [总结](#七总结)

---

## 一、vCPU 实现架构

### 1.1 整体架构图

```
┌─────────────────────────────────────────────────────────────┐
│                      Guest OS                               │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐        │
│  │Guest vCPU0│ │Guest vCPU1│ │Guest vCPU2│ │Guest vCPU3│        │
│  └────┬────┘  └────┬────┘  └────┬────┘  └────┬────┘        │
│       │            │            │            │              │
│  Guest Mode (Ring 3/Ring 0)                                  │
└───────┼────────────┼────────────┼────────────┼──────────────┘
        │            │            │            │
        ▼            ▼            ▼            ▼
┌─────────────────────────────────────────────────────────────┐
│                     VMM (QEMU/KVM)                          │
│  ┌──────────────────────────────────────────────────────┐  │
│  │  QEMU Thread Model                                   │  │
│  │  ┌─────────────┐ ┌─────────────┐ ┌─────────────┐     │  │
│  │  │ vCPU Thread │ │ vCPU Thread │ │ vCPU Thread │ ... │  │
│  │  │   (host)    │ │   (host)    │ │   (host)    │     │  │
│  │  └──────┬──────┘ └──────┬──────┘ └──────┬──────┘     │  │
│  │         │               │               │             │  │
│  │  ┌──────┴───────────────┴───────────────┴──────────┐  │  │
│  │  │       KVM (Kernel Virtual Machine)              │  │  │
│  │  │  ┌──────────────────────────────────────────┐   │  │  │
│  │  │  │  Hardware Virtualization Extensions     │   │  │  │
│  │  │  │  (Intel VT-x / AMD-V / ARM VE)          │   │  │  │
│  │  │  └──────────────────────────────────────────┘   │  │  │
│  │  └──────────────────────────────────────────────────┘  │  │
│  └────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

### 1.2 vCPU 状态定义

```c
// include/hw/core/cpu.h
struct CPUState {
    DeviceState parent_obj;

    int cpu_index;              // vCPU 编号
    int nr_cores;               // 核心数
    int nr_threads;             // 每核线程数

    // vCPU 线程相关
    QemuThread *thread;         // 线程句柄
    int thread_id;              // 线程 ID
    bool running;               // 是否运行中
    bool stopped;               // 是否已停止
    bool halted;                // 是否处于 HALT

    // 同步原语
    QemuCond *halt_cond;        // HALT 条件变量
    QemuMutex work_mutex;       // 工作队列锁

    // 地址空间
    AddressSpace *as;           // 地址空间
    MemoryRegion *memory;       // 系统内存根节点

    // 中断与异常
    uint32_t interrupt_request; // 中断请求标志
    uint32_t halted_cond;       // HALT 条件
    bool exit_request;          // 退出请求

    // TCG 相关
    uint32_t tcg_cflags;        // TCG 编译标志
    int64_t icount_budget;      // 指令计数预算
    int64_t icount_extra;       // 额外指令计数

    // 创建/销毁状态
    bool created;               // 是否已创建
    bool unplug;                // 是否待移除

    // 加速器相关
    struct hax_vcpu_state *hax_vcpu;    // HAXM
    struct hvf_vcpu_state *hvf_vcpu;    // HVF (macOS)
    struct kvm_vcpu *kvm_vcpu;          // KVM
    struct whpx_vcpu *whpx_vcpu;        // Windows Hyper-V
};

// 线程局部变量：当前 CPU
__thread CPUState *current_cpu;
```

---

## 二、软件模拟：TCG

### 2.1 TCG 架构

**TCG (Tiny Code Generator)** 是 QEMU 的动态二进制翻译引擎，将客户机指令翻译为主机指令。

```
Guest x86 Instruction
        │
        ▼
┌───────────────────┐
│   TCG Frontend    │  ← 将 guest 指令转换为 TCG 中间码 (IR)
│  (target-i386)    │     位于 target/i386/translate.c
└─────────┬─────────┘
          │ TCG ops (平台无关中间表示)
          ▼
┌───────────────────┐
│   TCG Backend     │  ← 将 TCG IR 转换为主机指令
│   (host-x86_64)   │     位于 tcg/host/ 目录
└─────────┬─────────┘
          │ Host x86_64 Instruction
          ▼
┌───────────────────┐
│   Execute on Host │  ← 直接执行
└───────────────────┘
```

### 2.2 TCG 主执行循环

```c
// accel/tcg/tcg-accel-ops-mttcg.c
static void *mttcg_cpu_thread_fn(void *arg)
{
    CPUState *cpu = arg;

    rcu_register_thread();
    tcg_register_thread();

    qemu_mutex_lock_iothread();  // 获取 BQL
    cpu->thread_id = qemu_get_thread_id();
    cpu->can_do_io = 1;
    current_cpu = cpu;
    cpu_thread_signal_created(cpu);

    do {
        if (cpu_can_run(cpu)) {
            int r;
            // 释放 BQL 执行客户机代码
            qemu_mutex_unlock_iothread();
            r = tcg_cpus_exec(cpu);  // 执行 TCG
            qemu_mutex_lock_iothread();  // 重新获取 BQL

            switch (r) {
            case EXCP_DEBUG:     // 调试断点
                cpu_handle_guest_debug(cpu);
                break;
            case EXCP_HALTED:    // CPU 停机
                g_assert(cpu->halted);
                break;
            case EXCP_ATOMIC:    // 原子操作需要特殊处理
                qemu_mutex_unlock_iothread();
                cpu_exec_step_atomic(cpu);
                qemu_mutex_lock_iothread();
                break;
            }
        }

        qatomic_mb_set(&cpu->exit_request, 0);
        qemu_wait_io_event(cpu);  // 等待 IO 事件
    } while (!cpu->unplug || cpu_can_run(cpu));

    tcg_cpus_destroy(cpu);
    qemu_mutex_unlock_iothread();
    rcu_unregister_thread();
    return NULL;
}
```

### 2.3 TCG 翻译与执行

```c
// cpu_loop.c / cpu-exec.c
int cpu_exec(CPUState *cpu)
{
    CPUClass *cc = CPU_GET_CLASS(cpu);

    cc->cpu_exec_enter(cpu);

    while (!cpu->unplug && !cpu->exit_request) {
        // 1. 查找翻译后的代码块 (Translation Block)
        tb = tb_find(cpu, last_tb, tb_exit, cflags);

        // 2. 执行翻译后的代码
        //    这是一个 goto_tb 跳转，不是函数调用
        cpu_loop_exec_tb(cpu, tb, &last_tb, &tb_exit);

        // 3. 处理异常/中断
        process_icount_data(cpu);
    }

    cc->cpu_exec_exit(cpu);
    return ret;
}
```

### 2.4 Soft TLB（软件 TLB）

TCG 使用软件 TLB 加速地址转换：

```c
// accel/tcg/cputlb.c
// CPUTLBEntry 缓存最近地址转换
typedef struct CPUTLBEntry {
    target_ulong addr_read;      // 读权限检查地址
    target_ulong addr_write;     // 写权限检查地址
    target_ulong addr_code;      // 执行权限检查地址
    target_ulong addend;         // 转换为 HVA 的偏移量
} CPUTLBEntry;

// TLB 查找与地址转换
static inline void *tlb_entry_addr(CPUArchState *env, CPUTLBEntry *entry,
                                   target_ulong addr)
{
    return (void *)((uintptr_t)addr + entry->addend);
}

// 内存访问助手函数
static inline uint8_t cpu_ldb_mmu(CPUArchState *env, target_ulong addr,
                                  MemTxAttrs attrs, uintptr_t retaddr)
{
    CPUTLBEntry *entry = tlb_entry(env, addr);

    // 快速路径：TLB 命中
    if (likely(tlb_hit(entry->addr_read, addr))) {
        uintptr_t haddr = addr + entry->addend;
        return *(uint8_t *)haddr;
    }

    // 慢速路径：TLB 未命中，需要查找页表
    return load_helper(env, addr, attrs, retaddr, MMU_DATA_LOAD, MO_UB);
}
```

---

## 三、硬件虚拟化：KVM

### 3.1 KVM 架构

KVM (Kernel-based Virtual Machine) 利用 CPU 硬件虚拟化扩展（Intel VT-x / AMD-V）。

```
Guest Mode (Non-root Mode)
┌──────────────────────────────┐
│  Guest OS Ring 0             │ ← 客户机内核态直接运行
│  (Hardware assisted)         │     大部分指令无需模拟
└──────────────┬───────────────┘
               │ VM Exit
               ▼ (发生特权操作/异常时退出)
┌──────────────────────────────┐
│  Host Mode (Root Mode)       │
│  KVM Kernel Module           │
│  - Handle MMIO/PIO           │
│  - Inject interrupts         │
│  - Update virtual APIC       │
└──────────────┬───────────────┘
               │ ioctl (KVM_RUN)
               ▼
┌──────────────────────────────┐
│  QEMU Userspace              │
│  - Device emulation          │
│  - Memory management         │
│  - I/O handling              │
└──────────────────────────────┘
```

### 3.2 KVM vCPU 创建与执行

```c
// accel/kvm/kvm-all.c
int kvm_init_vcpu(CPUState *cpu)
{
    KVMState *s = kvm_state;
    int ret;

    // 1. 创建 vCPU
    cpu->kvm_fd = kvm_vcpu_ioctl(s, KVM_CREATE_VCPU, cpu->cpu_index);

    // 2. 映射共享内存区 (kvm_run 结构)
    cpu->kvm_run = mmap(NULL, s->kvm_run_size, PROT_READ | PROT_WRITE,
                        MAP_SHARED, cpu->kvm_fd, 0);

    // 3. 初始化寄存器
    ret = kvm_arch_init_vcpu(cpu);

    return ret;
}

// KVM vCPU 执行循环
int kvm_cpu_exec(CPUState *cpu)
{
    struct kvm_run *run = cpu->kvm_run;

    while (run_cpu_thread(cpu)) {
        // 1. 设置 vCPU 状态到硬件
        kvm_arch_pre_run(cpu, run);

        // 2. 进入 KVM (VM entry)
        //    这会一直运行直到发生 VM exit
        ret = kvm_vcpu_ioctl(cpu, KVM_RUN, 0);

        // 3. 处理 VM exit
        switch (run->exit_reason) {
        case KVM_EXIT_IO:           // I/O 端口访问
            kvm_handle_io(run->io.port, ...);
            break;

        case KVM_EXIT_MMIO:         // MMIO 访问
            cpu_physical_memory_rw(run->mmio.phys_addr,
                                   run->mmio.data, run->mmio.len,
                                   run->mmio.is_write);
            break;

        case KVM_EXIT_IRQ_WINDOW_OPEN:  // 中断窗口打开
            kvm_handle_interrupt(cpu);
            break;

        case KVM_EXIT_HLT:          // HLT 指令
            cpu->halted = 1;
            break;

        case KVM_EXIT_DEBUG:        // 调试事件
            kvm_handle_debug(cpu);
            break;

        case KVM_EXIT_FAIL_ENTRY:   // VM entry 失败
        case KVM_EXIT_INTERNAL_ERROR: // 内部错误
            error_report("KVM internal error");
            abort();
        }
    }

    return ret;
}
```

### 3.3 VMCS / VMCB（虚拟机控制结构）

**Intel VT-x 使用 VMCS (Virtual Machine Control Structure)**：

```c
// VMCS 字段（部分）
enum vmcs_field {
    // Guest state
    GUEST_CR0 = 0x00006800,
    GUEST_CR3 = 0x00006802,
    GUEST_CR4 = 0x00006804,
    GUEST_RIP = 0x0000681e,
    GUEST_RSP = 0x0000681c,
    GUEST_RFLAGS = 0x00006820,
    GUEST_ES_SELECTOR = 0x00000800,
    GUEST_CS_SELECTOR = 0x00000802,
    // ...

    // Host state
    HOST_CR0 = 0x00006c00,
    HOST_CR3 = 0x00006c02,
    HOST_CR4 = 0x00006c04,
    HOST_RIP = 0x00006c16,  // VM exit 后跳转地址
    HOST_RSP = 0x00006c14,
    // ...

    // Control fields
    CPU_BASED_VM_EXEC_CONTROL = 0x00004002,
    PIN_BASED_VM_EXEC_CONTROL = 0x00004000,
    VM_ENTRY_CONTROLS = 0x00004012,
    VM_EXIT_CONTROLS = 0x0000400c,
};

// 设置 VMCS
static int vmx_set_cr0(struct kvm_vcpu *vcpu, unsigned long cr0)
{
    // 写入 Guest CR0 到 VMCS
    vmcs_writel(GUEST_CR0, cr0);

    // 设置 CR0 读影子（Guest 读 CR0 时返回的值）
    vmcs_writel(CR0_READ_SHADOW, cr0);

    return 0;
}
```

**VM Exit 原因**：

| Exit 原因 | 说明 | 处理者 |
|-----------|------|--------|
| `KVM_EXIT_IO` | I/O 端口访问 | QEMU |
| `KVM_EXIT_MMIO` | 内存映射 I/O | QEMU |
| `KVM_EXIT_EPT_VIOLATION` | EPT 缺页 | KVM/QEMU |
| `KVM_EXIT_HLT` | HLT 指令 | KVM |
| `KVM_EXIT_IRQ_WINDOW` | 中断窗口 | KVM |
| `KVM_EXIT_DEBUG` | 调试异常 | QEMU |

### 3.4 MTTCG vs KVM 对比

| 特性 | TCG (MTTCG) | KVM |
|------|-------------|-----|
| **性能** | 10-30% 原生 | 95-100% 原生 |
| **CPU 支持** | 任意主机 | 需要 VT-x/AMD-V |
| **跨架构** | 支持 (x86→ARM) | 不支持 |
| **调试** | 容易 | 较复杂 |
| **嵌套虚拟化** | 软件模拟 | 硬件支持 |

---

## 四、虚拟内存架构

### 4.1 三级地址转换

```
Guest Virtual Address (GVA)
        │
        │ Guest Page Tables (CR3 指向)
        │ 由 Guest OS 管理
        ▼
Guest Physical Address (GPA)
        │
        │  ┌─────────────────────────────────────────┐
        │  │  Memory Region Mapping (QEMU)          │
        │  │  ┌─────────┐ ┌─────────┐ ┌─────────┐  │
        │  │  │  RAM    │ │ MMIO    │ │  ROM    │  │
        │  │  │ Block   │ │ Region  │ │         │  │
        │  │  └────┬────┘ └────┬────┘ └────┬────┘  │
        │  └───────┼───────────┼───────────┼───────┘
        │          │           │           │
        ▼          ▼           ▼           ▼
Host Virtual Address (HVA)
        │
        │ Host Page Tables (由 Host 内核管理)
        ▼
Host Physical Address (HPA)
```

### 4.2 QEMU 内存层次结构

```
┌─────────────────────────────────────────────────────────────────┐
│                     AddressSpace                                │
│  ┌───────────────────────────────────────────────────────────┐ │
│  │                    FlatView                                │ │
│  │  ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐          │ │
│  │  │FlatRange│ │FlatRange│ │FlatRange│ │FlatRange│  ...     │ │
│  │  │ (RAM)   │ │ (MMIO)  │ │ (ROM)   │ │ (Alias) │          │ │
│  │  └────┬────┘ └────┬────┘ └────┬────┘ └────┬────┘          │ │
│  └───────┼───────────┼───────────┼───────────┼────────────────┘ │
│          │           │           │           │                   │
│  ┌───────┴───────────┴───────────┴───────────┴────────────────┐ │
│  │                   MemoryRegion                              │ │
│  │  ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐          │ │
│  │  │  RAM    │ │ Device  │ │  ROM    │ │  Alias  │          │ │
│  │  │ Block   │ │  MMIO   │ │         │ │         │          │ │
│  │  └─────────┘ └─────────┘ └─────────┘ └─────────┘          │ │
│  └────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
```

### 4.3 核心数据结构

```c
// include/exec/memory.h

// MemoryRegion：内存区域描述
struct MemoryRegion {
    Object parent_obj;

    // 层次关系
    MemoryRegion *container;     // 父节点
    Int128 addr;                 // 在父节点中的偏移
    Int128 size;                 // 大小

    // RAM 相关
    RamBlock *ram_block;         // 指向 RAM 块

    // MMIO 相关
    const MemoryRegionOps *ops;  // 操作函数表
    void *opaque;                // 设备私有数据

    // 子区域
    QTAILQ_HEAD(, MemoryRegion) subregions;
    QTAILQ_ENTRY(MemoryRegion) subregions_link;
};

// MemoryRegionOps：设备 MMIO 操作
struct MemoryRegionOps {
    uint64_t (*read)(void *opaque, hwaddr addr, unsigned size);
    void (*write)(void *opaque, hwaddr addr, uint64_t data, unsigned size);

    MemTxResult (*read_with_attrs)(void *opaque, hwaddr addr,
                                   uint64_t *data, unsigned size,
                                   MemTxAttrs attrs);
    MemTxResult (*write_with_attrs)(void *opaque, hwaddr addr,
                                    uint64_t data, unsigned size,
                                    MemTxAttrs attrs);

    enum device_endian endianness;  // 字节序

    struct {
        unsigned min_access_size;
        unsigned max_access_size;
        bool unaligned;
    } valid;  // 访问约束
};

// AddressSpace：地址空间
struct AddressSpace {
    struct rcu_head rcu;
    char *name;
    MemoryRegion *root;          // 根 MemoryRegion
    FlatView *current_map;       // 当前扁平化视图
    QTAILQ_HEAD(, MemoryListener) listeners;
};

// FlatRange：扁平化后的连续内存范围
struct FlatRange {
    MemoryRegion *mr;
    hwaddr offset_in_region;
    AddrRange addr;              // 地址范围
    uint8_t dirty_log_mask;      // 脏页追踪
    bool romd_mode;              // ROM 直接读模式
    bool readonly;
    bool nonvolatile;
};

// FlatView：地址空间的扁平化视图
struct FlatView {
    struct rcu_head rcu;
    unsigned ref;
    FlatRange *ranges;           // 排序后的 FlatRange 数组
    unsigned nr;                 // 范围数量
    MemoryRegion *root;
    AddressSpaceDispatch *dispatch;  // TLB 优化结构
};
```

### 4.4 RAM 块管理

```c
// include/exec/ramlist.h
typedef struct RAMBlock {
    struct rcu_head rcu;
    struct MemoryRegion *mr;
    uint8_t *host;              // 主机虚拟地址 (HVA)
    ram_addr_t offset;          // 在 ram_list 中的偏移
    ram_addr_t used_length;     // 当前使用长度
    ram_addr_t max_length;      // 最大长度
    uint32_t flags;
    size_t page_size;           // 页面大小（用于大页）
    char idstr[256];            // 标识字符串
    QSIMPLEQ_ENTRY(RAMBlock) next;
} RAMBlock;

// RAM 标志
#define RAM_PREALLOC   (1 << 0)   // 预分配内存
#define RAM_SHARED     (1 << 1)   // MAP_SHARED mmap
#define RAM_RESIZEABLE (1 << 2)   // 可调整大小
#define RAM_MIGRATABLE (1 << 4)   // 可迁移
#define RAM_PMEM       (1 << 5)   // 持久内存
```

---

## 五、内存虚拟化技术

### 5.1 影子页表（Shadow Page Tables）- 软件方案

**原理**：QEMU/KVM 维护一个"影子"页表，直接将 GVA→HPA 映射。

```
Guest Page Table          Shadow Page Table
┌──────────────┐          ┌──────────────┐
│ GVA → GPA    │          │ GVA → HPA    │
│ 0x1000→0xA000│    →     │ 0x1000→0xF000│
│ 0x2000→0xB000│          │ 0x2000→0xE000│
└──────────────┘          └──────────────┘

当 Guest 修改页表时触发 VM exit，
KVM 更新对应的 Shadow Page Table 项
```

**处理流程**：

```c
// 处理 Guest 页表修改 (KVM)
static int handle_ept_violation(struct kvm_vcpu *vcpu)
{
    gpa_t gpa = vcpu->arch.exit_qualification;

    // 1. 检查是否是 Guest 页表访问
    if (is_guest_page_table_access(gpa)) {
        // 2. 同步影子页表
        sync_shadow_page_table(vcpu, gpa);
    } else {
        // 3. 普通内存访问，建立 EPT 映射
        kvm_mmu_page_fault(vcpu, gpa, error_code);
    }
}
```

### 5.2 EPT（扩展页表）- 硬件方案

**Intel EPT**：硬件维护第二级页表，直接完成 GVA→GPA→HPA。

```
EPT 页表结构（4 级页表，与 x86-64 类似）：

Guest Virtual Address (48-bit)
├─ PML4 Index ─┼─ PDPT Index ─┼─ PD Index ─┼─ PT Index ─┼─ Offset ─┤
    9 bits         9 bits        9 bits       9 bits      12 bits

Level 4: EPT PML4 (Page Map Level 4)
    EPTP[51:12] + GVA[47:39] × 8 → PDPTE

Level 3: EPT PDPT (Page Directory Pointer)
    PDPTE[51:12] + GVA[38:30] × 8 → PDE

Level 2: EPT PD (Page Directory)
    PDE[51:12] + GVA[29:21] × 8 → PTE

Level 1: EPT PT (Page Table)
    PTE[51:12] + GVA[20:12] × 8 → Physical Page

PTE Format:
├─ Reserved ─┼─ PFN (40 bits) ─┼─ Avail ─┼─ Flags ─┤
                    │                         │
                    ▼                         ▼
               Physical Frame            R/W/X permissions
```

**EPT 缺页处理**（KVM）：

```c
static int kvm_mmu_page_fault(struct kvm_vcpu *vcpu, gpa_t gpa,
                              u32 error_code)
{
    // 1. 查找对应的 MemorySlot
    struct kvm_memory_slot *slot = gfn_to_memslot(vcpu->kvm, gpa >> PAGE_SHIFT);

    // 2. 获取 HPA
    hpa_t hpa = slot->arch.ram_gpa + (gpa - slot->base_gfn * PAGE_SIZE);

    // 3. 设置 EPT 页表项
    u64 *sptep = get_ept_pte(vcpu, gpa);
    *sptep = (hpa & PAGE_MASK) | EPT_VI_READ | EPT_VI_WRITE | EPT_VI_EXECUTE;

    // 4. 刷新 EPT TLB
    __kvm_flush_tlb(vcpu->kvm);
}
```

### 5.3 内存访问流程对比

| 场景 | 技术 | 流程 | 性能 |
|------|------|------|------|
| **Guest RAM 访问** | EPT | GVA→GPA→HPA 硬件完成 | 接近原生 (~95-100%) |
| **Guest RAM 访问** | Shadow | GVA→HVA 软件查表 | 中等 (~80-90%) |
| **MMIO 访问** | VM Exit | GVA→GPA → VM Exit → QEMU 模拟 | 慢 (~1000x) |
| **DMA (设备直通)** | IOMMU | IOMMU 转换 GPA→HPA | 接近原生 |

### 5.4 内存访问代码示例

**正常内存读（KVM + EPT）**：

```c
// Guest 执行: mov (%rax), %rbx

// 1. 硬件自动完成 (无 VM exit)
//    - GVA → GPA (Guest CR3)
//    - GPA → HPA (EPT)
//    - 数据从 HPA 读取到 %rbx

// 无软件开销！
```

**MMIO 访问（需要模拟）**：

```c
// Guest 执行: mov $0x1, 0xfee00000  // 写 LAPIC EOI 寄存器

// 1. 硬件触发 VM Exit (EPT violation / MMIO)
//    Exit reason: EXIT_REASON_EPT_VIOLATION

// 2. KVM 内核处理
kvm_handle_ept_violation(vcpu, gpa=0xfee00000, exit_qual=WRITE)
{
    if (memslot_is_mmio(gpa)) {
        vcpu->run->exit_reason = KVM_EXIT_MMIO;
        return;  // 返回用户态
    }
}

// 3. QEMU 处理 MMIO
kvm_cpu_exec(CPUState *cpu)
{
    switch (run->exit_reason) {
    case KVM_EXIT_MMIO:
        // 找到对应的 MemoryRegion
        mr = memory_region_find(as, run->mmio.phys_addr);

        // 调用设备的 write 回调
        mr->ops->write(mr->opaque, addr, run->mmio.data, size);
        break;
    }
}
```

---

## 六、DMA 与设备访问

### 6.1 DMA 架构

```
Guest OS
   │
   │ 1. 设置 DMA 描述符 (GPA)
   ▼
┌─────────────┐
│  VirtIO NIC │  ← 模拟设备
│  (QEMU)     │
└──────┬──────┘
       │ 2. DMA 请求 (GPA)
       ▼
┌─────────────┐
│  AddressSpace│  ← 地址空间翻译
│  Translate   │
└──────┬──────┘
       │ 3. GPA → HVA
       ▼
┌─────────────┐
│   RAMBlock   │
│   Host Memory│
└──────────────┘
```

### 6.2 DMA 操作代码

```c
// DMA 内存访问
void dma_memory_read(AddressSpace *as, dma_addr_t addr,
                     void *buf, dma_len_t len)
{
    // 1. 地址空间转换
    MemoryRegionSection section = address_space_translate(as, addr, &plen, false);

    // 2. 获取 HVA
    void *hva = memory_region_get_ram_ptr(section.mr) +
                section.offset_within_region +
                (addr - section.offset_within_address_space);

    // 3. 执行 DMA 复制
    memcpy(buf, hva, len);
}
```

### 6.3 IOMMU（设备直通）

当使用 VFIO 设备直通时，需要 IOMMU（Intel VT-d / AMD-Vi）进行地址转换。

```
Device (PCIe)
   │
   │ 1. Device Virtual Address (DVA)
   ▼
┌─────────────┐
│   IOMMU     │  ← 硬件页表转换
│   (VT-d)    │
└──────┬──────┘
       │ 2. GPA (Guest Physical Address)
       ▼
┌─────────────┐
│    EPT      │  ← KVM 扩展页表
└──────┬──────┘
       │ 3. HPA (Host Physical Address)
       ▼
┌─────────────┐
│  Host RAM   │
└─────────────┘
```

---

## 七、总结

### 7.1 vCPU 实现要点

| 方案 | 核心机制 | 性能 | 适用场景 |
|------|----------|------|----------|
| **TCG** | 动态二进制翻译 | 10-30% 原生 | 跨架构、无硬件虚拟化 |
| **KVM** | 硬件辅助虚拟化 | 95-100% 原生 | 同架构、生产环境 |

### 7.2 内存虚拟化要点

| 技术 | 类型 | 作用 | 性能 |
|------|------|------|------|
| **Soft TLB** | 软件缓存 | 加速 GVA→HVA | 快 |
| **Shadow Page Table** | 软件翻译 | GVA→HPA | 中等 |
| **EPT/NPT** | 硬件翻译 | GVA→GPA→HPA | 接近原生 |
| **IOMMU** | 硬件翻译 | DVA→HPA (设备) | 接近原生 |

### 7.3 关键代码路径

| 功能 | 文件路径 |
|------|----------|
| vCPU 管理 | `accel/kvm/kvm-all.c` |
| KVM 执行 | `accel/kvm/kvm-cpus.c` |
| TCG 翻译 | `accel/tcg/tcg-accel-ops-mttcg.c` |
| 内存管理 | `softmmu/memory.c` |
| DMA 操作 | `softmmu/dma-helpers.c` |
| 内存头文件 | `include/exec/memory.h` |

### 7.4 性能优化建议

1. **使用 KVM + EPT**：获得接近原生的性能
2. **启用大页**：减少 TLB miss，提高 EPT 效率
3. **设备直通**：使用 VFIO + IOMMU 绕过模拟层
4. **减少 VM Exit**：合并 I/O 操作，使用 virtio

---
