#pragma once
#include <vector>
#include <cstdint>
#include <algorithm>
#include <iostream>

class Bitmap {
public:
    explicit Bitmap(size_t n_bits = 0) { resize(n_bits); }

    // 重新指定位数，旧内容会被清空
    void resize(size_t n_bits) {
        bits_.clear();
        bits_.resize((n_bits + 63) / 64, 0);
        n_bits_ = n_bits;
    }

    // 访问底层块，方便外部遍历
    const std::vector<uint64_t>& data() const { return bits_; }
    size_t capacity() const { return n_bits_; }

    // 基本位操作
    void set(size_t pos) {
        if (pos < n_bits_) bits_[pos >> 6] |= 1ULL << (pos & 63);
    }
    void reset(size_t pos) {
        if (pos < n_bits_) bits_[pos >> 6] &= ~(1ULL << (pos & 63));
    }
    bool test(size_t pos) const {
        return pos < n_bits_ && (bits_[pos >> 6] >> (pos & 63) & 1);
    }
    void flip(size_t pos) {
        if (pos < n_bits_) bits_[pos >> 6] ^= 1ULL << (pos & 63);
    }

    // 统计 1 的个数（popcount）
    size_t count() const {
        size_t c = 0;
        for (uint64_t x : bits_) c += __builtin_popcountll(x);
        return c;
    }

    // 找第一个 0 或 1；找不到返回 size_t(-1)
    size_t find_first_zero() const {
        for (size_t i = 0; i < bits_.size(); ++i) {
            uint64_t w = ~bits_[i];
            if (w) return (i << 6) + __builtin_ctzll(w);
        }
        return size_t(-1);
    }
    size_t find_first_one() const {
        for (size_t i = 0; i < bits_.size(); ++i) {
            if (bits_[i]) return (i << 6) + __builtin_ctzll(bits_[i]);
        }
        return size_t(-1);
    }
private:
    std::vector<uint64_t> bits_;
    size_t n_bits_ = 0;
};

int main() {
    Bitmap bm(100);          // 100 位
    bm.set(5);
    bm.set(99);
    std::cout << bm.test(5) << '\n';   // 1
    std::cout << bm.test(0) << '\n';   // 0
    std::cout << bm.count() << '\n';   // 2
    std::cout << bm.find_first_zero() << '\n'; // 0
}
