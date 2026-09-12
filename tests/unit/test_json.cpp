/* test_json.cpp — 严格 JSON 取值（util.h 的「严格 JSON 取值」一节）
 *
 * 为什么这些用例值得写：这段代码是从"子串搜索"改成"词法扫描"的，
 * 而旧实现的三类问题都会导致**控制通道的参数走私**：
 *   1. 键名出现在字符串值里也会命中
 *   2. 非法输入被静默当默认值（调用方以为拿到了参数）
 *   3. 重复键无语义（攻击者可以再塞一个同名键）
 * 下面每条断言都对着其中一类。
 */
#include "harness.h"
#include "util.h"

#include <utility>
#include <vector>

PP_TEST(json, parse_object_basic) {
    std::vector<std::pair<std::string, std::string>> kv;
    std::string err;
    PP_CHECK(pp::jsonParseObject("{\"a\":1,\"b\":\"x\"}", &kv, &err));
    PP_EQ(kv.size(), (size_t)2);
    PP_STR_EQ(kv[0].first, "a");
    PP_STR_EQ(kv[0].second, "1");            /* 值以**原文**返回，未反转义 */
    PP_STR_EQ(kv[1].first, "b");
    PP_STR_EQ(kv[1].second, "\"x\"");
    PP_CHECK(err.empty());
}

PP_TEST(json, parse_object_allows_whitespace_and_empty) {
    std::vector<std::pair<std::string, std::string>> kv;
    PP_CHECK(pp::jsonParseObject("  {  }  ", &kv, nullptr));
    PP_CHECK(kv.empty());
    PP_CHECK(pp::jsonParseObject("{ \"a\" : 1 , \"b\" : 2 }", &kv, nullptr));
    PP_EQ(kv.size(), (size_t)2);
    PP_CHECK(pp::jsonParseObject("{\n\t\"a\"\n:\n[1,2]\n}", &kv, nullptr));
    PP_EQ(kv.size(), (size_t)1);
}

PP_TEST(json, parse_object_rejects_duplicate_keys) {
    /* 重复键必须判非法：否则"先出现者胜出"，攻击者多塞一个键就能改变语义 */
    std::vector<std::pair<std::string, std::string>> kv;
    std::string err;
    PP_CHECK_MSG(!pp::jsonParseObject("{\"method\":\"ping\",\"method\":\"bad\"}", &kv, &err),
                 "重复键竟然被接受");
    PP_CHECK(err.find("重复键") != std::string::npos);
    /* 同名键出现在嵌套对象里不算顶层重复 */
    PP_CHECK(pp::jsonParseObject("{\"a\":{\"x\":1},\"b\":{\"x\":2}}", &kv, nullptr));
}

PP_TEST(json, parse_object_rejects_malformed) {
    std::vector<std::pair<std::string, std::string>> kv;
    PP_CHECK(!pp::jsonParseObject("", &kv, nullptr));
    PP_CHECK(!pp::jsonParseObject("[]", &kv, nullptr));             /* 不是对象 */
    PP_CHECK(!pp::jsonParseObject("null", &kv, nullptr));
    PP_CHECK(!pp::jsonParseObject("{\"a\":1", &kv, nullptr));       /* 未闭合 */
    PP_CHECK(!pp::jsonParseObject("{\"a\":1}}", &kv, nullptr));     /* 多余内容 */
    PP_CHECK(!pp::jsonParseObject("{\"a\":1} garbage", &kv, nullptr));
    PP_CHECK(!pp::jsonParseObject("{a:1}", &kv, nullptr));          /* 键没引号 */
    PP_CHECK(!pp::jsonParseObject("{\"a\"1}", &kv, nullptr));       /* 缺冒号 */
    PP_CHECK(!pp::jsonParseObject("{\"a\":}", &kv, nullptr));       /* 值缺失 */
    PP_CHECK(!pp::jsonParseObject("{\"a\":1,}", &kv, nullptr));     /* 尾逗号 */
    PP_CHECK(!pp::jsonParseObject("{\"a\" 1}", &kv, nullptr));
    PP_CHECK(!pp::jsonParseObject("{\"a\":01}", &kv, nullptr));     /* JSON 不允许前导 0 */
    PP_CHECK(!pp::jsonParseObject("{\"a\":+1}", &kv, nullptr));
    PP_CHECK(!pp::jsonParseObject("{\"a\":.5}", &kv, nullptr));
    PP_CHECK(!pp::jsonParseObject("{\"a\":1.}", &kv, nullptr));
    PP_CHECK(!pp::jsonParseObject("{\"a\":tru}", &kv, nullptr));
    /* 未转义的控制字符在字符串里非法 */
    PP_CHECK(!pp::jsonParseObject("{\"a\":\"x\ny\"}", &kv, nullptr));
}

