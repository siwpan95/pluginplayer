/* main.cpp — 单元测试入口（注册表遍历 + 过滤 + 汇总）
 *
 * 与项目其它测试入口保持同一口径：
 *   · 每条用例一行 [PASS]/[FAIL]
 *   · 通过数**只算通过**，跳过单列（跳过 != 通过）
 *   · 退出码 0 = 全通过，1 = 有失败
 */
#include "harness.h"

#include <exception>

int pp_unit_main(int argc, char** argv) {
    const char* filter = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--filter") == 0 && i + 1 < argc) filter = argv[++i];
        else if (std::strcmp(argv[i], "--list") == 0) {
            for (const auto& c : pp_test::registry())
                std::printf("%s.%s\n", c.suite, c.name);
            return 0;
        } else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            std::printf("用法: %s [--filter 子串] [--list]\n", argv[0]);
            return 0;
        } else {
            std::printf("未知参数: %s（用 --help 看用法）\n", argv[i]);
            return 2;
        }
    }

    int passed = 0, failed = 0, skipped = 0;
    for (const auto& c : pp_test::registry()) {
        std::string full = std::string(c.suite) + "." + c.name;
        if (filter && full.find(filter) == std::string::npos) { ++skipped; continue; }
        int fails = 0;
        /* 用例里抛异常也算失败，而不是让整个进程崩掉 —— 一个用例崩溃不该吃掉后面的结果 */
        try {
            c.fn(&fails);
        } catch (const std::exception& e) {
            std::printf("        [x] 抛出异常: %s\n", e.what());
            ++fails;
        } catch (...) {
            std::printf("        [x] 抛出未知异常\n");
            ++fails;
        }
        if (fails == 0) {
            std::printf("[PASS] %s\n", full.c_str());
            ++passed;
        } else {
            std::printf("[FAIL] %s   （%d 处断言失败）\n", full.c_str(), fails);
            ++failed;
        }
    }

    std::printf("\n==== 单元测试: %d/%d 项通过，跳过 %d 项，失败 %d 项 ====\n",
                passed, passed + failed, skipped, failed);
    if (skipped > 0 && filter)
        std::printf("（--filter %s 过滤掉了 %d 项，不计入通过数）\n", filter, skipped);
    return failed ? 1 : 0;
}

int main(int argc, char** argv) { return pp_unit_main(argc, argv); }
