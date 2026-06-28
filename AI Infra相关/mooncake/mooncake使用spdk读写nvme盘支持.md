# 在 Mooncake 中启用 SPDK / NVMe-oF（NOF）支持 — 构建与运行配置指南\

本文目标  
- 在 Linux 节点上安装并配置 SPDK（及必要依赖）以支持裸 NVMe 设备的高性能 IO。  
- 编译 Mooncake，使其包含 NOF/SPDK 支持（启用相应编译选项）。  
- 配置运行时（驱动绑定、hugepages、权限、env）并给出验证与调优建议。  

重要前提（请确认）
- 你有对目标节点的 root 权限（或 sudo）。SPDK 典型需要特权（绑定驱动、hugepages、VFIO/DPDK 等）。  
- 节点上有可用的 NVMe 设备用于 SPDK（非被系统 fs 使用或已正确解绑）。  
- 编译环境：gcc/clang、cmake/ninja、meson/ninja（用于 SPDK）、python3、pkg-config 等已安装。  
- Mooncake 源码已可访问并你能构建它（本指南给出变动点）。

警告  
- 生产环境请先在单机或测试环境验证；SPDK 操作（绑驱动、VFIO）会让设备不可由普通内核驱动访问（可能影响系统）。  
- 操作需谨慎备份重要数据。

---

## 概览步骤
1. 安装 SPDK（源码构建）并验证例子。  
2. 绑定 NVMe 设备到 SPDK 支持的驱动（vfio-pci 或 uio_pci_generic / igb_uio 取决于配置）。  
3. 配置系统（hugepages、kernel params、users/groups、rlimit）。  
4. 在 Mooncake 源树中启用 NOF/SPDK 支持并编译（设置 CMake / build flags）。  
5. 运行 Mooncake 的 FileStorage / transfer engine，确认 SPDK 路径工作正常。  
6. 调优参数（chunk size / inflight limit / worker count / NUMA binding）。  

---

## 1) 安装 SPDK（推荐使用官方源码 & meson/ninja 构建）
（以 SPDK 23.x/24.x 风格为例；检查最新 SPDK 文档）

示例（Ubuntu 20.04+）：
```bash
# 安装依赖（示例）
sudo apt update
sudo apt install -y build-essential git python3 python3-pip meson ninja \
  libnuma-dev libpcap-dev libssl-dev libaio-dev

# 克隆 SPDK
git clone https://github.com/spdk/spdk.git
cd spdk
# 可选：切换到受支持的 tag，例如 v24.01
git checkout v24.01

# 初始化 & 构建（默认使用 DPDK 或 VFIO）
./configure   # 可传参数，例如 --enable-dpdk (视平台)
# 或使用 meson/ninja (新版本)
meson setup build
ninja -C build

# 安装 python 工具到虚拟环境（用于 spdk scripts）
python3 -m pip install meson ninja
```

注意：SPDK 的具体构建命令会随版本不同。参考 SPDK 官方 README，根据需要启用 DPDK、VFIO、或其它 transports。

---

## 2) 解除内核驱动并绑定到 VFIO/igb_uio（或 uio_pci_generic）
重要：解绑设备会导致该 NVMe 设备不可由系统使用，请确保设备上无重要数据。

示例（假设 NVMe 的 PCI 地址为 0000:81:00.0）：
```bash
# 查看 NVMe 设备
lsblk
lspci -nn | grep -i nvme

# 安装 vfio 模块（若未安装）
sudo modprobe vfio-pci

# 解绑设备的内核驱动（示例）
PCI=0000:81:00.0
echo $PCI | sudo tee /sys/bus/pci/devices/$PCI/driver/unbind

# 绑定到 vfio-pci（需要 vendor:device ids）
VENDOR=$(cat /sys/bus/pci/devices/$PCI/vendor)
DEVICE=$(cat /sys/bus/pci/devices/$PCI/device)
echo $VENDOR $DEVICE   # 用于确认

# 绑定（示例）
echo $VENDOR $DEVICE | sudo tee /sys/bus/pci/drivers/vfio-pci/new_id
echo $PCI | sudo tee /sys/bus/pci/drivers/vfio-pci/bind
```

