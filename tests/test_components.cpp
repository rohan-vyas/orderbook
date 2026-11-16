#include <random>
#include <set>
#include <sstream>
#include <unordered_map>

#include "orderbook/id_map.hpp"
#include "orderbook/price_bitmap.hpp"
#include "orderbook/workload.hpp"
#include "test_framework.hpp"

using namespace ob;

TEST(bitmap_find_next_prev_match_std_set) {
    auto bm = std::make_unique<PriceBitmap>();
    std::set<std::int32_t> ref;
    std::mt19937_64 rng(7);
    const std::int32_t N = PriceBitmap::kMaxTicks;
    for (int step = 0; step < 200000; ++step) {
        // Mix of clustered and spread-out prices so all three bitmap levels are exercised.
        const std::int32_t p = (rng() % 4 == 0) ? static_cast<std::int32_t>(rng() % N)
                                                : static_cast<std::int32_t>(130000 + rng() % 300);
        if (rng() % 2) {
            bm->set(p);
            ref.insert(p);
        } else {
            bm->clear(p);
            ref.erase(p);
        }
        const std::int32_t q = static_cast<std::int32_t>(rng() % N);
        auto it = ref.lower_bound(q);
        const std::int32_t expNext = it == ref.end() ? PriceBitmap::kNone : *it;
        auto jt = ref.upper_bound(q);
        const std::int32_t expPrev = jt == ref.begin() ? PriceBitmap::kNone : *std::prev(jt);
        REQUIRE(bm->findNext(q) == expNext);
        REQUIRE(bm->findPrev(q) == expPrev);
        REQUIRE(bm->test(p) == (ref.count(p) == 1));
    }
    CHECK(bm->findNext(-5) == (ref.empty() ? PriceBitmap::kNone : *ref.begin()));
    CHECK(bm->findPrev(N + 5) == (ref.empty() ? PriceBitmap::kNone : *ref.rbegin()));
}

TEST(bitmap_boundaries) {
    auto bm = std::make_unique<PriceBitmap>();
    const std::int32_t last = PriceBitmap::kMaxTicks - 1;
    CHECK(bm->findNext(0) == PriceBitmap::kNone);
    CHECK(bm->findPrev(last) == PriceBitmap::kNone);
    bm->set(0);
    bm->set(last);
    CHECK(bm->findNext(1) == last);
    CHECK(bm->findPrev(last - 1) == 0);
    CHECK(bm->findNext(last) == last);
    CHECK(bm->findPrev(0) == 0);
    bm->clear(0);
    CHECK(bm->findPrev(last - 1) == PriceBitmap::kNone);
    bm->clear(last);
    CHECK(bm->findNext(0) == PriceBitmap::kNone);
}

TEST(id_map_matches_unordered_map) {
    const std::size_t cap = 5000;
    IdMap m(cap);
    std::unordered_map<OrderId, std::uint32_t> ref;
    std::mt19937_64 rng(11);
    for (int step = 0; step < 500000; ++step) {
        // Narrow key space forces long probe runs and frequent backward shifts.
        const OrderId k = (step % 3 == 0) ? rng() : rng() % 20000;
        const auto r = rng() % 3;
        if (r == 0 && ref.size() < cap && !ref.count(k)) {
            const auto v = static_cast<std::uint32_t>(rng() % 1000000);
            m.insert(k, v);
            ref[k] = v;
        } else if (r == 1) {
            REQUIRE(m.erase(k) == (ref.erase(k) == 1));
        }
        const auto it = ref.find(k);
        REQUIRE(m.find(k) == (it == ref.end() ? IdMap::kNone : it->second));
        REQUIRE(m.size() == ref.size());
    }
    for (const auto& [k, v] : ref) REQUIRE(m.find(k) == v);
}

TEST(csv_round_trip) {
    WorkloadParams p;
    p.prefill = 100;
    p.ops = 5000;
    p.pctModify = 5;
    const Workload w = generateWorkload(p);
    std::stringstream ss;
    ss << "# synthetic\n";
    for (const auto& op : w.prefill) writeCsv(ss, op);
    for (const auto& op : w.ops) writeCsv(ss, op);
    std::vector<Op> back;
    std::string err;
    REQUIRE(readCsv(ss, back, &err));
    REQUIRE(back.size() == w.prefill.size() + w.ops.size());
    bool same = true;
    for (std::size_t i = 0; i < w.prefill.size(); ++i) same = same && back[i] == w.prefill[i];
    for (std::size_t i = 0; i < w.ops.size(); ++i) {
        Op a = back[w.prefill.size() + i], b = w.ops[i];
        // Fields that the CSV line does not carry are normalised before comparing.
        if (b.type == OpType::Cancel || b.type == OpType::Modify) a.side = b.side;
        same = same && a == b;
    }
    CHECK(same);
    std::stringstream badIn("A,1,Q,10,5\n");
    std::vector<Op> tmp;
    CHECK(!readCsv(badIn, tmp, &err));
}
