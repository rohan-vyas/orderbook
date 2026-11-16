#pragma once

#include <cstdint>
#include <vector>

namespace ob {

using OrderId = std::uint64_t;
using Price = std::int32_t;  // integer ticks, valid range [0, numTicks)
using Qty = std::uint32_t;

enum class Side : std::uint8_t { Buy, Sell };

constexpr Side opposite(Side s) noexcept { return s == Side::Buy ? Side::Sell : Side::Buy; }

enum class Status : std::uint8_t {
    Ok,
    DuplicateId,   // a resting order already uses this id
    UnknownId,     // cancel/modify of an id that is not resting
    InvalidPrice,  // price outside [0, numTicks)
    InvalidQty,    // zero quantity
    PoolFull,      // preallocated order storage exhausted (engine only)
};

inline const char* toString(Status s) noexcept {
    switch (s) {
        case Status::Ok: return "Ok";
        case Status::DuplicateId: return "DuplicateId";
        case Status::UnknownId: return "UnknownId";
        case Status::InvalidPrice: return "InvalidPrice";
        case Status::InvalidQty: return "InvalidQty";
        case Status::PoolFull: return "PoolFull";
    }
    return "?";
}

// Emitted once per fill. The price is always the resting (maker) order's price.
struct Trade {
    OrderId taker;
    OrderId maker;
    Price price;
    Qty qty;
    Side takerSide;
    friend bool operator==(const Trade&, const Trade&) = default;
};

struct RestingOrder {
    OrderId id;
    Qty qty;
    friend bool operator==(const RestingOrder&, const RestingOrder&) = default;
};

struct LevelView {
    Price price;
    std::uint64_t totalQty;
    std::vector<RestingOrder> orders;  // in time priority (front = next to fill)
    friend bool operator==(const LevelView&, const LevelView&) = default;
};

// Full book state. Bids best (highest) first, asks best (lowest) first.
struct BookSnapshot {
    std::vector<LevelView> bids;
    std::vector<LevelView> asks;
    friend bool operator==(const BookSnapshot&, const BookSnapshot&) = default;
};

}  // namespace ob
