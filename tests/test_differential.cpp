// Randomised differential test: the optimised engine and the reference book are fed
// identical random order streams (fixed seeds) and must agree exactly on every return
// status, every emitted trade, top of book and order count after every operation,
// and on the full book state (every level, every order, in queue order) periodically.

#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "orderbook/order_book.hpp"
#include "orderbook/reference_book.hpp"
#include "orderbook/workload.hpp"
#include "test_framework.hpp"

using namespace ob;

namespace {

struct Collect {
    std::vector<Trade> trades;
    void onTrade(const Trade& t) { trades.push_back(t); }
};

struct StreamConfig {
    const char* name;
    Price numTicks;
    Price center;
    Price spread;  // prices drawn mostly from [center - spread, center + spread]
};

constexpr StreamConfig kConfigs[] = {
    {"tight", 64, 32, 40},              // tiny book, constant crossing, edge ticks
    {"realistic", 4096, 2048, 30},      // clustered around a mid
    {"wide", PriceBitmap::kMaxTicks, PriceBitmap::kMaxTicks / 2, 120000},  // sparse, far jumps
};

struct Totals {
    std::size_t streams = 0, ops = 0, trades = 0, snapshots = 0, invariantChecks = 0;
};

bool runStream(const StreamConfig& cfg, std::uint64_t seed, std::size_t nOps, Totals& tot, std::string& why) {
    Collect ce, cr;
    auto engine = std::make_unique<OrderBook<Collect>>(cfg.numTicks, 1u << 17, ce);
    ReferenceBook<Collect> ref(cfg.numTicks, 0, cr);
    std::mt19937_64 rng(seed);
    auto U = [&](std::uint64_t n) { return rng() % n; };
    std::vector<OrderId> ids;  // every id ever used, for cancels/modifies/duplicates
    OrderId nextId = 1;

    auto price = [&]() -> Price {
        if (U(50) == 0) return U(2) ? -1 - static_cast<Price>(U(3)) : cfg.numTicks + static_cast<Price>(U(3));
        if (U(30) == 0) return U(2) ? 0 : cfg.numTicks - 1;
        const Price p = cfg.center - cfg.spread + static_cast<Price>(U(2 * static_cast<std::uint64_t>(cfg.spread) + 1));
        return std::clamp<Price>(p, 0, cfg.numTicks - 1);
    };
    auto qty = [&]() -> Qty { return U(40) == 0 ? 0 : static_cast<Qty>(1 + U(U(10) == 0 ? 500 : 50)); };
    auto someId = [&]() -> OrderId {
        if (ids.empty() || U(10) == 0) return 1 + U(nextId + 5);  // may be unknown
        return ids[U(ids.size())];
    };

    for (std::size_t i = 0; i < nOps; ++i) {
        Op op{};
        const auto r = U(100);
        const Side side = U(2) ? Side::Buy : Side::Sell;
        if (r < 45) {
            const OrderId id = U(25) == 0 ? someId() : nextId++;  // occasional duplicate id
            op = Op{OpType::Limit, side, id, price(), qty()};
            ids.push_back(id);
        } else if (r < 55) {
            op = Op{OpType::Market, side, nextId++, 0, qty()};
        } else if (r < 80) {
            op = Op{OpType::Cancel, side, someId(), 0, 0};
        } else {
            // Modify: half the time keep the price (pure qty change), else a new price.
            const OrderId id = someId();
            Price px = price();
            if (U(2)) {
                if (auto cur = ref.priceOf(id)) px = *cur;
            }
            op = Op{OpType::Modify, side, id, px, qty()};
        }

        const Status se = apply(*engine, op);
        const Status sr = apply(ref, op);
        ++tot.ops;
        auto fail = [&](const std::string& what) {
            why = std::string(cfg.name) + " seed=" + std::to_string(seed) + " op#" + std::to_string(i) + ": " + what;
            return false;
        };
        if (se != sr) return fail(std::string("status ") + toString(se) + " vs " + toString(sr));
        if (ce.trades != cr.trades) return fail("trades differ");
        tot.trades += ce.trades.size();
        ce.trades.clear();
        cr.trades.clear();
        if (engine->bestBid() != ref.bestBid() || engine->bestAsk() != ref.bestAsk()) return fail("top of book differs");
        if (engine->orderCount() != ref.orderCount()) return fail("order count differs");
        if (i % 100 == 99 || i + 1 == nOps) {
            if (engine->snapshot() != ref.snapshot()) return fail("snapshot differs");
            ++tot.snapshots;
        }
        const std::size_t invEvery = cfg.numTicks > 10000 ? 2500 : 250;
        if (i % invEvery == invEvery - 1 || i + 1 == nOps) {
            std::string inv;
            if (!engine->checkInvariants(&inv)) return fail("engine invariant: " + inv);
            ++tot.invariantChecks;
        }
    }
    ++tot.streams;
    return true;
}

std::size_t envOr(const char* name, std::size_t def) {
    const char* v = std::getenv(name);
    return v ? static_cast<std::size_t>(std::strtoull(v, nullptr, 10)) : def;
}

}  // namespace

TEST(differential_engine_vs_reference_random_streams) {
    const std::size_t streams = envOr("OB_DIFF_STREAMS", 300);
    const std::size_t opsPerStream = envOr("OB_DIFF_OPS", 10000);
    Totals tot;
    for (std::size_t s = 0; s < streams; ++s) {
        const StreamConfig& cfg = kConfigs[s % std::size(kConfigs)];
        std::string why;
        const bool ok = runStream(cfg, 1000 + s, opsPerStream, tot, why);
        if (!ok) std::fprintf(stderr, "    mismatch: %s\n", why.c_str());
        REQUIRE(ok);
    }
    std::printf("    differential: %zu streams x %zu ops = %zu ops, %zu trades compared, %zu full-book snapshots "
                "compared, %zu engine invariant checks\n",
                tot.streams, opsPerStream, tot.ops, tot.trades, tot.snapshots, tot.invariantChecks);
}

TEST(differential_on_benchmark_workload) {
    // The exact generator used by the benchmark (60/30/10 mix, plus 5% modifies here).
    WorkloadParams p;
    p.seed = 99;
    p.prefill = 10000;
    p.ops = 300000;
    p.pctAdd = 57;
    p.pctCancel = 28;
    p.pctModify = 5;
    const Workload w = generateWorkload(p);
    Collect ce, cr;
    auto engine = std::make_unique<OrderBook<Collect>>(p.numTicks, 1u << 20, ce);
    ReferenceBook<Collect> ref(p.numTicks, 0, cr);
    std::size_t trades = 0;
    for (const auto* stream : {&w.prefill, &w.ops}) {
        for (const Op& op : *stream) {
            REQUIRE(apply(*engine, op) == apply(ref, op));
            REQUIRE(ce.trades == cr.trades);
            trades += ce.trades.size();
            ce.trades.clear();
            cr.trades.clear();
        }
    }
    CHECK(engine->snapshot() == ref.snapshot());
    CHECK(engine->checkInvariants());
    std::printf("    benchmark-workload differential: %zu ops, %zu trades compared\n",
                w.prefill.size() + w.ops.size(), trades);
}
