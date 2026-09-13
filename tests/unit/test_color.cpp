/* test_color.cpp — YUV→RGB 的矩阵/范围选择（chooseYuvColor）
 *
 * 这个函数决定"用哪套系数把 YUV 还原成 RGB"。它错了不会崩、不会报错，
 * 只会让画面**偏色或发灰** —— 属于最不容易被发现的一类缺陷，所以逐条钉住：
 * 标注优先、未标注时的分辨率规则、范围的三种取值、以及"取值必须与
 * libswscale 的 SWS_CS_* 一致"这个契约（player.cpp 会把返回值直接喂给
 * sws_getCoefficients()，这里对不上就会静默用错矩阵）。
 */
#include "harness.h"
#include "util.h"

/* FFmpeg 的 AVColorSpace / AVColorRange 取值（本文件刻意不 include FFmpeg 头） */
enum {
    CS_RGB = 0, CS_BT709 = 1, CS_UNSPECIFIED = 2, CS_FCC = 4,
    CS_BT470BG = 5, CS_SMPTE170M = 6, CS_SMPTE240M = 7,
    CS_YCGCO = 8, CS_BT2020_NCL = 9, CS_BT2020_CL = 10,
};
enum { CR_UNSPECIFIED = 0, CR_MPEG = 1, CR_JPEG = 2 };

PP_TEST(color, matrix_values_match_libswscale) {
    /* 契约：这些取值就是 SWS_CS_ITU709/ITU601/FCC/SMPTE240M/BT2020，
     * player.cpp 直接拿它去 sws_getCoefficients()。改这里必须同步改那边。 */
    PP_EQ((int)pp::YUV_MATRIX_BT709, 1);
    PP_EQ((int)pp::YUV_MATRIX_FCC, 4);
    PP_EQ((int)pp::YUV_MATRIX_BT601, 5);
    PP_EQ((int)pp::YUV_MATRIX_SMPTE240M, 7);
    PP_EQ((int)pp::YUV_MATRIX_BT2020, 9);
}

PP_TEST(color, signaled_matrix_is_honored) {
    /* 素材标注什么就用什么，**不受分辨率影响** —— 这是本函数存在的全部意义。 */
    PP_EQ(pp::chooseYuvColor(CS_BT709, CR_MPEG, 320, 240).matrix, (int)pp::YUV_MATRIX_BT709);
    PP_EQ(pp::chooseYuvColor(CS_BT470BG, CR_MPEG, 1920, 1080).matrix, (int)pp::YUV_MATRIX_BT601);
    PP_EQ(pp::chooseYuvColor(CS_SMPTE170M, CR_MPEG, 1920, 1080).matrix, (int)pp::YUV_MATRIX_BT601);
    PP_EQ(pp::chooseYuvColor(CS_SMPTE240M, CR_MPEG, 720, 576).matrix, (int)pp::YUV_MATRIX_SMPTE240M);
    PP_EQ(pp::chooseYuvColor(CS_FCC, CR_MPEG, 720, 480).matrix, (int)pp::YUV_MATRIX_FCC);
    PP_EQ(pp::chooseYuvColor(CS_BT2020_NCL, CR_MPEG, 3840, 2160).matrix, (int)pp::YUV_MATRIX_BT2020);
    PP_EQ(pp::chooseYuvColor(CS_BT2020_CL, CR_MPEG, 3840, 2160).matrix, (int)pp::YUV_MATRIX_BT2020);

    /* 回归用例：SD 尺寸 + 标注 BT.709。旧的"按分辨率猜"会给出 BT.601（错的）。 */
    pp::YuvColorChoice c = pp::chooseYuvColor(CS_BT709, CR_MPEG, 320, 240);
    PP_EQ(c.matrix, (int)pp::YUV_MATRIX_BT709);
    PP_CHECK_MSG(c.matrix != (int)pp::YUV_MATRIX_BT601,
                 "SD 尺寸不能覆盖素材自己标注的 BT.709");
}

