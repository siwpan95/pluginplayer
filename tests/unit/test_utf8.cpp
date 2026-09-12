/* test_utf8.cpp — UTF-8 解码与东亚全宽判定（P1-a 真字体 OSD 的纯逻辑层）
 *
 * 这些函数错了不会崩、不会报错，只会让**整行 OSD 文字错位或宽度算错** ——
 * 和色彩矩阵是同一类"静默错误"，所以逐条钉住：
 *   · 1~4 字节序列的正确解码（含 ASCII 边界与 4 字节 emoji 区）
 *   · 非法序列必须返回 U+FFFD 且**前进若干字节**（返回 0 会让调用方死循环）
 *   · 超长编码 / 代理区 / 超上界必须判非法（否则会被当成有效码点送进字体）
 *   · 全宽判定覆盖 CJK 汉字、假名、全角 ASCII、全角标点，且不误伤半角
 *
 * 放在 tests/unit 而不是 tests/python：它是 C++ 侧 util 的逻辑，
 * 必须能在 Linux CI（无 SDL / 无字体）上跑。
 */
#include "harness.h"
#include "util.h"

#include <cstring>
#include <string>

PP_TEST(utf8, decode_ascii_and_boundaries) {
    uint32_t cp = 0;
    PP_EQ(pp::utf8Decode("A", 1, &cp), 1);
    PP_EQ((int)cp, 0x41);
    PP_EQ(pp::utf8Decode("\x7F", 1, &cp), 1);
    PP_EQ((int)cp, 0x7F);
    /* 空串 / 长度 0：返回 0（表示串尾），不写 out */
    PP_EQ(pp::utf8Decode(nullptr, 4, &cp), 0);
    PP_EQ(pp::utf8Decode("A", 0, &cp), 0);
}

PP_TEST(utf8, decode_multibyte) {
    uint32_t cp = 0;
    /* 2 字节：U+00E9 (é) */
    PP_EQ(pp::utf8Decode("\xC3\xA9", 2, &cp), 2);
    PP_EQ((int)cp, 0xE9);
    /* 3 字节：U+64AD (播) —— 中文 OSD 的主力区间 */
    PP_EQ(pp::utf8Decode("\xE6\x92\xAD", 3, &cp), 3);
    PP_EQ((int)cp, 0x64AD);
    /* 4 字节：U+1F600 */
    PP_EQ(pp::utf8Decode("\xF0\x9F\x98\x80", 4, &cp), 4);
    PP_EQ((int)cp, 0x1F600);
    /* out 为 nullptr 时只返回长度（调用方可能只想推进游标） */
    PP_EQ(pp::utf8Decode("\xE6\x92\xAD", 3, nullptr), 3);
}

PP_TEST(utf8, invalid_sequences_yield_replacement_and_advance) {
    uint32_t cp = 0;
    /* 孤立续字节 0x80：非法，必须吃 1 字节（返回 0 会死循环） */
    PP_EQ(pp::utf8Decode("\x80", 1, &cp), 1);
    PP_EQ((int)cp, (int)0xFFFD);
    /* 0xC0/0xC1 是超长编码的起始字节，非法 */
    cp = 0;
    PP_EQ(pp::utf8Decode("\xC0\xAF", 2, &cp), 1);
    PP_EQ((int)cp, (int)0xFFFD);
    /* 首字节合法但续字节不合规：只吃掉已确认的前缀，不能越过后续内容 */
    cp = 0;
    PP_EQ(pp::utf8Decode("\xE6\x41\x81", 3, &cp), 1);
    PP_EQ((int)cp, (int)0xFFFD);
    /* 序列被截断（长度不够）：按非法处理并前进，不读越界 */
    cp = 0;
    PP_EQ(pp::utf8Decode("\xE6\x92", 2, &cp), 1);
}

