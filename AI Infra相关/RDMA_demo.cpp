#include <infiniband/verbs.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <cstring>

#define CHECK_IBV(call) \
do { \
    int ret = call; \
    if (ret) { \
        std::cerr << "IBV error at " << __FILE__ << ":" << __LINE__ << ": " \
                  << strerror(ret) << std::endl; \
        exit(EXIT_FAILURE); \
    } \
} while(0)

class InfinibandRDMA {
private:
    struct ibv_context* context;
    struct ibv_pd* protection_domain;
    struct ibv_cq* completion_queue;
    struct ibv_qp* queue_pair;
    struct ibv_mr* memory_region;
    
    char* buffer;
    size_t buffer_size;
    
    uint32_t remote_qp_num;
    uint32_t remote_psn;
    union ibv_gid remote_gid;

public:
    InfinibandRDMA(size_t buf_size = 4096) 
        : context(nullptr), protection_domain(nullptr), completion_queue(nullptr),
          queue_pair(nullptr), memory_region(nullptr), buffer(nullptr), 
          buffer_size(buf_size), remote_qp_num(0), remote_psn(0) {
        memset(&remote_gid, 0, sizeof(remote_gid));
    }
    
    ~InfinibandRDMA() {
        cleanup();
    }
    
    // 1. 初始化Infiniband设备
    bool initialize() {
        std::cout << "Initializing Infiniband RDMA..." << std::endl;
        
        // 获取设备列表
        int num_devices;
        struct ibv_device** device_list = ibv_get_device_list(&num_devices);
        if (num_devices == 0) {
            std::cerr << "No Infiniband devices found" << std::endl;
            return false;
        }
        
        std::cout << "Found " << num_devices << " Infiniband device(s)" << std::endl;
        
        // 打开第一个设备
        context = ibv_open_device(device_list[0]);
        if (!context) {
            std::cerr << "Failed to open device" << std::endl;
            ibv_free_device_list(device_list);
            return false;
        }
        
        std::cout << "Opened device: " << ibv_get_device_name(device_list[0]) << std::endl;
        
        // 创建保护域
        protection_domain = ibv_alloc_pd(context);
        if (!protection_domain) {
            std::cerr << "Failed to allocate protection domain" << std::endl;
            return false;
        }
        
        // 创建完成队列
        completion_queue = ibv_create_cq(context, 10, nullptr, nullptr, 0);
        if (!completion_queue) {
            std::cerr << "Failed to create completion queue" << std::endl;
            return false;
        }
        
        // 分配内存缓冲区
        buffer = new char[buffer_size];
        if (!buffer) {
            std::cerr << "Failed to allocate buffer" << std::endl;
            return false;
        }
        
        // 注册内存区域
        memory_region = ibv_reg_mr(protection_domain, buffer, buffer_size, 
                                  IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | 
                                  IBV_ACCESS_REMOTE_WRITE);
        if (!memory_region) {
            std::cerr << "Failed to register memory region" << std::endl;
            return false;
        }
        
        // 配置队列对属性
        struct ibv_qp_init_attr qp_init_attr;
        memset(&qp_init_attr, 0, sizeof(qp_init_attr));
        qp_init_attr.qp_type = IBV_QPT_RC;
        qp_init_attr.sq_sig_all = 1;
        qp_init_attr.send_cq = completion_queue;
        qp_init_attr.recv_cq = completion_queue;
        qp_init_attr.cap.max_send_wr = 10;
        qp_init_attr.cap.max_recv_wr = 10;
        qp_init_attr.cap.max_send_sge = 1;
        qp_init_attr.cap.max_recv_sge = 1;
        
        // 创建队列对
        queue_pair = ibv_create_qp(protection_domain, &qp_init_attr);
        if (!queue_pair) {
            std::cerr << "Failed to create queue pair" << std::endl;
            return false;
        }
        
        ibv_free_device_list(device_list);
        
        std::cout << "Infiniband RDMA initialized successfully" << std::endl;
        std::cout << "Local buffer: " << static_cast<void*>(buffer) 
                  << " (size: " << buffer_size << " bytes)" << std::endl;
        std::cout << "Memory region LKEY: " << memory_region->lkey 
                  << ", RKEY: " << memory_region->rkey << std::endl;
        
        return true;
    }
    
