// Replays an order-flow CSV through the engine and prints a summary.
// Usage: ob_replay FILE [--ticks N]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>

#include "orderbook/order_book.hpp"
#include "orderbook/workload.hpp"

namespace {
struct Summary {
    std::uint64_t trades = 0, volume = 0;
    void onTrade(const ob::Trade& t) {
        ++trades;
        volume += t.qty;
    }
};
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: ob_replay FILE [--ticks N]\n");
        return 2;
    }
    ob::Price ticks = 1 << 16;
    for (int i = 2; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--ticks") == 0) ticks = static_cast<ob::Price>(std::atoi(argv[i + 1]));

    std::ifstream in(argv[1]);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    std::vector<ob::Op> ops;
    std::string err;
    if (!ob::readCsv(in, ops, &err)) {
        std::fprintf(stderr, "parse error: %s\n", err.c_str());
        return 1;
    }

    Summary s;
    auto book = std::make_unique<ob::OrderBook<Summary>>(ticks, 1u << 20, s);
    std::size_t rejected[6] = {};
    const auto t0 = std::chrono::steady_clock::now();
    for (const auto& op : ops) ++rejected[static_cast<int>(ob::apply(*book, op))];
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::printf("replayed %zu ops in %.3f s (%.2f M ops/s, parsing excluded)\n", ops.size(), sec,
                double(ops.size()) / sec / 1e6);
    std::printf("trades %llu, volume %llu\n", static_cast<unsigned long long>(s.trades),
                static_cast<unsigned long long>(s.volume));
    for (int i = 0; i < 6; ++i)
        if (rejected[i]) std::printf("  status %-12s %zu\n", ob::toString(static_cast<ob::Status>(i)), rejected[i]);
    const auto bb = book->bestBid(), ba = book->bestAsk();
    std::printf("final book: %zu resting orders, best bid %s, best ask %s\n", book->orderCount(),
                bb ? std::to_string(*bb).c_str() : "-", ba ? std::to_string(*ba).c_str() : "-");
    return 0;
}
