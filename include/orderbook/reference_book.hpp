#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>

#include "types.hpp"

namespace ob {

// Deliberately simple, obviously-correct order book used as a test oracle and as a
// performance baseline: std::map of price -> std::list FIFO, plus an unordered_map
// from id to list iterator. Same public API and semantics as OrderBook, except it
// never returns PoolFull (it allocates per order).
template <class Listener>
class ReferenceBook {
public:
    ReferenceBook(Price numTicks, std::uint32_t /*maxOrders*/, Listener& listener)
        : listener_(listener), numTicks_(numTicks) {}

    Status addLimit(OrderId id, Side side, Price price, Qty qty) {
        if (qty == 0) return Status::InvalidQty;
        if (price < 0 || price >= numTicks_) return Status::InvalidPrice;
        if (index_.count(id)) return Status::DuplicateId;
        qty = side == Side::Buy ? match(id, side, asks_, price, qty) : match(id, side, bids_, price, qty);
        if (qty != 0) {
            auto& q = side == Side::Buy ? bids_[price] : asks_[price];
            q.push_back({id, qty});
            index_[id] = Loc{side, price, std::prev(q.end())};
        }
        return Status::Ok;
    }

    Status addMarket(OrderId id, Side side, Qty qty) {
        if (qty == 0) return Status::InvalidQty;
        if (side == Side::Buy) match(id, side, asks_, numTicks_ - 1, qty);
        else match(id, side, bids_, 0, qty);
        return Status::Ok;
    }

    Status cancel(OrderId id) {
        auto it = index_.find(id);
        if (it == index_.end()) return Status::UnknownId;
        erase(it->second);
        index_.erase(it);
        return Status::Ok;
    }

    Status modify(OrderId id, Price price, Qty qty) {
        auto it = index_.find(id);
        if (it == index_.end()) return Status::UnknownId;
        if (qty == 0) return Status::InvalidQty;
        if (price < 0 || price >= numTicks_) return Status::InvalidPrice;
        Loc loc = it->second;
        if (price == loc.price && qty <= loc.it->qty) {
            loc.it->qty = qty;  // keeps time priority
            return Status::Ok;
        }
        erase(loc);
        index_.erase(it);
        return addLimit(id, loc.side, price, qty);
    }

    std::optional<Price> bestBid() const {
        return bids_.empty() ? std::nullopt : std::optional<Price>(bids_.begin()->first);
    }
    std::optional<Price> bestAsk() const {
        return asks_.empty() ? std::nullopt : std::optional<Price>(asks_.begin()->first);
    }
    std::uint64_t volumeAt(Side side, Price price) const {
        std::uint64_t v = 0;
        if (side == Side::Buy) {
            if (auto it = bids_.find(price); it != bids_.end())
                for (auto& o : it->second) v += o.qty;
        } else {
            if (auto it = asks_.find(price); it != asks_.end())
                for (auto& o : it->second) v += o.qty;
        }
        return v;
    }
    std::size_t orderCount() const { return index_.size(); }
    bool contains(OrderId id) const { return index_.count(id) != 0; }
    std::optional<Price> priceOf(OrderId id) const {
        auto it = index_.find(id);
        return it == index_.end() ? std::nullopt : std::optional<Price>(it->second.price);
    }

    BookSnapshot snapshot() const {
        BookSnapshot s;
        auto dump = [](const auto& side, std::vector<LevelView>& out) {
            for (const auto& [px, q] : side) {
                LevelView v{px, 0, {}};
                for (const auto& o : q) {
                    v.orders.push_back({o.id, o.qty});
                    v.totalQty += o.qty;
                }
                out.push_back(std::move(v));
            }
        };
        dump(bids_, s.bids);
        dump(asks_, s.asks);
        return s;
    }

private:
    struct Entry {
        OrderId id;
        Qty qty;
    };
    using Queue = std::list<Entry>;
    struct Loc {
        Side side;
        Price price;
        typename Queue::iterator it;
    };

    template <class Map>
    Qty match(OrderId taker, Side takerSide, Map& book, Price limit, Qty qty) {
        while (qty != 0 && !book.empty()) {
            auto lvl = book.begin();
            const Price px = lvl->first;
            if (takerSide == Side::Buy ? px > limit : px < limit) break;
            Queue& q = lvl->second;
            while (qty != 0 && !q.empty()) {
                Entry& maker = q.front();
                const Qty fill = std::min(qty, maker.qty);
                listener_.onTrade(Trade{taker, maker.id, px, fill, takerSide});
                maker.qty -= fill;
                qty -= fill;
                if (maker.qty == 0) {
                    index_.erase(maker.id);
                    q.pop_front();
                }
            }
            if (q.empty()) book.erase(lvl);
        }
        return qty;
    }

    void erase(const Loc& loc) {
        if (loc.side == Side::Buy) {
            auto lvl = bids_.find(loc.price);
            lvl->second.erase(loc.it);
            if (lvl->second.empty()) bids_.erase(lvl);
        } else {
            auto lvl = asks_.find(loc.price);
            lvl->second.erase(loc.it);
            if (lvl->second.empty()) asks_.erase(lvl);
        }
    }

    Listener& listener_;
    Price numTicks_;
    std::map<Price, Queue, std::greater<Price>> bids_;
    std::map<Price, Queue, std::less<Price>> asks_;
    std::unordered_map<OrderId, Loc> index_;
};

}  // namespace ob
