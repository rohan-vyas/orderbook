// Scenario tests. BOTH_TEST scenarios run against the optimised engine and the
// reference implementation; TEST scenarios are engine-specific.

#include <vector>

#include "orderbook/order_book.hpp"
#include "orderbook/reference_book.hpp"
#include "test_framework.hpp"

using namespace ob;

namespace {

constexpr Price kTicks = 1000;
constexpr std::uint32_t kCap = 1024;

struct Collect {
    std::vector<Trade> trades;
    void onTrade(const Trade& t) { trades.push_back(t); }
};

template <template <class> class BookT>
struct Fix {
    Collect c;
    BookT<Collect> book{kTicks, kCap, c};
    std::vector<Trade> take() {
        auto t = std::move(c.trades);
        c.trades.clear();
        return t;
    }
};

Trade T(OrderId taker, OrderId maker, Price px, Qty q, Side s) { return Trade{taker, maker, px, q, s}; }

template <class B>
std::vector<RestingOrder> queueAt(const B& book, Side side, Price px) {
    const BookSnapshot s = book.snapshot();
    for (const auto& lv : side == Side::Buy ? s.bids : s.asks)
        if (lv.price == px) return lv.orders;
    return {};
}

}  // namespace

BOTH_TEST(empty_book_has_no_best_prices) {
    Fix<BookT> f;
    CHECK(!f.book.bestBid());
    CHECK(!f.book.bestAsk());
    CHECK(f.book.orderCount() == 0);
    CHECK(f.book.snapshot() == BookSnapshot{});
}

BOTH_TEST(market_order_on_empty_book_does_nothing) {
    Fix<BookT> f;
    CHECK(f.book.addMarket(1, Side::Buy, 10) == Status::Ok);
    CHECK(f.book.addMarket(2, Side::Sell, 10) == Status::Ok);
    CHECK(f.take().empty());
    CHECK(f.book.orderCount() == 0);
}

BOTH_TEST(cancel_and_modify_unknown_id_rejected) {
    Fix<BookT> f;
    CHECK(f.book.cancel(42) == Status::UnknownId);
    CHECK(f.book.modify(42, 100, 5) == Status::UnknownId);
}

BOTH_TEST(invalid_inputs_rejected_without_side_effects) {
    Fix<BookT> f;
    CHECK(f.book.addLimit(1, Side::Buy, 100, 0) == Status::InvalidQty);
    CHECK(f.book.addLimit(1, Side::Buy, -1, 5) == Status::InvalidPrice);
    CHECK(f.book.addLimit(1, Side::Buy, kTicks, 5) == Status::InvalidPrice);
    CHECK(f.book.addMarket(1, Side::Buy, 0) == Status::InvalidQty);
    CHECK(f.book.orderCount() == 0);
    CHECK(f.book.addLimit(1, Side::Buy, 100, 5) == Status::Ok);
    CHECK(f.book.addLimit(1, Side::Sell, 200, 5) == Status::DuplicateId);
    CHECK(f.book.orderCount() == 1);
    CHECK(!f.book.bestAsk());
}

BOTH_TEST(non_crossing_orders_rest_and_set_best_prices) {
    Fix<BookT> f;
    f.book.addLimit(1, Side::Buy, 99, 10);
    f.book.addLimit(2, Side::Buy, 98, 10);
    f.book.addLimit(3, Side::Sell, 101, 10);
    f.book.addLimit(4, Side::Sell, 102, 10);
    CHECK(f.take().empty());
    CHECK(f.book.bestBid() == 99);
    CHECK(f.book.bestAsk() == 101);
    CHECK(f.book.orderCount() == 4);
    CHECK(f.book.volumeAt(Side::Buy, 99) == 10);
    CHECK(f.book.volumeAt(Side::Sell, 102) == 10);
}

BOTH_TEST(exact_cross_fills_both_sides_at_maker_price) {
    Fix<BookT> f;
    f.book.addLimit(1, Side::Sell, 100, 10);
    f.book.addLimit(2, Side::Buy, 100, 10);
    CHECK(f.take() == std::vector<Trade>{T(2, 1, 100, 10, Side::Buy)});
    CHECK(f.book.orderCount() == 0);
    CHECK(!f.book.bestBid());
    CHECK(!f.book.bestAsk());
}