替代：若使用 igb_uio（旧方案），需安装 DPDK 或编译 igb_uio 驱动并绑定。参考 SPDK 文档。

---

## 3) 配置 hugepages、sysctl、权限
SPDK 常用 hugepages/hugetlbfs 或 DPDK hugepages。

示例：
```bash
# 预留 hugepages（以 2MB 为例）
echo 2048 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

# 或使用 hugetlbfs mount
sudo mkdir -p /mnt/huge
sudo mount -t hugetlbfs nodev /mnt/huge

# 设置 ulimit（打开文件数）
ulimit -n 1048576

# 系统级别打开文件限制（/etc/security/limits.conf 或 systemd service）
# 编辑 /etc/security/limits.conf:
# username soft nofile 1048576
# username hard nofile 1048576
```

如果在 systemd 启动 mooncake 服务，记得在 unit 文件中设置 LimitNOFILE=1048576 和 PrivateDevices/CapabilityBoundingSet 等。

---

## 4) 在 Mooncake 中启用 NOF/SPDK 支持并编译
Mooncake 的代码包含条件编译分支（#ifdef USE_NOF），因此需在构建时开启对应宏并链接 SPDK wrapper 库。

大致步骤（假定 Mooncake 使用 CMake）：

1) 检查 Mooncake 的构建系统（repo 根目录），找到 CMakeLists.txt 或 build scripts。  
2) 给 CMake 传入 -DUSE_NOF=ON（或等价宏），以及 SPDK include/lib 路径。

示例（假设使用 cmake）：
```bash
cd /path/to/Mooncake
mkdir -p build && cd build

# 假设 SPDK 安装或源码在 /opt/spdk
export SPDK_DIR=/opt/spdk   # 或 spdk 源码目录
cmake .. -DUSE_NOF=ON -DSPDK_ROOT=$SPDK_DIR -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

如果 Mooncake 的构建不是 CMake，请查找 build 脚本中的宏定义位置（例如 Makefile 或 bazel），将 USE_NOF 定义加入编译器选项：-DUSE_NOF。

链接步骤要点：
- 确保编译器能找到 spdk headers（-I/path/to/spdk/include）和 spdk_wrapper 或 spdk 库（-L/path/to/spdk/build -lspdk_wrapper 或 spdk libraries）。  
- 根据代码提示，transfer_task.h 包含 `"spdk/spdk_wrapper.h"`，可能项目自带 spdk wrapper 或需要你将 SPDK 的 wrapper 放在 include 路径。

如果构建出错，检查：
- 未找到 spdk headers -> 指定 SPDK include 路径。
- 未链接 libspdk -> 添加相应 linker flags（SPDK 的构建可能生成 libspdk.a 或 libspdk.so，或要求使用 spdk userspace app linking model）。

---

## 5) 运行时配置（Mooncake / FileStorage）
- 在 FileStorageConfig 中选择合适的 backend，例如 offset_allocator_storage_backend 或 distributed_storage_backend，具体后端需支持 SPDK 写入路径。通常你需要把环境变量设置为使用 offload 后端：
```bash
export MOONCAKE_OFFLOAD_STORAGE_BACKEND_DESCRIPTOR=offset_allocator_storage_backend
# 或者 bucket_storage_backend 视实现而定
```

- 若需要使用 NOF/SPDK 后端，还需配置后端的设备/segment 参数（可能为 env 或 config file）。例如：
```bash
export MC_NOF_DEVICES="0000:81:00.0"        # PCI addresses to use for NOF
export MC_NOF_WORKERS=4                     # optional, worker count
```
（注：变量名示例，实际 Mooncake 配置名可能不同，请根据项目文档 / config keys 调整）

- 启动 Mooncake 服务或 FileStorage 时确保运行进程有权限（若使用 VFIO，需要 CAP_SYS_ADMIN 或以 root 运行，或使用 setcap 赋予必要能力）。

---

## 6) 验证 SPDK/NOF 路径是否正常工作
基本验证步骤：
1. 启动 Mooncake（或相关节点组件）并在日志中搜索 NOF/SPDK 相关初始化信息（TransferEngine init / SpdkNofWorkerPool 启动 / spdk wrapper 初始化消息）。  
2. 执行一个小的 offload 或 promotion 操作（FileStorage::OffloadObjects 或 PromotionAllocStart + PromotionWrite），观察 logs 是否有 SPDK NVMe write 提交与完成记录。  
3. 用 SPDK 自带示例（例如 spdk/examples/nvme/perf）测试设备带宽/延迟独立于 Mooncake。  
4. 在 Mooncake 中触发一个 write 操作，并观察 TransferFuture 返回与 FileStorage 报告（NotifyOffloadSuccess）是否最后上报给 Master。

示例检查命令：
```bash
# 查看 Mooncake 日志（systemd 或直接 stdout）
journalctl -u mooncake.service -f