PP_TEST(json, parse_object_rejects_deep_nesting) {
    /* 深度上限 32：没有上限的话递归下降会被一个几千层的数组爆栈 */
    std::string deep(4000, '[');
    deep += "1";
    deep += std::string(4000, ']');
    std::string body = "{\"a\":" + deep + "}";
    std::vector<std::pair<std::string, std::string>> kv;
    PP_CHECK_MSG(!pp::jsonParseObject(body, &kv, nullptr),
                 "超深嵌套没有被拒绝（会有爆栈风险）");
    /* 合法深度不能误杀 */
    std::string ok(30, '[');
    ok += "1" + std::string(30, ']');
    PP_CHECK(pp::jsonParseObject("{\"a\":" + ok + "}", &kv, nullptr));
}

PP_TEST(json, get_string_basics) {
    const std::string doc = "{\"method\":\"player.seek\",\"n\":3}";
    PP_STR_EQ(pp::jsonGetString(doc, "method", "?"), "player.seek");
    PP_STR_EQ(pp::jsonGetString(doc, "missing", "DEF"), "DEF");
    PP_STR_EQ(pp::jsonGetString(doc, nullptr, "DEF"), "DEF");
    /* 类型不符要退回默认值，而不是把 "3" 当字符串给出来 */
    PP_STR_EQ(pp::jsonGetString(doc, "n", "DEF"), "DEF");
}

PP_TEST(json, get_string_unquotes_and_handles_escapes) {
    PP_STR_EQ(pp::jsonGetString("{\"a\":\"x\\\"y\"}", "a", "?"), "x\"y");
    PP_STR_EQ(pp::jsonGetString("{\"a\":\"p\\\\q\"}", "a", "?"), "p\\q");
    PP_STR_EQ(pp::jsonGetString("{\"a\":\"l\\nr\\tt\"}", "a", "?"), "l\nr\tt");
    PP_STR_EQ(pp::jsonGetString("{\"a\":\"\\u0041\\u00e9\"}", "a", "?"),
              "A\xc3\xa9");                       /* BMP 码点转成 UTF-8 */
    PP_STR_EQ(pp::jsonGetString("{\"a\":\"\"}", "a", "?"), "");
}

PP_TEST(json, key_inside_value_must_not_match) {
    /* 旧实现就是栽在这里：用 find("\"id\"") 搜索子串，
     * 于是 {"path":"a\"id\"b","id":7} 会取到字符串值里的那个假 id。 */
    const std::string doc = "{\"path\":\"a\\\"id\\\"b\",\"id\":7}";
    PP_NEAR(pp::jsonGetNumber(doc, "id", -1), 7.0, 1e-9);
    PP_STR_EQ(pp::jsonGetString(doc, "path", "?"), "a\"id\"b");

    /* 键名出现在**另一个键的值**里，取那个键必须拿不到东西 */
    PP_STR_EQ(pp::jsonGetString("{\"x\":\"method\",\"y\":\"ok\"}", "method", "MISS"), "MISS");
    PP_NEAR(pp::jsonGetNumber("{\"x\":\"seconds\",\"seconds\":2}", "seconds", -1), 2.0, 1e-9);
}

PP_TEST(json, get_number_type_strictness) {
    PP_NEAR(pp::jsonGetNumber("{\"v\":1}", "v", -1), 1.0, 1e-9);
    PP_NEAR(pp::jsonGetNumber("{\"v\":-2.5}", "v", -1), -2.5, 1e-9);
    PP_NEAR(pp::jsonGetNumber("{\"v\":1e3}", "v", -1), 1000.0, 1e-9);
    PP_NEAR(pp::jsonGetNumber("{\"v\":1.25E-2}", "v", -1), 0.0125, 1e-9);
    PP_NEAR(pp::jsonGetNumber("{\"v\":0}", "v", -1), 0.0, 1e-9);
    /* 字符串不算数字（否则 "12abc" 这种就能当参数） */
    PP_NEAR(pp::jsonGetNumber("{\"v\":\"5\"}", "v", -1), -1.0, 1e-9);
    /* 不存在的键 → 默认值 */
    PP_NEAR(pp::jsonGetNumber("{\"v\":1}", "other", 42.0), 42.0, 1e-9);
    PP_NEAR(pp::jsonGetNumber("{\"v\":true}", "v", -1), -1.0, 1e-9);
    PP_NEAR(pp::jsonGetNumber("{\"v\":null}", "v", -1), -1.0, 1e-9);
    /* 整个文档非法 → 默认值（不能"尽力而为"地取半个） */
    PP_NEAR(pp::jsonGetNumber("{\"v\":1", "v", -1), -1.0, 1e-9);
}

