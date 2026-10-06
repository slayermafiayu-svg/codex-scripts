// Minimal test harness (no third-party dependency).
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace wmptest {

struct Case {
    const char* name;
    std::function<void()> fn;
};

std::vector<Case>& registry();
int& failures();
const char*& currentCase();

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

inline void report(const char* file, int line, const std::string& msg) {
    ++failures();
    std::fprintf(stderr, "  FAIL %s (%s:%d): %s\n", currentCase(), file, line, msg.c_str());
}

}  // namespace wmptest

#define WMP_CAT2(a, b) a##b
#define WMP_CAT(a, b) WMP_CAT2(a, b)
#define TEST(name)                                                            \
    static void name();                                                       \
    static ::wmptest::Registrar WMP_CAT(reg_, name)(#name, name);             \
    static void name()

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) ::wmptest::report(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                 \
    do {                                                                      \
        const double wmp_a_ = static_cast<double>(a);                         \
        const double wmp_b_ = static_cast<double>(b);                         \
        if (!(std::fabs(wmp_a_ - wmp_b_) <= (tol))) {                         \
            char wmp_buf_[256];                                               \
            std::snprintf(wmp_buf_, sizeof(wmp_buf_), "%s = %.12g, %s = %.12g, tol %.3g", #a, wmp_a_, #b, wmp_b_, \
                          static_cast<double>(tol));                          \
            ::wmptest::report(__FILE__, __LINE__, wmp_buf_);                  \
        }                                                                     \
    } while (0)
