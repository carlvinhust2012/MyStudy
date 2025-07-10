相关文件：
braft/src/braft/raft.h
braft/src/braft/raft.cpp
braft/src/braft/node.h
braft/src/braft/node.cpp

// raft::Node写日志
void Node::apply(const Task& task) {
    _impl->apply(task);
}

void NodeImpl::apply(const Task& task) {
    LogEntry* entry = new LogEntry;
    entry->AddRef();
    entry->data.swap(*task.data); // 将task中带来的用户数据，一般是操作日志，交换到entry->data中
    LogEntryAndClosure m;
    m.entry = entry;
    m.done = task.done;
    m.expected_term = task.expected_term;
    // 将LogEntryAndClosure m放到bthread::ExecutionQueue中
    if (_apply_queue->execute(m, &bthread::TASK_OPTIONS_INPLACE, NULL) != 0) {
        task.done->status().set_error(EPERM, "Node is down");
        entry->Release();
        return run_closure_in_bthread(task.done);
    }
}

// Node初始化时，会给node创建bthread 任务执行队列，execute_applying_tasks作为队列任务执行函数
int NodeImpl::init(const NodeOptions& options) {
    if (bthread::execution_queue_start(&_apply_queue_id, NULL,
                                       execute_applying_tasks, this) != 0) {
        LOG(ERROR) << "node " << _group_id << ":" << _server_id 
                   << " fail to start execution_queue";
        return -1;
    }
}

// task最终会来到这里处理
int NodeImpl::execute_applying_tasks(
        void* meta, bthread::TaskIterator<LogEntryAndClosure>& iter) {
    if (iter.is_queue_stopped()) {
        return 0;
    }
    // TODO: the batch size should limited by both task size and the total log
    // size
    const size_t batch_size = FLAGS_raft_apply_batch;
    // 定义一个tasks数组
    DEFINE_SMALL_ARRAY(LogEntryAndClosure, tasks, batch_size, 256);
    size_t cur_size = 0;
    NodeImpl* m = (NodeImpl*)meta;
    for (; iter; ++iter) {
        if (cur_size == batch_size) { // 如果满足了预定的个数，则直接执行下一步
            m->apply(tasks, cur_size);
            cur_size = 0;
        }
        tasks[cur_size++] = *iter; // 继续装填tasks数组
    }
    if (cur_size > 0) {
        m->apply(tasks, cur_size);
    }
    return 0;
}

void NodeImpl::apply(LogEntryAndClosure tasks[], size_t size) {
    // 前面省略一段有效性检查代码， task都是批量处理，size个分1组
    for (size_t i = 0; i < size; ++i) {
        // task中的日志任期不能是 -1 也不能是 _current_term ？？？
        if (tasks[i].expected_term != -1 && tasks[i].expected_term != _current_term) {
            BRAFT_VLOG << "node " << _group_id << ":" << _server_id
                      << " can't apply taks whose expected_term=" << tasks[i].expected_term
                      << " doesn't match current_term=" << _current_term;
            if (tasks[i].done) {
                tasks[i].done->status().set_error(
                        EPERM, "expected_term=%" PRId64 " doesn't match current_term=%" PRId64,
                        tasks[i].expected_term, _current_term);
                run_closure_in_bthread(tasks[i].done);
            }
            tasks[i].entry->Release();
            continue;
        }
        // 重新组装entries
        entries.push_back(tasks[i].entry);
        entries.back()->id.term = _current_term;
        entries.back()->type = ENTRY_TYPE_DATA;
        // 加入投票箱的pending队列？？？
        _ballot_box->append_pending_task(_conf.conf,
                                         _conf.stable() ? NULL : &_conf.old_conf,
                                         tasks[i].done);
    }
    _log_manager->append_entries(&entries,
                               new LeaderStableClosure(
                                        NodeId(_group_id, _server_id),
                                        entries.size(),
                                        _ballot_box));
    // update _conf.first
    _log_manager->check_and_set_configuration(&_conf);
}

// 写完日志之后，会提交投票结果，超过半数则认为成功，状态机更新状态
void LeaderStableClosure::Run() {
    if (status().ok()) {
        if (_ballot_box) {
            // ballot_box check quorum ok, will call fsm_caller
            _ballot_box->commit_at(
                    _first_log_index, _first_log_index + _nentries - 1, _node_id.peer_id);
        }
    }
}

// node如果是leader，开始写日志
void LogManager::append_entries(
            std::vector<LogEntry*> *entries, StableClosure* done) {
    // 前面省略有效性检查
    for (size_t i = 0; i < entries->size(); ++i) {
        // Add ref for disk_thread
        (*entries)[i]->AddRef();
        // 若日志项是配置，则加入配置管理
        if ((*entries)[i]->type == ENTRY_TYPE_CONFIGURATION) {
            ConfigurationEntry conf_entry(*((*entries)[i]));
            _config_manager->add(conf_entry);
        }
    }
    
    // 将日志项插入到内存结构中
    if (!entries->empty()) {
        done->_first_log_index = entries->front()->id.index;
        _logs_in_memory.insert(_logs_in_memory.end(), entries->begin(), entries->end());
    }

    done->_entries.swap(*entries);
    int ret = bthread::execution_queue_execute(_disk_queue, done); // 将done加入到execution_queue
    CHECK_EQ(0, ret) << "execq execute failed, ret: " << ret << " err: " << berror();
    wakeup_all_waiter(lck);    
}

