Mooncake 软件架构（分析文档）
一句话概述
Mooncake 是一个以 KVCache 为中心的、面向大模型（LLM）推理的分布式/解耦（disaggregated）服务平台；核心目标是把 KV 缓存与大模型推理的 prefill/decoding 工作流分离，通过高性能的数据传输与分布式存储（Transfer Engine + Mooncake Store）来提升吞吐并满足延迟 SLO。