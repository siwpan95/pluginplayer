/* test_string.cpp — 字符串工具（util.h 的「字符串」一节）
 *
 * 这些函数是日志、配置解析、插件文件名过滤的基础，出错的后果是**静默的**：
 * 例如 endsWith 大小写敏感会让 Windows 上 .DLL 过滤器漏掉一半插件。
 */
#include "harness.h"
#include "util.h"

PP_TEST(string, trim) {
    PP_STR_EQ(pp::trim("  a b  "), "a b");
    PP_STR_EQ(pp::trim("\t\r\n x \n\r\t"), "x");
    PP_STR_EQ(pp::trim(""), "");
    PP_STR_EQ(pp::trim("     "), "");
    PP_STR_EQ(pp::trim("\t\r\n"), "");
    PP_STR_EQ(pp::trim("abc"), "abc");          /* 无空白：原样 */
    /* 只掐首尾，中间的空格必须保留（配置值里可能真的有） */
    PP_STR_EQ(pp::trim("  a  b  "), "a  b");
    /* 全角空格不是 ASCII 空白，不应被去掉（避免误改中文配置值） */
    PP_CHECK(!pp::trim("\u3000x\u3000").empty());
}

PP_TEST(string, lower) {
    PP_STR_EQ(pp::lower("ABC"), "abc");
    PP_STR_EQ(pp::lower("aBc123"), "abc123");
    PP_STR_EQ(pp::lower(""), "");
    PP_STR_EQ(pp::lower("already"), "already");
    PP_STR_EQ(pp::lower("A/B\\C.TXT"), "a/b\\c.txt");
    /* 高位字节（UTF-8 中文）必须原样保留 —— tolower 只该动 ASCII */
    const std::string zh = "\xe4\xb8\xad\xe6\x96\x87";
    PP_STR_EQ(pp::lower(zh), zh);
}

PP_TEST(string, endsWith_is_case_insensitive) {
    /* 注意：这是**大小写不敏感**的，因为插件目录用 "*.dll" 过滤，
     * 而 Windows 上文件可能是 .DLL —— 大小写敏感会漏掉插件。 */
    PP_CHECK(pp::endsWith("a.dll", ".dll"));
    PP_CHECK(pp::endsWith("a.DLL", ".dll"));
    PP_CHECK(pp::endsWith("a.Dll", ".DLL"));
    PP_CHECK(!pp::endsWith("a.dl", ".dll"));
    PP_CHECK(!pp::endsWith("dll", ".dll"));
    PP_CHECK(pp::endsWith("x.dll", ""));            /* 空后缀：恒真 */
    PP_CHECK(!pp::endsWith("", ".dll"));            /* 空串配非空后缀：假 */
    PP_CHECK(pp::endsWith("", ""));                 /* 都空：真 */
    PP_CHECK(pp::endsWith(".dll", ".dll"));         /* 整串就是后缀 */
}

PP_TEST(string, split_keeps_empty_parts) {
    /* split 的契约：结果至少 1 个元素；空段保留（否则 "a,,b" 会静默丢一个字段） */
    auto v = pp::split("a,b,c", ',');
    PP_EQ(v.size(), (size_t)3);
    PP_STR_EQ(v[0], "a"); PP_STR_EQ(v[1], "b"); PP_STR_EQ(v[2], "c");

    v = pp::split("", ',');
    PP_EQ(v.size(), (size_t)1);
    PP_STR_EQ(v[0], "");

    v = pp::split("a,,b", ',');
    PP_EQ(v.size(), (size_t)3);
    PP_STR_EQ(v[1], "");

    v = pp::split("a,b,", ',');
    PP_EQ(v.size(), (size_t)3);
    PP_STR_EQ(v[2], "");

    v = pp::split(",", ',');
    PP_EQ(v.size(), (size_t)2);
    PP_STR_EQ(v[0], ""); PP_STR_EQ(v[1], "");

    v = pp::split("nosep", ',');
    PP_EQ(v.size(), (size_t)1);
    PP_STR_EQ(v[0], "nosep");

    /* 多字符分隔符不存在这个概念：只有单个字符 */
    v = pp::split("a;b", ',');
    PP_EQ(v.size(), (size_t)1);
}

PP_TEST(string, format_printf_style) {
    PP_STR_EQ(pp::format("%d-%s", 42, "x"), "42-x");
    PP_STR_EQ(pp::format("%.2f", 1.25), "1.25");   /* 用二进制可精确表示的数，别用 1.005 那种 */
    PP_STR_EQ(pp::format("%s", ""), "");
    PP_STR_EQ(pp::format("no-args"), "no-args");
    /* 超长输入不该崩（内部缓冲有上限，只要求不越界、返回可用字符串） */
    std::string big(5000, 'a');
    std::string r = pp::format("%s", big.c_str());
    PP_CHECK(!r.empty());
    PP_CHECK(r.size() <= big.size());
}