BOTH_TEST(crossed_limit_trades_at_resting_price_not_limit) {
    Fix<BookT> f;
    f.book.addLimit(1, Side::Buy, 100, 10);
    f.book.addLimit(2, Side::Sell, 95, 4);  // aggressive seller gets price improvement
    CHECK(f.take() == std::vector<Trade>{T(2, 1, 100, 4, Side::Sell)});
    CHECK(f.book.volumeAt(Side::Buy, 100) == 6);
    CHECK(!f.book.bestAsk());
}

BOTH_TEST(price_priority_best_level_fills_first) {
    Fix<BookT> f;
    f.book.addLimit(1, Side::Sell, 101, 10);
    f.book.addLimit(2, Side::Sell, 100, 10);
    f.book.addLimit(3, Side::Sell, 102, 10);
    f.book.addMarket(9, Side::Buy, 25);
    CHECK(f.take() == (std::vector<Trade>{T(9, 2, 100, 10, Side::Buy), T(9, 1, 101, 10, Side::Buy),
                                         T(9, 3, 102, 5, Side::Buy)}));
    CHECK(f.book.bestAsk() == 102);
    CHECK(f.book.volumeAt(Side::Sell, 102) == 5);
}

BOTH_TEST(time_priority_within_level) {
    Fix<BookT> f;
    f.book.addLimit(1, Side::Sell, 100, 10);
    f.book.addLimit(2, Side::Sell, 100, 10);
    f.book.addLimit(3, Side::Sell, 100, 10);
    f.book.addLimit(9, Side::Buy, 100, 15);
    CHECK(f.take() == (std::vector<Trade>{T(9, 1, 100, 10, Side::Buy), T(9, 2, 100, 5, Side::Buy)}));
    CHECK(queueAt(f.book, Side::Sell, 100) == (std::vector<RestingOrder>{{2, 5}, {3, 10}}));
}

BOTH_TEST(partially_filled_maker_keeps_queue_position) {
    Fix<BookT> f;
    f.book.addLimit(1, Side::Buy, 100, 10);
    f.book.addLimit(2, Side::Buy, 100, 10);
    f.book.addMarket(8, Side::Sell, 3);
    f.book.addMarket(9, Side::Sell, 3);
    CHECK(f.take() == (std::vector<Trade>{T(8, 1, 100, 3, Side::Sell), T(9, 1, 100, 3, Side::Sell)}));
    CHECK(queueAt(f.book, Side::Buy, 100) == (std::vector<RestingOrder>{{1, 4}, {2, 10}}));
}

BOTH_TEST(partially_filled_taker_rests_remainder_at_its_limit) {
    Fix<BookT> f;
    f.book.addLimit(1, Side::Sell, 100, 5);
    f.book.addLimit(2, Side::Buy, 101, 8);
    CHECK(f.take() == std::vector<Trade>{T(2, 1, 100, 5, Side::Buy)});
    CHECK(f.book.bestBid() == 101);
    CHECK(f.book.volumeAt(Side::Buy, 101) == 3);
    CHECK(!f.book.bestAsk());
}

BOTH_TEST(crossing_limit_sweeps_levels_only_up_to_its_limit) {
    Fix<BookT> f;
    f.book.addLimit(1, Side::Sell, 100, 5);
    f.book.addLimit(2, Side::Sell, 101, 5);
    f.book.addLimit(3, Side::Sell, 103, 5);
    f.book.addLimit(9, Side::Buy, 102, 20);
    CHECK(f.take() == (std::vector<Trade>{T(9, 1, 100, 5, Side::Buy), T(9, 2, 101, 5, Side::Buy)}));
    CHECK(f.book.bestBid() == 102);
    CHECK(f.book.volumeAt(Side::Buy, 102) == 10);
    CHECK(f.book.bestAsk() == 103);
}

BOTH_TEST(market_order_against_thin_book_discards_remainder) {
    Fix<BookT> f;
    f.book.addLimit(1, Side::Sell, 100, 3);
    CHECK(f.book.addMarket(9, Side::Buy, 10) == Status::Ok);
    CHECK(f.take() == std::vector<Trade>{T(9, 1, 100, 3, Side::Buy)});
    CHECK(f.book.orderCount() == 0);
    CHECK(!f.book.bestBid());  // market remainder never rests
    CHECK(!f.book.bestAsk());
}

BOTH_TEST(market_sell_sweeps_bids_from_highest) {
    Fix<BookT> f;
    f.book.addLimit(1, Side::Buy, 98, 4);
    f.book.addLimit(2, Side::Buy, 99, 4);
    f.book.addLimit(3, Side::Buy, 97, 4);
    f.book.addMarket(9, Side::Sell, 9);
    CHECK(f.take() == (std::vector<Trade>{T(9, 2, 99, 4, Side::Sell), T(9, 1, 98, 4, Side::Sell),
                                         T(9, 3, 97, 1, Side::Sell)}));
    CHECK(f.book.bestBid() == 97);
}

