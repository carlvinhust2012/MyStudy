# LMCache KV Cache 存储模型分析

## 一、LMCache 概述

LMCache 是一个 LLM KV Cache 缓存系统，用于加速大语言模型推理。核心功能：
- 缓存已计算的 KV (Key-Value) 张量，避免重复计算
- 支持多级存储 (L1 CPU 内存 / L2 远程存储)
- 与 vLLM、SGLang 等推理框架深度集成

## 二、整体架构

```mermaid
flowchart TB
    subgraph LLM["LLM 推理引擎"]
        vLLM["vLLM"]
        SGLang["SGLang"]
    end

    subgraph LMCache["LMCache 层"]
        CE["LMCacheEngine"]
        SM["StorageManager"]
        L1["L1Manager CPU 内存缓存"]
        L2["L2 Adapters 远程存储"]
    end

    subgraph Storage["存储后端"]
        CPU["LocalCPUBackend 热缓存"]
        Remote["RemoteBackend Redis/S3/FS"]
        P2P["P2PBackend 点对点传输"]
    end

    vLLM --> CE
    SGLang --> CE
    CE --> SM
    SM --> L1
    SM --> L2
    L1 --> CPU
    L2 --> Remote
    L2 --> P2P
```

## 三、核心数据结构

### 1. CacheEngineKey

```python
@dataclass(slots=True)
class CacheEngineKey:
    model_name: str       # 模型名称
    world_size: int       # 分布式世界大小
    worker_id: int        # Worker ID
    chunk_hash: int       # Token chunk 的哈希值
    dtype: torch.dtype    # 数据类型
    request_configs: Optional[dict]  # 请求级配置
```

### 2. MemoryObj

```python
class MemoryObj:
    raw_data: Tensor          # 实际数据
    meta: MemoryObjMetadata   # 元数据
    ref_count: int            # 引用计数
    _pinned: bool             # 是否被 pin
```

### 3. MemoryFormat

```
KV_2LTD: [2, num_layers, num_tokens, hidden_dim]  # 完整 KV
KV_T2D:  [num_tokens, 2, hidden_dim]              # 层级 KV
KV_2TD:  [2, num_tokens, hidden_dim]              # 混合格式
```

## 四、存储层次结构

### 1. L1 缓存 (CPU 内存)

```mermaid
flowchart LR
    subgraph L1Manager["L1Manager"]
        Objects["_objects: Dict[ObjectKey, L1ObjectState]"]
        MM["L1MemoryManager"]
    end

    subgraph L1ObjectState["L1ObjectState"]
        MO["memory_obj: MemoryObj"]
        WL["write_lock: TTLLock"]
        RL["read_lock: TTLLock"]
        TMP["is_temporary: bool"]
    end

    Objects --> L1ObjectState
    L1ObjectState --> MO
    MM --> MO
```

### 2. L2 存储 (远程后端)

| 后端 | 说明 |
|------|------|
| RemoteBackend | Redis / S3 / 文件系统 |
| P2PBackend | 节点间直接传输 |
| PDBackend | Prefill-Decode 分离 |
| GDSBackend | GPU Direct Storage |

## 五、写入流程时序图

```mermaid
sequenceDiagram
    participant vLLM as vLLM Scheduler
    participant CE as LMCacheEngine
    participant SM as StorageManager
    participant L1 as L1Manager
    participant L2 as L2Adapter
    participant CPU as LocalCPUBackend
    participant Remote as RemoteBackend

    rect rgb(255, 245, 230)
        Note right of vLLM: Step 1: Prefill 完成，保存 KV
        vLLM->>CE: save_kv_layer(token_ids, kv_tensors)
        CE->>CE: 计算 chunk_hash
        CE->>CE: 生成 CacheEngineKey
    end

    rect rgb(240, 255, 230)
        Note right of CE: Step 2: 分配内存并写入
        CE->>SM: reserve_write(keys, layout_desc)
        SM->>L1: reserve_write(keys, layout_desc)
        L1->>L1: 分配 MemoryObj
        L1-->>SM: Dict[ObjectKey, MemoryObj]
        SM-->>CE: reserved objects
    end

    rect rgb(245, 240, 255)
        Note right of CE: Step 3: 写入数据
        CE->>CE: 将 KV 张量写入 MemoryObj
        CE->>SM: finish_write(keys)
        SM->>L1: finish_write(keys)
        L1->>L1: 释放 write_lock
    end

    rect rgb(255, 230, 245)
        Note right of L1: Step 4: 异步写入 L2
        L1->>L2: submit_put_task(keys, memory_objs)
        L2->>Remote: serialize + put(key, data)
        Remote->>Remote: 异步写入远程存储
    end
```

