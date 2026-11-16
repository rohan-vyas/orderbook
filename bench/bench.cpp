// Throughput and latency benchmark: optimised engine vs reference implementation,
// on the same pre-generated synthetic order stream.
//
// Usage: bench [--ops N] [--prefill N] [--reps N] [--latency-reps N] [--seed N]

#include <sys/resource.h>
#include <sys/sysctl.h>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "orderbook/order_book.hpp"
#include "orderbook/reference_book.hpp"
#include "orderbook/workload.hpp"

using namespace ob;
using Clock = std::chrono::steady_clock;

#ifndef OB_BUILD_FLAGS
#define OB_BUILD_FLAGS "unknown"
#endif

namespace {

struct Sink {
    std::uint64_t trades = 0, qty = 0, checksum = 0;
    void onTrade(const Trade& t) {
        ++trades;
        qty += t.qty;
        checksum = checksum * 1000003u ^ (t.taker * 31 + t.maker * 17 + static_cast<std::uint64_t>(t.price) * 7 + t.qty);
    }
};

std::string sysctlString(const char* name) {
    char buf[256] = {};
    std::size_t len = sizeof(buf);
    if (sysctlbyname(name, buf, &len, nullptr, 0) != 0) return "?";
    return buf;
}
std::uint64_t sysctlU64(const char* name) {
    std::uint64_t v = 0;
    std::size_t len = sizeof(v);
    if (sysctlbyname(name, &v, &len, nullptr, 0) != 0) return 0;
    return v;
}
std::string loadAvg() {
    double l[3] = {};
    if (getloadavg(l, 3) != 3) return "?";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.1f %.1f %.1f", l[0], l[1], l[2]);
    return buf;
}

struct Usage {
    double cpuSec;
    long invCsw;
};
Usage usageNow() {
    rusage r{};
    getrusage(RUSAGE_SELF, &r);
    return {static_cast<double>(r.ru_utime.tv_sec + r.ru_stime.tv_sec) +
                static_cast<double>(r.ru_utime.tv_usec + r.ru_stime.tv_usec) * 1e-6,
            r.ru_nivcsw};
}

template <template <class> class BookT>
std::unique_ptr<BookT<Sink>> freshBook(const WorkloadParams& p, Sink& s, const Workload& w) {
    auto b = std::make_unique<BookT<Sink>>(p.numTicks, 1u << 20, s);
    for (const Op& op : w.prefill) apply(*b, op);
    s = Sink{};
    return b;
}

struct ThroughputRun {
    double wallSec, cpuSec;
    long invCsw;
};

template <template <class> class BookT>
ThroughputRun runThroughput(const WorkloadParams& p, const Workload& w, Sink& sink) {
    auto book = freshBook<BookT>(p, sink, w);
    const Usage u0 = usageNow();
    const auto t0 = Clock::now();
    for (const Op& op : w.ops) apply(*book, op);
    const auto t1 = Clock::now();
    const Usage u1 = usageNow();
    return {std::chrono::duration<double>(t1 - t0).count(), u1.cpuSec - u0.cpuSec, u1.invCsw - u0.invCsw};
}

double pct(const std::vector<std::uint32_t>& sorted, double q) {
    if (sorted.empty()) return 0;
    std::size_t idx = static_cast<std::size_t>(q * static_cast<double>(sorted.size()));
    if (idx >= sorted.size()) idx = sorted.size() - 1;
    return sorted[idx];
}

struct LatencyStats {
    double p50, p90, p99, p999, max, mean;
    std::size_t n;
};

LatencyStats stats(std::vector<std::uint32_t> v) {
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (auto x : v) sum += x;
    return {pct(v, 0.50), pct(v, 0.90), pct(v, 0.99), pct(v, 0.999), v.empty() ? 0 : double(v.back()),
            v.empty() ? 0 : sum / double(v.size()), v.size()};
}

// Latency classes: what the op actually did (from the generator's simulation is not
// needed; we classify from the op and whether it produced trades).
enum Cls { kPassiveAdd, kCancel, kMarketable, kModify, kAll, kNumCls };
const char* clsName[kNumCls] = {"passive limit add", "cancel", "marketable (traded)", "modify", "all ops"};

template <template <class> class BookT>
std::vector<LatencyStats> runLatency(const WorkloadParams& p, const Workload& w) {
    Sink sink;
    auto book = freshBook<BookT>(p, sink, w);
    std::vector<std::uint32_t> lat(w.ops.size());
    std::vector<std::uint8_t> cls(w.ops.size());
    for (std::size_t i = 0; i < w.ops.size(); ++i) {
        const Op& op = w.ops[i];
        const std::uint64_t before = sink.trades;
        const auto t0 = Clock::now();
        apply(*book, op);
        const auto t1 = Clock::now();
        lat[i] = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        Cls c = kPassiveAdd;
        if (sink.trades != before) c = kMarketable;
        else if (op.type == OpType::Cancel) c = kCancel;
        else if (op.type == OpType::Modify) c = kModify;
        else if (op.type == OpType::Market) c = kMarketable;  // market order into an empty side
        cls[i] = static_cast<std::uint8_t>(c);
    }
    std::vector<std::vector<std::uint32_t>> by(kNumCls);
    for (std::size_t i = 0; i < lat.size(); ++i) by[cls[i]].push_back(lat[i]);
    by[kAll] = lat;
    std::vector<LatencyStats> out;
    for (int c = 0; c < kNumCls; ++c) out.push_back(stats(by[static_cast<std::size_t>(c)]));
    return out;
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

std::size_t argOr(int argc, char** argv, const char* name, std::size_t def) {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], name) == 0) return static_cast<std::size_t>(std::strtoull(argv[i + 1], nullptr, 10));
    return def;
}

}  // namespace

