相关文件：
rocksdb/util/hash.h
rocksdb/util/hash.cc

rocksdb/util/dynamic_bloom.h
rocksdb/util/dynamic_bloom.cc

// 存在于内存，支持并发访问
class DynamicBloom {
 public:
    explicit DynamicBloom(Allocator* allocator, uint32_t total_bits,
                        uint32_t num_probes = 6, size_t huge_page_tlb_size = 0,
                        Logger* logger = nullptr);
    ~DynamicBloom() {}

  // Assuming single threaded access to this function.
  void Add(const Slice& key);

  // Like Add, but may be called concurrent with other functions.
  void AddConcurrently(const Slice& key);

  // Assuming single threaded access to this function.
  void AddHash(uint32_t hash);

  // Like AddHash, but may be called concurrent with other functions.
  void AddHashConcurrently(uint32_t hash);

  // Multithreaded access to this function is OK
  bool MayContain(const Slice& key) const;

  void MayContain(int num_keys, Slice* keys, bool* may_match) const;

  // Multithreaded access to this function is OK
  bool MayContainHash(uint32_t hash) const;

  void Prefetch(uint32_t h);

 private:
  // Length of the structure, in 64-bit words. For this structure, "word"
  // will always refer to 64-bit words.
  uint32_t kLen;
  
  // We make the k probes in pairs, two for each 64-bit read/write. Thus,
  // this stores k/2, the number of words to double-probe.
  const uint32_t kNumDoubleProbes;

  std::atomic<uint64_t>* data_; // 保存bit标记的地方

  // or_func(ptr, mask) should effect *ptr |= mask with the appropriate
  // concurrency safety, working with bytes.
  template <typename OrFunc>
  void AddHash(uint32_t hash, const OrFunc& or_func);

  bool DoubleProbe(uint32_t h32, size_t a) const;
};

// TODO: consider rename to LegacyBloomHash32
inline uint32_t BloomHash(const Slice& key) {
  return Hash(key.data(), key.size(), 0xbc9f1d34);
}

// 添加key，实际是计算这个key的BloomHash
inline void DynamicBloom::Add(const Slice& key) { AddHash(BloomHash(key)); }

inline void DynamicBloom::AddConcurrently(const Slice& key) {
  AddHashConcurrently(BloomHash(key));
}

inline void DynamicBloom::AddHash(uint32_t hash) {
  AddHash(hash, [](std::atomic<uint64_t>* ptr, uint64_t mask) {
    ptr->store(ptr->load(std::memory_order_relaxed) | mask,
               std::memory_order_relaxed);
  });
}

inline void DynamicBloom::AddHashConcurrently(uint32_t hash) {
  AddHash(hash, [](std::atomic<uint64_t>* ptr, uint64_t mask) {
    // Happens-before between AddHash and MaybeContains is handled by
    // access to versions_->LastSequence(), so all we have to do here is
    // avoid races (so we don't give the compiler a license to mess up
    // our code) and not lose bits.  std::memory_order_relaxed is enough
    // for that.
    if ((mask & ptr->load(std::memory_order_relaxed)) != mask) {
      ptr->fetch_or(mask, std::memory_order_relaxed);
    }
  });
}

// 根据传入的 hash值 和 or函数 生成对应的data_
template <typename OrFunc>
inline void DynamicBloom::AddHash(uint32_t h32, const OrFunc& or_func) {
  size_t a = FastRange32(h32, kLen);
  PREFETCH(data_ + a, 0, 3);
  // Expand/remix with 64-bit golden ratio
  uint64_t h = 0x9e3779b97f4a7c13ULL * h32;
  for (unsigned i = 0;; ++i) {
    // Two bit probes per uint64_t probe
    uint64_t mask =
        ((uint64_t)1 << (h & 63)) | ((uint64_t)1 << ((h >> 6) & 63));
    or_func(&data_[a ^ i], mask);
    if (i + 1 >= kNumDoubleProbes) {
      return;
    }
    h = (h >> 12) | (h << 52);
  }
}
