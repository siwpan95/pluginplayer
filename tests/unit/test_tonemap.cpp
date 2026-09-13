/* test_tonemap.cpp — HDR→SDR 色调映射纯数学（video_tonemap 插件 / tonemap.h）
 *
 * 这类函数错了不会崩、不会报错，只会让 HDR 画面"过曝 / 发灰 / 暗部死黑"——
 * 属于最不容易被发现的一类缺陷，所以用已知输入逐条钉住：
 *   - PQ EOTF 的端点、与 OETF 的往返一致性、单调性
 *   - HLG OETF+OOTF 逆的端点与已知中灰值
 *   - BT.2390 族 EETF 的端点/单调/超界滚降
 *   - sRGB piecewise 的端点与已知值
 *   - gamut 软钳的界内不变式
 *   - 端到端 processPixel：strength=0 透传、已知 PQ/HLG 字节 → 期望 SDR 字节、单调性
 *
 * 本文件刻意不 include SDL/FFmpeg 头：tonemap.h 是纯数学（仅 <cmath>），
 * 这套用例在 Windows(MinGW) 与 Linux(g++) 上都能编能跑（Gitee Go 的 Ubuntu 环境）。
 */
#include "harness.h"
#include "../../src/plugins/video_tonemap/tonemap.h"

#include <cmath>

/* PQ OETF（前向编码），仅用于往返校验：L（线性光 0..1，1=10000 nits）→ 编码 0..1 */
static double pqOetf(double L) {
    double e = std::pow(L, tonemap::PQ_M1);
    return std::pow((tonemap::PQ_C1 + tonemap::PQ_C2 * e) / (1.0 + tonemap::PQ_C3 * e),
                    tonemap::PQ_M2);
}

PP_TEST(tonemap, pq_endpoints) {
    PP_NEAR(tonemap::pqToLinear(0.0), 0.0, 1e-12);
    PP_NEAR(tonemap::pqToLinear(1.0), 1.0, 1e-12);
}

PP_TEST(tonemap, pq_roundtrip_with_oetf) {
    /* 往返：encode→decode 应回到原线性光。覆盖 0.01 nits ~ 10000 nits 全动态范围。 */
    const double levels[] = {1e-6, 1e-4, 0.001, 0.01, 0.1, 0.5, 0.9, 0.9999};
    for (double L : levels) {
        double back = tonemap::pqToLinear(pqOetf(L));
        double tol = std::fmax(1e-7, L * 1e-4);
        PP_NEAR(back, L, tol);
    }
}

PP_TEST(tonemap, pq_monotonic) {
    PP_CHECK(tonemap::pqToLinear(0.2) < tonemap::pqToLinear(0.5));
    PP_CHECK(tonemap::pqToLinear(0.5) < tonemap::pqToLinear(0.8));
    PP_CHECK(tonemap::pqToLinear(0.8) < tonemap::pqToLinear(0.99));
}

PP_TEST(tonemap, hlg_endpoints_and_midtone) {
    PP_NEAR(tonemap::hlgToLinear(0.0), 0.0, 1e-12);
    /* 满码 = 1000 nits 峰值 → 10000-nits 尺度下 = 0.1 */
    PP_NEAR(tonemap::hlgToLinear(1.0), 0.1, 1e-12);
    /* 半码（E'=0.5 → 场景光 1/12 → OOTF^1.2 → ×0.1）：已知解析值 */
    PP_NEAR(tonemap::hlgToLinear(0.5), 0.0050718, 1e-4);
    PP_CHECK(tonemap::hlgToLinear(0.3) < tonemap::hlgToLinear(0.5));
    PP_CHECK(tonemap::hlgToLinear(0.5) < tonemap::hlgToLinear(0.8));
}

PP_TEST(tonemap, eetf_known_values_and_bounds) {
    /* Ln=0→0、Ln=1→1（峰值映射到 SDR 白） */
    PP_NEAR(tonemap::eetf(0.0), 0.0, 1e-12);
    PP_NEAR(tonemap::eetf(1.0), 1.0, 1e-12);
    /* 已知值：k=0.18 时 Ln=0.1 → 0.1×1.18/0.28 */
    PP_NEAR(tonemap::eetf(0.1, 0.18), 0.4214286, 1e-5);
    /* 单调 */
    PP_CHECK(tonemap::eetf(0.1) < tonemap::eetf(0.5));
    PP_CHECK(tonemap::eetf(0.5) < tonemap::eetf(0.9));
    /* 超界输入（源峰值归一后 Ln>1）不爆：软滚降，仍有上界 */
    double v = tonemap::eetf(10.0, 0.18);
    PP_CHECK(v > 1.0);
    PP_CHECK(v < 1.3);
}

PP_TEST(tonemap, srgb_eotf_inverse) {
    PP_NEAR(tonemap::linearToSrgb(0.0), 0.0, 1e-12);
    PP_NEAR(tonemap::linearToSrgb(1.0), 1.0, 1e-12);
    /* 线性段边界：0.0031308 → 12.92×0.0031308 */
    PP_NEAR(tonemap::linearToSrgb(0.0031308), 0.040450, 1e-5);
    /* 幂律段已知值：0.5 → 1.055×0.5^(1/2.4)−0.055 */
    PP_NEAR(tonemap::linearToSrgb(0.5), 0.735356, 1e-5);
    PP_CHECK(tonemap::linearToSrgb(0.2) < tonemap::linearToSrgb(0.5));
}

