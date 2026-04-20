#include <iostream>
#include <thread>
#include <mutex>
#include <deque>
#include <vector>
#include <chrono>
#include <condition_variable>

class ThreadPool {
public:
    typedef struct {
        void *(*func)(void *arg);
        void *arg;
        int task_id;
    } Task;
    
    void work_loop() {
        for (;;) {
            Task *task = dequeue_task();
            if (task == nullptr) {
                return;
            }
            task->func(task->arg);
        }
    }

public:
    ThreadPool(int thread_num, int queue_size) 
        : max_thread_num(thread_num),
          max_queue_size(queue_size),
          is_stop(false) {
              for (int i= 0; i < max_thread_num; i++) {
                  work_threads.emplace_back([this](){ work_loop();});
              }
    }
    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            is_stop = true;
        }
        cv.notify_all();
        for (auto &th : work_threads) {
            th.join();
        }
    }
    
    void enqueue_task(Task *task) {
        std::unique_lock<std::mutex> lock(_mutex);
        cv_full.wait(lock, [this]() { return work_queue.size() < max_queue_size || is_stop; });
        if (is_stop == true) {
            return;
        }
        task->task_id++;
        work_queue.push_back(task);
        cv.notify_one();
    }
    
    Task* dequeue_task() {
        std::unique_lock<std::mutex> lock(_mutex);
        cv.wait(lock, [this]() { return !work_queue.empty() || is_stop; });
        
        if (work_queue.empty() == true) {
            return nullptr;
        }
        
        Task* task = work_queue.front();
        work_queue.pop_front();
        cv_full.notify_one();
        return task;
    }
 
private:
    int max_thread_num;
    int max_queue_size;
    bool is_stop;
    std::vector<std::thread> work_threads;
    std::deque<Task*> work_queue;
    std::mutex _mutex;
    std::condition_variable cv;
    std::condition_variable cv_full;
};

void *test_task(void *arg) {
    int num = *(int *)arg;
    std::cout << "the num is " << num << std::endl;
    return nullptr;
}

int main() {
    ThreadPool threadpool(2, 10);
    
    int a = 1, b = 2;
    ThreadPool::Task task1{test_task, &a, 0};
    ThreadPool::Task task2{test_task, &b, 0};
    threadpool.enqueue_task(&task1);
    threadpool.enqueue_task(&task2);
    
    std::this_thread::sleep_for(std::chrono::seconds(1));
    return 0;
}