# brpc 收发包流程分析

## 目录

1. [概述](#1-概述)
2. [协议体系架构](#2-协议体系架构)
3. [IOBuf 零拷贝缓冲区](#3-iobuf-零拷贝缓冲区)
4. [发包流程（客户端发送）](#4-发包流程客户端发送)
5. [收包流程（服务端接收）](#5-收包流程服务端接收)
6. [响应发送流程（服务端回复）](#6-响应发送流程服务端回复)
7. [响应接收流程（客户端接收）](#7-响应接收流程客户端接收)
8. [Socket 写入机制](#8-socket-写入机制)
9. [Socket 读取与协议解析](#9-socket-读取与协议解析)
10. [InputMessenger 消息分发](#10-inputmessenger-消息分发)
11. [连接管理与复用](#11-连接管理与复用)
12. [流控与背压](#12-流控与背压)
13. [HTTP 协议收发包](#13-http-协议收发包)
14. [gRPC 协议收发包](#14-grpc-协议收发包)
15. [端到端完整时序](#15-端到端完整时序)
16. [对比总结](#16-对比总结)
17. [源码索引](#17-源码索引)

---

## 1. 概述

brpc 的收发包机制基于 **epoll + bthread 协程 + IOBuf 零拷贝** 构建：

```mermaid
graph TB
    subgraph "客户端"
        C_USR["用户代码<br/>stub.CallMethod()"]
        C_CNTL["Controller<br/>超时/重试/Tracing"]
        C_PROTO["Protocol 层<br/>PackRequest/PackResponse"]
        C_IOBUF["IOBuf<br/>零拷贝缓冲区"]
        C_SOCK["Socket<br/>TCP 连接"]
    end

    subgraph "网络"
        TCP["TCP/IP"]
    end

    subgraph "服务端"
        S_SOCK["Socket<br/>TCP 连接"]
        S_IOBUF["IOBuf<br/>接收缓冲区"]
        S_INMSG["InputMessenger<br/>协议分发"]
        S_PROTO["Protocol 层<br/>Parse/ProcessRequest"]
        S_USR["用户代码<br/>service.CallMethod()"]
    end

    C_USR --> C_CNTL --> C_PROTO --> C_IOBUF --> C_SOCK
    C_SOCK -->|TCP Write| TCP
    TCP -->|TCP Read| S_SOCK
    S_SOCK --> S_IOBUF --> S_INMSG --> S_PROTO --> S_USR
```

**核心设计原则**：

- **零拷贝序列化**：Protobuf 直接序列化到 IOBuf，无中间缓冲
- **协议无关传输**：Socket + IOBuf 不关心上层协议，协议解析由 InputMessenger 分发
- **协程化 I/O**：阻塞 write/read 通过 bthread yield 不阻塞 pthread
- **流式处理**：数据可以边接收边解析，不需要等完整报文

---

## 2. 协议体系架构

### 2.1 Protocol 接口

```c
// src/brpc/protocol.h
class Protocol {
public:
    // 协议名称 (如 "baidu_std", "http", "grpc")
    virtual const char* Name() const = 0;

    // 解析从 Socket 接收到的数据
    // 返回: ParseResult(MESSAGE)  → 完整消息
    //       ParseResult(NOT_ENOUGH) → 数据不完整，继续接收
    //       ParseResult(ERROR)     → 协议错误
    virtual ParseResult Parse(
        IOBuf* source, Socket* socket, bool read_eof,
        const void* arg) const = 0;

    // 序列化请求并打包到 IOBuf
    virtual int PackRequest(
        IOBuf* request_buf,
        Controller* cntl,
        const google::protobuf::Message* request) const = 0;

    // 序列化响应并打包到 IOBuf
    virtual int PackResponse(
        IOBuf* response_buf,
        Controller* cntl,
        const google::protobuf::Message* response) const = 0;

    // 处理已解析的请求（服务端）
    virtual void ProcessRequest(
        InputMessageBase* msg) const = 0;

    // 处理已解析的响应（客户端）
    virtual void ProcessResponse(
        InputMessageBase* msg) const = 0;

    // 验证请求合法性
    virtual bool VerifyRequest(
        const InputMessageBase* msg) const;

    // 支持的传输类型
    virtual bool SupportsTransport(TransportType t) const;

    // 支持的压缩类型
    virtual bool SupportsCompression(CompressType t) const;
};
```

### 2.2 协议注册

```c
// src/brpc/global.cpp
// 所有协议在启动时注册到全局列表
Protocol::RegisterProtocol(PROTOCOL_BAIDU_STD,   new BaiduStdProtocol);
Protocol::RegisterProtocol(PROTOCOL_HTTP,        new HttpProtocol);
Protocol::RegisterProtocol(PROTOCOL_HTTPS,       new HttpsProtocol);
Protocol::RegisterProtocol(PROTOCOL_H2,          new H2Protocol);
Protocol::RegisterProtocol(PROTOCOL_GRPC,        new GrpcProtocol);
Protocol::RegisterProtocol(PROTOCOL_THRIFT,      new ThriftProtocol);
Protocol::RegisterProtocol(PROTOCOL_MEMCACHE,    new MemcacheProtocol);
Protocol::RegisterProtocol(PROTOCOL_REDIS,       new RedisProtocol);
Protocol::RegisterProtocol(PROTOCOL_NOVA_PBRPC,  new NovaPbrpcProtocol);
Protocol::RegisterProtocol(PROTOCOL_PUBLIC_PBRPC, new PublicPbrpcProtocol);
Protocol::RegisterProtocol(PROTOCOL_DISCOVERY,   new DiscoveryProtocol);
Protocol::RegisterProtocol(PROTOCOL_ESP,         new EspProtocol);
// ...
```

### 2.3 协议识别流程

```mermaid
sequenceDiagram
    participant SOCK as Socket
    participant INMSG as InputMessenger
    participant P_LIST as 协议列表

    SOCK->>INMSG: OnNewMessages(socket, data)
    INMSG->>INMSG: 取前 N 字节特征

    loop 遍历已注册协议
        INMSG->>P_LIST: protocols[i]->Parse(ioBuf, socket)
        alt 返回 MESSAGE
            P_LIST-->>INMSG: ParseResult(msg, payload_size)
            INMSG->>INMSG: 创建对应 InputMessageBase 子类
        else 返回 NOT_ENOUGH
            P_LIST-->>INMSG: 继续尝试下一个协议
        else 返回 ERROR
            P_LIST-->>INMSG: 尝试下一个协议
        end
    end

    Note over INMSG: HTTP: 检查 GET/POST 前缀<br/>gRPC: 检查 content-type<br/>baidu_std: 检查 magic number
```

---

## 3. IOBuf 零拷贝缓冲区

### 3.1 IOBuf 结构

```c
// src/butil/iobuf.h
class IOBuf {
    // Block 链表结构
    struct BlockRef {
        Block*   offset;     // 指向 Block 的偏移位置
        size_t   length;     // 该段的长度
    };

    BlockRef* _refs;          // BlockRef 数组
    size_t    _ref_count;     // ref 数组大小
    size_t    _start;         // 第一个有效 ref 的索引
    size_t    _num_refs;      // 有效 ref 数量
};

// 每个 Block 是一块连续内存（默认 4KB 对齐）
struct Block {
    std::atomic<int> refcount; // 引用计数
    size_t   cap;              // Block 容量
    size_t   size;             // 已用大小
    size_t   abuf_size;        // abslify 大小
    char     data[0];          // 柔性数组：实际数据存储
};
```

### 3.2 IOBuf 内存布局

```
IOBuf
┌───────────────────────────────────────────────────────┐
│ BlockRef[0]  BlockRef[1]  BlockRef[2]                │
│ ┌──────────┐ ┌──────────┐ ┌──────────┐               │
│ │ offset───┼─→│ Block A  │ │ Block B  │ │ Block C  │  │
│ │ length=3KB│ │ 8KB cap  │ │ 8KB cap  │ │ 8KB cap  │  │
│ └──────────┘ │ [data...]│ │ [data...]│ │ [data...]│  │
│              └──────────┘ └──────────┘ └──────────┘  │
└───────────────────────────────────────────────────────┘

零拷贝: 数据存储在 Block 中，IOBuf 只持有 BlockRef 指针
追加数据: 在最后一个 Block 的剩余空间写入，或分配新 Block
切割数据: 只调整 BlockRef 的 offset/length，不移动数据
```

### 3.3 IOBuf 与 Protobuf 零拷贝

```c
// IOBufAsZeroCopyOutputStream - Protobuf 序列化直接写入 IOBuf
class IOBufAsZeroCopyOutputStream : public google::protobuf::io::ZeroCopyOutputStream {
    IOBuf* _buf;  // 目标 IOBuf

    bool Next(void** data, int* size) {
        // 返回 IOBuf 尾部的可写区域指针
        // Protobuf 直接写入该区域，无额外拷贝
        *data = _buf->append_space(*size);
        return true;
    }
};

// IOBufAsZeroCopyInputStream - Protobuf 反序列化直接从 IOBuf 读取
class IOBufAsZeroCopyInputStream : public google::protobuf::io::ZeroCopyInputStream {
    const IOBuf* _buf;  // 源 IOBuf

    bool Next(const void** data, int* size) {
        // 返回 IOBuf 当前 Block 的数据指针
        // Protobuf 直接从该区域读取，无额外拷贝
        const BlockRef& ref = _buf->_refs[_cur_ref];
        *data = ref.offset->data + ref.offset;
        *size = ref.length;
    }
};
```

---

## 4. 发包流程（客户端发送）

### 4.1 发包完整时序

```mermaid
sequenceDiagram
    participant USR as 用户代码
    participant STUB as Stub
    participant CNTL as Controller
    participant CH as Channel
    participant LB as LoadBalancer
    participant SOCK as Socket
    participant PROTO as Protocol
    participant IOBUF as IOBuf
    participant TCP as TCP 发送

    USR->>STUB: service.method(ctrl, request, response, done)
    STUB->>CNTL: CallMethod(method, ctrl, request, response, done)

    Note over CNTL: 1. 初始化请求状态
    CNTL->>CNTL: 设置 request_id, timeout_us, log_id

    Note over CNTL: 2. 选择目标服务器
    CNTL->>CH: channel.CallMethod(method, ctrl, request, response, done)
    CH->>LB: SelectServer(&server_id, &socket)
    LB-->>CH: 返回 Socket（从连接池）

    Note over CNTL: 3. 序列化请求
    CNTL->>PROTO: protocol->PackRequest(buf, ctrl, request)

    PROTO->>IOBUF: IOBufAsZeroCopyOutputStream
    Note over IOBUF: Protobuf 直接序列化到 IOBuf Block<br/>零拷贝

    PROTO->>IOBUF: 构造协议头
    Note over IOBUF: baidu_std: 12B header + body<br/>HTTP: "POST /service/method HTTP/1.1\r\n..."

    PROTO-->>CNTL: buf 已填充

    Note over CNTL: 4. 发送请求
    CNTL->>SOCK: socket->Write(buf, write_done_callback)
    Note over CNTL: 注册 timeout 定时器<br/>如果同步调用: butex_wait

    SOCK->>SOCK: StartWrite()
    SOCK->>SOCK: 将 buf 加入 Socket 写队列
    SOCK->>SOCK: 注册 EPOLLOUT
    SOCK->>SOCK: bthread_yield（让出 CPU）

    Note over SOCK: epoll_wait 返回 EPOLLOUT

    SOCK->>TCP: write(fd, iovec[], iovcnt)
    alt 全部发送完
        TCP-->>SOCK: nw == total_size
        SOCK->>SOCK: 写入完成
        SOCK->>CNTL: write_done_callback
    else 部分发送（EAGAIN）
        TCP-->>SOCK: nw < total_size
        SOCK->>SOCK: 更新未发送偏移
        SOCK->>SOCK: 继续等待 EPOLLOUT
    end
```

### 4.2 Channel::CallMethodImpl 详解

```c
// src/brpc/channel.cpp
void Channel::CallMethodImpl(
    const google::protobuf::MethodDescriptor* method,
    Controller* cntl,
    const google::protobuf::Message* request,
    google::protobuf::Message* response,
    google::protobuf::Closure* done) {

    // 1. 检查请求状态
    if (cntl->Failed()) { done->Run(); return; }

    // 2. 选择服务器（通过 LoadBalancer）
    SocketUniquePtr sock;
    if (_lb) {
        _lb->SelectServer(&cntl->_server_id, &sock);
    } else {
        // SingleServer 模式：直连
        sock = _server_socket;
    }

    // 3. 设置 Socket 和 Protocol 到 Controller
    cntl->_current_call.sending_sock = sock.get();
    cntl->_current_call.protocol = _options.protocol;

    // 4. 发起 RPC
    cntl->IssueRPC(method, request, response, done);
}
```

### 4.3 Controller::IssueRPC 详解

```c
// src/brpc/controller.cpp
void Controller::IssueRPC(
    const google::protobuf::MethodDescriptor* method,
    const google::protobuf::Message* request,
    google::protobuf::Message* response,
    google::protobuf::Closure* done) {

    // 1. 分配 request_id
    _request_id = butil::fmix32(butil::get_self_tid());

    // 2. 选择 Protocol
    Protocol* protocol = _current_call.protocol;
    if (!protocol) protocol = _options.protocol;

    // 3. 序列化 + 打包请求
    IOBuf buf;
    int rc = protocol->PackRequest(&buf, this, request);
    if (rc != 0) { HandleSendFailed(rc); return; }

    // 4. 记录请求元数据（用于重试、超时）
    _current_call.begin_time_us = butil::cpuwide_time_us();
    _current_call.real_timeout_ms = _timeout_ms;
    _current_call.need_feedback = _options.enable_feedback;

    // 5. 设置发送完成回调
    _on_rpc_end = done;

    // 6. 设置超时定时器
    if (_timeout_ms > 0) {
        bthread_timer_add(gettimeofday_us() + _timeout_ms * 1000L,
                          timer_callback, this);
    }

    // 7. 通过 Socket 发送
    Socket* s = _current_call.sending_sock;
    s->Write(buf, write_done_callback, this);

    // 8. 同步模式: 等待完成
    if (!_done) {
        butex_wait(_done_butex, 0, &_timeout_abs_time);
    }
}
```

---

## 5. 收包流程（服务端接收）

### 5.1 收包完整时序

```mermaid
sequenceDiagram
    participant EPOLL as epoll
    participant SOCK as Socket
    participant IOBUF as IOBuf
    participant INMSG as InputMessenger
    participant PROTO as Protocol
    as InputMsg as InputMessageBase
    participant ALLOC as 对象池分配
    participant PROC as Protocol::ProcessRequest
    participant BTH as bthread（处理协程）
    participant USR as Service::CallMethod

    EPOLL->>SOCK: EPOLLIN 事件就绪
    SOCK->>SOCK: StartInputEvent()<br/>bthread_start_urgent(on_input)

    SOCK->>SOCK: DoRead()

    loop 读取数据
        SOCK->>SOCK: readv(fd, iovec[])
        SOCK->>IOBUF: Append read data to IOBuf
    end

    SOCK->>INMSG: ProcessInputData(source_buf)

    Note over INMSG: 协议识别 + 解析循环

    INMSG->>PROTO: protocol->Parse(ioBuf, socket, eof)

    alt ParseResult(MESSAGE)
        PROTO-->>INMSG: 完整消息
        INMSG->>ALLOC: 从对象池创建 InputMessageBase 子类
        INMSG->>InputMsg: 填充: socket, io_buf, meta

        alt 请求类型（服务端）
            INMSG->>BTH: bthread_start_urgent(ProcessRequest, msg)
        else 响应类型（客户端）
            INMSG->>BTH: bthread_start_urgent(ProcessResponse, msg)
        end

        INMSG->>INMSG: 继续解析剩余数据
    else ParseResult(NOT_ENOUGH)
        PROTO-->>INMSG: 数据不完整
        INMSG->>INMSG: 保留在 IOBuf，等待更多数据
    else ParseResult(ERROR)
        PROTO-->>INMSG: 协议错误
        INMSG->>SOCK: SetFailed()
    end

    Note over BTH,USR: 新 bthread 中处理消息

    BTH->>PROC: msg->_process(msg)
    PROC->>PROC: ParsePbFromIOBuf(msg->_buf, request)
    PROC->>PROC: 验证请求
    PROC->>USR: service->CallMethod(cntl, request, response, done)

    USR->>USR: 执行用户业务逻辑
    USR->>PROC: 业务完成，填充 response
```

### 5.2 InputMessenger::ProcessInputData

```c
// src/brpc/input_messenger.cpp
void InputMessenger::OnNewMessages(Socket* socket) {
    IOBuf* source = socket->_input_buf;

    while (!source->empty()) {
        // 尝试用每个已注册协议解析
        for (size_t i = 0; i < _protocol_list.size(); ++i) {
            Protocol* p = _protocol_list[i];

            ParseResult result = p->Parse(source, socket, read_eof, arg);

            if (result.is_message()) {
                // 创建消息对象（从全局对象池分配，避免堆分配）
                InputMessageBase* msg = p->CreateMessage(socket, result);
                msg->_process = [p](InputMessageBase* m) {
                    p->ProcessRequest(m);
                };

                // 立即投递到当前 TaskGroup（urgent）
                bthread_start_urgent(msg->_tid, NULL,
                                    ProcessInputMessage, msg);
                break;
            } else if (result.is_not_enough()) {
                // 数据不完整，继续等待
                goto break_loop;
            } else if (result.is_error()) {
                socket->SetFailed(result.error_code());
                goto break_loop;
            }
        }
    }
    break_loop:
    // 保留未消费的数据在 source IOBuf 中
}
```

---

## 6. 响应发送流程（服务端回复）

```mermaid
sequenceDiagram
    participant USR as 用户业务
    participant DONE as Done 回调
    participant CNTL as Controller
    participant PROTO as Protocol
    participant IOBUF as IOBuf
    participant SOCK as Socket
    participant TCP as TCP 发送

    USR->>USR: 填充 response
    USR->>DONE: done->Run()

    DONE->>CNTL: OnRPCEnd(cntl, request, response)

    Note over CNTL: 1. 序列化响应
    CNTL->>PROTO: protocol->PackResponse(buf, cntl, response)
    PROTO->>IOBUF: IOBufAsZeroCopyOutputStream
    Note over IOBUF: Protobuf 零拷贝序列化

    PROTO->>IOBUF: 构造协议头（含 response header）

    Note over CNTL: 2. 发送响应
    CNTL->>SOCK: socket->Write(buf, response_done_cb)

    SOCK->>SOCK: StartWrite()
    SOCK->>SOCK: WriteQueue.add(buf)
    SOCK->>SOCK: EPOLLOUT 注册

    SOCK->>TCP: writev(fd, iovec[])
    TCP-->>SOCK: 写入完成
    SOCK->>CNTL: response_done_cb

    Note over CNTL: 3. 清理
    CNTL->>CNTL: 减少引用计数
    CNTL->>CNTL: 释放 request/response
```

---

## 7. 响应接收流程（客户端接收）

```mermaid
sequenceDiagram
    participant SOCK as Socket
    participant IOBUF as IOBuf
    participant INMSG as InputMessenger
    participant PROTO as Protocol
    as CNTL as Controller

    SOCK->>SOCK: EPOLLIN → OnNewMessages()
    SOCK->>IOBUF: readv() 追加数据

    SOCK->>INMSG: ProcessInputData(ioBuf)

    INMSG->>PROTO: protocol->Parse(ioBuf, socket)
    Note over PROTO: baidu_std: 匹配 request_id
    Note over PROTO: HTTP: 匹配 HTTP response status

    PROTO-->>INMSG: ParseResult(MESSAGE)
    INMSG->>INMSG: 创建 InputMessageBase（响应类型）

    INMSG->>INMSG: bthread_start_urgent(ProcessResponse, msg)

    Note over INMSG,CNTL: 响应处理 bthread

    INMSG->>PROTO: protocol->ProcessResponse(msg)

    PROTO->>PROTO: 解析 response header
    PROTO->>PROTO: ParsePbFromIOBuf(buf, response)
    PROTO->>CNTL: 设置 controller 状态

    alt 同步调用
        PROTO->>CNTL: butex_wake(cntl->_done_butex)
        Note over CNTL: 从 butex_wait 恢复
        CNTL-->>USR: 用户代码继续
    else 异步调用
        PROTO->>CNTL: cntl->_done->Run()
        Note over CNTL: 用户回调执行
    end
```

---

## 8. Socket 写入机制

### 8.1 Socket::Write 核心路径

```mermaid
sequenceDiagram
    participant CALL as 调用方
    participant SOCK as Socket
    participant WQ as WriteQueue
    participant FDL as fd_limit
    participant TCP as writev(fd)

    CALL->>SOCK: Write(IOBuf& buf, callback, arg)
    SOCK->>SOCK: 引用计数 +1

    alt Socket 已失败
        SOCK-->>CALL: 立即返回错误
    end

    SOCK->>WQ: 将 buf + callback 加入 WriteQueue

    alt 当前没有在途写入
        SOCK->>SOCK: StartWrite()
    else 有在途写入
        Note over SOCK: 追加到队列，等待当前写入完成
    end

    SOCK->>SOCK: KeepWrite()

    loop 直到数据发完或出错
        SOCK->>FDL: 检查 fd 写入限制
        SOCK->>SOCK: 构建 iovec[]（从 IOBuf BlockRef）
        SOCK->>TCP: writev(fd, iovec, iovcnt)

        alt nw > 0（发送成功）
            SOCK->>SOCK: 更新 IOBuf 偏移（消费已发送部分）
            SOCK->>WQ: 完成已发送的消息

            alt 还有数据
                SOCK->>SOCK: 继续下一轮 writev
            else 全部发完
                SOCK->>SOCK: 检查 WriteQueue 有无更多消息
                alt 有排队消息
                    Note over SOCK: 继续处理队列中的下一个
                else 队列空
                    SOCK->>SOCK: 注销 EPOLLOUT
                    SOCK->>SOCK: 写入完成
                end
            end
        else nw == 0（对端关闭）
            SOCK->>SOCK: SetFailed(ECONNRESET)
        else nw < 0
            alt errno == EAGAIN
                SOCK->>SOCK: 注册 EPOLLOUT
                SOCK->>SOCK: bthread_yield
                Note over SOCK: 等待 EPOLLOUT 后恢复
            else 其他错误
                SOCK->>SOCK: SetFailed(errno)
            end
        end
    end
```

### 8.2 IOBuf 到 iovec 的转换

```c
// Socket::DoWrite() 中:
int iovcnt = _write_buf.backing_block_num(&iovec[0], iovcnt_max);

// IOBuf::backing_block_num() 将 BlockRef 数组转换为 iovec 数组
// 每个 BlockRef 对应一个 iovec:
//   iov_base = block->data + ref_offset
//   iov_len  = ref_length
// 无需拷贝数据，直接传递 Block 指针给 writev
```

### 8.3 WriteQueue 消息合并

```
Socket WriteQueue:
┌──────────────────────────────────────┐
│ [Msg1: 4KB] [Msg2: 1KB] [Msg3: 8KB] │
└──────────────────────────────────────┘
       │
       ▼ writev() 一次性发送
┌──────────────────────────────────────┐
│ iovec[0] = Msg1.Block[0] (4KB)      │
│ iovec[1] = Msg1.Block[1] (2KB)      │
│ iovec[2] = Msg2.Block[0] (1KB)      │
│ iovec[3] = Msg3.Block[0] (4KB)      │
│ iovec[4] = Msg3.Block[1] (4KB)      │
└──────────────────────────────────────┘

优势: 多个消息的 writev 合并为一次系统调用
```

---

## 9. Socket 读取与协议解析

### 9.1 Socket 读取流程

```mermaid
sequenceDiagram
    participant EPOLL as epoll
    participant SOCK as Socket
    participant IOBUF as IOBuf（输入缓冲区）
    participant TCP as readv(fd)

    EPOLL->>SOCK: EPOLLIN
    SOCK->>SOCK: StartInputEvent()

    SOCK->>SOCK: DoRead()

    loop 读循环
        SOCK->>IOBUF: 预留空间 (append_space)
        SOCK->>TCP: readv(fd, iovec[], iovcnt)

        alt nr > 0（读到数据）
            SOCK->>IOBUF: 调整已用大小
            SOCK->>SOCK: _bytes_read += nr

            alt 超过水位线
                SOCK->>SOCK: 通知 InputMessenger
                SOCK->>IOBUF: ProcessInputData()
            end
        else nr == 0（对端关闭）
            SOCK->>SOCK: ProcessInputData(eof=true)
            SOCK->>SOCK: SetFailed()
        else nr < 0
            alt errno == EAGAIN
                Note over SOCK: 暂无更多数据
                SOCK->>SOCK: 停止读循环
            else 其他错误
                SOCK->>SOCK: SetFailed()
            end
        end
    end

    SOCK->>SOCK: 检查是否需要继续读
    alt IOBuf 中有未解析数据
        SOCK->>SOCK: 继续处理
    else 输入缓冲区为空
        Note over SOCK: 等待下一次 EPOLLIN
    end
```

---

## 10. InputMessenger 消息分发

### 10.1 InputMessenger 架构

```mermaid
graph TB
    subgraph "InputMessenger 全局实例"
        P_LIST["已注册协议列表<br/>_protocols[]"]
        F_LIST["Factory 列表<br/>_factories[]"]
    end

    subgraph "协议识别"
        PS_1["baidu_std<br/>magic: 0x53444252"]
        PS_2["HTTP<br/>GET/POST/HTTP"]
        PS_3["gRPC<br/>content-type"]
        PS_4["Thrift<br/>protocol id"]
    end

    subgraph "消息创建"
        MSG_1["MostCommonMessage<br/>(baidu_std)"]
        MSG_2["HttpMessage"]
        MSG_3["GrpcMessage"]
        MSG_4["ThriftMessage"]
    end

    P_LIST --> PS_1
    P_LIST --> PS_2
    P_LIST --> PS_3
    P_LIST --> PS_4

    PS_1 --> MSG_1
    PS_2 --> MSG_2
    PS_3 --> MSG_3
    PS_4 --> MSG_4
```

### 10.2 对象池分配

```c
// InputMessageBase 使用对象池避免频繁堆分配
// baidu_std 使用 MostCommonMessage（预分配大小固定）

// src/brpc/policy/most_common_message.h
class MostCommonMessage : public InputMessageBase {
    // 固定大小结构，可直接从对象池获取
    // 避免运行时多态的开销
};

// 对象池实现 (get_object()/return_object())
// 基于 per-thread 缓存 + 全局链表
```

---

## 11. 连接管理与复用

### 11.1 SocketMap 连接池

```mermaid
graph TB
    subgraph "Channel（客户端）"
        CH["Channel<br/>连接管理"]
        LB["LoadBalancer<br/>负载均衡"]
    end

    subgraph "SocketMap（全局连接池）"
        SM["SocketMap<br/>key=(ip, port, is_ssl)"]

        subgraph "连接1: server1:8080"
            S1["Socket fd=5<br/>refcnt=3"]
        end

        subgraph "连接2: server2:8080"
            S2["Socket fd=8<br/>refcnt=1"]
        end
    end

    subgraph "健康检查"
        HC["HealthCheckManager<br/>定期探测"]
        HCT["HealthCheckTask<br/>单连接探测"]
    end

    CH --> LB
    LB -->|"SelectServer"| SM
    SM --> S1
    SM --> S2
    HC --> HCT --> S1
    HC --> HCT --> S2
```

### 11.2 连接生命周期

```mermaid
stateDiagram-v2
    [*] --> CONNECTING: Socket::Create()
    CONNECTING --> CONNECTED: connect() 成功
    CONNECTED --> IDLE: 无在途 RPC
    IDLE --> CONNECTED: 新 RPC 发送
    CONNECTED --> FAILED: I/O 错误 / 超时
    FAILED --> CONNECTING: 自动重连
    CONNECTED --> CLOSED: Socket::SetFailed() + refcnt==0

    IDLE --> HC_CHECK: 健康检查超时
    HC_CHECK --> FAILED: 探测失败
    HC_CHECK --> IDLE: 探测成功
```

### 11.3 健康检查

```c
// src/brpc/details/health_check.cpp
class HealthCheckTask {
    void StartHealthCheck(Socket* socket) {
        // 定期发送探测请求
        // baidu_std: 发送空 RPC
        // HTTP: 发送 GET /status
        // 失败阈值: 3 次连续失败 → SetFailed
        // 成功: 重置失败计数
    }
};
```

---

## 12. 流控与背压

### 12.1 多级流控

```mermaid
graph TB
    subgraph "Level 1: Socket 写缓冲区"
        SWB["Socket WriteQueue<br/>限制未发送消息数"]
        SWL["_unwritten_bytes<br/>未写字节水位线"]
    end

    subgraph "Level 2: IOBuf 内存限制"
        IOM["IOBuf Block<br/>全局内存配额"]
        IOP["_process_input_data_watermark<br/>输入处理水位线"]
    end

    subgraph "Level 3: 连接级限制"
        CMQ["Max Connection<br/>最大连接数"]
        CRQ["Max Concurrent Request<br/>每连接最大并发"]
    end

    subgraph "Level 4: 服务端限流"
        CONC["ConcurrencyLimiter<br/>并发度限制"]
        QPS["QPSLimiter<br/>QPS 限制"]
    end

    SWB --> IOM
    IOM --> CMQ
    CMQ --> CONC
```

### 12.2 Socket 写缓冲区水位线

```c
// src/brpc/socket.h
class Socket {
    // 写缓冲区水位线
    size_t  _unwritten_bytes;           // 当前未写字节数
    size_t  _overcrowded_threshold;     // 拥挤阈值（默认 64MB）
    bool    _overcrowded;               // 是否拥挤

    bool IsCrowded() const {
        return _unwritten_bytes > _overcrowded_threshold;
    }

    // 调用方在发送前检查
    // 如果拥挤，负载均衡器会选择其他连接
};
```

### 12.3 服务端并发控制

```c
// ConcurrencyLimiter: 限制每个方法的并发请求数
class ConcurrencyLimiter {
    int _max_concurrency;        // 最大并发数
    int _current_concurrency;    // 当前并发数

    bool OnRequested() {
        if (++_current_concurrency <= _max_concurrency)
            return true;   // 允许
        --_current_concurrency;
        return false;      // 拒绝（ELIMIT）
    }

    void OnResponded() {
        --_current_concurrency;
    }
};
```

---

## 13. HTTP 协议收发包

### 13.1 HTTP 请求解析

```c
// src/brpc/policy/http_rpc_protocol.cpp
// HTTP 协议特征: GET/POST/PUT/DELETE/HEAD 前缀

ParseResult HttpProtocol::Parse(IOBuf* source, Socket* socket, ...) {
    // 1. 搜索 "\r\n\r\n"（头部结束标记）
    // 2. 解析请求行: "POST /EchoService/Echo HTTP/1.1"
    // 3. 解析头部: Content-Length, Content-Type 等
    // 4. 根据 Content-Length 判断 body 是否完整
    // 5. 切割 header 和 body 到独立 IOBuf
}
```

### 13.2 HTTP 收发时序

```mermaid
sequenceDiagram
    participant CNTL as Controller
    participant SOCK as Socket
    participant SRV as 服务端

    Note over CNTL: 发送 HTTP 请求
    CNTL->>SOCK: Write(IOBuf)
    Note over SOCK: "POST /EchoService/Echo HTTP/1.1\r\n<br/>Host: server:8080\r\n<br/>Content-Length: 128\r\n<br/>Content-Type: application/proto\r\n<br/>User-Agent: brpc/1.0\r\n<br/>\r\n<br/>[protobuf body]"

    SOCK->>SRV: TCP 传输

    Note over SRV: 接收 HTTP 请求
    SRV->>SRV: Parse: 搜索 \r\n\r\n 分割 header/body
    SRV->>SRV: 解析 URL → Service + Method
    SRV->>SRV: ParsePbFromIOBuf(body, request)
    SRV->>SRV: CallMethod()

    Note over SRV: 发送 HTTP 响应
    SRV->>SOCK: Write(IOBuf)
    Note over SOCK: "HTTP/1.1 200 OK\r\n<br/>Content-Type: application/proto\r\n<br/>Content-Length: 256\r\n<br/>\r\n<br/>[protobuf body]"

    SOCK->>CNTL: ProcessResponse
    CNTL->>CNTL: ParsePbFromIOBuf(body, response)
```

---

## 14. gRPC 协议收发包

### 14.1 gRPC Wire Format

```
gRPC Frame:
┌────────────────┬──────────────┬─────────────────────┐
│ Compressed (1B)│ Length (4B)  │  gRPC Message       │
│ 0=uncompressed │ big-endian   │  [protobuf encoded] │
└────────────────┴──────────────┴─────────────────────┘

HTTP/2 Frame:
┌──────────────┬─────────────┬──────────┬─────────────────────┐
│ Length (3B)  │ Type (1B)   │ Flags(1B)│ Stream ID (4B)     │
│              │ DATA = 0x00 │ END=0x01 │                     │
└──────────────┴─────────────┴──────────┴─────────────────────┘
│                     Payload                              │
│                     (gRPC Frame)                          │
└────────────────────────────────────────────────────────────┘
```

### 14.2 gRPC 收发时序

```mermaid
sequenceDiagram
    participant CNTL as Controller
    participant H2 as HTTP/2 Session
    participant SRV as 服务端

    Note over CNTL: gRPC 发送

    CNTL->>CNTL: 序列化 request → IOBuf
    CNTL->>CNTL: 构造 gRPC Frame<br/>[0x00][length][protobuf]
    CNTL->>H2: HPack 编码 header<br/>:method=POST, :path=/service/method,<br/>content-type=application/grpc+proto
    H2->>H2: 分片到 HTTP/2 DATA frames
    H2->>H2: stream_id 分配（客户端单数）

    H2->>SRV: HTTP/2 frames

    Note over SRV: gRPC 接收
    SRV->>SRV: HTTP/2 frame 解码
    SRV->>SRV: HPack 解码 header
    SRV->>SRV: gRPC Frame 解码
    SRV->>SRV: ParsePbFromIOBuf(message, request)
    SRV->>SRV: CallMethod()

    Note over SRV: gRPC 响应
    SRV->>H2: gRPC Frame + HTTP/2 frames
    H2->>CNTL: HPack 解码 + gRPC Frame 解码
    CNTL->>CNTL: ParsePbFromIOBuf(response)
```

---

## 15. 端到端完整时序

```mermaid
sequenceDiagram
    participant CLI_USR as 客户端用户
    participant CLI_CNT as Controller
    participant CLI_SOCK as Client Socket
    participant NET as TCP
    participant SVR_SOCK as Server Socket
    participant INMSG as InputMessenger
    participant SVR_BTH as 服务端 bthread
    participant SVR_USR as 服务端用户
    participant CLI_BTH as 客户端 bthread

    Note over CLI_USR,CLI_BTH: === 发送请求 ===

    CLI_USR->>CLI_CNT: stub.Echo(ctrl, req, res, done)
    CLI_CNT->>CLI_CNT: PackRequest(req) → IOBuf<br/>[baidu_std header + body]
    CLI_CNT->>CLI_SOCK: Write(IOBuf)
    CLI_SOCK->>NET: writev(fd)

    Note over CLI_USR,CLI_BTH: === 服务端接收 ===

    NET->>SVR_SOCK: 数据到达
    SVR_SOCK->>SVR_SOCK: readv(fd) → input_buf
    SVR_SOCK->>INMSG: ProcessInputData()
    INMSG->>INMSG: baidu_std Parse → MESSAGE
    INMSG->>SVR_BTH: bthread_start_urgent(ProcessRequest)

    SVR_BTH->>SVR_BTH: 解析 header: service/method/trace_id
    SVR_BTH->>SVR_BTH: ParsePbFromIOBuf(request)
    SVR_BTH->>SVR_USR: service->CallMethod(cntl, req, res, done)

    Note over CLI_USR,CLI_BTH: === 业务处理 ===

    SVR_USR->>SVR_USR: 执行业务逻辑
    SVR_USR->>SVR_BTH: 填充 response

    Note over CLI_USR,CLI_BTH: === 发送响应 ===

    SVR_BTH->>SVR_BTH: PackResponse(res) → IOBuf
    SVR_BTH->>SVR_SOCK: Write(IOBuf)
    SVR_SOCK->>NET: writev(fd)

    Note over CLI_USR,CLI_BTH: === 客户端接收响应 ===

    NET->>CLI_SOCK: 数据到达
    CLI_SOCK->>CLI_SOCK: readv(fd) → input_buf
    CLI_SOCK->>INMSG: ProcessInputData()
    INMSG->>INMSG: 匹配 request_id
    INMSG->>CLI_BTH: bthread_start_urgent(ProcessResponse)

    CLI_BTH->>CLI_BTH: 解析 response header
    CLI_BTH->>CLI_BTH: ParsePbFromIOBuf(response)
    CLI_BTH->>CLI_CNT: butex_wake(done_butex)
    CLI_CNT->>CLI_USR: 返回 response
```

---

## 16. 对比总结

### 16.1 brpc vs gRPC vs Thrift 网络层对比

| 特性 | brpc (baidu_std) | gRPC | Thrift (TBinaryProtocol) |
|---|---|---|---|
| 传输协议 | TCP（自定义 header） | HTTP/2 | TCP（自定义 framing） |
| 序列化 | Protobuf（零拷贝 IOBuf） | Protobuf（零拷贝） | Thrift IDL |
| 多路复用 | request_id 匹配 | HTTP/2 Stream | 连接级串行 |
| 头部大小 | 12B（紧凑） | ~20-200B（HPack） | ~10B（frame header） |
| 流控 | Socket 写水位线 + 并发限制 | HTTP/2 flow control | 无内置 |
| 超时 | per-RPC 定时器 + bthread_usleep | per-RPC deadline | per-call timeout |
| 重试 | 自动重试 + backup request | 无内置 | 无内置 |
| 连接管理 | SocketMap + 健康检查 | gRPC channel | 连接池 |

### 16.2 发包/收包路径对比

| 阶段 | brpc 发包 | brpc 收包 |
|---|---|---|
| 入口 | Controller::IssueRPC | InputMessenger::OnNewMessages |
| 序列化 | IOBufAsZeroCopyOutputStream | IOBufAsZeroCopyInputStream |
| 协议头 | PackRequest/PackResponse | Parse（增量解析） |
| I/O | Socket::Write → writev | Socket::DoRead → readv |
| 阻塞处理 | EAGAIN → EPOLLOUT + yield | EAGAIN → 等待 EPOLLIN |
| 回调 | write_done_callback | bthread_start_urgent |
| 协程 | bthread 执行 | bthread 执行 |

---

## 17. 源码索引

### 核心网络层

| 文件 | 内容 |
|---|---|
| `src/brpc/socket.h` | Socket 类（连接抽象、读写接口） |
| `src/brpc/socket.cpp` | Write、Read、StartWrite、KeepWrite、DoWrite、DoRead |
| `src/brpc/socket_inl.h` | 内联辅助函数 |
| `src/brpc/transport.h` | Transport 抽象基类 |
| `src/brpc/tcp_transport.h/.cpp` | TCP Transport 实现 |

### 协议层

| 文件 | 内容 |
|---|---|
| `src/brpc/protocol.h` | Protocol 接口、ParseResult、ProtocolType 枚举 |
| `src/brpc/protocol.cpp` | 协议注册、ParsePbFromIOBuf |
| `src/brpc/policy/baidu_rpc_protocol.h/.cpp` | baidu_std 协议实现 |
| `src/brpc/policy/most_common_message.h` | MostCommonMessage 消息对象 |
| `src/brpc/policy/http_rpc_protocol.h/.cpp` | HTTP 协议 |
| `src/brpc/policy/http2_rpc_protocol.h/.cpp` | HTTP/2 协议 |
| `src/brpc/policy/grpc.cpp` | gRPC 协议 |
| `src/brpc/policy/thrift_protocol.h/.cpp` | Thrift 协议 |
| `src/brpc/global.cpp` | 全局协议注册 |

### 消息处理

| 文件 | 内容 |
|---|---|
| `src/brpc/input_messenger.h` | InputMessenger 类 |
| `src/brpc/input_messenger.cpp` | ProcessInputData、OnNewMessages、CutInputMessage |
| `src/brpc/input_message_base.h` | InputMessageBase 基类 |
| `src/brpc/parse_result.h` | ParseResult 定义 |
| `src/brpc/socket_message.h` | SocketMessage 基类、AppendAndDestroySelf |

### 客户端

| 文件 | 内容 |
|---|---|
| `src/brpc/channel.h` | Channel 类 |
| `src/brpc/channel.cpp` | CallMethodImpl、InitSocketOptions |
| `src/brpc/controller.h` | Controller 类 |
| `src/brpc/controller.cpp` | IssueRPC、HandleSendFailed、HandleRecvFailed |
| `src/brpc/load_balancer.h/.cpp` | LoadBalancer 接口 |
| `src/brpc/socket_map.h/.cpp` | SocketMap 连接池 |

### IOBuf

| 文件 | 内容 |
|---|---|
| `src/butil/iobuf.h` | IOBuf、IOPortal、IOBufAsZeroCopyOutputStream/InputStream |
| `src/butil/iobuf.cpp` | Block 分配、内存池、append/cut 操作 |
| `src/butil/iobuf_inl.h` | 内联优化 |

### 服务端

| 文件 | 内容 |
|---|---|
| `src/brpc/server.h` | Server 类 |
| `src/brpc/server.cpp` | ProcessRequest、Service 注册 |
| `src/brpc/acceptor.h/.cpp` | Acceptor 连接接收 |
| `src/brpc/details/health_check.h/.cpp` | 健康检查 |

### 流控

| 文件 | 内容 |
|---|---|
| `src/brpc/adaptive_max_concurrency.h/.cpp` | 自适应并发限制 |
| `src/brpc/concurrency_limiter.h/.cpp` | 固定并发限制 |
| `src/brpc/qps_limiter.h/.cpp` | QPS 限制 |
