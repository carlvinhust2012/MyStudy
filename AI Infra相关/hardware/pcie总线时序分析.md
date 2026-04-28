# PCIe 协议时序流程图详解

## 1. PCIe 协议架构概述

PCIe 采用分层协议架构，从上到下分为：
- **事务层 (Transaction Layer)**: 处理 TLP (Transaction Layer Packet)
- **数据链路层 (Data Link Layer)**: 负责链路可靠性和完整性
- **物理层 (Physical Layer)**: 处理电气信号和编码

---

## 2. PCIe 初始化时序图

### 2.1 完整初始化流程

```mermaid
sequenceDiagram
    participant RC as Root Complex
    participant Link as PCIe Link
    participant EP as Endpoint

    Note over RC,EP: 阶段1: 系统上电与复位
    RC->>Link: Power On
    Link->>EP: PERST# (Fundamental Reset)
    EP->>EP: 内部初始化

    Note over RC,EP: 阶段2: 物理层链路训练
    RC->>Link: Detect.Presence
    Link->>RC: Presence Detected
    RC->>Link: Polling.Active (发送TS1有序集)
    EP->>Link: 响应TS1有序集
    RC->>Link: Polling.Compliance
    Link->>RC: Polling.Configuration
    RC->>Link: Configuration.Lanenum (发送TS2)
    EP->>Link: 确认链路编号
    RC->>Link: Configuration.Complete
    Link->>RC: L0 (链路激活)

    Note over RC,EP: 阶段3: 数据链路层初始化
    RC->>EP: DLLP (数据链路层包)
    EP->>RC: DLLP Ack
    RC->>EP: 初始化流控制 (InitFC)
    EP->>RC: 确认流控制参数

    Note over RC,EP: 阶段4: 配置空间枚举
    RC->>EP: Config Read (Bus=0, Device=0)
    EP->>RC: 返回Vendor ID/Device ID
    RC->>EP: Config Write (分配BAR地址)
    EP->>RC: 确认BAR配置
    RC->>EP: Memory/IO Enable
    EP->>RC: 设备就绪
```

### 2.2 链路训练状态机 (LTSSM)

```mermaid
stateDiagram-v2
    [*] --> Detect
    Detect --> Polling: 链路检测到设备
    Polling --> Configuration: 速率协商完成
    Configuration --> L0: 链路训练完成
    L0 --> L0s: ASPM L0s 进入
    L0 --> L1: ASPM L1 进入
    L0s --> L0: 唤醒
    L1 --> L0: 唤醒
    L0 --> Recovery: 链路错误
    Recovery --> L0: 恢复成功
    Recovery --> HotReset: 恢复失败
    HotReset --> Detect: 复位完成
    L0 --> Disabled: 链路禁用
    Disabled --> Detect: 重新启用
```

---

## 3. PCIe 数据读操作时序图

### 3.1 Memory Read 操作

```mermaid
sequenceDiagram
    participant Host as Host CPU
    participant RC as Root Complex
    participant EP as Endpoint
    participant Mem as Device Memory

    Note over Host,Mem: 发起读请求
    Host->>RC: CPU Read Request
    RC->>RC: 生成 MRd TLP
    RC->>EP: Memory Read TLP (请求者ID, 地址, 长度)

    Note over RC,Mem: 等待响应 (可能需要多个Completion)
    EP->>EP: 解析TLP, 准备数据
    EP->>Mem: 读取本地存储
    Mem->>EP: 返回数据
    EP->>EP: 封装 Cpl TLP
    EP->>RC: Completion TLP (数据块1)

    alt 数据量大需要多个完成包
        EP->>RC: Completion TLP (数据块2)
        EP->>RC: Completion TLP (数据块N)
    end

    RC->>RC: 合并数据, 更新流控
    RC->>Host: 返回数据到CPU

    Note over RC,EP: 隐式确认机制
    RC->>EP: Ack DLLP (确认收到)
    EP->>RC: 更新发送信用
```

### 3.2 Memory Read 详细时序参数

| 阶段 | 典型延迟 | 说明 |
|------|----------|------|
| TLP 封装 | 100-200ns | RC生成请求包 |
| 链路传输 | 1-10μs | 取决于链路宽度和速率 |
| EP 响应时间 | 变化大 | 取决于设备内部延迟 |
| Completion 返回 | 1-10μs | 链路传输时间 |

---

## 4. PCIe 数据写操作时序图

### 4.1 Memory Write 操作 (Posted)

