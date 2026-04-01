#include <iostream>
#include <vector>
#include <cstddef>
#include <cstdlib>
#include <new>

class MemoryPool {
private:
    struct Block {
        Block* next;
    };

    Block* freeList;   // 自由链表的头指针
    size_t blockSize;  // 每个内存块的大小
    size_t poolSize;   // 内存池的总大小
    char* pool;        // 内存池的起始地址

    void initializePool() {
        freeList = reinterpret_cast<Block*>(pool);
        for (size_t i = 0; i < poolSize - blockSize; i += blockSize) {
            freeList->next = reinterpret_cast<Block*>(pool + i + blockSize);
            freeList = freeList->next;
        }
        freeList->next = nullptr;
        freeList = reinterpret_cast<Block*>(pool);
    }

public:
    MemoryPool(size_t blockSize, size_t poolSize)
        : blockSize(blockSize), poolSize(poolSize) {
        pool = new char[poolSize];
        initializePool();
    }

    ~MemoryPool() {
        delete[] pool;
    }

    void* allocate() {
        if (freeList == nullptr) {
            throw std::bad_alloc();
        }
        Block* block = freeList;
        freeList = freeList->next;
        return block;
    }

    void deallocate(void* ptr) {
        Block* block = reinterpret_cast<Block*>(ptr);
        block->next = freeList;
        freeList = block;
    }
};

int main() {
    // 创建一个内存池，每个内存块大小为 64 字节，总大小为 1024 字节
    MemoryPool pool(64, 1024);

    // 分配内存块
    void* block1 = pool.allocate();
    void* block2 = pool.allocate();

    // 使用内存块（例如填充数据）
    memset(block1, 0, 64);
    memset(block2, 0, 64);

    // 释放内存块
    pool.deallocate(block1);
    pool.deallocate(block2);

    return 0;
}