int LogManager::start_disk_thread() {
    bthread::ExecutionQueueOptions queue_options;
    queue_options.bthread_attr = BTHREAD_ATTR_NORMAL;
    return bthread::execution_queue_start(&_disk_queue,
                                   &queue_options,
                                   disk_thread, // LogManager上执行队列的任务函数
                                   this);
}

int LogManager::disk_thread(void* meta,
                            bthread::TaskIterator<StableClosure*>& iter) {
    // 先构建出 AppendBatcher           
    LogManager* log_manager = static_cast<LogManager*>(meta);
    // FIXME(chenzhangyi01): it's buggy
    LogId last_id = log_manager->_disk_id;
    StableClosure* storage[256];
    AppendBatcher ab(storage, ARRAY_SIZE(storage), &last_id, log_manager);
    // 再从任务队列中批量获取
    for (; iter; ++iter) {
        StableClosure* done = *iter;
        if (!done->_entries.empty()) {
            ab.append(done); // 将这一批日志项都加入ab
        } else { // 若都加入完成，则开始刷盘？
            ab.flush(); // 此处刷盘？？？
            int ret = 0;
            do {} while(0);
            done->Run(); // 此处执行LeaderStableClosure::Run()
        }
    }
    
    // 刷盘 + 设定log_id 
    ab.flush();
    log_manager->set_disk_id(last_id);    
}

class AppendBatcher {
    void flush() {
        if (_size > 0) {
            IOMetric metric;
            // 此处直接刷盘？？
            _lm->append_to_storage(&_to_append, _last_id, &metric);
            g_storage_flush_batch_counter << _size;
            for (size_t i = 0; i < _size; ++i) {
                _storage[i]->_entries.clear();
                if (_lm->_has_error.load(butil::memory_order_relaxed)) {
                    _storage[i]->status().set_error(
                            EIO, "Corrupted LogStorage");
                }
                _storage[i]->update_metric(&metric);
                _storage[i]->Run();
            }
            _to_append.clear();
        }
        _size = 0;
        _buffer_size = 0;
    }
    
    void append(LogManager::StableClosure* done) {
        // 容量满了或者达到设定阈值，也要刷盘
        if (_size == _cap || 
                _buffer_size >= (size_t)FLAGS_raft_max_append_buffer_size) {
            flush();
        }
        _storage[_size++] = done;
        // 往_to_append加入日志项
        _to_append.insert(_to_append.end(), 
                         done->_entries.begin(), done->_entries.end());
        for (size_t i = 0; i < done->_entries.size(); ++i) {
            _buffer_size += done->_entries[i]->data.length();
        }
    }
 }；
 
 
 void LogManager::append_to_storage(std::vector<LogEntry*>* to_append, 
                                   LogId* last_id, IOMetric* metric) {
     if (!_has_error.load(butil::memory_order_relaxed)) {
        size_t written_size = 0;
        for (size_t i = 0; i < to_append->size(); ++i) {
            written_size += (*to_append)[i]->data.size();
        }
        butil::Timer timer;
        timer.start();
        g_storage_append_entries_concurrency << 1;
        // 此处做落盘处理
        int nappent = _log_storage->append_entries(*to_append, metric);
        g_storage_append_entries_concurrency << -1;
        timer.stop();
        // 后续省略结果判断+资源释放代码
     }                       
 }

// 写入内存介质？？
int MemoryLogStorage::append_entries(const std::vector<LogEntry*>& entries, 
                                     IOMetric* metric) {
    if (entries.empty()) {
        return 0;
    }
    for (size_t i = 0; i < entries.size(); i++) {
        LogEntry* entry = entries[i];
        append_entry(entry);
    }
    return entries.size();
}

 int MemoryLogStorage::append_entry(const LogEntry* input_entry) {
    std::unique_lock<raft_mutex_t> lck(_mutex);
    if (input_entry->id.index !=
            _last_log_index.load(butil::memory_order_relaxed) + 1) {
        CHECK(false) << "input_entry index=" << input_entry->id.index
                  << " _last_log_index=" << _last_log_index
                  << " _first_log_index=" << _first_log_index;
        return ERANGE;
    }
    input_entry->AddRef();
    _log_entry_data.push_back(const_cast<LogEntry*>(input_entry));
    _last_log_index.fetch_add(1, butil::memory_order_relaxed);
    lck.unlock();
    return 0;
}
// 或者是写入这种 Segment？？
int SegmentLogStorage::append_entries(const std::vector<LogEntry*>& entries, IOMetric* metric) {
    // 前面省略部分代码
    for (size_t i = 0; i < entries.size(); i++) {
        now = butil::cpuwide_time_us();
        LogEntry* entry = entries[i];
        
        scoped_refptr<Segment> segment = open_segment();
        if (FLAGS_raft_trace_append_entry_latency && metric) {
            delta_time_us = butil::cpuwide_time_us() - now;
            metric->open_segment_time_us += delta_time_us;
            g_open_segment_latency << delta_time_us;
        }
        if (NULL == segment) {
            return i;
        }
        int ret = segment->append(entry); // 此处追加写
        if (0 != ret) {
            return i;
        }
        if (entry->type == ENTRY_TYPE_CONFIGURATION) {
            has_conf = true;
        }

        _last_log_index.fetch_add(1, butil::memory_order_release);
        last_segment = segment;
    }
    now = butil::cpuwide_time_us();
    last_segment->sync(_enable_sync, has_conf); // 此处做sync
}