```mermaid
sequenceDiagram
    participant Host as Host CPU
    participant RC as Root Complex
    participant EP as Endpoint
    participant Mem as Device Memory

    Note over Host,Mem: Posted Write (无需等待响应)
    Host->>RC: CPU Write Request
    RC->>RC: 生成 MWr TLP
    RC->>RC: 检查流控信用

    alt 信用足够
        RC->>EP: Memory Write TLP (地址, 数据)
        Note right of RC: Posted事务，不等待Completion
    else 信用不足
        RC->>RC: 等待信用更新
        EP->>RC: UpdateFC DLLP
        RC->>EP: Memory Write TLP
    end

    EP->>EP: 校验CRC
    EP->>Mem: 写入本地存储
    Mem->>EP: 写入完成
    EP->>RC: Ack DLLP (确认接收)

    Note over Host: CPU可立即继续执行
```

### 4.2 Memory Write 操作 (Non-Posted)

```mermaid
sequenceDiagram
    participant Host as Host CPU
    participant RC as Root Complex
    participant EP as Endpoint
    participant Mem as Device Memory

    Note over Host,Mem: Non-Posted Write (需要响应确认)
    Host->>RC: CPU Write Request (Non-Posted)
    RC->>RC: 生成 MWr TLP (Non-Posted)
    RC->>EP: Memory Write TLP (地址, 数据, Tag)

    EP->>EP: 校验CRC
    EP->>Mem: 写入本地存储
    Mem->>EP: 写入完成
    EP->>RC: Completion TLP (确认写入)

    RC->>Host: 通知CPU写入完成
    RC->>EP: Ack DLLP
```

---

## 5. PCIe 数据传输流程图

### 5.1 TLP 包结构与传输

```mermaid
flowchart TB
    subgraph 传输层
        A[应用数据] --> B[添加TLP Header]
        B --> C[可选: 添加Digest]
    end

    subgraph 数据链路层
        C --> D[添加Sequence Number]
        D --> E[添加LCRC]
    end

    subgraph 物理层
        E --> F[添加Start帧]
        F --> G[可选: 添加ECRC]
        G --> H[添加End帧]
        H --> I[符号编码 8b/10b或128b/130b]
        I --> J[加扰与串行化]
    end

    subgraph 链路传输
        J --> K[Lane分配]
        K --> L[差分信号发送]
    end
```

### 5.2 数据链路层确认机制

```mermaid
sequenceDiagram
    participant Sender as 发送方
    participant Receiver as 接收方

    Note over Sender,Receiver: 正常传输
    Sender->>Receiver: TLP (Seq=1)
    Receiver->>Receiver: 校验LCRC
    Receiver->>Sender: Ack DLLP (AckSeq=1)
    Sender->>Sender: 清除重传缓冲区

    Note over Sender,Receiver: 错误检测与重传
    Sender->>Receiver: TLP (Seq=2)
    Receiver->>Receiver: CRC校验失败
    Receiver->>Sender: Nak DLLP (NakSeq=2)
    Sender->>Sender: 从重传缓冲区恢复
    Sender->>Receiver: 重传 TLP (Seq=2)
    Receiver->>Receiver: 校验通过
    Receiver->>Sender: Ack DLLP (AckSeq=2)
```

---

## 6. PCIe 流控制时序图

```mermaid
sequenceDiagram
    participant TX as 发送方
    participant FC as 流控管理器
    participant RX as 接收方

    Note over TX,RX: 初始化阶段
    RX->>FC: 初始化信用值 (Header=10, Data=1024)
    FC->>TX: 发送 InitFC DLLP
    TX->>TX: 记录初始信用

    Note over TX,RX: 正常传输
    TX->>TX: 检查可用信用
    TX->>FC: 消费信用 (Header-1, Data-4)
    TX->>RX: 发送TLP

    Note over TX,RX: 信用补充
    RX->>RX: 处理TLP, 释放缓冲区
    RX->>FC: 更新可用信用
    FC->>TX: UpdateFC DLLP (新信用值)
    TX->>TX: 更新信用计数

    Note over TX,RX: 信用不足场景
    TX->>TX: 检查信用 (不足)
    TX->>TX: 等待UpdateFC
    FC->>TX: UpdateFC DLLP
    TX->>RX: 继续发送TLP
```

---

## 7. PCIe 电源管理与退出时序图

### 7.1 进入低功耗状态 (ASPM L1)

```mermaid
sequenceDiagram
    participant Host as Host
    participant RC as Root Complex
    participant EP as Endpoint

    Note over Host,EP: 检测空闲，准备进入L1
    RC->>RC: PM_L1.ENTRY

    RC->>EP: PM_Enter_L1 DLLP
    EP->>EP: 准备进入L1
    EP->>EP: 完成挂起事务
    EP->>RC: PM_Request_Ack DLLP

    RC->>EP: PM_L1.ENTRY 确认
    EP->>EP: 进入电气空闲
    RC->>RC: 进入电气空闲

    Note over RC,EP: L1 状态 (低功耗)
```

