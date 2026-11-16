#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>

#include "test_framework.hpp"

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int passed = 0, failed = 0;
    for (const auto& t : tf::registry()) {
        if (filter && t.name.find(filter) == std::string::npos) continue;
        const long before = tf::stats().failures;
        const auto t0 = std::chrono::steady_clock::now();
        bool threw = false;
        try {
            t.fn();
        } catch (const tf::AbortTest&) {
            threw = true;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "    unexpected exception: %s\n", e.what());
            ++tf::stats().failures;
            threw = true;
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const bool ok = !threw && tf::stats().failures == before;
        std::printf("[%s] %s (%.1f ms)\n", ok ? "PASS" : "FAIL", t.name.c_str(), ms);
        (ok ? passed : failed)++;
    }
    std::printf("\n%d/%d tests passed, %ld assertions checked, %ld failed assertions\n", passed, passed + failed,
                tf::stats().assertions, tf::stats().failures);
    return failed == 0 && passed > 0 ? 0 : 1;
}
