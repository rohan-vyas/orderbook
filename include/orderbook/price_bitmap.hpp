#pragma once

#include <array>
#include <bit>
#include <cstdint>

namespace ob {

// Three-level bitmap over price ticks: bit p is set when price level p is non-empty.
// Finding the next non-empty level above/below a price touches at most three
// 64-bit words, so it is O(1) regardless of how far away that level is.
//   level 0: one bit per tick         (4096 words)
//   level 1: one bit per level-0 word (64 words)
//   level 2: one bit per level-1 word (1 word)
class PriceBitmap {
public:
    static constexpr std::int32_t kMaxTicks = 1 << 18;  // 262,144 ticks
    static constexpr std::int32_t kNone = -1;

    void set(std::int32_t p) noexcept {
        const auto u = static_cast<std::uint32_t>(p);
        l0_[u >> 6] |= bit(u & 63);
        l1_[u >> 12] |= bit((u >> 6) & 63);
        l2_ |= bit(u >> 12);
    }

    void clear(std::int32_t p) noexcept {
        const auto u = static_cast<std::uint32_t>(p);
        l0_[u >> 6] &= ~bit(u & 63);
        if (l0_[u >> 6] != 0) return;
        l1_[u >> 12] &= ~bit((u >> 6) & 63);
        if (l1_[u >> 12] != 0) return;
        l2_ &= ~bit(u >> 12);
    }

    bool test(std::int32_t p) const noexcept {
        const auto u = static_cast<std::uint32_t>(p);
        return (l0_[u >> 6] & bit(u & 63)) != 0;
    }

    // Lowest set index >= p, or kNone.
    std::int32_t findNext(std::int32_t p) const noexcept {
        if (p < 0) p = 0;
        if (p >= kMaxTicks) return kNone;
        const auto u = static_cast<std::uint32_t>(p);
        std::uint32_t w = u >> 6;
        std::uint64_t m = l0_[w] & (~0ULL << (u & 63));
        if (m) return static_cast<std::int32_t>((w << 6) | ctz(m));

        std::uint32_t w1 = w >> 6;
        const std::uint32_t b1 = (w & 63) + 1;
        m = b1 < 64 ? (l1_[w1] & (~0ULL << b1)) : 0;
        if (!m) {
            const std::uint32_t b2 = w1 + 1;
            const std::uint64_t m2 = b2 < 64 ? (l2_ & (~0ULL << b2)) : 0;
            if (!m2) return kNone;
            w1 = ctz(m2);
            m = l1_[w1];
        }
        w = (w1 << 6) | ctz(m);
        return static_cast<std::int32_t>((w << 6) | ctz(l0_[w]));
    }

    // Highest set index <= p, or kNone.
    std::int32_t findPrev(std::int32_t p) const noexcept {
        if (p < 0) return kNone;
        if (p >= kMaxTicks) p = kMaxTicks - 1;
        const auto u = static_cast<std::uint32_t>(p);
        std::uint32_t w = u >> 6;
        const std::uint32_t s = u & 63;
        std::uint64_t m = l0_[w] & (s == 63 ? ~0ULL : (bit(s + 1) - 1));
        if (m) return static_cast<std::int32_t>((w << 6) | msb(m));

        std::uint32_t w1 = w >> 6;
        m = l1_[w1] & (bit(w & 63) - 1);
        if (!m) {
            const std::uint64_t m2 = l2_ & (bit(w1) - 1);
            if (!m2) return kNone;
            w1 = msb(m2);
            m = l1_[w1];
        }
        w = (w1 << 6) | msb(m);
        return static_cast<std::int32_t>((w << 6) | msb(l0_[w]));
    }

private:
    static constexpr std::uint64_t bit(std::uint32_t i) noexcept { return 1ULL << i; }
    static std::uint32_t ctz(std::uint64_t x) noexcept { return static_cast<std::uint32_t>(std::countr_zero(x)); }
    static std::uint32_t msb(std::uint64_t x) noexcept { return 63u - static_cast<std::uint32_t>(std::countl_zero(x)); }

    std::array<std::uint64_t, kMaxTicks / 64> l0_{};
    std::array<std::uint64_t, kMaxTicks / 4096> l1_{};
    std::uint64_t l2_ = 0;
};

}  // namespace ob
