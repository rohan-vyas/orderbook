#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "types.hpp"

namespace ob {

// Fixed-capacity open-addressing hash map OrderId -> pool index.
// Linear probing with backward-shift deletion (no tombstones), Fibonacci hashing.
// All storage is allocated in the constructor; insert/find/erase never allocate.
// Capacity is at least 2x maxEntries, so the load factor stays <= 0.5.
class IdMap {
public:
    static constexpr std::uint32_t kNone = UINT32_MAX;

    explicit IdMap(std::size_t maxEntries) {
        const std::size_t cap = std::bit_ceil(std::max<std::size_t>(16, maxEntries * 2));
        shift_ = 64 - static_cast<unsigned>(std::countr_zero(cap));
        mask_ = cap - 1;
        slots_.assign(cap, Slot{0, kNone});
    }

    std::uint32_t find(OrderId key) const noexcept {
        std::size_t i = home(key);
        for (;;) {
            const Slot& s = slots_[i];
            if (s.val == kNone) return kNone;
            if (s.key == key) return s.val;
            i = (i + 1) & mask_;
        }
    }

    // Precondition: key is not present and size() < maxEntries.
    void insert(OrderId key, std::uint32_t val) noexcept {
        std::size_t i = home(key);
        while (slots_[i].val != kNone) i = (i + 1) & mask_;
        slots_[i] = Slot{key, val};
        ++size_;
    }

    bool erase(OrderId key) noexcept {
        std::size_t i = home(key);
        for (;;) {
            const Slot& s = slots_[i];
            if (s.val == kNone) return false;
            if (s.key == key) break;
            i = (i + 1) & mask_;
        }
        // Backward-shift: pull later entries of the probe run into the hole when legal.
        std::size_t j = i;
        for (;;) {
            j = (j + 1) & mask_;
            if (slots_[j].val == kNone) break;
            const std::size_t h = home(slots_[j].key);
            // The entry at j may move to i only if its home is not cyclically in (i, j].
            const bool homeInRange = (i <= j) ? (i < h && h <= j) : (i < h || h <= j);
            if (!homeInRange) {
                slots_[i] = slots_[j];
                i = j;
            }
        }
        slots_[i].val = kNone;
        --size_;
        return true;
    }

    std::size_t size() const noexcept { return size_; }

private:
    struct Slot {
        OrderId key;
        std::uint32_t val;
    };

    std::size_t home(OrderId key) const noexcept {
        return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ULL) >> shift_);
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    unsigned shift_ = 0;
    std::size_t size_ = 0;
};

}  // namespace ob