## 六、读取流程时序图

```mermaid
sequenceDiagram
    participant vLLM as vLLM Scheduler
    participant CE as LMCacheEngine
    participant SM as StorageManager
    participant L1 as L1Manager
    participant L2 as L2Adapter
    participant CPU as LocalCPUBackend
    participant Remote as RemoteBackend

    rect rgb(255, 245, 230)
        Note right of vLLM: Step 1: 新请求，查找缓存
        vLLM->>CE: lookup(token_ids)
        CE->>CE: 计算 chunk_hash 列表
        CE->>SM: submit_prefetch_task(keys)
    end

    rect rgb(240, 255, 230)
        Note right of SM: Step 2: L1 查找
        SM->>L1: reserve_read(keys)
        L1->>L1: 检查 _objects
        alt L1 命中
            L1->>L1: 增加 read_lock
            L1-->>SM: (SUCCESS, MemoryObj)
        else L1 未命中
            L1-->>SM: (KEY_NOT_EXIST, None)
        end
    end

    rect rgb(245, 240, 255)
        Note right of SM: Step 3: L2 预取未命中的 key
        SM->>L2: submit_prefetch_request(remaining_keys)
        L2->>Remote: batched_get_non_blocking(keys)
        Remote-->>L2: compressed_memory_objs
        L2->>L2: deserialize
        L2->>L1: 写入 L1 (write_lock -> read_lock)
    end

    rect rgb(255, 230, 245)
        Note right of vLLM: Step 4: 读取数据
        vLLM->>CE: retrieve(keys)
        CE->>SM: read_prefetched_results(keys)
        SM->>L1: unsafe_read(keys)
        L1-->>SM: MemoryObj 列表
        SM-->>CE: MemoryObj 列表
        CE->>CE: 加载到 GPU
        CE-->>vLLM: KV tensors
    end

    rect rgb(230, 255, 245)
        Note right of vLLM: Step 5: 释放读锁
        vLLM->>CE: finish_read(keys)
        CE->>SM: finish_read_prefetched(keys)
        SM->>L1: finish_read(keys)
        L1->>L1: 减少 read_lock
    end
```

## 七、淘汰策略

### 1. L1 淘汰

```python
class LRUCachePolicy:
    def get_evict_candidates(self, cache_dict, num_candidates=1):
        evict_keys = []
        for key, cache in cache_dict.items():
            if not cache.can_evict:  # 检查 ref_count 和 pin 状态
                continue
            evict_keys.append(key)
            if len(evict_keys) == num_candidates:
                break
        return evict_keys
```

### 2. L2 淘汰

```mermaid
flowchart TB
    subgraph L2Eviction["L2 淘汰控制器"]
        Monitor["监控使用量"]
        Check{"usage > max_capacity?"}
        Select["选择淘汰候选"]
        Delete["从 L2 删除"]
    end

    Monitor --> Check
    Check -->|是| Select
    Check -->|否| Monitor
    Select --> Delete
    Delete --> Monitor
```

## 八、状态机模型

### L1 对象状态转换

```mermaid
stateDiagram-v2
    [*] --> None: 初始状态

    None --> write_locked: reserve_write()
    write_locked --> ready: finish_write()
    write_locked --> None: write_lock 过期

    ready --> read_locked: reserve_read()
    read_locked --> ready: finish_read() count>0
    read_locked --> None: finish_read() count=0

    ready --> None: delete()

    ready --> write_locked: reserve_write() update mode
```

