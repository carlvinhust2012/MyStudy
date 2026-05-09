# QEMU vCPU 架构与 KVM_RUN 分析

## 目录
1. [vCPU 架构概述](#一vcpu-架构概述)
2. [KVM 模式详解](#二kvm-模式详解)
3. [ioctl(KVM_RUN) 深入分析](#三ioctlkvm_run-深入分析)
4. [TCG 模式对比](#四tcg-模式对比)
5. [性能与优化](#五性能与优化)

---

## 一、vCPU 架构概述

### 1.1 常见误解澄清

**误区**: "QEMU 的 vCPU 是一个结构体，里面有任务队列，将收到的任务放到操作系统线程上执行"

**真相**: QEMU 的 vCPU **不是**传统意义上的"任务分发"模型。它的实现取决于虚拟化模式：

| 模式 | vCPU 本质 | 执行方式 |
|------|----------|----------|
| **KVM** | Linux 线程 + 硬件虚拟化 | 线程通过 `ioctl(KVM_RUN)` 变成真正的 vCPU |
| **TCG** | 代码翻译执行单元 | 在用户空间翻译并执行 Guest 代码 |

### 1.2 核心数据结构

```c
// include/hw/core/cpu.h
struct CPUState {
    // ========== 标识与状态 ==========
    int cpu_index;                    // vCPU 编号 (0, 1, 2...)
    uint32_t halted;                  // 是否暂停
    uint32_t stopped;                 // 是否停止

    // ========== KVM 相关 ==========
    int kvm_fd;                       // /dev/kvm 创建的 vCPU fd
    struct kvm_run *kvm_run;          // 与内核共享的 mmap 区域
    int kvm_vcpu_dirty;               // 寄存器是否被修改

    // ========== 线程相关 ==========
    QemuThread thread;                // 宿主线程对象
    bool created;                     // 线程是否已创建
    bool running;                     // 是否正在运行
    bool queued_work_first;           // 挂起的工作项（不是执行队列！）

    // ========== 执行统计 ==========
    int64_t icount_budget;            // 指令计数预算
    int32_t icount_extra;             // 额外指令计数

    // ========== 架构相关 ==========
    CPUArchState *env_ptr;            // 指向 X86CPU/ARMCPU 等

    // ========== 中断相关 ==========
    struct qemu_work_item *queued_work;  // 用于状态同步，非任务队列
    bool interrupt_request;              // 中断请求标志
    int32_t interrupt_inject_count;      // 注入计数

    // ========== 内存相关 ==========
    CPUBreakpoint *breakpoints;       // 调试断点
    CPUWatchpoint *watchpoints;       // 观察点
};

// X86 特定结构 (target/i386/cpu.h)
typedef struct X86CPU {
    CPUState parent_obj;              // 继承 CPUState

    // x86 特有寄存器
    CPUX86State env;                  // 所有 x86 寄存器状态
    // - general_regs[16]           // RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI...
    // - eip                        // 指令指针
    // - eflags                     // 标志寄存器
    // - segs[6]                    // CS, DS, ES, SS, FS, GS
    // - cr[5]                      // CR0-CR4
    // - dr[8]                      // 调试寄存器
    // - xcr[2]                     // XCR0-XCR1
    // - mtrr                         // 内存类型范围寄存器
    // - msr                          // 模式特定寄存器
    // - fpu                          // 浮点单元状态
    // - xmm_regs[32]               // SSE/AVX 寄存器
} X86CPU;
```

---

## 二、KVM 模式详解

### 2.1 KVM 执行模型

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              宿主机 Linux 系统                               │
│                                                                             │
│  ┌─────────────────────────────────┐      ┌─────────────────────────────┐  │
│  │         QEMU 进程               │      │      KVM 内核模块            │  │
│  │                                 │      │                             │  │
│  │  ┌─────────────────────────┐   │      │  ┌─────────────────────┐    │  │
│  │  │    Main Thread          │   │      │  │  /dev/kvm           │    │  │
│  │  │  ├─ 事件循环处理        │   │      │  │  ├─ vm fd          │    │  │
│  │  │  ├─ 设备模拟            │   │      │  │  └─ vcpu fd × N    │    │  │
│  │  │  └─ 中断路由            │   │      │  │                    │    │  │
│  │  └─────────────────────────┘   │      │  │  ┌─────────────┐    │    │  │
│  │                                 │      │  │  │  VM Struct  │    │    │  │
│  │  ┌─────────────────────────┐   │      │  │  │  ├─ memslots │    │    │  │
│  │  │   vCPU Thread 0         │   │      │  │  │  ├─ vcpus[]  │    │    │  │
│  │  │  ├─ ioctl(KVM_RUN) ────────┼──────┼──┼─>│  └─ mmu       │    │    │  │
│  │  │  │                      │   │      │  │  └─────────────┘    │    │  │
│  │  │  │  共享内存: kvm_run   │◄──┼──────┼──┤                     │    │  │
│  │  │  │                      │   │ mmap │  │  ┌─────────────┐    │    │  │
│  │  │  └─────────────────────┘   │      │  │  │ vCPU Struct │    │    │  │
│  │  │                            │      │  │  │ ├─ regs     │    │    │  │
│  │  │  ┌────────────────────────┐│      │  │  │ ├─ kvm_run ─┼────┘    │  │
│  │  │  │   vCPU Thread 1        ││      │  │  │ └─ vmcs/vcb │         │  │
│  │  │  │  ├─ ioctl(KVM_RUN) ───────┼───┼──┤  └─────────────┘         │  │
│  │  │  │  │                     ││      │  │                          │  │
│  │  │  │  共享内存: kvm_run    │◄─┼─────┼──┤                          │  │
│  │  │  │                     ││      │  │                          │  │
│  │  │  └─────────────────────┘│      │  │                          │  │
│  │  └─────────────────────────┘      │  │                          │  │
│  │                                    │  │                          │  │
│  └────────────────────────────────────┘  └──────────────────────────┘  │
│                                                                             │
│  ==================================== 硬件分界线 =========================  │
│                                                                             │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                          CPU 物理核心                               │   │
│  │                                                                     │   │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────────┐  │   │
│  │  │   VMX Root   │  │  VMX Root    │  │      VMX Non-Root        │  │   │
│  │  │   (Host)     │  │   (Host)     │  │      (Guest)             │  │   │
│  │  │              │  │              │  │                          │  │   │
│  │  │ QEMU Thread 0│  │ QEMU Thread 1│  │  ┌──────────────────┐   │  │   │
│  │  │ ├─用户空间代码│  │ ├─用户空间代码│  │  │   Guest OS       │   │  │   │
│  │  │ └─ioctl()    │  │ └─ioctl()    │  │  │   ├─ Ring 0      │   │  │   │
│  │  │              │  │              │  │  │   ├─ Ring 3      │   │  │   │
│  │  │              │  │              │  │  │   └─ 应用程序    │   │  │   │
│  │  │              │  │              │  │  └──────────────────┘   │  │   │
│  │  │              │  │              │  │                          │  │   │
│  │  │  VM Entry    │  │  VM Entry    │  │  直接在物理 CPU 上执行！  │  │   │
│  │  │     ↑↓       │  │     ↑↓       │  │  ├─ Guest 指令         │  │   │
│  │  │              │  │              │  │  ├─ Guest 页表(EPT)    │  │   │
│  │  │              │  │              │  │  └─ Guest 中断         │  │   │
│  │  └──────────────┘  └──────────────┘  └──────────────────────────┘  │   │
│  │                                                                     │   │
│  │  关键: VMX Non-Root 模式下，CPU 直接执行 Guest 代码，不是模拟！       │   │
│  │                                                                     │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 2.2 KVM vCPU 线程生命周期

```c
// accel/kvm/kvm-all.c

// 1. vCPU 创建
int kvm_init_vcpu(CPUState *cpu) {
    // 创建 vCPU
    cpu->kvm_fd = ioctl(vmfd, KVM_CREATE_VCPU, cpu->cpu_index);

    // 映射共享内存区域 (kvm_run 结构)
    cpu->kvm_run = mmap(NULL, mmap_size, PROT_READ|PROT_WRITE, MAP_SHARED,
                        cpu->kvm_fd, 0);

    // 创建线程
    qemu_thread_create(cpu->thread, "CPU", qemu_kvm_cpu_thread_fn,
                       cpu, QEMU_THREAD_JOINABLE);
}

// 2. vCPU 线程入口函数
static void *qemu_kvm_cpu_thread_fn(void *arg) {
    CPUState *cpu = arg;

    // 绑定到特定 CPU (可选)
    if (cpu->thread_id != -1) {
        qemu_thread_set_affinity(cpu->thread, cpu->thread_id);
    }

    // 主循环
    while (1) {
        // 2.1 处理主线程发来的工作请求
        //     (如: 重置 vCPU、修改寄存器)
        qemu_wait_io_event(cpu);

        // 2.2 检查是否需要退出
        if (cpu->stop || cpu->stopped) {
            break;
        }

        // 2.3 关键！进入 KVM 执行
        //     此时线程"变成" vCPU，直接在硬件上运行 Guest
        kvm_cpu_exec(cpu);

        // 2.4 处理 VM Exit
        //     (IO/MMIO/中断等)
        process_vm_exit(cpu);
    }

    return NULL;
}

// 3. 进入 KVM 执行
int kvm_cpu_exec(CPUState *cpu) {
    struct kvm_run *run = cpu->kvm_run;
    int ret;

    // 同步寄存器状态到内核
    if (cpu->kvm_vcpu_dirty) {
        kvm_arch_put_registers(cpu, KVM_PUT_RUNTIME_STATE);
        cpu->kvm_vcpu_dirty = 0;
    }

    do {
        // ========== 关键系统调用 ==========
        // 进入内核，线程变成 vCPU
        ret = ioctl(cpu->kvm_fd, KVM_RUN, 0);
        // ==================================

        // 从 KVM 返回，处理退出原因
        switch (run->exit_reason) {
        case KVM_EXIT_IO:
            // Guest 执行了 IN/OUT 指令
            kvm_handle_io(cpu, &run->io);
            break;

        case KVM_EXIT_MMIO:
            // Guest 访问了模拟设备的内存
            address_space_rw(&run->mmio);
            break;

        case KVM_EXIT_IRQ_WINDOW_OPEN:
            // 可以注入中断了
            kvm_try_inject_interrupts(cpu);
            break;

        case KVM_EXIT_HLT:
            // Guest 执行了 HLT
            cpu->halted = 1;
            break;

        case KVM_EXIT_INTERNAL_ERROR:
            // 内部错误
            fprintf(stderr, "KVM internal error: %d\n", run->internal.suberror);
            abort();

        // ... 其他 exit 原因
        }

    } while (ret == 0);

    // 同步寄存器状态到 QEMU
    kvm_arch_get_registers(cpu);

    return ret;
}
```

---

## 三、ioctl(KVM_RUN) 深入分析

### 3.1 系统调用入口

```c
// virt/kvm/kvm_main.c

static long kvm_vcpu_ioctl(struct file *filp, unsigned int ioctl, unsigned long arg) {
    struct kvm_vcpu *vcpu = filp->private_data;

    switch (ioctl) {
    case KVM_RUN:
        // 检查权限
        if (!vcpu->arch.mp_state.kvm_in_progress) {
            vcpu->arch.mp_state.kvm_in_progress = 1;
        }

        // 关键：执行 vCPU
        r = kvm_arch_vcpu_ioctl_run(vcpu, vcpu->run);
        break;

    // ... 其他 ioctl
    }

    return r;
}
```

### 3.2 Intel VT-x 执行流程

```c
// arch/x86/kvm/vmx/vmx.c

static int vmx_vcpu_run(struct kvm_vcpu *vcpu) {
    // ========== 阶段 1: VM Entry 准备 ==========

    // 1.1 加载 VMCS (Virtual Machine Control Structure)
    // VMCS 包含 Guest/Host 的所有状态和控制信息
    vmx_load_vmcs(vcpu);

    // 1.2 检查并注入待处理的中断
    if (kvm_check_request(KVM_REQ_EVENT, vcpu)) {
        inject_pending_event(vcpu);
    }

    // 1.3 禁用抢占和中断（关键区域）
    local_irq_disable();

    // 1.4 保存宿主机 FPU 状态，加载 Guest FPU
    kvm_load_guest_fpu(vcpu);

    // ========== 阶段 2: VM Entry (进入 Guest) ==========

    // 汇编代码执行 VM Entry
    // 关键指令: vmlaunch (首次) 或 vmresume (后续)
    vmx_vcpu_run_asm(vcpu, vmx);

    /*
     * vmx_vcpu_run_asm 汇编代码 (arch/x86/kvm/vmx/vmenter.S):
     *
     * ENTRY(vmx_vcpu_run)
     *     // 保存宿主寄存器
     *     push %r15
     *     push %r14
     *     push %r13
     *     push %r12
     *     push %rbp
     *     push %rbx
     *
     *     // 加载 Guest 寄存器 (从 kvm_vcpu_arch)
     *     mov VCPU_REGS_RAX(%rsi), %rax
     *     mov VCPU_REGS_RBX(%rsi), %rbx
     *     ...
     *     mov VCPU_REGS_R15(%rsi), %r15
     *
     *     // 保存当前 RSP 到 VMCS Host RSP
     *     mov %rsp, VMCS_HOST_RSP(%rdx)
     *
     *     // ========== VM ENTRY ==========
     *     // 这是魔法发生的地方！
     *     // CPU 自动:
     *     // 1. 保存 Host 状态到 VMCS Host Area
     *     // 2. 从 VMCS Guest Area 加载 Guest 状态
     *     // 3. 开始执行 Guest 代码
     *     testb %cl, %cl           // 检查是否是首次运行
     *     jne 1f
     *     vmlaunch                  // 首次运行
     *     jmp 2f
     * 1:  vmresume                  // 恢复运行
     * 2:
     *     // ========== VM EXIT (从这里继续) ==========
     *     // CPU 自动:
     *     // 1. 保存 Guest 状态到 VMCS Guest Area
     *     // 2. 从 VMCS Host Area 恢复 Host 状态
     *
     *     // 保存 Guest 寄存器
     *     mov %rax, VCPU_REGS_RAX(%rsi)
     *     mov %rbx, VCPU_REGS_RBX(%rsi)
     *     ...
     *     mov %r15, VCPU_REGS_R15(%rsi)
     *
     *     // 恢复宿主寄存器
     *     pop %rbx
     *     pop %rbp
     *     pop %r12
     *     pop %r13
     *     pop %r14
     *     pop %r15
     *     ret
     * ENDPROC(vmx_vcpu_run)
     */

    // ========== 阶段 3: VM Exit 处理 ==========

    // 3.1 恢复中断和抢占
    local_irq_enable();

    // 3.2 保存 Guest FPU，恢复宿主机 FPU
    kvm_save_guest_fpu(vcpu);

    // 3.3 读取 VM Exit 原因
    // VMCS_EXIT_REASON 字段自动由 CPU 设置
    exit_reason = vmcs_read32(VM_EXIT_REASON);

    // 3.4 分发处理
    return vmx_handle_exit(vcpu, exit_reason);
}
```

### 3.3 VMCS (Virtual Machine Control Structure) 详解

```c
// VMCS 是 Intel VT-x 的核心数据结构
// 它控制 VM Entry/Exit 的所有行为

VMCS 内存布局 (4KB 页):
┌─────────────────────────────────────────────────────────┐
│ 偏移 0x000-0x3FF: VMCS 数据                             │
├─────────────────────────────────────────────────────────┤
│ 区域                    │ 说明                          │
├─────────────────────────┼───────────────────────────────┤
│ Guest State Area        │ Guest 的完整 CPU 状态         │
│ (偏移 0x000-0x1FF)      │                               │
│                         │ ├─ RIP, RSP, RFLAGS          │
│                         │ ├─ CS, SS, DS, ES, FS, GS    │
│                         │ ├─ CR0, CR3, CR4, GDTR, IDTR │
│                         │ ├- LDTR, TR                  │
│                         │ ├─ MSR (EFER, SYSENTER_*)    │
│                         │ └─ 浮点/SSE/AVX 状态         │
├─────────────────────────┼───────────────────────────────┤
│ Host State Area         │ Host 状态（VM Exit 后恢复）   │
│ (偏移 0x200-0x2FF)      │                               │
│                         │ ├─ RIP (VM Exit 后执行地址)   │
│                         │ ├─ RSP, CS, SS               │
│                         │ ├─ FS Base, GS Base          │
│                         │ ├─ TR, GDTR, IDTR            │
│                         │ └─ MSRs                      │
├─────────────────────────┼───────────────────────────────┤
│ VM Execution Control    │ 控制什么导致 VM Exit          │
│ (偏移 0x300-0x380)      │                               │
│                         │ ├─ Pin-based controls:        │
│                         │ │  ├─ External Interrupt      │
│                         │ │  ├─ NMI                     │
│                         │ │  └─ Virtual NMIs            │
│                         │ │                             │
│                         │ ├─ Primary Processor-based:   │
│                         │ │  ├─ Interrupt-window        │
│                         │ │  ├─ Use TPR shadow          │
│                         │ │  ├─ HLT exiting             │
│                         │ │  ├─ INVLPG exiting          │
│                         │ │  ├─ MWAIT exiting           │
│                         │ │  ├─ RDPMC exiting           │
│                         │ │  ├─ RDTSC exiting           │
│                         │ │  ├─ CR3/CR8 load/store      │
│                         │ │  └─ Unconditional IO exit   │
│                         │ │                             │
│                         │ ├─ Secondary Processor-based: │
│                         │ │  ├─ Enable EPT              │
│                         │ │  ├─ Enable VPID             │
│                         │ │  ├─ Unrestricted Guest      │
│                         │ │  ├─ APIC Register Virt.     │
│                         │ │  ├─ Virtual Interrupt Del.  │
│                         │ │  └─ PAUSE-loop exiting      │
│                         │ │                             │
│                         │ └─ Exception Bitmap           │
│                         │    (哪些异常导致 VM Exit)      │
├─────────────────────────┼───────────────────────────────┤
│ VM Exit Control         │ VM Exit 行为控制              │
│ (偏移 0x380-0x3C0)      │                               │
│                         │ ├─ Save/Load MSR 列表         │
│                         │ └─ Host Address Space Size    │
├─────────────────────────┼───────────────────────────────┤
│ VM Entry Control        │ VM Entry 行为控制             │
│ (偏移 0x3C0-0x3F0)      │                               │
│                         │ ├─ Load MSR 列表              │
│                         │ ├─ Inject Event (中断/异常)   │
│                         │ └─ IA-32e Mode Guest          │
├─────────────────────────┼───────────────────────────────┤
│ VM Exit Information     │ VM Exit 信息（只读）          │
│ (偏移 0x3F0-0x400)      │                               │
│                         │ ├─ Exit Reason                │
│                         │ ├─ Exit Qualification         │
│                         │ ├─ Guest Linear Address       │
│                         │ ├─ Guest Physical Address     │
│                         │ └─ VM-Instruction Error       │
└─────────────────────────────────────────────────────────┘
```

### 3.4 VM Entry → Guest Run → VM Exit 完整时序

```
时间 ─────────────────────────────────────────────────────────────────────────>

阶段 1: 准备 (内核空间)
─────────────────────────────────────────

[T0]  QEMU 调用 ioctl(KVM_RUN)
[T1]  进入 kvm_vcpu_ioctl() [virt/kvm/kvm_main.c]
[T2]  调用 kvm_arch_vcpu_ioctl_run() [arch/x86/kvm/x86.c]
[T3]  检查 pending requests
      ├─ KVM_REQ_EVENT: 有需要注入的事件
      ├─ KVM_REQ_MMU_RELOAD: 需要重载页表
      └─ KVM_REQ_CLOCK_UPDATE: 需要更新时钟
[T4]  检查信号 pending (KVM_RUN 可被信号中断)
[T5]  调用 vcpu_enter_guest() [arch/x86/kvm/x86.c]

阶段 2: VM Entry 准备 (关中断状态)
─────────────────────────────────────────

[T6]  local_irq_disable()          // 禁用本地中断
[T7]  preempt_disable()            // 禁用抢占
[T8]  guest_enter_irqoff()         // 统计: 进入 Guest 模式
[T9]  kvm_load_guest_fpu()         // 加载 Guest FPU 状态
[T10] vmx_prepare_switch_to_guest() // VT-x 特定准备
[T11] sync_regs_to_vmcs()          // 同步寄存器到 VMCS

阶段 3: VM Entry (硬件自动执行)
─────────────────────────────────────────

[T12] vmx_vcpu_run_asm()           // 汇编入口

      // 保存 Host 寄存器到栈
[T13] push %r15; push %r14; ... push %rbx

      // 加载 Guest 寄存器
[T14] mov VCPU_REGS_RAX, %rax; ... mov VCPU_REGS_R15, %r15

      // ========== CPU 硬件执行 ==========
[T15] vmlaunch / vmresume          // 关键指令！

      // 此时硬件自动:
      // ├─ 保存 Host 状态到 VMCS Host State Area
      // ├─ 加载 Guest 状态从 VMCS Guest State Area
      // ├─ RIP = Guest RIP (开始执行 Guest 代码)
      // ├─ RSP = Guest RSP
      // ├─ 页表切换到 EPT (扩展页表)
      // └─ CPU 模式切换到 VMX Non-Root

      // Guest OS 执行 (可能执行数千到数百万条指令)
      // ...

阶段 4: VM Exit 触发 (硬件自动)
─────────────────────────────────────────

[Tx]  某个事件触发 VM Exit:
      ├─ Guest 执行 IN/OUT (IO 指令)
      ├─ Guest 访问未映射内存 (EPT Violation)
      ├─ Guest 执行 HLT/CPUID/RDMSR/WRMSR
      ├─ Guest 写入 CR3 (页表切换)
      ├─ 外部中断到达 (如定时器中断)
      └─ Guest 执行 INT (中断指令)

      // 硬件自动:
      // ├─ 保存 Guest 状态到 VMCS Guest State Area
      // ├─ 保存 Exit Reason 到 VMCS
      // ├─ 加载 Host 状态从 VMCS Host State Area
      // ├─ RIP = vmx_vcpu_run_asm 中 vmlaunch 之后
      // └─ CPU 模式切换回 VMX Root

阶段 5: VM Exit 处理
─────────────────────────────────────────

[Tx+1] 从 vmlaunch/vmresume 之后继续执行 (汇编)

       // 保存 Guest 寄存器
[Tx+2] mov %rax, VCPU_REGS_RAX; ... mov %r15, VCPU_REGS_R15

       // 恢复 Host 寄存器
[Tx+3] pop %rbx; ... pop %r15

[Tx+4] ret  // 返回到 C 代码

[Tx+5] vmx_handle_exit()           // 处理退出原因
       ├─ 读取 VMCS_EXIT_REASON
       ├─ 根据原因分发处理
       └─ 填充 kvm_run 结构体

[Tx+6] kvm_save_guest_fpu()        // 保存 Guest FPU
[Tx+7] guest_exit_irqoff()         // 统计: 退出 Guest
[Tx+8] preempt_enable()            // 启用抢占
[Tx+9] local_irq_enable()          // 启用中断

阶段 6: 返回用户空间
─────────────────────────────────────────

[Tx+10] kvm_arch_vcpu_ioctl_run() 返回
[Tx+11] kvm_vcpu_ioctl() 返回
[Tx+12] 系统调用返回，回到 QEMU

[Tx+13] QEMU 的 kvm_cpu_exec() 继续执行
        ├─ 读取 cpu->kvm_run->exit_reason
        ├─ switch-case 处理不同退出原因
        └─ 可能重新进入 KVM_RUN
```

### 3.5 VM Exit 原因详解

```c
// include/uapi/linux/kvm.h

// 常见 VM Exit 原因及处理

switch (exit_reason) {
// ========== 正常操作 ==========

case KVM_EXIT_IO: {
    // Guest 执行 IN/OUT 指令
    // 处理: 模拟端口 IO
    struct kvm_io *io = &run->io;

    if (io->direction == KVM_EXIT_IO_IN) {
        // 从设备读取数据到 io->data
        port_read(io->port, io->size, io->data);
    } else {
        // 向设备写入数据
        port_write(io->port, io->size, io->data);
    }
    // 返回后，Guest 从 io->data 读取结果
    break;
}

case KVM_EXIT_MMIO: {
    // Guest 访问内存映射 IO
    // 处理: 模拟设备内存访问
    struct kvm_mmio *mmio = &run->mmio;

    address_space_rw(mmio->phys_addr, mmio->data,
                     mmio->len, mmio->is_write);
    break;
}

case KVM_EXIT_IRQ_WINDOW_OPEN: {
    // 中断窗口打开，可以注入中断了
    // 处理: 尝试注入 pending 中断
    kvm_try_inject_interrupts(vcpu);
    break;
}

case KVM_EXIT_HLT: {
    // Guest 执行 HLT 指令
    // 处理: vCPU 进入暂停状态，等待中断
    cpu->halted = 1;
    // 通常会让出 CPU，等待中断事件
    break;
}

// ========== CPUID/MSR 操作 ==========

case KVM_EXIT_CPUID: {
    // Guest 执行 CPUID
    // 处理: 模拟 CPUID 结果
    kvm_emulate_cpuid(vcpu);
    break;
}

case KVM_EXIT_RDMSR:
case KVM_EXIT_WRMSR: {
    // Guest 读取/写入 MSR
    // 处理: 模拟 MSR 访问
    if (is_special_msr(msr_index)) {
        emulate_msr_access(vcpu, msr_index, msr_value, is_write);
    }
    break;
}

// ========== 内存管理 ==========

case KVM_EXIT_EPT_VIOLATION: {
    // EPT 页表违规
    // 原因:
    // 1. Guest 访问未映射的物理内存
    // 2. 权限不足 (写只读页)

    gpa_t gpa = vmcs_read64(GUEST_PHYSICAL_ADDRESS);

    if (is_mmio(gpa)) {
        // MMIO 区域，需要模拟
        return KVM_EXIT_MMIO;
    } else {
        // 分配内存并建立 EPT 映射
        kvm_mmu_page_fault(vcpu, gpa, error_code);
    }
    break;
}

case KVM_EXIT_EXCP_BASE + PF_VECTOR: {
    // Guest 发生页错误 (#PF)
    // 通常 Guest OS 自己处理
    // 但如果是由 EPT 违规导致，需要特殊处理
    break;
}

// ========== 调试 ==========

case KVM_EXIT_DEBUG: {
    // 调试事件 (断点/单步)
    // 处理: 通知 GDB
    kvm_handle_debug(vcpu);
    break;
}

case KVM_EXIT_EXCEPTION: {
    // Guest 发生异常
    // 处理: 尝试注入 Guest，或终止
    if (can_inject_to_guest(exception)) {
        kvm_inject_exception(vcpu, exception);
    } else {
        kill_guest();
    }
    break;
}

// ========== 系统事件 ==========

case KVM_EXIT_SHUTDOWN: {
    // Guest 执行 triple fault 或 SHUTDOWN
    // 处理: 通常重启或终止 VM
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
    break;
}

case KVM_EXIT_FAIL_ENTRY: {
    // VM Entry 失败 (硬件错误)
    // 这不应该发生，表示严重问题
    fprintf(stderr, "KVM: VM Entry failed!\n");
    abort();
}

case KVM_EXIT_INTERNAL_ERROR: {
    // KVM 内部错误
    // 可能原因:
    // - 无效的 VMCS 状态
    // - MSR 列表错误
    // - 分页错误
    handle_internal_error(vcpu, suberror);
    break;
}
}
```

---

## 四、TCG 模式对比

### 4.1 TCG 执行模型

```
KVM 模式 vs TCG 模式:

KVM:
Guest Code ──→ 物理 CPU (VMX Non-Root) ──→ 直接执行！
              ↑↓ VM Entry/Exit
              内核 KVM 模块
              ↑↓ ioctl
              QEMU 用户空间

TCG:
Guest Code ──→ TCG Frontend ──→ TCG IR ──→ TCG Backend ──→ Host 代码
                                                                  ↓
                                                            QEMU 进程中执行
                                                                  ↓
                                                              无 VM Exit

TCG 详细流程:
─────────────────────────────────────────

Guest Binary (x86)
    │
    ├─ 基本块 (Translation Block)
    │   ├─ 从 RIP 开始，到分支/跳转/结束指令
    │   └─ 通常 1-100 条指令
    │
    ├─ TCG Frontend (target/i386/translate.c)
    │   ├─ 解码 x86 指令
    │   ├─ 生成 TCG IR (Intermediate Representation)
    │   └─ 如: mov %eax, %ebx → mov_i32 tmp, eax; mov_i32 ebx, tmp
    │
    ├─ TCG Optimizer
    │   ├─ 常量传播
    │   ├─ 死代码消除
    │   └─ 窥孔优化
    │
    ├─ TCG Backend (tcg/host/*)
    │   ├─ 将 TCG IR 翻译为 Host 汇编
    │   ├─ x86_64 Host: 生成 x86_64 代码
    │   ├─ ARM Host: 生成 ARM 代码
    │   └─ 存储到 Translation Cache
    │
    └─ 执行生成的 Host 代码
        ├─ 在 QEMU 进程中直接执行
        ├─ 使用 Host 寄存器模拟 Guest 寄存器
        └─ 内存访问: 需要地址翻译 (SoftMMU)
```

### 4.2 TCG 代码示例

```c
// accel/tcg/cpu-exec.c

int cpu_exec(CPUState *cpu) {
    CPUArchState *env = cpu->env_ptr;
    TranslationBlock *tb;

    // 主执行循环
    while (!cpu->stopped) {

        // 1. 获取当前指令指针
        target_ulong pc = env->eip + env->segs[R_CS].base;

        // 2. 查找 Translation Block (代码块缓存)
        tb = tb_lookup_pc(pc);

        if (tb == NULL) {
            // 3. 未找到，需要翻译
            tb = tb_gen_code(cpu, pc, cs_base, flags, cflags);

            /* tb_gen_code 内部:
             *
             * a. 分配 TB 结构
             * b. disas_insn() - 解码 Guest 指令
             * c. translator_sparc/i386/arm/... - 生成 TCG IR
             * d. tcg_gen_code() - 生成 Host 代码
             * e. 存储到 TB cache
             */
        }

        // 4. 执行翻译后的代码
        // tcg_qemu_tb_exec 是汇编函数，直接跳转执行生成的代码
        ret = tcg_qemu_tb_exec(cpu, tb->tc_ptr);

        // 5. 根据返回结果处理
        switch (ret) {
        case TB_EXIT_REQUESTED:
            // 有外部请求（如中断、停止）
            break;

        case TB_EXIT_ICOUNT_EXPIRED:
            // 指令计数到期
            break;

        default:
            // 正常执行完一个 TB
            // 更新 PC，继续下一个 TB
            break;
        }

        // 6. 检查和处理事件
        // ├─ 检查中断
        // ├─ 检查 timer
        // └─ 检查 IO
        process_interrupts(cpu);
    }

    return ret;
}
```

### 4.3 KVM vs TCG 对比

| 特性 | KVM | TCG |
|------|-----|-----|
| **执行方式** | 硬件虚拟化，直接执行 | 软件模拟，动态翻译 |
| **性能** | 接近原生 (~95%) | 慢 5-20 倍 |
| **CPU 要求** | 需要 VT-x/AMD-V | 任何 CPU |
| **Guest 兼容性** | 支持 x86/x86_64/ARM... | 支持更多架构 |
| **调试能力** | 受限 | 强大 (可单步每条指令) |
| **内存开销** | 较低 | 较高 (TC cache) |
| **启动速度** | 快 | 慢 (需要翻译) |
| **典型使用** | 生产环境 | 开发/调试/跨架构 |

---

## 五、性能与优化

### 5.1 KVM 性能优化技术

```
1. EPT (Extended Page Table)
─────────────────────────────────────────

无 EPT (Shadow Paging):
Guest CR3 ──→ Shadow Page Table ──→ Host 物理地址
    ↑           ↓
    └──────── 每次 Guest 修改页表，需要 VM Exit 同步

有 EPT:
Guest CR3 ──→ Guest 页表 ──→ EPT ──→ Host 物理地址
    ↑                           ↓
    └────────────────────── 硬件自动翻译，无 VM Exit

性能提升: 10-30% (内存密集型工作负载)

2. VPID (Virtual Processor ID)
─────────────────────────────────────────

无 VPID:
每次 VM Entry/Exit 刷新 TLB
├─ VM Entry: 加载 Guest 页表，TLB 为空
├─ Guest 运行: 逐步填充 TLB
├─ VM Exit: 刷新 TLB
└─ 重复...

有 VPID:
VPID 标记 TLB 条目归属
├─ VM Entry: TLB 中 Guest 条目仍有效
├─ Guest 运行: 使用缓存的 TLB
├─ VM Exit: Host 和 Guest TLB 共存
└─ 减少 TLB miss

性能提升: 5-15%

3. APICv (APIC Virtualization)
─────────────────────────────────────────

无 APICv:
每次中断: VM Exit → QEMU → KVM → VM Entry
延迟: ~3μs

有 APICv:
硬件自动处理中断投递
├─ Posted Interrupt Descriptor
├─ 硬件检查中断目标 vCPU
├─ 直接写入 Guest APIC
└─ 无需 VM Exit

延迟: ~0.5μs
性能提升: 显著 (高 I/O 负载)

4. IRQFD
─────────────────────────────────────────

传统中断注入:
设备 → eventfd → QEMU → ioctl(KVM_IRQ_LINE) → KVM → VM Entry

IRQFD:
设备 → eventfd ──────────────────────────────→ KVM → 直接注入
              (内核直接处理，绕过 QEMU)

延迟减少: 2-3μs
```

### 5.2 性能数据

| 操作 | KVM | TCG | 物理机 |
|------|-----|-----|--------|
| CPU 密集型 | 95% | 20-50% | 100% |
| 内存访问 | 90% | 30-60% | 100% |
| 磁盘 I/O | 85% | 40-70% | 100% |
| 网络 I/O | 80% | 30-60% | 100% |
| 系统调用 | 70% | 10-30% | 100% |

*百分比相对于物理机性能*

### 5.3 关键延迟指标

| 事件 | 延迟 | 说明 |
|------|------|------|
| VM Entry | 300-500 cycles | 保存 Host，加载 Guest |
| VM Exit | 400-600 cycles | 保存 Guest，加载 Host |
| VM Exit → Handler | 1-3μs | 内核处理退出原因 |
| Kernel → QEMU | 1-2μs | 系统调用返回 |
| 完整 IO Exit | 3-8μs | 从 Guest IO 到 QEMU 处理 |
| IRQFD 注入 | 0.5μs | 最优路径，无 VM Exit |
| APICv 中断 | 0.5μs | 硬件直接注入 |

---

## 附录: 关键代码路径

### KVM_RUN 完整调用链

```
QEMU (用户空间)
└─ kvm_cpu_exec()
   └─ ioctl(KVM_RUN)
      ↓ 系统调用

KVM (内核空间)
└─ kvm_vcpu_ioctl()
   └─ kvm_arch_vcpu_ioctl_run() [x86.c]
      └─ vcpu_enter_guest()
         └─ kvm_x86_ops->run() → vmx_vcpu_run() [vmx.c]
            └─ vmx_vcpu_run_asm() [vmenter.S]
               ├─ 保存 Host 寄存器
               ├─ 加载 Guest 寄存器
               ├─ vmlaunch/vmresume
               │  ↓ 硬件执行
               │  Guest OS 运行
               │  ↓ VM Exit
               ├─ 保存 Guest 寄存器
               ├─ 加载 Host 寄存器
               └─ ret
            └─ vmx_handle_exit()
               └─ 根据 exit_reason 分发
      └─ kvm_x86_ops->handle_exit()
   └─ 返回到 QEMU

QEMU (用户空间)
└─ 处理 kvm_run->exit_reason
   └─ 可能重新 KVM_RUN
```

---
