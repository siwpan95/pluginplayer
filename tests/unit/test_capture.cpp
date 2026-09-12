/* test_capture.cpp — 抓屏取证的像素占比工具（rgbaColorRatioInRect）
 *
 * 这个函数是"自检抓屏里到底有没有画面"那条断言的**判据**。它自己错了，
 * 那条断言就会静默失效（要么永远为绿、要么永远为红），比没有断言更糟 ——
 * 所以这里逐条钉住：子区域统计、pitch 带行填充、矩形裁剪、alpha 不参与比较、
 * 非法参数返回 -1（而不是 0，否则"没量到"会被读成"一个都没匹配"）。
 */
#include "harness.h"
#include "util.h"

#include <cstdint>
#include <vector>

namespace {

/* 造一块 pitch 步进、w x h 可见区域的 RGBA 图，可见像素统一填 (r,g,b,a)。
 * 行填充部分刻意填成 0xFF，这样"按 w*4 而不是按 pitch 走"的错实现会读到它。 */
std::vector<uint8_t> makeImage(int w, int h, int pitch,
                               uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    std::vector<uint8_t> buf((size_t)pitch * (size_t)h, 0xFF);
    for (int y = 0; y < h; ++y) {
        uint8_t* row = buf.data() + (size_t)y * (size_t)pitch;
        for (int x = 0; x < w; ++x) {
            row[x * 4 + 0] = r;
            row[x * 4 + 1] = g;
            row[x * 4 + 2] = b;
            row[x * 4 + 3] = a;
        }
    }
    return buf;
}

}  // namespace

PP_TEST(capture, ratio_all_pixels_match) {
    auto img = makeImage(4, 3, 16, 16, 16, 20, 255);
    PP_NEAR(pp::rgbaColorRatioInRect(img.data(), 16, 4, 3, 0, 0, 4, 3, 16, 16, 20), 1.0, 1e-9);
}

PP_TEST(capture, ratio_none_match) {
    /* 纯黑 = YUV 有限范围黑场转出来的结果。它是**合法内容**，
     * 绝不能被当成"清屏色"而把这一项判红。 */
    auto img = makeImage(4, 3, 16, 0, 0, 0, 255);
    PP_NEAR(pp::rgbaColorRatioInRect(img.data(), 16, 4, 3, 0, 0, 4, 3, 16, 16, 20), 0.0, 1e-9);
}

PP_TEST(capture, alpha_not_part_of_comparison) {
    /* 只比 RGB：清屏是全不透明，但混合后的像素可能带不同 alpha，
     * 若把 alpha 也算进相等判定，取证会漏掉那些像素。 */
    auto img = makeImage(2, 2, 8, 16, 16, 20, 0);
    PP_NEAR(pp::rgbaColorRatioInRect(img.data(), 8, 2, 2, 0, 0, 2, 2, 16, 16, 20), 1.0, 1e-9);
}

PP_TEST(capture, only_counts_inside_rect) {
    /* 左半边清屏色、右半边黑。只统计哪一半，结论必须跟着变 ——
     * 否则"画面矩形"参数就是摆设，对整窗统计会被 OSD 的占比淹掉。 */
    auto img = makeImage(8, 2, 32, 16, 16, 20, 255);
    for (int y = 0; y < 2; ++y) {
        uint8_t* row = img.data() + (size_t)y * 32;
        for (int x = 4; x < 8; ++x) {
            row[x * 4 + 0] = 0;
            row[x * 4 + 1] = 0;
            row[x * 4 + 2] = 0;
        }
    }
    PP_NEAR(pp::rgbaColorRatioInRect(img.data(), 32, 8, 2, 0, 0, 4, 2, 16, 16, 20), 1.0, 1e-9);
    PP_NEAR(pp::rgbaColorRatioInRect(img.data(), 32, 8, 2, 4, 0, 4, 2, 16, 16, 20), 0.0, 1e-9);

    /* 是 50% 的时候也要给对：这是阈值判定的实际工作点 */
    PP_NEAR(pp::rgbaColorRatioInRect(img.data(), 32, 8, 2, 0, 0, 8, 2, 16, 16, 20), 0.5, 1e-9);
}

PP_TEST(capture, honors_pitch_with_row_padding) {
    /* SDL 的读回缓冲行距常常大于 w*4。若实现按 w*4 走，就会读到行填充的 0xFF
     * 并把占比算错 —— 这个用例就是为那个错法准备的。 */
    auto img = makeImage(4, 3, 24, 16, 16, 20, 255);
    PP_NEAR(pp::rgbaColorRatioInRect(img.data(), 24, 4, 3, 0, 0, 4, 3, 16, 16, 20), 1.0, 1e-9);
}

PP_TEST(capture, rect_clipped_to_image) {
    /* 矩形超出图像：只统计交集（这里 4x3 里能落下的 2x2），越界部分不能算进去 */
    auto img = makeImage(4, 3, 16, 16, 16, 20, 255);
    PP_NEAR(pp::rgbaColorRatioInRect(img.data(), 16, 4, 3, 2, 1, 8, 8, 16, 16, 20), 1.0, 1e-9);

    /* 负起点也要正确裁剪 */
    PP_NEAR(pp::rgbaColorRatioInRect(img.data(), 16, 4, 3, -2, -1, 3, 2, 16, 16, 20), 1.0, 1e-9);
}

PP_TEST(capture, invalid_params_return_negative) {
    auto img = makeImage(4, 3, 16, 16, 16, 20, 255);
    /* 空指针 */
    PP_CHECK(pp::rgbaColorRatioInRect(nullptr, 16, 4, 3, 0, 0, 4, 3, 16, 16, 20) < 0.0);
    /* 尺寸非正 */
    PP_CHECK(pp::rgbaColorRatioInRect(img.data(), 16, 0, 3, 0, 0, 4, 3, 16, 16, 20) < 0.0);
    PP_CHECK(pp::rgbaColorRatioInRect(img.data(), 16, 4, -3, 0, 0, 4, 3, 16, 16, 20) < 0.0);
    /* pitch 容不下一行：配错了要大声失败，不能"猜着走" */
    PP_CHECK(pp::rgbaColorRatioInRect(img.data(), 15, 4, 3, 0, 0, 4, 3, 16, 16, 20) < 0.0);
    /* 矩形宽高非正 */
    PP_CHECK(pp::rgbaColorRatioInRect(img.data(), 16, 4, 3, 0, 0, 0, 3, 16, 16, 20) < 0.0);
    PP_CHECK(pp::rgbaColorRatioInRect(img.data(), 16, 4, 3, 0, 0, 4, 0, 16, 16, 20) < 0.0);
    /* 矩形完全落在图像之外：没量到 ≠ 一个都没匹配 */
    PP_CHECK(pp::rgbaColorRatioInRect(img.data(), 16, 4, 3, 4, 0, 2, 2, 16, 16, 20) < 0.0);
    PP_CHECK(pp::rgbaColorRatioInRect(img.data(), 16, 4, 3, 0, 3, 2, 2, 16, 16, 20) < 0.0);
}

PP_TEST(capture, full_mismatch_is_zero_not_negative) {
    /* 边界对照：占比真的是 0 时必须返回 0（而不是 -1），
     * 否则调用方无法区分"量到 0%"与"没量到"。 */
    auto img = makeImage(3, 1, 12, 17, 16, 20, 255);   /* 只差一个字节 */
    double v = pp::rgbaColorRatioInRect(img.data(), 12, 3, 1, 0, 0, 3, 1, 16, 16, 20);
    PP_CHECK(v == 0.0);
}