## 九、与 vLLM 集成

### vLLM V1 Adapter

```python
class LMCacheConnectorV1Impl(KVConnectorBase_V1):
    def start_load_kv(self, forward_context):
        """开始加载 KV 缓存"""
        
    def wait_for_layer_load(self, layer_name):
        """等待特定层加载完成"""
        
    def save_kv_layer(self, layer_name, kv_tuple):
        """保存 KV 层到缓存"""
        
    def get_num_new_matched_tokens(self, request):
        """返回匹配的 token 数量"""
```

### 请求处理流程

```mermaid
sequenceDiagram
    participant User as 用户
    participant vLLM as vLLM
    participant Adapter as LMCacheConnector
    participant CE as LMCacheEngine

    User->>vLLM: 发送请求 (prompt tokens)
    vLLM->>Adapter: get_num_new_matched_tokens()
    Adapter->>CE: lookup(token_ids)
    CE-->>Adapter: 命中的 chunk 数量
    Adapter-->>vLLM: matched_tokens
    
    alt 缓存命中
        vLLM->>Adapter: start_load_kv()
        Adapter->>CE: retrieve(keys)
        CE-->>Adapter: KV tensors
        Adapter->>Adapter: 加载到 GPU
        vLLM->>vLLM: 跳过已缓存的 prefill
    else 缓存未命中
        vLLM->>vLLM: 执行完整 prefill
    end

    vLLM->>Adapter: save_kv_layer()
    Adapter->>CE: store(keys, kv_tensors)
    vLLM-->>User: 返回响应
```

## 十、关键配置

| 配置项 | 说明 | 默认值 |
|--------|------|--------|
| max_local_cpu_size | L1 CPU 内存大小 (GB) | - |
| chunk_size | KV chunk 的 token 数 | 256 |
| cache_policy | 淘汰策略 | LRU |
| remote_url | 远程存储 URL | None |
| remote_serde | 序列化方式 | naive |
| enable_p2p | 启用 P2P 传输 | False |
| write_ttl_seconds | 写锁 TTL | 30 |
| read_ttl_seconds | 读锁 TTL | 30 |

## 十一、性能优化

### 1. 批量操作

```python
# 批量分配
def batched_allocate(shapes, dtypes, batch_size):
    memory_objs = allocator.batched_allocate(shapes, dtypes, batch_size)
    # 如果内存不足，触发淘汰
    while memory_objs is None:
        evict_keys = policy.get_evict_candidates(hot_cache)
        batched_remove(evict_keys)
        memory_objs = allocator.batched_allocate(...)
    return memory_objs
```

### 2. 异步写入

```python
# L2 写入异步执行，不阻塞推理
def submit_put_task(key, memory_obj):
    memory_obj.ref_count_up()
    compressed = serializer.serialize(memory_obj)
    future = asyncio.run_coroutine_threadsafe(
        connection.put(key, compressed), loop
    )
    future.add_done_callback(put_callback)
```

### 3. 预取机制

```python
# 提前预取，隐藏网络延迟
def submit_prefetch_task(keys, layout_desc):
    # L1 前缀匹配
    l1_result = l1_manager.reserve_read(keys)
    hit_count = count_hits(l1_result)
    
    # L2 异步预取剩余
    remaining_keys = keys[hit_count:]
    prefetch_controller.submit_prefetch_request(remaining_keys)
```

## 十二、总结

| 特性 | 说明 |
|------|------|
| **多级存储** | L1 (CPU 内存) + L2 (远程存储) |
| **缓存键** | model_name + worker_id + chunk_hash |
| **淘汰策略** | LRU / LFU / FIFO / MRU 可选 |
| **并发控制** | TTLLock 实现读写锁，带超时 |
| **集成方式** | vLLM KVConnector 接口 |
| **序列化** | 支持 naive / CacheGen / KIVI |
