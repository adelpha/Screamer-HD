#pragma once
// Minimal self-registering test framework (no external dependency).
//
//   TEST_CASE("name") { CHECK(a == b); REQUIRE(ptr); }
//
// The runner accepts an optional substring filter: unit_tests framebuffer

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace testing {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

struct State {
    int failures = 0;
};
inline State& state() {
    static State s;
    return s;
}

struct RequireFailed {};

inline void report(const char* file, int line, const char* expr) {
    ++state().failures;
    std::fprintf(stderr, "  %s(%d): FAILED: %s\n", file, line, expr);
}

// Thrown to mark a test as skipped (e.g. game data not present).
struct Skip {
    std::string reason;
};

} // namespace testing

#define TESTING_CAT2(a, b) a##b
#define TESTING_CAT(a, b) TESTING_CAT2(a, b)
#define TEST_CASE(name)                                                                             \
    static void TESTING_CAT(test_fn_, __LINE__)();                                                  \
    static ::testing::Registrar TESTING_CAT(test_reg_, __LINE__)(name, &TESTING_CAT(test_fn_, __LINE__)); \
    static void TESTING_CAT(test_fn_, __LINE__)()

#define CHECK(expr) \
    do { if (!(expr)) ::testing::report(__FILE__, __LINE__, #expr); } while (0)
#define REQUIRE(expr) \
    do { if (!(expr)) { ::testing::report(__FILE__, __LINE__, #expr); throw ::testing::RequireFailed{}; } } while (0)
#define SKIP(reason) throw ::testing::Skip{reason}