PP_TEST(color, unspecified_uses_resolution_rule) {
    /* 未标注时按 FFmpeg vf_scale 的 auto 规则：> 576 行算 HD(709)，否则 SD(601)。
     * 边界 576 本身属于 SD。 */
    PP_EQ(pp::chooseYuvColor(CS_UNSPECIFIED, CR_MPEG, 720, 480).matrix, (int)pp::YUV_MATRIX_BT601);
    PP_EQ(pp::chooseYuvColor(CS_UNSPECIFIED, CR_MPEG, 720, 576).matrix, (int)pp::YUV_MATRIX_BT601);
    PP_EQ(pp::chooseYuvColor(CS_UNSPECIFIED, CR_MPEG, 1280, 577).matrix, (int)pp::YUV_MATRIX_BT709);
    PP_EQ(pp::chooseYuvColor(CS_UNSPECIFIED, CR_MPEG, 1920, 1080).matrix, (int)pp::YUV_MATRIX_BT709);
    PP_EQ(pp::chooseYuvColor(CS_UNSPECIFIED, CR_MPEG, 3840, 2160).matrix, (int)pp::YUV_MATRIX_BT709);
}

PP_TEST(color, rgb_and_exotic_spaces_fall_back) {
    /* RGB 源（cs=0）与 YCGCO（8）都没有 YUV 矩阵可言，交给分辨率规则兜底，
     * 不能返回一个"看起来有值"的假矩阵。 */
    PP_EQ(pp::chooseYuvColor(CS_RGB, CR_MPEG, 1920, 1080).matrix, (int)pp::YUV_MATRIX_BT709);
    PP_EQ(pp::chooseYuvColor(CS_YCGCO, CR_MPEG, 320, 240).matrix, (int)pp::YUV_MATRIX_BT601);
    /* 越界/未知值也不能崩 */
    PP_EQ(pp::chooseYuvColor(999, CR_MPEG, 320, 240).matrix, (int)pp::YUV_MATRIX_BT601);
    PP_EQ(pp::chooseYuvColor(-5, CR_MPEG, 1920, 1080).matrix, (int)pp::YUV_MATRIX_BT709);
}

PP_TEST(color, range_mapping) {
    PP_EQ(pp::chooseYuvColor(CS_BT709, CR_MPEG, 1920, 1080).fullRange, 0);
    PP_EQ(pp::chooseYuvColor(CS_BT709, CR_JPEG, 1920, 1080).fullRange, 1);
    /* 未标注 → 有限范围（swscale 自己的默认，也是绝大多数素材的情况） */
    PP_EQ(pp::chooseYuvColor(CS_BT709, CR_UNSPECIFIED, 1920, 1080).fullRange, 0);
    /* 未知值也按有限范围兜底，不能出现"第三种范围" */
    PP_EQ(pp::chooseYuvColor(CS_BT709, 77, 1920, 1080).fullRange, 0);
}

PP_TEST(color, matrix_and_range_are_independent) {
    /* 矩阵与范围是两个独立轴：任何组合都要能各自选对 */
    pp::YuvColorChoice a = pp::chooseYuvColor(CS_BT709, CR_JPEG, 320, 240);
    PP_EQ(a.matrix, (int)pp::YUV_MATRIX_BT709);
    PP_EQ(a.fullRange, 1);

    pp::YuvColorChoice b = pp::chooseYuvColor(CS_BT470BG, CR_MPEG, 1920, 1080);
    PP_EQ(b.matrix, (int)pp::YUV_MATRIX_BT601);
    PP_EQ(b.fullRange, 0);
}

/* P2-L1：原色 + 传输函数决定 HDR，并映射到正确的 SDL_Colorspace。
 * 这些值就是 FFmpeg 的 AVColorPrimaries / AVColorTransferCharacteristic，
 * player.cpp 读出来后透传给 sdlColorspaceFor()。 */