PP_TEST(string, fmtTime) {
    PP_STR_EQ(pp::fmtTime(0), "00:00:00");
    PP_STR_EQ(pp::fmtTime(59.9), "00:00:59");
    PP_STR_EQ(pp::fmtTime(60), "00:01:00");
    PP_STR_EQ(pp::fmtTime(3600), "01:00:00");
    PP_STR_EQ(pp::fmtTime(3661), "01:01:01");
    /* 负数与非数都是"未知"，必须给占位符而不是乱码或负数时间 */
    PP_STR_EQ(pp::fmtTime(-1), "--:--:--");
    const double nan_v = std::nan("");
    PP_STR_EQ(pp::fmtTime(nan_v), "--:--:--");
    /* withMs：只到 0.1 秒精度 */
    PP_STR_EQ(pp::fmtTime(61.25, true), "01:01.2");
    PP_STR_EQ(pp::fmtTime(-1, true), "--:--.-");
    PP_STR_EQ(pp::fmtTime(3661.5, true), "1:01:01.5");
}

PP_TEST(string, fmtBytes) {
    PP_STR_EQ(pp::fmtBytes(0), "0 B");
    PP_STR_EQ(pp::fmtBytes(512), "512 B");
    PP_STR_EQ(pp::fmtBytes(1024), "1.0 KB");
    PP_STR_EQ(pp::fmtBytes(1536), "1.5 KB");
    PP_STR_EQ(pp::fmtBytes(1024ULL * 1024), "1.0 MB");
    PP_STR_EQ(pp::fmtBytes(1024ULL * 1024 * 1024), "1.0 GB");
    /* 极大值不该越界读单位表（表只有 5 项，到 TB 封顶） */
    PP_CHECK(pp::fmtBytes(~0ULL).find("TB") != std::string::npos);
}

PP_TEST(string, pathJoin) {
    PP_STR_EQ(pp::pathJoin("a", "b"), "a/b");
    PP_STR_EQ(pp::pathJoin("a/", "b"), "a/b");
    PP_STR_EQ(pp::pathJoin("a\\", "b"), "a\\b");     /* 已有分隔符就不再补 */
    PP_STR_EQ(pp::pathJoin("", "b"), "b");
    PP_STR_EQ(pp::pathJoin("a", ""), "a");
    PP_STR_EQ(pp::pathJoin("", ""), "");
    PP_STR_EQ(pp::pathJoin("C:/x", "y.dll"), "C:/x/y.dll");
    PP_STR_EQ(pp::pathJoin("C:\\x", "y.dll"), "C:\\x/y.dll");
}

PP_TEST(string, baseName_dirName) {
    PP_STR_EQ(pp::baseName("a/b/c.dll"), "c.dll");
    PP_STR_EQ(pp::baseName("C:\\a\\c.dll"), "c.dll");
    PP_STR_EQ(pp::baseName("c.dll"), "c.dll");
    PP_STR_EQ(pp::baseName(""), "");
    PP_STR_EQ(pp::baseName("a/b/"), "");            /* 以分隔符结尾：最后一段是空 */
    PP_STR_EQ(pp::baseName("/"), "");

    PP_STR_EQ(pp::dirName("a/b/c.dll"), "a/b");
    PP_STR_EQ(pp::dirName("C:\\a\\c.dll"), "C:\\a");
    PP_STR_EQ(pp::dirName("c.dll"), "");            /* 无目录成分 */
    PP_STR_EQ(pp::dirName(""), "");
}

PP_TEST(string, log_level_names_are_fixed_width) {
    /* levelName 的输出会进日志行 "[HH:MM:SS][LEVEL] ..."，
     * 长度不齐会让日志列错位 —— 所以 INFO/WARN 后面有补位空格，这是刻意的。 */
    PP_EQ(pp::Log::levelName(pp::L_TRACE).size(), (size_t)5);
    PP_EQ(pp::Log::levelName(pp::L_DEBUG).size(), (size_t)5);
    PP_EQ(pp::Log::levelName(pp::L_INFO).size(), (size_t)5);
    PP_EQ(pp::Log::levelName(pp::L_WARN).size(), (size_t)5);
    PP_EQ(pp::Log::levelName(pp::L_ERROR).size(), (size_t)5);
    PP_STR_EQ(pp::Log::levelName(pp::L_INFO), "INFO ");
    PP_STR_EQ(pp::Log::levelName(pp::L_WARN), "WARN ");
    PP_STR_EQ(pp::Log::levelName(pp::L_ERROR), "ERROR");
}
