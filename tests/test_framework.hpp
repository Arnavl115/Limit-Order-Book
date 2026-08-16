#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

// Minimal dependency-free test harness. No external test framework required so
// the project builds offline with a bare MSVC toolchain.

namespace testfw {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

struct Registrar {
    Registrar(std::string name, std::function<void()> fn) {
        registry().push_back({std::move(name), std::move(fn)});
    }
};

inline int failures = 0;
inline const char* current_test = "";

inline void check(bool cond, const char* expr, const char* file, int line) {
    if (!cond) {
        std::printf("  FAIL %s:%d [%s]: %s\n", file, line, current_test, expr);
        ++failures;
    }
}

}  // namespace testfw

#define TEST(name)                                                   \
    static void name();                                              \
    static ::testfw::Registrar name##_reg{#name, name};              \
    static void name()

#define CHECK(expr) ::testfw::check((expr), #expr, __FILE__, __LINE__)

#define RUN()                                                                  \
    int main() {                                                              \
        for (auto& t : ::testfw::registry()) {                                 \
            ::testfw::current_test = t.name.c_str();                            \
            std::printf("[ RUN  ] %s\n", t.name.c_str());                       \
            t.fn();                                                            \
        }                                                                      \
        std::printf("Passed %zu/%zu tests, %d failures\n",                      \
                    ::testfw::registry().size() - (size_t)::testfw::failures,  \
                    ::testfw::registry().size(), ::testfw::failures);           \
        return ::testfw::failures == 0 ? 0 : 1;                                \
    }