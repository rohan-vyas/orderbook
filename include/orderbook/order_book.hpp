#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "id_map.hpp"
#include "price_bitmap.hpp"
#include "types.hpp"

namespace ob {

// Single-instrument limit order book with price-time priority matching.
//
// Layout (all memory is allocated in the constructor; the hot path never allocates):
//  * Orders live in a fixed pool (vector<Order>) and are addressed by 32-bit index.
//    Free slots form an intrusive free list, so alloc/free are O(1) with no heap calls.
//  * Each side has a dense array of price levels indexed directly by tick. A level is
//    an intrusive doubly linked FIFO of pool indices (head = oldest = next to fill).
//  * A three-level bitmap per side marks non-empty levels, so the next best price
//    after a level empties is found in O(1) (at most three word scans).
//  * Best bid / best ask are cached integers: reading them is O(1).
//  * An open-addressing IdMap gives O(1) expected lookup from OrderId to pool index,
//    which makes cancel and modify O(1).
//
// Listener must provide `void onTrade(const Trade&)`. It is called synchronously for
// every fill, in execution order.
template <class Listener>
class OrderBook {
public:
    OrderBook(Price numTicks, std::uint32_t maxOrders, Listener& listener)
        : listener_(listener),
          numTicks_(numTicks),
          bids_(std::make_unique<PriceBitmap>()),
          asks_(std::make_unique<PriceBitmap>()),
          ids_(maxOrders),
          bestAsk_(numTicks) {
        if (numTicks <= 0 || numTicks > PriceBitmap::kMaxTicks)
            throw std::invalid_argument("numTicks must be in [1, 262144]");
        if (maxOrders == 0 || maxOrders >= kNil) throw std::invalid_argument("bad maxOrders");
        bidLevels_.resize(static_cast<std::size_t>(numTicks));
        askLevels_.resize(static_cast<std::size_t>(numTicks));
        pool_.resize(maxOrders);
        for (std::uint32_t i = 0; i < maxOrders; ++i) pool_[i].next = (i + 1 < maxOrders) ? i + 1 : kNil;
        freeHead_ = 0;
    }

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    // Limit order: matches against the opposite side while it crosses, rests any remainder.
    Status addLimit(OrderId id, Side side, Price price, Qty qty) {
        if (qty == 0) return Status::InvalidQty;
        if (price < 0 || price >= numTicks_) return Status::InvalidPrice;
        if (ids_.find(id) != IdMap::kNone) return Status::DuplicateId;
        if (freeHead_ == kNil) return Status::PoolFull;
        qty = side == Side::Buy ? match<Side::Buy>(id, price, qty) : match<Side::Sell>(id, price, qty);
        if (qty != 0) rest(id, side, price, qty);
        return Status::Ok;
    }

    // Market order: immediate-or-cancel at any price. Unfilled quantity is discarded.
    Status addMarket(OrderId id, Side side, Qty qty) {
        if (qty == 0) return Status::InvalidQty;
        if (side == Side::Buy)
            match<Side::Buy>(id, numTicks_ - 1, qty);
        else
            match<Side::Sell>(id, 0, qty);
        return Status::Ok;
    }

    Status cancel(OrderId id) {
        const std::uint32_t idx = ids_.find(id);
        if (idx == IdMap::kNone) return Status::UnknownId;
        unlink(idx);
        ids_.erase(id);
        freeOrder(idx);
        return Status::Ok;
    }

    // Modify sets a new price and new remaining quantity.
    //  * Same price and quantity reduced (or unchanged): done in place, keeps time priority.
    //  * Anything else: cancel-replace. The order goes to the back of the queue at the new
    //    price and, if the new price crosses, executes as an aggressive (taker) order.
    Status modify(OrderId id, Price price, Qty qty) {
        const std::uint32_t idx = ids_.find(id);
        if (idx == IdMap::kNone) return Status::UnknownId;
        if (qty == 0) return Status::InvalidQty;
        if (price < 0 || price >= numTicks_) return Status::InvalidPrice;
        Order& o = pool_[idx];
        if (price == o.price && qty <= o.qty) {
            level(o.side, o.price).totalQty -= (o.qty - qty);
            o.qty = qty;
            return Status::Ok;
        }
        const Side side = o.side;
        unlink(idx);
        ids_.erase(id);
        freeOrder(idx);
        return addLimit(id, side, price, qty);
    }

