相关文件
rocksdb/memtable/skiplist.h

// 模板类，按key插入的链表，需要比较器
template <typename Key, class Comparator>
class SkipList {
 private:
  struct Node;
  // Insert key into the list.
  // REQUIRES: nothing that compares equal to key is currently in the list.
  void Insert(const Key& key);

  // Returns true iff an entry that compares equal to key is in the list.
  bool Contains(const Key& key) const;

  // Return estimated number of entries smaller than `key`.
  uint64_t EstimateCount(const Key& key) const;
  
  // Iteration over the contents of a skip list
  class Iterator {
    // Change the underlying skiplist used for this iterator
    // This enables us not changing the iterator without deallocating
    // an old one and then allocating a new one
    void SetList(const SkipList* list);

    // Returns true iff the iterator is positioned at a valid node.
    bool Valid() const;

    // Returns the key at the current position.
    // REQUIRES: Valid()
    const Key& key() const;

    // Advances to the next position.
    // REQUIRES: Valid()
    void Next();

    // Advances to the previous position.
    // REQUIRES: Valid()
    void Prev();

    // Advance to the first entry with a key >= target
    void Seek(const Key& target);
    
  private:
    const SkipList* list_;
    Node* node_;
    // Intentionally copyable
  };
  
 private:
  const uint16_t kMaxHeight_;
  const uint16_t kBranching_;
  const uint32_t kScaledInverseBranching_;

  // Immutable after construction
  Comparator const compare_;
  Allocator* const allocator_;  // Allocator used for allocations of nodes

  Node* const head_;

  // Modified only by Insert().  Read racily by readers, but stale
  // values are ok.
  std::atomic<int> max_height_;  // Height of the entire list

  // Used for optimizing sequential insert patterns.  Tricky.  prev_[i] for
  // i up to max_height_ is the predecessor of prev_[0] and prev_height_
  // is the height of prev_[0].  prev_[0] can only be equal to head before
  // insertion, in which case max_height_ and prev_height_ are 1.
  Node** prev_;
  int32_t prev_height_;
};