BOTH_TEST(cancel_middle_of_queue_preserves_others) {
    Fix<BookT> f;
    f.book.addLimit(1, Side::Buy, 100, 1);
    f.book.addLimit(2, Side::Buy, 100, 2);
    f.book.addLimit(3, Side::Buy, 100, 3);
    CHECK(f.book.cancel(2) == Status::Ok);
    CHECK(queueAt(f.book, Side::Buy, 100) == (std::vector<RestingOrder>{{1, 1}, {3, 3}}));
    CHECK(f.book.volumeAt(Side::Buy, 100) == 4);
    CHECK(f.book.cancel(2) == Status::UnknownId);
}

BOTH_TEST(cancel_best_level_moves_best_price) {
    Fix<BookT> f;
    f.book.addLimit(1, Side::Buy, 100, 1);
    f.book.addLimit(2, Side::Buy, 90, 1);
    f.book.addLimit(3, Side::Sell, 110, 1);
    f.book.addLimit(4, Side::Sell, 150, 1);
    f.book.cancel(1);
    f.book.cancel(3);
    CHECK(f.book.bestBid() == 90);
    CHECK(f.book.bestAsk() == 150);
    f.book.cancel(2);
    f.book.cancel(4);
    CHECK(!f.book.bestBid());
    CHECK(!f.book.bestAsk());
    CHECK(f.book.orderCount() == 0);
}

BOTH_TEST(modify_quantity_down_keeps_priority) {
    Fix<BookT> f;
    f.book.addLimit(1, Side::Sell, 100, 10);
    f.book.addLimit(2, Side::Sell, 100, 10);
    CHECK(f.book.modify(1, 100, 4) == Status::Ok);
    CHECK(f.book.volumeAt(Side::Sell, 100) == 14);
    f.book.addMarket(9, Side::Buy, 6);
    CHECK(f.take() == (std::vector<Trade>{T(9, 1, 100, 4, Side::Buy), T(9, 2, 100, 2, Side::Buy)}));
}

BOTH_TEST(modify_same_price_same_qty_is_noop_and_keeps_priority) {
    Fix<BookT> f;
    f.book.addLimit(1, Side::Sell, 100, 10);
    f.book.addLimit(2, Side::Sell, 100, 10);
    CHECK(f.book.modify(1, 100, 10) == Status::Ok);
    CHECK(queueAt(f.book, Side::Sell, 100) == (std::vector<RestingOrder>{{1, 10}, {2, 10}}));
}

BOTH_TEST(modify_quantity_up_loses_priority) {
    Fix<BookT> f;
    f.book.addLimit(1, Side::Sell, 100, 10);
    f.book.addLimit(2, Side::Sell, 100, 10);
    CHECK(f.book.modify(1, 100, 20) == Status::Ok);
    CHECK(queueAt(f.book, Side::Sell, 100) == (std::vector<RestingOrder>{{2, 10}, {1, 20}}));
    f.book.addMarket(9, Side::Buy, 6);
    CHECK(f.take() == std::vector<Trade>{T(9, 2, 100, 6, Side::Buy)});
}

BOTH_TEST(modify_price_moves_order_and_loses_priority) {
    Fix<BookT> f;
    f.book.addLimit(1, Side::Buy, 100, 5);
    f.book.addLimit(2, Side::Buy, 99, 5);
    CHECK(f.book.modify(1, 99, 5) == Status::Ok);  // moves behind order 2
    CHECK(f.book.bestBid() == 99);
    CHECK(queueAt(f.book, Side::Buy, 99) == (std::vector<RestingOrder>{{2, 5}, {1, 5}}));
    CHECK(f.book.modify(1, 99, 3) == Status::Ok);  // now a pure reduction: stays behind 2
    CHECK(queueAt(f.book, Side::Buy, 99) == (std::vector<RestingOrder>{{2, 5}, {1, 3}}));
}

BOTH_TEST(modify_to_crossing_price_executes_as_taker) {
    Fix<BookT> f;
    f.book.addLimit(1, Side::Buy, 99, 10);
    f.book.addLimit(2, Side::Sell, 101, 4);
    CHECK(f.book.modify(1, 101, 10) == Status::Ok);
    CHECK(f.take() == std::vector<Trade>{T(1, 2, 101, 4, Side::Buy)});
    CHECK(f.book.bestBid() == 101);
    CHECK(f.book.volumeAt(Side::Buy, 101) == 6);
    CHECK(!f.book.bestAsk());
}