int main(int argc, char** argv) {
    WorkloadParams p;
    p.ops = argOr(argc, argv, "--ops", 5'000'000);
    p.prefill = argOr(argc, argv, "--prefill", 10'000);
    p.seed = argOr(argc, argv, "--seed", 42);
    const std::size_t reps = argOr(argc, argv, "--reps", 5);
    const std::size_t latReps = argOr(argc, argv, "--latency-reps", 3);

    std::printf("== Environment ==\n");
    std::printf("cpu: %s, logical cpus: %" PRIu64 ", memory: %" PRIu64 " bytes\n",
                sysctlString("machdep.cpu.brand_string").c_str(), sysctlU64("hw.ncpu"), sysctlU64("hw.memsize"));
    std::printf("os: %s %s\n", sysctlString("kern.ostype").c_str(), sysctlString("kern.osrelease").c_str());
    std::printf("compiler: clang %s, flags: %s\n", __clang_version__, OB_BUILD_FLAGS);
    std::printf("load average at start: %s\n", loadAvg().c_str());

    // Timer overhead: back-to-back steady_clock::now() calls.
    {
        std::vector<std::uint32_t> d(2'000'000);
        for (auto& x : d) {
            const auto a = Clock::now();
            const auto b = Clock::now();
            x = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
        }
        const LatencyStats s = stats(d);
        std::printf("steady_clock back-to-back delta: p50 %.0f ns, p99 %.0f ns, mean %.1f ns (period %lld/%lld s)\n",
                    s.p50, s.p99, s.mean, static_cast<long long>(Clock::period::num),
                    static_cast<long long>(Clock::period::den));
    }

    std::printf("\n== Workload (synthetic) ==\n");
    const auto g0 = Clock::now();
    const Workload w = generateWorkload(p);
    std::printf("seed %" PRIu64 ", prefill %zu passive orders (untimed), measured ops %zu, tick range [0,%d), mid %d\n",
                p.seed, w.prefill.size(), w.ops.size(), p.numTicks, p.mid);
    auto pctOf = [&](std::size_t n) { return 100.0 * double(n) / double(w.ops.size()); };
    std::printf("mix: passive limit adds %zu (%.1f%%), cancels %zu (%.1f%%), market orders %zu (%.1f%%), "
                "marketable limits %zu (%.1f%%), modifies %zu (%.1f%%)\n",
                w.limitAdds, pctOf(w.limitAdds), w.cancels, pctOf(w.cancels), w.markets, pctOf(w.markets),
                w.marketableLimits, pctOf(w.marketableLimits), w.modifies, pctOf(w.modifies));
    std::printf("ops that produced >=1 trade: %zu (%.1f%%), trades: %zu, resting orders: mean %.0f, final %zu\n",
                w.opsThatTraded, pctOf(w.opsThatTraded), w.trades, w.meanRestingOrders, w.finalRestingOrders);
    std::printf("(generation took %.2f s, not part of any measurement)\n",
                std::chrono::duration<double>(Clock::now() - g0).count());

    // Cross-check: both implementations must produce identical results on this stream.
    {
        Sink se, sr;
        auto e = freshBook<OrderBook>(p, se, w);
        auto r = freshBook<ReferenceBook>(p, sr, w);
        for (const Op& op : w.ops) {
            if (apply(*e, op) != apply(*r, op)) {
                std::printf("MISMATCH in status\n");
                return 1;
            }
        }
        const bool same = se.trades == sr.trades && se.qty == sr.qty && se.checksum == sr.checksum &&
                          e->snapshot() == r->snapshot();
        std::printf("cross-check engine vs reference: %s (%" PRIu64 " trades, %" PRIu64 " shares)\n",
                    same ? "identical" : "MISMATCH", se.trades, se.qty);
        if (!same) return 1;
    }

    std::printf("\n== Throughput (%zu reps, wall clock over %zu ops, no per-op timing) ==\n", reps, w.ops.size());
    std::vector<double> eWall, rWall, eCpu, rCpu;
    for (std::size_t i = 0; i < reps; ++i) {
        Sink s1, s2;
        const ThroughputRun e = runThroughput<OrderBook>(p, w, s1);
        const ThroughputRun r = runThroughput<ReferenceBook>(p, w, s2);
        const double n = double(w.ops.size());
        std::printf("rep %zu: engine %.2f M ops/s (cpu-time %.2f M ops/s, %ld invol. ctx sw) | reference %.2f M ops/s "
                    "(cpu-time %.2f M ops/s, %ld invol. ctx sw)\n",
                    i + 1, n / e.wallSec / 1e6, n / e.cpuSec / 1e6, e.invCsw, n / r.wallSec / 1e6, n / r.cpuSec / 1e6,
                    r.invCsw);
        eWall.push_back(n / e.wallSec);
        rWall.push_back(n / r.wallSec);
        eCpu.push_back(n / e.cpuSec);
        rCpu.push_back(n / r.cpuSec);
    }
    auto mn = [](const std::vector<double>& v) { return *std::min_element(v.begin(), v.end()); };
    auto mx = [](const std::vector<double>& v) { return *std::max_element(v.begin(), v.end()); };
    std::printf("SUMMARY engine:    median %.2f M ops/s (min %.2f, max %.2f); mean %.1f ns/op at median\n",
                median(eWall) / 1e6, mn(eWall) / 1e6, mx(eWall) / 1e6, 1e9 / median(eWall));
    std::printf("SUMMARY reference: median %.2f M ops/s (min %.2f, max %.2f); mean %.1f ns/op at median\n",
                median(rWall) / 1e6, mn(rWall) / 1e6, mx(rWall) / 1e6, 1e9 / median(rWall));
    std::printf("SUMMARY speedup (median/median): %.2fx\n", median(eWall) / median(rWall));

    std::printf("\n== Latency (per-op steady_clock, includes timer overhead; %zu reps, medians reported) ==\n",
                latReps);
    std::vector<std::vector<std::vector<LatencyStats>>> all(2);
    for (std::size_t i = 0; i < latReps; ++i) {
        all[0].push_back(runLatency<OrderBook>(p, w));
        all[1].push_back(runLatency<ReferenceBook>(p, w));
    }
    const char* implName[2] = {"engine", "reference"};
    std::printf("%-10s %-22s %10s %8s %8s %8s %9s %10s %9s\n", "impl", "op class", "count", "p50", "p90", "p99",
                "p99.9", "max", "mean");
    for (int impl = 0; impl < 2; ++impl) {
        for (int c = 0; c < kNumCls; ++c) {
            std::vector<double> v50, v90, v99, v999, vmax, vmean;
            for (const auto& rep : all[static_cast<std::size_t>(impl)]) {
                const LatencyStats& s = rep[static_cast<std::size_t>(c)];
                v50.push_back(s.p50);
                v90.push_back(s.p90);
                v99.push_back(s.p99);
                v999.push_back(s.p999);
                vmax.push_back(s.max);
                vmean.push_back(s.mean);
            }
            const std::size_t n = all[static_cast<std::size_t>(impl)][0][static_cast<std::size_t>(c)].n;
            if (n == 0) continue;
            std::printf("%-10s %-22s %10zu %6.0fns %6.0fns %6.0fns %7.0fns %8.0fns %7.1fns\n", implName[impl],
                        clsName[c], n, median(v50), median(v90), median(v99), median(v999), median(vmax),
                        median(vmean));
        }
    }
    std::printf("\nload average at end: %s\n", loadAvg().c_str());
    return 0;
}