PP_TEST(json, get_string_array) {
    std::vector<std::string> a;
    PP_CHECK(pp::jsonGetStringArray("{\"args\":[]}", "args", &a));
    PP_CHECK(a.empty());

    PP_CHECK(pp::jsonGetStringArray("{\"args\":[\"gray\"]}", "args", &a));
    PP_EQ(a.size(), (size_t)1);
    PP_STR_EQ(a[0], "gray");

    PP_CHECK(pp::jsonGetStringArray("{\"args\":[\"a\",\"b c\",\"\"]}", "args", &a));
    PP_EQ(a.size(), (size_t)3);
    PP_STR_EQ(a[1], "b c");
    PP_STR_EQ(a[2], "");

    /* 元素不是字符串（或混入非字符串）必须整体判非法 —— 这正是旧实现出错的地方：
     * 手工找引号配对，遇到转义引号或数字元素就会错乱。 */
    PP_CHECK(!pp::jsonGetStringArray("{\"args\":[1]}", "args", &a));
    PP_CHECK(!pp::jsonGetStringArray("{\"args\":[\"a\",2]}", "args", &a));
    PP_CHECK(!pp::jsonGetStringArray("{\"args\":[true]}", "args", &a));
    PP_CHECK(!pp::jsonGetStringArray("{\"args\":{\"x\":1}}", "args", &a));
    PP_CHECK(!pp::jsonGetStringArray("{\"args\":\"gray\"}", "args", &a));
    PP_CHECK(!pp::jsonGetStringArray("{\"other\":[]}", "args", &a));
    PP_CHECK(!pp::jsonGetStringArray("{\"args\":[\"a\",]}", "args", &a));
    /* 缺键时 out 必须被清空（调用方可能复用同一个 vector） */
    a.push_back("stale");
    PP_CHECK(!pp::jsonGetStringArray("{\"x\":1}", "args", &a));
    PP_CHECK_MSG(a.empty(), "取数组失败时应清空输出，否则调用方会读到上一次的残留");
}

PP_TEST(json, escape_roundtrip) {
    PP_STR_EQ(pp::jsonEscape("plain"), "plain");
    PP_STR_EQ(pp::jsonEscape("a\"b"), "a\\\"b");
    PP_STR_EQ(pp::jsonEscape("a\\b"), "a\\\\b");
    PP_STR_EQ(pp::jsonEscape("a\nb"), "a\\nb");
    PP_STR_EQ(pp::jsonEscape("a\rb"), "a\\rb");
    PP_STR_EQ(pp::jsonEscape("a\tb"), "a\\tb");
    /* 控制字符走 \u00XX */
    PP_STR_EQ(pp::jsonEscape(std::string("a\x01")), "a\\u0001");
    /* 高位字节（UTF-8）原样透出，不转义 —— 客户端按 UTF-8 解析即可 */
    const std::string zh = "\xe4\xb8\xad";
    PP_STR_EQ(pp::jsonEscape(zh), zh);

    /* 转义后的结果重新解析必须回到原值（真正的不变量） */
    const std::string raw = "q\"\\\n\t\r\x02";
    std::string doc = "{\"v\":\"" + pp::jsonEscape(raw) + "\"}";
    PP_STR_EQ(pp::jsonGetString(doc, "v", "?"), raw);
}

PP_TEST(json, key_injection_cannot_forge_extra_keys) {
    /* 攻击面：把键名塞进值里，试图让"键名即值"的取值逻辑多认出一个键 */
    const std::string doc = "{\"args\":[\"\\\",\\\"evil\\\":1\"],\"name\":\"x\"}";
    std::vector<std::pair<std::string, std::string>> kv;
    PP_CHECK(pp::jsonParseObject(doc, &kv, nullptr));
    PP_EQ(kv.size(), (size_t)2);                    /* 仍然只有两个顶层键 */
    std::vector<std::string> a;
    PP_CHECK(pp::jsonGetStringArray(doc, "args", &a));
    PP_EQ(a.size(), (size_t)1);
    PP_STR_EQ(a[0], "\",\"evil\":1");               /* 整体是一个字符串元素 */
}