    std::optional<Price> bestBid() const noexcept {
        return bestBid_ >= 0 ? std::optional<Price>(bestBid_) : std::nullopt;
    }
    std::optional<Price> bestAsk() const noexcept {
        return bestAsk_ < numTicks_ ? std::optional<Price>(bestAsk_) : std::nullopt;
    }
    std::uint64_t volumeAt(Side side, Price price) const noexcept {
        if (price < 0 || price >= numTicks_) return 0;
        return level(side, price).totalQty;
    }
    std::size_t orderCount() const noexcept { return liveOrders_; }
    Price numTicks() const noexcept { return numTicks_; }

    BookSnapshot snapshot() const {
        BookSnapshot s;
        for (Price p = bestBid_; p >= 0; p = bids_->findPrev(p - 1)) s.bids.push_back(view(Side::Buy, p));
        for (Price p = bestAsk_; p < numTicks_; p = nextAsk(p + 1)) s.asks.push_back(view(Side::Sell, p));
        return s;
    }

    // Exhaustive O(numTicks + orders) consistency check, for tests only.
    bool checkInvariants(std::string* why = nullptr) const {
        auto fail = [&](const std::string& m) {
            if (why) *why = m;
            return false;
        };
        std::size_t seen = 0;
        Price hiBid = -1, loAsk = numTicks_;
        for (int s = 0; s < 2; ++s) {
            const Side side = s == 0 ? Side::Buy : Side::Sell;
            const auto& levels = side == Side::Buy ? bidLevels_ : askLevels_;
            const PriceBitmap& bm = side == Side::Buy ? *bids_ : *asks_;
            for (Price p = 0; p < numTicks_; ++p) {
                const Level& lv = levels[static_cast<std::size_t>(p)];
                const bool empty = lv.head == kNil;
                if (empty != (lv.tail == kNil)) return fail("head/tail mismatch");
                if (empty == bm.test(p)) return fail("bitmap disagrees at " + std::to_string(p));
                if (empty) {
                    if (lv.totalQty != 0 || lv.count != 0) return fail("empty level has qty");
                    continue;
                }
                if (side == Side::Buy) hiBid = std::max(hiBid, p);
                else loAsk = std::min(loAsk, p);
                std::uint64_t sum = 0;
                std::uint32_t n = 0, prev = kNil;
                for (std::uint32_t i = lv.head; i != kNil; i = pool_[i].next) {
                    const Order& o = pool_[i];
                    if (o.prev != prev) return fail("bad prev link");
                    if (o.price != p || o.side != side) return fail("order on wrong level");
                    if (o.qty == 0) return fail("zero-qty resting order");
                    if (ids_.find(o.id) != i) return fail("id index mismatch");
                    sum += o.qty;
                    ++n;
                    prev = i;
                }
                if (prev != lv.tail) return fail("bad tail");
                if (sum != lv.totalQty || n != lv.count) return fail("level aggregate mismatch");
                seen += n;
            }
        }
        if (seen != liveOrders_ || seen != ids_.size()) return fail("order count mismatch");
        if (hiBid != bestBid_ || loAsk != bestAsk_) return fail("cached best price wrong");
        if (bestBid_ >= 0 && bestAsk_ < numTicks_ && bestBid_ >= bestAsk_) return fail("book crossed");
        return true;
    }

private:
    static constexpr std::uint32_t kNil = UINT32_MAX;

    struct Order {
        OrderId id = 0;
        Price price = 0;
        Qty qty = 0;
        std::uint32_t prev = kNil;
        std::uint32_t next = kNil;
        Side side = Side::Buy;
    };

    struct Level {
        std::uint32_t head = kNil;
        std::uint32_t tail = kNil;
        std::uint64_t totalQty = 0;
        std::uint32_t count = 0;
    };

    Level& level(Side side, Price p) noexcept {
        return (side == Side::Buy ? bidLevels_ : askLevels_)[static_cast<std::size_t>(p)];
    }
    const Level& level(Side side, Price p) const noexcept {
        return (side == Side::Buy ? bidLevels_ : askLevels_)[static_cast<std::size_t>(p)];
    }

    Price nextAsk(Price from) const noexcept {
        const Price p = asks_->findNext(from);
        return p == PriceBitmap::kNone ? numTicks_ : p;
    }
    Price prevBid(Price from) const noexcept { return bids_->findPrev(from); }  // kNone == -1