enum {
    PRI_UNSPECIFIED = 2, PRI_BT2020 = 9,
    TRC_UNSPECIFIED = 2, TRC_SMPTE2084 = 16, TRC_ARIB_STD_B67 = 18,
};

PP_TEST(color, sdr_matrix_range_maps_as_before) {
    /* P2-L1 前的行为必须完全不变：SDR 只按矩阵+范围映射。 */
    pp::YuvColorChoice c;
    c.matrix = (int)pp::YUV_MATRIX_BT709; c.fullRange = 0;
    int fb = 0;
    PP_EQ(pp::sdlColorspaceFor(c, &fb), (int)pp::PP_SDL_CS_BT709_LIMITED);
    PP_EQ(fb, 0);
    c.fullRange = 1;
    PP_EQ(pp::sdlColorspaceFor(c, nullptr), (int)pp::PP_SDL_CS_BT709_FULL);
    c.matrix = (int)pp::YUV_MATRIX_BT601; c.fullRange = 0;
    PP_EQ(pp::sdlColorspaceFor(c, &fb), (int)pp::PP_SDL_CS_BT601_LIMITED);
    PP_EQ(fb, 0);
}

PP_TEST(color, hdr10_pq_maps_to_hdr10) {
    /* BT.2020 原色 + PQ 传输 = HDR10（SDL 唯一有枚举的 HDR 传输函数）。 */
    pp::YuvColorChoice c;
    c.matrix = (int)pp::YUV_MATRIX_BT2020; c.fullRange = 1;
    c.primaries = PRI_BT2020; c.trc = TRC_SMPTE2084;
    int fb = 0;
    PP_EQ(pp::sdlColorspaceFor(c, &fb), (int)pp::PP_SDL_CS_HDR10);
    PP_EQ(fb, 0);
    PP_CHECK_MSG(pp::isHdrColorChoice(c), "BT.2020+PQ 必须判为 HDR");
}

PP_TEST(color, hdr_hlg_maps_to_bt2020_surface) {
    /* BT.2020 + HLG：SDL 没有 HLG 枚举，退回 BT.2020（已知限制，见 util.h 注释）。 */
    pp::YuvColorChoice c;
    c.matrix = (int)pp::YUV_MATRIX_BT2020; c.fullRange = 0;
    c.primaries = PRI_BT2020; c.trc = TRC_ARIB_STD_B67;
    PP_EQ(pp::sdlColorspaceFor(c, nullptr), (int)pp::PP_SDL_CS_BT2020_LIMITED);
    PP_CHECK_MSG(pp::isHdrColorChoice(c), "BT.2020+HLG 必须判为 HDR");
}

PP_TEST(color, bt2020_without_hdr_transfer_is_sdr) {
    /* BT.2020 原色但没标 HDR 传输函数 = 普通 SDR BT.2020（不进 HDR 路径）。 */
    pp::YuvColorChoice c;
    c.matrix = (int)pp::YUV_MATRIX_BT2020; c.fullRange = 0;
    c.primaries = PRI_BT2020; c.trc = TRC_UNSPECIFIED;
    PP_EQ(pp::sdlColorspaceFor(c, nullptr), (int)pp::PP_SDL_CS_BT2020_LIMITED);
    PP_CHECK_MSG(!pp::isHdrColorChoice(c), "无 HDR 传输函数不能判为 HDR");
}

PP_TEST(color, hdr_requires_bt2020_primaries) {
    /* 仅 PQ 传输、但原色不是 BT.2020（非法组合）→ 不判 HDR，按矩阵映射。 */
    pp::YuvColorChoice c;
    c.matrix = (int)pp::YUV_MATRIX_BT709; c.fullRange = 0;
    c.primaries = PRI_UNSPECIFIED; c.trc = TRC_SMPTE2084;
    PP_EQ(pp::sdlColorspaceFor(c, nullptr), (int)pp::PP_SDL_CS_BT709_LIMITED);
    PP_CHECK_MSG(!pp::isHdrColorChoice(c), "非 BT.2020 原色 + PQ 不能判为 HDR");
}

