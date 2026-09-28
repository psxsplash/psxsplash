#pragma once

#include <stdio.h>
#include <string.h>

/**
 * A ~50-line test framework. The project's convention is hand-rolled binary and
 * static_asserts over third-party dependencies, and the host harness has no
 * reason to be the one place that drags in GoogleTest — these tests must build
 * anywhere the engine's own toolchain does.
 */
namespace psxsplash::test {

struct Registry {
    static constexpr int kMaxTests = 128;
    struct Entry {
        const char* name;
        void (*fn)();
    };
    Entry tests[kMaxTests];
    int count = 0;
    int checks = 0;
    int failures = 0;
    const char* current = "";
    bool currentFailed = false;

    static Registry& get() {
        static Registry r;
        return r;
    }
};

struct Register {
    Register(const char* name, void (*fn)()) {
        Registry& r = Registry::get();
        if (r.count < Registry::kMaxTests) {
            r.tests[r.count].name = name;
            r.tests[r.count].fn = fn;
            r.count++;
        }
    }
};

inline void reportFailure(const char* file, int line, const char* expr) {
    Registry& r = Registry::get();
    r.failures++;
    if (!r.currentFailed) {
        printf("\n  FAIL %s\n", r.current);
        r.currentFailed = true;
    }
    printf("    %s:%d: %s\n", file, line, expr);
}

inline bool checkTrue(bool cond, const char* file, int line, const char* expr) {
    Registry::get().checks++;
    if (!cond) {
        reportFailure(file, line, expr);
        return false;
    }
    return true;
}

inline bool checkEqInt(long long a, long long b, const char* file, int line, const char* expr) {
    Registry::get().checks++;
    if (a != b) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s  (%lld != %lld)", expr, a, b);
        reportFailure(file, line, buf);
        return false;
    }
    return true;
}

inline bool checkEqMem(const void* a, const void* b, size_t n, const char* file, int line, const char* expr) {
    Registry::get().checks++;
    if (memcmp(a, b, n) != 0) {
        reportFailure(file, line, expr);
        return false;
    }
    return true;
}

inline int runAll() {
    Registry& r = Registry::get();
    for (int i = 0; i < r.count; i++) {
        r.current = r.tests[i].name;
        r.currentFailed = false;
        r.tests[i].fn();
        if (!r.currentFailed) printf("  ok   %s\n", r.tests[i].name);
    }
    printf("\n%d checks, %d failures across %d tests\n", r.checks, r.failures, r.count);
    return r.failures == 0 ? 0 : 1;
}

}  // namespace psxsplash::test

#define TEST(name)                                                       \
    static void name();                                                  \
    static ::psxsplash::test::Register reg_##name(#name, &name);         \
    static void name()

#define CHECK(expr) ::psxsplash::test::checkTrue((expr), __FILE__, __LINE__, #expr)
#define CHECK_EQ(a, b) ::psxsplash::test::checkEqInt((long long)(a), (long long)(b), __FILE__, __LINE__, #a " == " #b)
#define CHECK_MEM(a, b, n) ::psxsplash::test::checkEqMem((a), (b), (n), __FILE__, __LINE__, "memcmp(" #a ", " #b ")")