### 7.2 从低功耗状态唤醒

```mermaid
sequenceDiagram
    participant Host as Host
    participant RC as Root Complex
    participant EP as Endpoint

    Note over RC,EP: L1 状态
    Host->>RC: 唤醒请求
    RC->>RC: 退出电气空闲
    RC->>EP: 发送WAKE#信号

    EP->>EP: 退出电气空闲
    EP->>RC: TS1有序集
    RC->>EP: TS1有序集响应

    Note over RC,EP: 链路恢复
    RC->>EP: TS2有序集
    EP->>RC: TS2有序集确认

    RC->>RC: 进入L0状态
    EP->>EP: 进入L0状态
    Note over RC,EP: 链路激活，可传输数据
```

---

## 8. PCIe 正常退出流程

```mermaid
sequenceDiagram
    participant OS as 操作系统
    participant Driver as 驱动程序
    participant RC as Root Complex
    participant EP as Endpoint

    Note over OS,EP: 正常关机/设备移除流程
    OS->>Driver: 请求停止设备
    Driver->>EP: 禁用中断
    Driver->>EP: 完成挂起I/O

    Driver->>RC: 通知移除设备
    RC->>EP: 清除Memory/IO使能
    EP->>EP: 停止DMA传输
    EP->>RC: 确认停止

    RC->>EP: 发送链路禁用命令
    EP->>RC: 确认禁用

    Note over RC,EP: 链路断开
    RC->>EP: 电气空闲 (Electrical Idle)
    EP->>EP: 进入Disabled状态
    RC->>RC: 进入Disabled状态

    OS->>OS: 设备移除完成
```

---

## 9. PCIe 异常处理时序图

### 9.1 错误检测与恢复

```mermaid
sequenceDiagram
    participant EP as Endpoint
    participant Link as PCIe Link
    participant RC as Root Complex
    participant Driver as 驱动程序

    Note over EP,Driver: 错误检测
    EP->>Link: 发送TLP
    Link--xRC: 传输错误 (CRC失败)

    RC->>RC: 检测到错误
    RC->>EP: Nak DLLP (请求重传)

    alt 重传成功
        EP->>Link: 重传TLP
        Link->>RC: 正确接收
        RC->>EP: Ack DLLP
    else 重传失败超过阈值
        RC->>RC: 触发链路恢复
        RC->>EP: 进入Recovery状态
        RC->>EP: 重新训练链路
        EP->>RC: 链路恢复确认
        RC->>Driver: 报告恢复事件
    end
```

### 9.2 致命错误处理

```mermaid
flowchart TD
    A[检测到致命错误] --> B{错误类型}

    B -->|数据链路错误| C[触发链路复位]
    B -->|协议错误| D[报告AER]
    B -->|硬件故障| E[禁用设备]

    C --> F[Hot Reset]
    F --> G[重新训练链路]
    G --> H{恢复成功?}

    H -->|是| I[恢复正常操作]
    H -->|否| J[进入Disabled状态]

    D --> K[记录错误日志]
    K --> L[通知驱动程序]
    L --> M[驱动决定处理方式]

    E --> N[系统报告故障]
    N --> O[需要人工干预]
```

---

## 10. 完整的 PCIe 操作时序总结

| 操作类型 | Posted | 完成时间 | 可靠性机制 |
|----------|--------|----------|------------|
| Memory Read | 否 | 等待Completion | 重试 + Ack/Nak |
| Memory Write | 是/否 | Posted立即/Non-Posted等待 | LCRC + Ack/Nak |
| Config Read | 否 | 等待Completion | 超时机制 |
| Config Write | 否 | 等待Completion | 超时机制 |
| Message | 是 | 立即完成 | 无确认 |
| Interrupt | 是 | 立即完成 | MSI确认 |

---

## 11. 时序参数参考

### 11.1 链路训练时序

| 参数 | PCIe 3.0 | PCIe 4.0 | PCIe 5.0 |
|------|----------|----------|----------|
| 最小训练时间 | 20ms | 20ms | 20ms |
| 典型训练时间 | 50-100ms | 50-100ms | 50-100ms |
| Recovery时间 | 1-10ms | 1-10ms | 1-10ms |

### 11.2 数据传输时序

| 参数 | PCIe 3.0 x16 | PCIe 4.0 x16 | PCIe 5.0 x16 |
|------|--------------|--------------|--------------|
| 带宽 | ~16 GB/s | ~32 GB/s | ~63 GB/s |
| TLP延迟 | 200-500ns | 200-500ns | 200-500ns |
| 最大Payload | 4096字节 | 4096字节 | 4096字节 |

---