PP_TEST(utf8, overlong_surrogate_and_out_of_range_rejected) {
    uint32_t cp = 0;
    /* 超长编码：0xC1 0x81 名义上表示 'A'，必须判非法 */
    PP_EQ(pp::utf8Decode("\xC1\x81", 2, &cp), 1);
    PP_EQ((int)cp, (int)0xFFFD);
    /* 代理区 U+D800（UTF-8 编码为 ED A0 80）不允许出现在合法 UTF-8 里 */
    cp = 0;
    PP_EQ(pp::utf8Decode("\xED\xA0\x80", 3, &cp), 3);
    PP_EQ((int)cp, (int)0xFFFD);
    /* 超出 U+10FFFF */
    cp = 0;
    PP_EQ(pp::utf8Decode("\xF5\x80\x80\x80", 4, &cp), 1);
    PP_EQ((int)cp, (int)0xFFFD);
}

PP_TEST(utf8, roundtrip_over_string) {
    /* 混合串：ASCII + 中文 + 全角标点，逐字解码能还原出原码点序列 */
    const char* s = "a播\xEF\xBC\x9A";       /* "a播：" */
    size_t len = strlen(s);
    uint32_t want[3] = {0x61, 0x64AD, 0xFF1A};
    size_t i = 0;
    int k = 0;
    while (i < len) {
        uint32_t cp = 0;
        int n = pp::utf8Decode(s + i, len - i, &cp);
        PP_CHECK(n > 0);
        i += (size_t)n;
        PP_EQ((int)cp, (int)want[k]);
        k++;
    }
    PP_EQ(k, 3);
    PP_EQ((int)i, (int)len);
}

PP_TEST(utf8, wide_codepoint_classification) {
    /* 全宽：CJK 汉字、平假名、片假名、全角 ASCII、全角冒号、表意空格 */
    PP_CHECK(pp::isWideCodepoint(0x64AD));   /* 播 */
    PP_CHECK(pp::isWideCodepoint(0x3042));   /* あ */
    PP_CHECK(pp::isWideCodepoint(0x30A2));   /* ア */
    PP_CHECK(pp::isWideCodepoint(0xFF1A));   /* ：全角冒号 */
    PP_CHECK(pp::isWideCodepoint(0xFF21));   /* Ａ 全角 A */
    PP_CHECK(pp::isWideCodepoint(0x3000));   /* 表意空格 */
    PP_CHECK(pp::isWideCodepoint(0xAC00));   /* 가 */
    /* 半宽：ASCII、拉丁扩展、西里尔、半角片假名 */
    PP_CHECK(!pp::isWideCodepoint(0x41));
    PP_CHECK(!pp::isWideCodepoint(0xE9));
    PP_CHECK(!pp::isWideCodepoint(0x0410));
    PP_CHECK(!pp::isWideCodepoint(0xFF66));  /* ｦ 半角片假名（在 FF01..FF60 之外） */
}

PP_TEST(utf8, display_width_counts_fullwidth_as_two) {
    /* 不变式：一个全角字符的显示宽度 = 两个半角字符 */
    PP_EQ(pp::utf8DisplayWidth("ab"), 2);
    PP_EQ(pp::utf8DisplayWidth("播放"), 4);
    PP_EQ(pp::utf8DisplayWidth("a播"), 3);
    PP_EQ(pp::utf8DisplayWidth(""), 0);
    PP_EQ(pp::utf8DisplayWidth(nullptr), 0);
    /* 非法字节也占 1 个宽度单位（替换字符），不能让宽度塌成 0 */
    PP_EQ(pp::utf8DisplayWidth("\x80\x80"), 2);
}

PP_TEST(utf8, decode_does_not_read_past_buffer) {
    /* 故意把缓冲区截短：末尾是不完整的 3 字节序列，解码不能越过 len */
    const char buf[] = {'a', (char)0xE6, (char)0x92};
    uint32_t cp = 0;
    PP_EQ(pp::utf8Decode(buf, 1, &cp), 1);
    PP_EQ((int)cp, 0x61);
    cp = 0;
    PP_EQ(pp::utf8Decode(buf + 1, 2, &cp), 1);   /* 截断：前进 1，得到替换字符 */
    PP_EQ((int)cp, (int)0xFFFD);
}
