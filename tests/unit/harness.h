#pragma once
/* harness.h — 极简 C++ 单元测试框架（零第三方依赖）
 *
 * 为什么自己写一个：本项目的既有约定是「不依赖 cmake / make、不引入第三方库」，
 * 测试框架也不该例外。整个框架百来行，够用且看得懂。
 *
 * 用法：
 *     #include "harness.h"
 *     #include "util.h"
 *
 *     PP_TEST(string, trim_basic) {
 *         PP_STR_EQ(pp::trim("  a b  "), "a b");
 *         PP_CHECK(pp::trim("   ").empty());
 *     }
 *
 * 每个测试独立注册、独立报错；断言失败**不中断**该测试（一次能看全所有问题）。
 * 退出码非零 = 有失败。命令行：--filter 子串 只跑名字含该子串的用例。
 *
 * 设计上刻意保持**可移植**：被测的 util / config / eventbus / registry 四个模块
 * 不依赖 SDL / FFmpeg / WinAPI，所以这套测试在 Windows(MinGW) 与 Linux(g++) 上
 * 都能编能跑 —— Gitee Go 的 Ubuntu 构建环境正是靠这一点跑 C++ 单元测试的。
 */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "util.h"   // 仅为 tmpDir() 复用项目自己的 makeDirs/pathJoin

namespace pp_test {

struct Case {
    const char* suite;
    const char* name;
    void (*fn)(int* fail);
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

struct Registrar {
    Registrar(const char* suite, const char* name, void (*fn)(int*)) {
        registry().push_back(Case{suite, name, fn});
    }
};

/* ---- 值转字符串（断言失败时把两边的实际值打出来） ---- */
inline std::string asStr(const std::string& v) { return v; }
inline std::string asStr(const char* v) { return v ? std::string(v) : std::string("(null)"); }
inline std::string asStr(std::string_view v) { return std::string(v); }
inline std::string asStr(bool v) { return v ? "true" : "false"; }
inline std::string asStr(double v) { return pp::format("%.10g", v); }
inline std::string asStr(long double v) { return pp::format("%.10Lg", v); }
inline std::string asStr(int v) { return pp::format("%d", v); }
inline std::string asStr(long v) { return pp::format("%ld", v); }
inline std::string asStr(long long v) { return pp::format("%lld", v); }
inline std::string asStr(unsigned v) { return pp::format("%u", v); }
inline std::string asStr(unsigned long v) { return pp::format("%lu", v); }
/* 注意：不要另外写一个 asStr(size_t) —— MinGW 上 size_t 就是 unsigned long long，
 * Linux(x86-64) 上是 unsigned long，两边各自已有精确重载，多写一个会导致
 * "redefinition"（Windows）或二义性（Linux）。PP_EQ(..., (size_t)3) 能落到
 * 对应的那个重载上。 */
inline std::string asStr(unsigned long long v) { return pp::format("%llu", v); }

/* ---- 相等判定 ----
 * 直接写 `a == b` 有个真实的坑：两个 const char* 比的是**指针**，
 * 于是 PP_EQ("abc", "abc") 会在某些编译器上为假。这里统一按值比。
 */
template <class A, class B>
inline bool eq(const A& a, const B& b) { return a == b; }
inline bool eq(const std::string& a, const char* b) { return a == asStr(b); }
inline bool eq(const char* a, const std::string& b) { return asStr(a) == b; }
inline bool eq(const char* a, const char* b) { return asStr(a) == asStr(b); }
inline bool eq(std::string_view a, const char* b) { return std::string(a) == asStr(b); }
inline bool eq(const char* a, std::string_view b) { return asStr(a) == std::string(b); }

/** 临时目录：跟着 TEMP/TMP/TMPDIR 走，不存在就建。落盘类用例统一用它。 */
inline std::string tmpDir() {
    const char* t = std::getenv("TEMP");
    if (!t || !*t) t = std::getenv("TMP");
    if (!t || !*t) t = std::getenv("TMPDIR");   /* Linux */
    std::string d = (t && *t) ? std::string(t) : std::string(".");
    d = pp::pathJoin(d, "pp_unit_tests");
    pp::makeDirs(d);
    return d;
}

inline std::string tmpPath(const char* name) {
    return pp::pathJoin(tmpDir(), name);
}

/** 在临时目录里造一个内容确定的文件（返回绝对路径）。 */
inline std::string writeTmpFile(const char* name, const std::string& body) {
    std::string p = tmpPath(name);
    FILE* f = std::fopen(p.c_str(), "wb");
    if (f) {
        if (!body.empty()) std::fwrite(body.data(), 1, body.size(), f);
        std::fclose(f);
    }
    return p;
}

inline void removeTmp(const char* name) {
    std::remove(tmpPath(name).c_str());
}

} // namespace pp_test

/* ---- 注册宏 ---- */
#define PP_TEST(suite_, name_)                                                 \
    static void pp_tc_##suite_##_##name_(int* pp_fail);                        \
    static ::pp_test::Registrar pp_tr_##suite_##_##name_(                      \
        #suite_, #name_, &pp_tc_##suite_##_##name_);                           \
    static void pp_tc_##suite_##_##name_(int* pp_fail)

/* ---- 断言宏 ---- */

#define PP_CHECK(cond)                                                         \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("        [x] %s  (%s:%d)\n", #cond, __FILE__, __LINE__); \
            ++(*pp_fail);                                                      \
        }                                                                      \
    } while (0)

#define PP_CHECK_MSG(cond, msg)                                                \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("        [x] %s  -- %s  (%s:%d)\n", #cond, (msg),      \
                        __FILE__, __LINE__);                                   \
            ++(*pp_fail);                                                      \
        }                                                                      \
    } while (0)

#define PP_EQ(a, b)                                                            \
    do {                                                                       \
        auto pp_va = (a);                                                      \
        auto pp_vb = (b);                                                      \
        if (!::pp_test::eq(pp_va, pp_vb)) {                                    \
            std::printf("        [x] %s == %s   实际 \"%s\" vs 期望 \"%s\"  (%s:%d)\n", \
                        #a, #b, ::pp_test::asStr(pp_va).c_str(),               \
                        ::pp_test::asStr(pp_vb).c_str(), __FILE__, __LINE__);  \
            ++(*pp_fail);                                                      \
        }                                                                      \
    } while (0)

/* 字符串专用（其实是 PP_EQ 的别名，留着是为了让意图在调用处一眼可见） */
#define PP_STR_EQ(a, b) PP_EQ(a, b)

#define PP_NEAR(a, b, eps)                                                     \
    do {                                                                       \
        double pp_va = (double)(a), pp_vb = (double)(b);                       \
        if (!(std::fabs(pp_va - pp_vb) <= (eps))) {                            \
            std::printf("        [x] %s ~ %s   实际 %.10g vs 期望 %.10g (tol %.1g) (%s:%d)\n", \
                        #a, #b, pp_va, pp_vb, (double)(eps), __FILE__, __LINE__); \
            ++(*pp_fail);                                                      \
        }                                                                      \
    } while (0)

/* ---- 入口（实现在 tests/unit/main.cpp） ---- */
int pp_unit_main(int argc, char** argv);
