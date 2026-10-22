// A deliberately tiny test harness: no dependency, one executable.
//
//   TEST(suite, name) { CHECK(cond); CHECK_EQ(a, b); }
//
// `onyx-unit-tests` runs every test; `onyx-unit-tests <suite>` runs one
// suite (ctest registers one entry per suite). A failed CHECK prints the
// file, line and expression and marks the test failed; the run continues.
#pragma once

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace onyx_test {

struct test_case {
    const char * suite;
    const char * name;
    std::function<void()> fn;
};

inline std::vector<test_case> & registry() {
    static std::vector<test_case> tests;
    return tests;
}

inline int & failures() {
    static int n = 0;
    return n;
}

struct registrar {
    registrar(const char * suite, const char * name, std::function<void()> fn) {
        registry().push_back({suite, name, std::move(fn)});
    }
};

template <typename A, typename B>
void check_eq(const A & a, const B & b, const char * ea, const char * eb, const char * file, int line) {
    if (a == b) {
        return;
    }
    std::ostringstream os;
    os << a << " != " << b;
    fprintf(stderr, "  %s:%d: CHECK_EQ(%s, %s) failed: %s\n", file, line, ea, eb, os.str().c_str());
    failures()++;
}

} // namespace onyx_test

#define ONYX_CAT2(a, b) a##b
#define ONYX_CAT(a, b) ONYX_CAT2(a, b)

#define TEST(suite, name)                                                                     \
    static void ONYX_CAT(test_, ONYX_CAT(suite, ONYX_CAT(_, name)))();                        \
    static onyx_test::registrar ONYX_CAT(reg_, ONYX_CAT(suite, ONYX_CAT(_, name)))(           \
        #suite, #name, ONYX_CAT(test_, ONYX_CAT(suite, ONYX_CAT(_, name))));                  \
    static void ONYX_CAT(test_, ONYX_CAT(suite, ONYX_CAT(_, name)))()

#define CHECK(cond)                                                                           \
    do {                                                                                      \
        if (!(cond)) {                                                                        \
            fprintf(stderr, "  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);        \
            onyx_test::failures()++;                                                          \
        }                                                                                     \
    } while (0)

#define CHECK_EQ(a, b) onyx_test::check_eq((a), (b), #a, #b, __FILE__, __LINE__)

#define CHECK_THROWS(expr)                                                                    \
    do {                                                                                      \
        bool thrown_ = false;                                                                 \
        try { (void) (expr); } catch (...) { thrown_ = true; }                                \
        if (!thrown_) {                                                                       \
            fprintf(stderr, "  %s:%d: CHECK_THROWS(%s) did not throw\n", __FILE__, __LINE__, #expr); \
            onyx_test::failures()++;                                                          \
        }                                                                                     \
    } while (0)
