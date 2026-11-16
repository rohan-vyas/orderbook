# orderbook

A single-instrument limit order book and matching engine in C++20, built for low
per-operation latency, with a differential test against a simple reference
implementation and reproducible benchmarks.

Header-only (`include/orderbook/`), no third-party dependencies. Tests use a small
self-contained runner; benchmarks use `std::chrono::steady_clock`.

## Features

- Order types: limit, market (immediate-or-cancel, remainder discarded), cancel, modify.
- Price-time priority. Trades execute at the resting (maker) order's price.
- Partial fills on both sides; a limit order's unfilled remainder rests at its limit.
- Modify uses cancel-replace semantics: the order keeps its queue position only when the
  price is unchanged and the quantity does not increase. Otherwise it goes to the back of
  the queue at the new price, and if the new price crosses, it executes as a taker.
- Trades are emitted synchronously as events to a listener (`void onTrade(const Trade&)`),
  supplied as a template parameter so the call is inlined.
- Status codes for every rejection (`DuplicateId`, `UnknownId`, `InvalidPrice`,
  `InvalidQty`, `PoolFull`); a rejected request never changes the book.

## Design

| Concern | Choice | Cost |
|---|---|---|
| Prices | Integer ticks in `[0, numTicks)`, up to 262,144 ticks | no floating point |
| Price levels | Dense array per side, indexed directly by tick | O(1) level lookup |
| Queue per level | Intrusive doubly linked FIFO of 32-bit pool indices | O(1) append, O(1) unlink |
| Order storage | Fixed pool allocated up front, intrusive free list | O(1) alloc/free, no heap calls on the hot path |
| Order id index | Open-addressing hash map (linear probing, backward-shift delete, Fibonacci hash), sized at construction to load factor <= 0.5 | O(1) expected find/insert/erase, no allocation |
| Best bid / ask | Cached integers | O(1) read |
| Next best level after a level empties | Three-level bitmap of non-empty levels (64 x 64 x 64 bits), `ctz`/`clz` scans | O(1): at most three word scans, independent of the gap |

Operation complexity (F = number of fills an order generates):

| Operation | Engine | Reference (`std::map` + `std::list` + `std::unordered_map`) |
|---|---|---|
| Add limit, no cross | O(1) | O(log L) |
| Add limit / market that crosses | O(F) plus O(1) per emptied level | O(F + levels emptied x log L) |
| Cancel | O(1) | O(log L) |
| Modify (in place or cancel-replace) | O(1), plus O(F) if it crosses | O(log L) |
| Best bid / ask | O(1) | O(1) |

L = number of non-empty price levels.

Memory is allocated only in the constructor: `numTicks` levels per side (24 bytes each),
`maxOrders` pool slots (32 bytes each) and `2 x maxOrders` rounded up to a power of two
id-map slots (16 bytes each). When the pool is full, new resting orders get `PoolFull`
rather than growing storage. The pool free list is LIFO, so recently freed slots (still
in cache) are reused first.

`checkInvariants()` walks the whole book and verifies links, level aggregates, the
bitmaps, the cached best prices, the id index and that the book is not crossed. Tests
call it throughout the differential run.

## Correctness

- **Scenario tests** (`tests/test_order_book.cpp`): empty book, market orders on an empty
  and on a thin book, price priority, time priority, partial fills of maker and taker,
  crossed limit orders (price improvement, sweeping several levels only up to the limit),
  cancels (middle of queue, best level, twice), modifies (reduce keeps priority, increase
  or reprice loses it, reprice into a crossing price trades), invalid inputs, id reuse,
  extreme ticks. Each scenario runs against both the engine and the reference.
- **Component tests**: the price bitmap against `std::set` (200,000 random ops over the
  full 262,144-tick range), the id map against `std::unordered_map` (500,000 random ops),
  CSV round trip.
- **Differential test** (`tests/test_differential.cpp`): 300 random streams x 10,000 ops
  (3,000,000 ops, fixed seeds 1000 to 1299) over three configurations (64-tick book with
  constant crossing, 4,096-tick book clustered around a mid, full 262,144-tick sparse book).
  Streams include duplicate ids, unknown ids, zero quantities, out-of-range and edge
  prices. After every op the two implementations must agree on the returned status, the
  exact trade list, best bid/ask and order count; every 100 ops the full book (every
  level, every order, in queue order) must be identical. Last run: 1,432,241 trades and
  30,000 full-book snapshots compared, 8,400 engine invariant checks. A second
  differential test replays the benchmark generator's stream (310,000 ops including 5%
  modifies, 107,556 trades).

Result: **59/59 tests passed, 2,592,007 assertions, 0 failures** (release build), and the
same 59/59 under `-fsanitize=address,undefined` with no sanitizer reports. LeakSanitizer
is not available on Apple arm64, so leaks are not checked by that run.

## Benchmark results

Measured on this machine, raw output in
[`results/bench_m4max_2026-11-16.txt`](results/bench_m4max_2026-11-16.txt).

- Hardware: Apple M4 Max, 14 logical CPUs, 38,654,705,664 bytes (36 GiB) RAM,
  macOS (Darwin 27.0.0).
- Compiler: Apple clang 21.0.0, `-std=c++20 -O3 -DNDEBUG`, no `-march`/`-mcpu` flags.
- Single thread, no pinning (macOS does not support thread affinity).
- **Machine was heavily loaded** while benchmarking (load average about 450 to 500 on
  14 CPUs from other applications). Each throughput rep also reports CPU-time
  throughput and involuntary context switches; wall-clock and CPU-time figures agreed
  within 1% in the run below. Expect equal or better numbers on an idle machine.