PP_TEST(tonemap, soft_clip_preserves_in_gamut) {
    double r = 0.3, g = 0.6, b = 0.9;
    tonemap::softClip(r, g, b);
    PP_NEAR(r, 0.3, 1e-12);
    PP_NEAR(g, 0.6, 1e-12);
    PP_NEAR(b, 0.9, 1e-12);
}

PP_TEST(tonemap, soft_clip_brings_out_of_gamut_in) {
    /* 超上限：去饱和后 max ≤ 1 */
    double r = 1.5, g = 0.4, b = 0.2;
    tonemap::softClip(r, g, b);
    PP_CHECK(r <= 1.0 + 1e-9);
    PP_CHECK(g >= 0.0 && g <= 1.0);
    PP_CHECK(b >= 0.0 && b <= 1.0);
    /* 中性灰超上限 → 全 1（不做任何偏色） */
    r = g = b = 1.2;
    tonemap::softClip(r, g, b);
    PP_NEAR(r, 1.0, 1e-12);
    PP_NEAR(g, 1.0, 1e-12);
    PP_NEAR(b, 1.0, 1e-12);
    /* 负值 → 软抬升回界内 */
    r = 0.5; g = -0.3; b = 0.5;
    tonemap::softClip(r, g, b);
    PP_CHECK(g >= -1e-9);
    PP_CHECK(r <= 1.0 && b <= 1.0);
}

static void px(tonemap::Params& p, int transfer, double peak, double strength) {
    p.transfer = transfer;
    p.peakNits = peak;
    p.strength = strength;
    p.knee = tonemap::EETF_KNEE;
}

PP_TEST(tonemap, pipeline_strength_zero_passthrough) {
    unsigned char c[4] = {130, 60, 200, 255};
    unsigned char orig[4] = {130, 60, 200, 255};
    tonemap::Params p;
    px(p, tonemap::TRANSFER_PQ, 1000.0, 0.0);
    tonemap::processPixel(c, p);
    PP_EQ((int)c[0], (int)orig[0]);
    PP_EQ((int)c[1], (int)orig[1]);
    PP_EQ((int)c[2], (int)orig[2]);
    PP_EQ((int)c[3], 255);   /* alpha 不动 */
}

PP_TEST(tonemap, pipeline_pq_known_bytes) {
    /* PQ 码 130/255 ≈ 100 nits（10000 nits 尺度 0.01）；peak=1000 → Ln=0.1 →
     * EETF 0.4214 → sRGB 0.6810 → 字节 174（±3 容忍取整） */
    tonemap::Params p;
    px(p, tonemap::TRANSFER_PQ, 1000.0, 1.0);
    unsigned char c[4] = {130, 130, 130, 255};
    tonemap::processPixel(c, p);
    PP_CHECK((int)c[0] >= 170 && (int)c[0] <= 178);
    PP_EQ((int)c[0], (int)c[1]);
    PP_EQ((int)c[1], (int)c[2]);
    /* 满码白：PQ 10000 nits → Ln=10 → 软钳中性灰 → 255（不染色、不裁黑） */
    unsigned char w[4] = {255, 255, 255, 255};
    tonemap::processPixel(w, p);
    PP_EQ((int)w[0], 255);
    PP_EQ((int)w[1], 255);
    PP_EQ((int)w[2], 255);
}

PP_TEST(tonemap, pipeline_hlg_known_bytes) {
    /* HLG 码 128/255 ≈ 51 nits → 已知值 ≈ 140 */
    tonemap::Params p;
    px(p, tonemap::TRANSFER_HLG, 1000.0, 1.0);
    unsigned char c[4] = {128, 128, 128, 255};
    tonemap::processPixel(c, p);
    PP_CHECK((int)c[0] >= 136 && (int)c[0] <= 144);
    /* HLG 满码（1000 nits 峰）→ Ln=1 → SDR 白 255 */
    unsigned char w[4] = {255, 255, 255, 255};
    tonemap::processPixel(w, p);
    PP_EQ((int)w[0], 255);
}

PP_TEST(tonemap, pipeline_monotonic_and_strength) {
    /* 更亮的输入必须得到更亮（或相等）的输出——色调映射不得翻转亮度序 */
    tonemap::Params p;
    px(p, tonemap::TRANSFER_PQ, 1000.0, 1.0);
    unsigned char lo[4] = {100, 100, 100, 255};
    unsigned char hi[4] = {200, 200, 200, 255};
    tonemap::processPixel(lo, p);
    tonemap::processPixel(hi, p);
    PP_CHECK((int)lo[0] <= (int)hi[0]);
    /* strength 混合：0.5 落在 透传值 与 全量值 之间 */
    unsigned char mid[4] = {130, 130, 130, 255};
    px(p, tonemap::TRANSFER_PQ, 1000.0, 0.5);
    tonemap::processPixel(mid, p);
    PP_CHECK((int)mid[0] > 130);                 /* 比透传亮（100nits→174 全量） */
    PP_CHECK((int)mid[0] < 174 + 3);             /* 比全量暗 */
}
