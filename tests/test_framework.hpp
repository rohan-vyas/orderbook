#pragma once

// Minimal self-contained test runner (no external dependencies).

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace tf {

struct TestCase {
    std::string name;
    void (*fn)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

struct Stats {
    long assertions = 0;
    long failures = 0;
};
inline Stats& stats() {
    static Stats s;
    return s;
}

struct AbortTest : std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline bool registerTest(std::string name, void (*fn)()) {
    registry().push_back({std::move(name), fn});
    return true;
}

inline void report(bool ok, const char* expr, const char* file, int line, bool fatal) {
    ++stats().assertions;
    if (ok) return;
    ++stats().failures;
    std::fprintf(stderr, "    %s:%d: CHECK FAILED: %s\n", file, line, expr);
    if (fatal) throw AbortTest(expr);
}

}  // namespace tf

#define TF_CAT2(a, b) a##b
#define TF_CAT(a, b) TF_CAT2(a, b)

#define TEST(name)                                                                     \
    static void name();                                                                \
    [[maybe_unused]] static const bool TF_CAT(tf_reg_, name) = ::tf::registerTest(#name, name); \
    static void name()

// Defines a scenario templated on the book type and registers it for both the
// optimised engine and the reference implementation.
#define BOTH_TEST(name)                                                                       \
    template <template <class> class BookT>                                                   \
    static void name();                                                                       \
    [[maybe_unused]] static const bool TF_CAT(tf_reg_e_, name) =                              \
        ::tf::registerTest(std::string(#name) + " [engine]", name<::ob::OrderBook>);          \
    [[maybe_unused]] static const bool TF_CAT(tf_reg_r_, name) =                              \
        ::tf::registerTest(std::string(#name) + " [reference]", name<::ob::ReferenceBook>);   \
    template <template <class> class BookT>                                                   \
    static void name()

#define CHECK(cond) ::tf::report(static_cast<bool>(cond), #cond, __FILE__, __LINE__, false)
#define REQUIRE(cond) ::tf::report(static_cast<bool>(cond), #cond, __FILE__, __LINE__, true)