**Workload (synthetic).** Seed 42. 10,000 passive limit orders prefill the book
(untimed), then 5,000,000 measured operations:

- 60.0% passive limit adds: random side, price `mid +/- (1 + Exp(mean 10) ticks)`,
  quantity uniform 1 to 100;
- 30.0% cancels of a uniformly chosen live resting order;
- 5.0% market orders and 5.0% limit orders priced 2 ticks through the opposite best,
  quantity uniform 1 to 300 (1 to 400 while more than 20,000 orders rest, which keeps
  the book at a steady depth).

11.8% of ops produced at least one trade (1,982,654 trades in total). The book held
19,703 resting orders on average. Before timing, the benchmark checks that the engine
and the reference produce identical trades and final book on this stream.

**Throughput** (5 reps, each on a fresh book, wall clock over all 5M ops, no per-op timing):

| Implementation | Median | Min | Max | Mean time per op (at median) |
|---|---|---|---|---|
| Engine | **31.33 M ops/s** | 28.35 M | 32.73 M | 31.9 ns |
| Reference (`std::map` + `std::list`) | 11.35 M ops/s | 10.86 M | 12.00 M | 88.1 ns |

Speedup at the median: 2.76x.

**Latency** (per operation, `steady_clock::now()` before and after each op, median of 3 reps):

| Implementation | Op class | Count | p50 | p90 | p99 | p99.9 | Mean |
|---|---|---|---|---|---|---|---|
| Engine | all ops | 5,000,000 | 42 ns | 84 ns | 167 ns | 250 ns | 49.6 ns |
| Engine | passive limit add | 2,908,828 | 42 ns | 84 ns | 167 ns | 250 ns | 54.7 ns |
| Engine | cancel | 1,499,273 | 41 ns | 84 ns | 167 ns | 250 ns | 39.7 ns |
| Engine | marketable (traded) | 591,899 | 42 ns | 84 ns | 167 ns | 292 ns | 49.4 ns |
| Reference | all ops | 5,000,000 | 83 ns | 125 ns | 292 ns | 459 ns | 83.1 ns |
| Reference | passive limit add | 2,908,828 | 83 ns | 84 ns | 166 ns | 209 ns | 69.5 ns |
| Reference | cancel | 1,499,273 | 83 ns | 125 ns | 250 ns | 375 ns | 87.7 ns |
| Reference | marketable (traded) | 591,899 | 125 ns | 291 ns | 458 ns | 709 ns | 138.5 ns |

How to read the latency table honestly:

- On Apple silicon `steady_clock` ticks at 24 MHz, so every single measurement is a
  multiple of about 41.7 ns. The percentiles are quantised to that grid (42, 84, 125,
  167 ns...). Sub-tick differences, and anything under about 42 ns, are invisible here;
  the throughput figure (31.9 ns mean per op) is the better measure of average cost.
- Each sample includes the cost of one `now()` call; back-to-back calls measured a mean
  of 13.7 ns in the same run.
- Maximums (tens of microseconds) are dominated by OS preemption on the loaded machine
  and are left out of the table; they are in the raw file.

The replay tool on a 1,010,000-op synthetic CSV (5% modifies, `make replay-demo`)
processed 37.03 M ops/s, parsing excluded, in a single run.

## Build, test, benchmark

Requires a C++20 compiler and `make` (CMake is not needed).

```sh
make test          # release build of the test suite, runs all tests
make sanitize      # same tests under -fsanitize=address,undefined
make bench         # 5M-op benchmark; options: ./build/bench --ops N --reps N --latency-reps N --seed N
make replay-demo   # generate a synthetic CSV and replay it through the engine
```

Differential test size can be scaled with `OB_DIFF_STREAMS` and `OB_DIFF_OPS`.

### Synthetic message format

`ob_gen` writes, and `ob_replay` reads, a CSV of order messages. The data is entirely
synthetic (generated from a seeded RNG), not real market data.

```
A,<id>,<B|S>,<price>,<qty>   add limit
M,<id>,<B|S>,<qty>           market
X,<id>                       cancel
R,<id>,<price>,<qty>         modify (cancel-replace)
```

## Layout

```
include/orderbook/order_book.hpp      engine
include/orderbook/price_bitmap.hpp    3-level bitmap of non-empty levels
include/orderbook/id_map.hpp          fixed-capacity open-addressing id index
include/orderbook/reference_book.hpp  std::map/std::list reference (oracle + baseline)
include/orderbook/workload.hpp        synthetic flow generator, CSV I/O
tests/                                scenario, component and differential tests
bench/bench.cpp                       throughput and latency benchmark
tools/ob_gen.cpp, tools/ob_replay.cpp synthetic generator and replay
results/                              raw benchmark output
```

## Limitations

- Single-threaded, single instrument. No networking, no wire protocol (no FIX/ITCH
  decoding), no persistence or recovery.
- All benchmark data is synthetic; real order flow has different price, size and
  cancel distributions.
- Prices must fit a fixed tick range chosen at construction (at most 262,144 ticks).
  Order capacity is fixed; the engine returns `PoolFull` rather than growing.
- No self-trade prevention, no order types beyond limit/market (no stop, iceberg,
  post-only, IOC/FOK flags on limit orders), no auction phases.
- Market order ids are not checked for uniqueness since they never rest.
- Latency was measured with a 24 MHz clock on a busy desktop OS, not with hardware
  timestamps on an isolated core.