    // Fills `qty` against the opposite side up to `limit`. Returns the unfilled remainder.
    template <Side TakerSide>
    Qty match(OrderId taker, Price limit, Qty qty) {
        constexpr bool kBuy = TakerSide == Side::Buy;
        auto& levels = kBuy ? askLevels_ : bidLevels_;
        Price& best = kBuy ? bestAsk_ : bestBid_;
        while (qty != 0 && (kBuy ? best <= limit : best >= limit)) {
            // For a buy taker best < numTicks_ here because limit < numTicks_;
            // for a sell taker best >= 0 because limit >= 0.
            const Price px = best;
            Level& lv = levels[static_cast<std::size_t>(px)];
            std::uint32_t idx = lv.head;
            while (qty != 0 && idx != kNil) {
                Order& o = pool_[idx];
                const Qty fill = std::min(qty, o.qty);
                listener_.onTrade(Trade{taker, o.id, px, fill, TakerSide});
                o.qty -= fill;
                qty -= fill;
                lv.totalQty -= fill;
                const std::uint32_t next = o.next;
                if (o.qty == 0) {
                    lv.head = next;
                    if (next != kNil) pool_[next].prev = kNil;
                    else lv.tail = kNil;
                    --lv.count;
                    ids_.erase(o.id);
                    freeOrder(idx);
                }
                idx = next;
            }
            if (lv.head == kNil) {
                if constexpr (kBuy) {
                    asks_->clear(px);
                    best = nextAsk(px + 1);
                } else {
                    bids_->clear(px);
                    best = prevBid(px - 1);
                }
            }
        }
        return qty;
    }

    void rest(OrderId id, Side side, Price price, Qty qty) {
        const std::uint32_t idx = freeHead_;
        freeHead_ = pool_[idx].next;
        Order& o = pool_[idx];
        o.id = id;
        o.price = price;
        o.qty = qty;
        o.side = side;
        o.next = kNil;
        Level& lv = level(side, price);
        o.prev = lv.tail;
        if (lv.tail != kNil) {
            pool_[lv.tail].next = idx;
        } else {
            lv.head = idx;
            if (side == Side::Buy) {
                bids_->set(price);
                if (price > bestBid_) bestBid_ = price;
            } else {
                asks_->set(price);
                if (price < bestAsk_) bestAsk_ = price;
            }
        }
        lv.tail = idx;
        lv.totalQty += qty;
        ++lv.count;
        ids_.insert(id, idx);
        ++liveOrders_;
    }

    // Removes a resting order from its level (does not touch the id index or pool).
    void unlink(std::uint32_t idx) noexcept {
        Order& o = pool_[idx];
        Level& lv = level(o.side, o.price);
        if (o.prev != kNil) pool_[o.prev].next = o.next;
        else lv.head = o.next;
        if (o.next != kNil) pool_[o.next].prev = o.prev;
        else lv.tail = o.prev;
        lv.totalQty -= o.qty;
        --lv.count;
        if (lv.head == kNil) {
            if (o.side == Side::Buy) {
                bids_->clear(o.price);
                if (o.price == bestBid_) bestBid_ = prevBid(o.price - 1);
            } else {
                asks_->clear(o.price);
                if (o.price == bestAsk_) bestAsk_ = nextAsk(o.price + 1);
            }
        }
    }

    void freeOrder(std::uint32_t idx) noexcept {
        pool_[idx].next = freeHead_;
        freeHead_ = idx;
        --liveOrders_;
    }

    LevelView view(Side side, Price p) const {
        const Level& lv = level(side, p);
        LevelView v{p, lv.totalQty, {}};
        v.orders.reserve(lv.count);
        for (std::uint32_t i = lv.head; i != kNil; i = pool_[i].next) v.orders.push_back({pool_[i].id, pool_[i].qty});
        return v;
    }

    Listener& listener_;
    Price numTicks_;
    std::vector<Level> bidLevels_;
    std::vector<Level> askLevels_;
    std::unique_ptr<PriceBitmap> bids_;
    std::unique_ptr<PriceBitmap> asks_;
    std::vector<Order> pool_;
    IdMap ids_;
    std::uint32_t freeHead_ = kNil;
    std::size_t liveOrders_ = 0;
    Price bestBid_ = -1;  // -1 when no bids
    Price bestAsk_;       // numTicks_ when no asks
};

}  // namespace ob