# 或直接在进程日志中搜索 spdk/no f 字样
grep -i spdk build_logs.txt || true
```

---

## 7) 常见问题与排障
- 编译未找到 spdk headers / linker errors：确认 SPDK_DIR、include 与 lib 路径是否传给构建系统；检查 SPDK 版本与 API 兼容性。  
- 设备无法绑定到 vfio：确保 IOMMU 已启用（kernel 参数 intel_iommu=on 或 amd_iommu=on），检查 dmesg 错误。  
- 运行时权限错误（无法 open /dev/vfio/...）：确保进程有权限或使用 root。systemd unit 可配置 User=root 或添加 CapabilityBoundingSet。  
- SPDK App 启动失败：检查 hugepages、memlock limits、huge tlb mount、uio/igb_uio 模块是否正确加载。  
- IO 性能低：检查 NUMA、CPU pinning、transport（RDMA vs TCP）与 SPDK worker 绑定，调整 chunk size、inflight limit 与 worker 数量。

---

## 8) 调优建议（基于 transfer_task.h 的默认）
transfer_task.h 中的默认常量（可据此调整）：
- kDefaultSpdkNofSubmitChunkBytes = 128K（分片大小，适中；对大对象提高可并行度）
- kDefaultSpdkNofInflightBytesLimit = 32M（并发 IO 上限）
- kDefaultSpdkNofWorkers = 4（worker 数量）

建议：
- 若设备延迟低且带宽高，可增大 inflight limit（例如 128M）以提高并发度，但注意占用内存与设备资源。  
- 对 NUMA 多 socket 机器，在 SpdkNofWorkerPool 中启用按 NUMA 绑定（若实现支持），将 worker 绑定到靠近设备的 CPU。  
- chunk size 对小对象/大对象影响不同：小对象建议减小分片以减少单 IO 延迟；大对象使用较大 chunk 减少提交开销。  
- 监控 SPDK/IO 完成队列、CPU 占用与传输延迟。TransferEngineImpl 已有 metrics 支持（可启用 MC_TE_METRIC）。

---

## 9) 示例：从零到可测（简化脚本）
该脚本只是示意流程（请按实际 PCI/路径调整）：
```bash
# 1. Prepare system (example)
sudo modprobe vfio-pci
echo 2048 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

# 2. Build SPDK (assume cloned in /opt/spdk)
cd /opt/spdk
meson setup build
ninja -C build

# 3. Bind device
PCI=0000:81:00.0
sudo echo $PCI | sudo tee /sys/bus/pci/devices/$PCI/driver/unbind
VENDOR=$(cat /sys/bus/pci/devices/$PCI/vendor)
DEVICE=$(cat /sys/bus/pci/devices/$PCI/device)
echo $VENDOR $DEVICE | sudo tee /sys/bus/pci/drivers/vfio-pci/new_id
echo $PCI | sudo tee /sys/bus/pci/drivers/vfio-pci/bind

# 4. Build Mooncake with USE_NOF
cd /path/to/mooncake
mkdir -p build && cd build
cmake .. -DUSE_NOF=ON -DSPDK_ROOT=/opt/spdk
make -j$(nproc)

# 5. Start mooncake/FileStorage (run as root or with capabilities)
sudo ./bin/mooncake-store --config /etc/mooncake/config.yaml
# 然后触发 offload/promotion 并观察日志
```

---