    // 2. 建立QP连接（服务端）
    bool setup_server(const char* ip, int port) {
        std::cout << "Setting up server on " << ip << ":" << port << std::endl;
        
        // 初始化QP到RESET状态
        transition_qp_state(IBV_QPS_RESET);
        
        // 转换到INIT状态
        struct ibv_qp_attr init_attr;
        memset(&init_attr, 0, sizeof(init_attr));
        init_attr.qp_state = IBV_QPS_INIT;
        init_attr.pkey_index = 0;
        init_attr.port_num = 1;
        init_attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
        
        CHECK_IBV(ibv_modify_qp(queue_pair, &init_attr, 
                               IBV_QP_STATE | IBV_QP_PKEY_INDEX | 
                               IBV_QP_PORT | IBV_QP_ACCESS_FLAGS));
        
        // 监听连接
        int listen_sock = create_listen_socket(ip, port);
        if (listen_sock < 0) {
            return false;
        }
        
        std::cout << "Waiting for client connection..." << std::endl;
        
        int client_sock = accept(listen_sock, nullptr, nullptr);
        if (client_sock < 0) {
            std::cerr << "Failed to accept connection" << std::endl;
            close(listen_sock);
            return false;
        }
        
        // 交换QP信息
        exchange_qp_info(client_sock, true);
        
        close(listen_sock);
        close(client_sock);
        
        return transition_to_rts();
    }
    
    // 3. 建立QP连接（客户端）
    bool setup_client(const char* server_ip, int port) {
        std::cout << "Connecting to server " << server_ip << ":" << port << std::endl;
        
        // 初始化QP到RESET状态
        transition_qp_state(IBV_QPS_RESET);
        
        // 转换到INIT状态
        struct ibv_qp_attr init_attr;
        memset(&init_attr, 0, sizeof(init_attr));
        init_attr.qp_state = IBV_QPS_INIT;
        init_attr.pkey_index = 0;
        init_attr.port_num = 1;
        init_attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
        
        CHECK_IBV(ibv_modify_qp(queue_pair, &init_attr, 
                               IBV_QP_STATE | IBV_QP_PKEY_INDEX | 
                               IBV_QP_PORT | IBV_QP_ACCESS_FLAGS));
        
        // 连接到服务器
        int sock = connect_to_server(server_ip, port);
        if (sock < 0) {
            return false;
        }
        
        // 交换QP信息
        exchange_qp_info(sock, false);
        
        close(sock);
        
        return transition_to_rts();
    }
    
    // 4. RDMA写操作
    bool rdma_write(uint64_t remote_addr, uint32_t rkey, const void* data, size_t size) {
        if (size > buffer_size) {
            std::cerr << "Data size exceeds buffer" << std::endl;
            return false;
        }
        
        // 复制数据到本地缓冲区
        memcpy(buffer, data, size);
        
        struct ibv_sge sge;
        sge.addr = reinterpret_cast<uint64_t>(buffer);
        sge.length = size;
        sge.lkey = memory_region->lkey;
        
        struct ibv_send_wr wr;
        memset(&wr, 0, sizeof(wr));
        wr.wr_id = 1;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.opcode = IBV_WR_RDMA_WRITE;
        wr.send_flags = IBV_SEND_SIGNALED;
        
        struct ibv_send_wr* bad_wr = nullptr;
        
        wr.wr.rdma.remote_addr = remote_addr;
        wr.wr.rdma.rkey = rkey;
        
        if (ibv_post_send(queue_pair, &wr, &bad_wr)) {
            std::cerr << "Failed to post RDMA write" << std::endl;
            return false;
        }
        
        // 等待完成
        return wait_for_completion();
    }
    
    // 5. RDMA读操作
    bool rdma_read(uint64_t remote_addr, uint32_t rkey, void* result, size_t size) {
        if (size > buffer_size) {
            std::cerr << "Read size exceeds buffer" << std::endl;
            return false;
        }
        
        struct ibv_sge sge;
        sge.addr = reinterpret_cast<uint64_t>(buffer);
        sge.length = size;
        sge.lkey = memory_region->lkey;
        
        struct ibv_send_wr wr;
        memset(&wr, 0, sizeof(wr));
        wr.wr_id = 2;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.opcode = IBV_WR_RDMA_READ;
        wr.send_flags = IBV_SEND_SIGNALED;
        
        struct ibv_send_wr* bad_wr = nullptr;
        
        wr.wr.rdma.remote_addr = remote_addr;
        wr.wr.rdma.rkey = rkey;
        
        if (ibv_post_send(queue_pair, &wr, &bad_wr)) {
            std::cerr << "Failed to post RDMA read" << std::endl;
            return false;
        }
        
        // 等待完成
        if (!wait_for_completion()) {
            return false;
        }
        
        // 复制结果
        memcpy(result, buffer, size);
        return true;
    }
    
