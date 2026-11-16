#pragma once

// Synthetic order-flow generation, CSV (de)serialisation and replay helpers.
// Everything produced here is synthetic; it is not derived from any real market data.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <istream>
#include <ostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "reference_book.hpp"
#include "types.hpp"

namespace ob {

enum class OpType : std::uint8_t { Limit, Market, Cancel, Modify };

struct Op {
    OpType type;
    Side side;  // unused for Cancel/Modify
    OrderId id;
    Price price;  // unused for Market/Cancel
    Qty qty;      // unused for Cancel
    friend bool operator==(const Op&, const Op&) = default;
};

template <class Book>
inline Status apply(Book& book, const Op& op) {
    switch (op.type) {
        case OpType::Limit: return book.addLimit(op.id, op.side, op.price, op.qty);
        case OpType::Market: return book.addMarket(op.id, op.side, op.qty);
        case OpType::Cancel: return book.cancel(op.id);
        case OpType::Modify: return book.modify(op.id, op.price, op.qty);
    }
    return Status::Ok;
}

struct WorkloadParams {
    std::uint64_t seed = 42;
    std::size_t prefill = 10'000;   // passive limit orders placed before the measured stream
    std::size_t ops = 1'000'000;    // measured operations
    Price numTicks = 1 << 16;
    Price mid = 1 << 15;            // fixed reference mid price (ticks)
    double meanOffset = 10.0;       // mean distance of passive orders from mid (ticks)
    unsigned pctAdd = 60;           // passive limit adds
    unsigned pctCancel = 30;        // cancels of a random live resting order
    unsigned pctModify = 0;         // modifies of a random live resting order
    // Marketable order size is uniform in [1, 300] while the book holds fewer than
    // targetResting orders and [1, 400] otherwise. This simple feedback keeps the
    // book near a steady depth instead of drifting to empty or growing without bound.
    std::size_t targetResting = 20'000;
    // Remaining percentage is marketable flow, split evenly between market orders and
    // limit orders priced through the opposite best.
};

struct Workload {
    std::vector<Op> prefill;
    std::vector<Op> ops;
    // Counts from simulating the stream on the reference book while generating it.
    std::size_t limitAdds = 0, cancels = 0, modifies = 0, markets = 0, marketableLimits = 0;
    std::size_t opsThatTraded = 0, trades = 0;
    std::size_t finalRestingOrders = 0;
    double meanRestingOrders = 0;
};

namespace detail {
struct TradeCounter {
    std::size_t n = 0;
    void onTrade(const Trade&) { ++n; }
};
}  // namespace detail

// Generates a stream by simulating it on the reference book, so cancels and modifies
// always target orders that are actually resting at that point in the stream.
inline Workload generateWorkload(const WorkloadParams& p) {
    Workload w;
    std::mt19937_64 rng(p.seed);
    auto uniform = [&](std::uint64_t n) { return rng() % n; };  // tiny bias is irrelevant here
    auto unit = [&] { return static_cast<double>(rng() >> 11) * 0x1.0p-53; };
    detail::TradeCounter tc;
    ReferenceBook<detail::TradeCounter> book(p.numTicks, 0, tc);
    std::vector<OrderId> live;  // may contain ids that have since been filled (lazy removal)
    OrderId nextId = 1;

    auto passive = [&]() -> Op {
        const Side side = uniform(2) ? Side::Buy : Side::Sell;
        const auto off = static_cast<Price>(std::min(500.0, std::floor(-std::log(1.0 - unit()) * p.meanOffset)));
        Price px = side == Side::Buy ? p.mid - 1 - off : p.mid + off;
        px = std::clamp<Price>(px, 0, p.numTicks - 1);
        return Op{OpType::Limit, side, nextId++, px, static_cast<Qty>(1 + uniform(100))};
    };
    auto pickLive = [&](OrderId& out) {
        while (!live.empty()) {
            const std::size_t i = uniform(live.size());
            const OrderId id = live[i];
            live[i] = live.back();
            live.pop_back();
            if (book.contains(id)) {
                out = id;
                return true;
            }
        }
        return false;
    };

    for (std::size_t i = 0; i < p.prefill; ++i) {
        Op op = passive();
        apply(book, op);
        if (book.contains(op.id)) live.push_back(op.id);
        w.prefill.push_back(op);
    }

    double restingSum = 0;
    w.ops.reserve(p.ops);
    for (std::size_t i = 0; i < p.ops; ++i) {
        const unsigned r = static_cast<unsigned>(uniform(100));
        Op op{};
        OrderId target = 0;
        if (r < p.pctAdd) {
            op = passive();
            ++w.limitAdds;
        } else if (r < p.pctAdd + p.pctCancel && pickLive(target)) {
            op = Op{OpType::Cancel, Side::Buy, target, 0, 0};
            ++w.cancels;
        } else if (r >= p.pctAdd + p.pctCancel && r < p.pctAdd + p.pctCancel + p.pctModify && pickLive(target)) {
            // New quantity 1..100; price unchanged half the time, else moved by up to +-5 ticks.
            const Price cur = *book.priceOf(target);
            Price np = uniform(2) ? cur : cur + static_cast<Price>(uniform(11)) - 5;
            np = std::clamp<Price>(np, 0, p.numTicks - 1);
            op = Op{OpType::Modify, Side::Buy, target, np, static_cast<Qty>(1 + uniform(100))};
            live.push_back(target);
            ++w.modifies;
        } else if (r >= p.pctAdd + p.pctCancel + p.pctModify) {
            const Side side = uniform(2) ? Side::Buy : Side::Sell;
            const Qty qty = static_cast<Qty>(1 + uniform(book.orderCount() < p.targetResting ? 300 : 400));
            if (uniform(2)) {
                op = Op{OpType::Market, side, nextId++, 0, qty};
                ++w.markets;
            } else {
                const auto opp = side == Side::Buy ? book.bestAsk() : book.bestBid();
                Price px = opp ? (side == Side::Buy ? *opp + 2 : *opp - 2) : p.mid;
                px = std::clamp<Price>(px, 0, p.numTicks - 1);
                op = Op{OpType::Limit, side, nextId++, px, qty};
                ++w.marketableLimits;
            }
        } else {
            op = passive();  // cancel/modify requested but nothing is resting
            ++w.limitAdds;
        }
        const std::size_t before = tc.n;
        apply(book, op);
        if (tc.n != before) ++w.opsThatTraded;
        if (op.type == OpType::Limit && book.contains(op.id)) live.push_back(op.id);
        w.ops.push_back(op);
        restingSum += static_cast<double>(book.orderCount());
    }
    w.trades = tc.n;
    w.finalRestingOrders = book.orderCount();
    w.meanRestingOrders = p.ops ? restingSum / static_cast<double>(p.ops) : 0;
    return w;
}

// CSV format, one op per line ('#' lines are comments):
//   A,<id>,<B|S>,<price>,<qty>   limit
//   M,<id>,<B|S>,<qty>           market
//   X,<id>                       cancel
//   R,<id>,<price>,<qty>         modify (cancel-replace semantics)
inline void writeCsv(std::ostream& out, const Op& op) {
    const char s = op.side == Side::Buy ? 'B' : 'S';
    switch (op.type) {
        case OpType::Limit: out << "A," << op.id << ',' << s << ',' << op.price << ',' << op.qty << '\n'; break;
        case OpType::Market: out << "M," << op.id << ',' << s << ',' << op.qty << '\n'; break;
        case OpType::Cancel: out << "X," << op.id << '\n'; break;
        case OpType::Modify: out << "R," << op.id << ',' << op.price << ',' << op.qty << '\n'; break;
    }
}

// Returns false on a malformed line (and sets *err).
inline bool readCsv(std::istream& in, std::vector<Op>& out, std::string* err = nullptr) {
    std::string line;
    std::size_t lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> f;
        std::stringstream ss(line);
        for (std::string tok; std::getline(ss, tok, ',');) f.push_back(tok);
        auto bad = [&] {
            if (err) *err = "line " + std::to_string(lineNo) + ": " + line;
            return false;
        };
        try {
            auto side = [&](const std::string& t, Side& sd) {
                if (t == "B") sd = Side::Buy;
                else if (t == "S") sd = Side::Sell;
                else return false;
                return true;
            };
            Op op{};
            if (f[0] == "A" && f.size() == 5) {
                op.type = OpType::Limit;
                op.id = std::stoull(f[1]);
                if (!side(f[2], op.side)) return bad();
                op.price = std::stoi(f[3]);
                op.qty = static_cast<Qty>(std::stoul(f[4]));
            } else if (f[0] == "M" && f.size() == 4) {
                op.type = OpType::Market;
                op.id = std::stoull(f[1]);
                if (!side(f[2], op.side)) return bad();
                op.qty = static_cast<Qty>(std::stoul(f[3]));
            } else if (f[0] == "X" && f.size() == 2) {
                op.type = OpType::Cancel;
                op.id = std::stoull(f[1]);
            } else if (f[0] == "R" && f.size() == 4) {
                op.type = OpType::Modify;
                op.id = std::stoull(f[1]);
                op.price = std::stoi(f[2]);
                op.qty = static_cast<Qty>(std::stoul(f[3]));
            } else {
                return bad();
            }
            out.push_back(op);
        } catch (const std::exception&) {
            return bad();
        }
    }
    return true;
}

}  // namespace ob
