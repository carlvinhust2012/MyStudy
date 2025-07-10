// centos 上安装libaio需要的依赖
sudo yum install libaio-devel

// ubuntu or Debian上安装依赖
sudo apt-get update
sudo apt-get install libaio-dev

#include <libaio.h>
#include <fcntl.h>
#include <unistd.h>

// 使用 io_setup 函数初始化异步 I/O 上下文
io_context_t ctx;
// 创建一个支持 10 个并发 I/O 操作的上下文
int ret = io_setup(10, &ctx);
if (ret < 0) {
    perror("io_setup failed");
    exit(EXIT_FAILURE);
}


struct iocb cb;
struct io_event event;
char buffer[1024];

// 打开文件
int fd = open("file.txt", O_RDONLY);
if (fd < 0) {
    perror("open failed");
    exit(EXIT_FAILURE);
}

// 初始化 iocb 结构
io_prep_pread(&cb, fd, buffer, sizeof(buffer), 0);

// 使用 io_submit 函数提交异步 I/O 请求
if (io_submit(ctx, 1, &cb) < 0) {
    perror("io_submit failed");
    exit(EXIT_FAILURE);
}


struct io_event events[10];
int num_events;

// 使用 io_getevents 函数等待并获取完成的 I/O 事件
// 等待至少一个 I/O 事件完成
num_events = io_getevents(ctx, 1, 10, events, NULL);
if (num_events < 0) {
    perror("io_getevents failed");
    exit(EXIT_FAILURE);
}

// 处理完成的 I/O 事件
for (int i = 0; i < num_events; i++) {
    // 检查状态并处理数据
}

// 在所有 I/O 操作完成后，销毁异步 I/O 上下文
io_destroy(ctx);