    // 6. 发送和接收操作（传统消息）
    bool send_message(const void* data, size_t size) {
        if (size > buffer_size) {
            std::cerr << "Message size exceeds buffer" << std::endl;
            return false;
        }
        
        memcpy(buffer, data, size);
        
        struct ibv_sge sge;
        sge.addr = reinterpret_cast<uint64_t>(buffer);
        sge.length = size;
        sge.lkey = memory_region->lkey;
        
        struct ibv_send_wr wr;
        memset(&wr, 0, sizeof(wr));
        wr.wr_id = 3;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.opcode = IBV_WR_SEND;
        wr.send_flags = IBV_SEND_SIGNALED;
        
        struct ibv_send_wr* bad_wr = nullptr;
        
        if (ibv_post_send(queue_pair, &wr, &bad_wr)) {
            std::cerr << "Failed to post send" << std::endl;
            return false;
        }
        
        return wait_for_completion();
    }
    
    bool receive_message(void* result, size_t size) {
        // 预投递接收请求
        struct ibv_sge sge;
        sge.addr = reinterpret_cast<uint64_t>(buffer);
        sge.length = buffer_size;
        sge.lkey = memory_region->lkey;
        
        struct ibv_recv_wr wr;
        memset(&wr, 0, sizeof(wr));
        wr.wr_id = 4;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        
        struct ibv_recv_wr* bad_wr = nullptr;
        
        if (ibv_post_recv(queue_pair, &wr, &bad_wr)) {
            std::cerr << "Failed to post receive" << std::endl;
            return false;
        }
        
        // 等待完成（这里简化处理）
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        memcpy(result, buffer, std::min(size, buffer_size));
        return true;
    }
    
    // 获取本地缓冲区信息（供远程访问）
    uint64_t get_buffer_addr() const { 
        return reinterpret_cast<uint64_t>(buffer); 
    }
    
    uint32_t get_rkey() const { 
        return memory_region->rkey; 
    }
    
private:
    void transition_qp_state(enum ibv_qp_state target_state) {
        struct ibv_qp_attr attr;
        memset(&attr, 0, sizeof(attr));
        attr.qp_state = target_state;
        
        CHECK_IBV(ibv_modify_qp(queue_pair, &attr, IBV_QP_STATE));
    }
    
    bool transition_to_rts() {
        // 转换到RTR状态
        struct ibv_qp_attr rtr_attr;
        memset(&rtr_attr, 0, sizeof(rtr_attr));
        rtr_attr.qp_state = IBV_QPS_RTR;
        rtr_attr.path_mtu = IBV_MTU_1024;
        rtr_attr.dest_qp_num = remote_qp_num;
        rtr_attr.rq_psn = remote_psn;
        rtr_attr.max_dest_rd_atomic = 1;
        rtr_attr.min_rnr_timer = 12;
        
        // 设置GID（如果使用RoCE）
        rtr_attr.ah_attr.is_global = 1;
        rtr_attr.ah_attr.grh.dgid = remote_gid;
        rtr_attr.ah_attr.grh.sgid_index = 0;
        rtr_attr.ah_attr.grh.hop_limit = 1;
        rtr_attr.ah_attr.port_num = 1;
        
        CHECK_IBV(ibv_modify_qp(queue_pair, &rtr_attr, 
                               IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | 
                               IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | 
                               IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER));
        
        // 转换到RTS状态
        struct ibv_qp_attr rts_attr;
        memset(&rts_attr, 0, sizeof(rts_attr));
        rts_attr.qp_state = IBV_QPS_RTS;
        rts_attr.timeout = 14;
        rts_attr.retry_cnt = 7;
        rts_attr.rnr_retry = 7;
        rts_attr.sq_psn = remote_psn;
        rts_attr.max_rd_atomic = 1;
        
        CHECK_IBV(ibv_modify_qp(queue_pair, &rts_attr, 
                               IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | 
                               IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC));
        
        std::cout << "QP transitioned to RTS state successfully" << std::endl;
        return true;
    }
    