BOTH_TEST(modify_invalid_args_leave_order_untouched) {
    Fix<BookT> f;
    f.book.addLimit(1, Side::Buy, 100, 5);
    CHECK(f.book.modify(1, 100, 0) == Status::InvalidQty);
    CHECK(f.book.modify(1, -3, 5) == Status::InvalidPrice);
    CHECK(f.book.modify(1, kTicks, 5) == Status::InvalidPrice);
    CHECK(queueAt(f.book, Side::Buy, 100) == (std::vector<RestingOrder>{{1, 5}}));
}

BOTH_TEST(id_can_be_reused_after_fill_or_cancel) {
    Fix<BookT> f;
    f.book.addLimit(1, Side::Buy, 100, 5);
    f.book.addMarket(2, Side::Sell, 5);
    CHECK(f.book.addLimit(1, Side::Buy, 100, 5) == Status::Ok);
    CHECK(f.book.cancel(1) == Status::Ok);
    CHECK(f.book.addLimit(1, Side::Sell, 200, 5) == Status::Ok);
    CHECK(f.book.bestAsk() == 200);
}

BOTH_TEST(extreme_tick_prices_work) {
    Fix<BookT> f;
    CHECK(f.book.addLimit(1, Side::Buy, 0, 5) == Status::Ok);
    CHECK(f.book.addLimit(2, Side::Sell, kTicks - 1, 5) == Status::Ok);
    CHECK(f.book.bestBid() == 0);
    CHECK(f.book.bestAsk() == kTicks - 1);
    f.book.addMarket(3, Side::Buy, 5);
    f.book.addMarket(4, Side::Sell, 5);
    CHECK(f.take() == (std::vector<Trade>{T(3, 2, kTicks - 1, 5, Side::Buy), T(4, 1, 0, 5, Side::Sell)}));
    CHECK(f.book.orderCount() == 0);
}

BOTH_TEST(sweep_across_far_apart_levels) {
    Fix<BookT> f;
    f.book.addLimit(1, Side::Sell, 3, 1);
    f.book.addLimit(2, Side::Sell, 500, 1);
    f.book.addLimit(3, Side::Sell, 997, 1);
    f.book.addMarket(9, Side::Buy, 2);
    CHECK(f.book.bestAsk() == 997);
    f.book.addMarket(10, Side::Buy, 1);
    CHECK(!f.book.bestAsk());
    CHECK(f.take().size() == 3);
}

// ---- engine-only ----

TEST(engine_pool_full_rejects_then_recovers) {
    Collect c;
    OrderBook<Collect> book(kTicks, 4, c);
    for (OrderId i = 1; i <= 4; ++i) CHECK(book.addLimit(i, Side::Buy, 100, 1) == Status::Ok);
    CHECK(book.addLimit(5, Side::Buy, 100, 1) == Status::PoolFull);
    CHECK(book.cancel(2) == Status::Ok);
    CHECK(book.addLimit(5, Side::Buy, 100, 1) == Status::Ok);
    CHECK(book.modify(5, 101, 3) == Status::Ok);  // cancel-replace with a full pool still works
    CHECK(book.orderCount() == 4);
    CHECK(book.checkInvariants());
}

TEST(engine_pool_slots_are_recycled) {
    Collect c;
    OrderBook<Collect> book(kTicks, 8, c);
    for (OrderId i = 1; i <= 100000; ++i) {
        REQUIRE(book.addLimit(i, i % 2 ? Side::Buy : Side::Sell, i % 2 ? 100 : 200, 1) == Status::Ok);
        REQUIRE(book.cancel(i) == Status::Ok);
    }
    CHECK(book.orderCount() == 0);
    CHECK(book.checkInvariants());
}

TEST(engine_invariants_hold_through_scenario) {
    Collect c;
    OrderBook<Collect> book(kTicks, kCap, c);
    std::string why;
    book.addLimit(1, Side::Buy, 10, 5);
    book.addLimit(2, Side::Buy, 10, 5);
    book.addLimit(3, Side::Sell, 20, 5);
    CHECK(book.checkInvariants(&why));
    book.modify(1, 25, 7);  // crosses, sweeps 3, rests 2 at 25
    CHECK(book.checkInvariants(&why));
    book.addMarket(4, Side::Sell, 100);
    CHECK(book.checkInvariants(&why));
    CHECK(book.orderCount() == 0);
}