    int create_listen_socket(const char* ip, int port) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            std::cerr << "Failed to create socket" << std::endl;
            return -1;
        }
        
        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        
        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, ip, &addr.sin_addr);
        
        if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "Failed to bind socket" << std::endl;
            close(sock);
            return -1;
        }
        
        if (listen(sock, 1) < 0) {
            std::cerr << "Failed to listen on socket" << std::endl;
            close(sock);
            return -1;
        }
        
        return sock;
    }
    
    int connect_to_server(const char* server_ip, int port) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            std::cerr << "Failed to create socket" << std::endl;
            return -1;
        }
        
        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, server_ip, &addr.sin_addr);
        
        if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "Failed to connect to server" << std::endl;
            close(sock);
            return -1;
        }
        
        return sock;
    }
    
    void exchange_qp_info(int sock, bool is_server) {
        // 发送本地QP信息
        struct {
            uint32_t qp_num;
            uint32_t psn;
            union ibv_gid gid;
        } local_info;
        
        local_info.qp_num = queue_pair->qp_num;
        local_info.psn = 1000; // 初始PSN
        memset(&local_info.gid, 0, sizeof(local_info.gid));
        
        send(sock, &local_info, sizeof(local_info), 0);
        
        // 接收远程QP信息
        recv(sock, &remote_qp_num, sizeof(remote_qp_num), 0);
        recv(sock, &remote_psn, sizeof(remote_psn), 0);
        recv(sock, &remote_gid, sizeof(remote_gid), 0);
        
        std::cout << (is_server ? "Server" : "Client") << " QP info exchanged:" << std::endl;
        std::cout << "  Local QPN: " << local_info.qp_num << ", PSN: " << local_info.psn << std::endl;
        std::cout << "  Remote QPN: " << remote_qp_num << ", PSN: " << remote_psn << std::endl;
    }
    
    bool wait_for_completion() {
        struct ibv_wc wc;
        int ret;
        
        do {
            ret = ibv_poll_cq(completion_queue, 1, &wc);
        } while (ret == 0);
        
        if (ret < 0) {
            std::cerr << "Failed to poll completion queue" << std::endl;
            return false;
        }
        
        if (wc.status != IBV_WC_SUCCESS) {
            std::cerr << "Work completion error: " << ibv_wc_status_str(wc.status) << std::endl;
            return false;
        }
        
        return true;
    }
    
    void cleanup() {
        if (queue_pair) {
            ibv_destroy_qp(queue_pair);
            queue_pair = nullptr;
        }
        if (memory_region) {
            ibv_dereg_mr(memory_region);
            memory_region = nullptr;
        }
        if (completion_queue) {
            ibv_destroy_cq(completion_queue);
            completion_queue = nullptr;
        }
        if (protection_domain) {
            ibv_dealloc_pd(protection_domain);
            protection_domain = nullptr;
        }
        if (context) {
            ibv_close_device(context);
            context = nullptr;
        }
        if (buffer) {
            delete[] buffer;
            buffer = nullptr;
        }
    }
};

// 演示函数
void server_demo() {
    std::cout << "=== Infiniband RDMA Server Demo ===" << std::endl;
    
    InfinibandRDMA rdma(8192); // 8KB缓冲区
    
    if (!rdma.initialize()) {
        return;
    }
    
    if (!rdma.setup_server("127.0.0.1", 8888)) {
        return;
    }
    
    // 等待客户端数据
    char received_data[256];
    if (rdma.receive_message(received_data, sizeof(received_data))) {
        std::cout << "Server received: " << received_data << std::endl;
    }
    
    // 发送响应
    const char* response = "Hello from Server via RDMA!";
    if (rdma.send_message(response, strlen(response) + 1)) {
        std::cout << "Server sent response" << std::endl;
    }
    
    std::cout << "Server demo completed" << std::endl;
}

void client_demo() {
    std::cout << "=== Infiniband RDMA Client Demo ===" << std::endl;
    
    InfinibandRDMA rdma(8192); // 8KB缓冲区
    
    if (!rdma.initialize()) {
        return;
    }
    
    std::this_thread::sleep_for(std::chrono::seconds(1)); // 等待服务器启动
    
    if (!rdma.setup_client("127.0.0.1", 8888)) {
        return;
    }
    
    // 发送数据到服务器
    const char* message = "Hello from Client via RDMA!";
    if (rdma.send_message(message, strlen(message) + 1)) {
        std::cout << "Client sent message" << std::endl;
    }
    
    // 接收响应
    char response[256];
    if (rdma.receive_message(response, sizeof(response))) {
        std::cout << "Client received: " << response << std::endl;
    }
    
    std::cout << "Client demo completed" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cout << "Usage: " << argv[0] << " [server|client]" << std::endl;
        return 1;
    }
    
    if (strcmp(argv[1], "server") == 0) {
        server_demo();
    } else if (strcmp(argv[1], "client") == 0) {
        client_demo();
    } else {
        std::cout << "Invalid argument. Use 'server' or 'client'" << std::endl;
        return 1;
    }
    
    return 0;
}