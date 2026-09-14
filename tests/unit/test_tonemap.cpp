/* test_tonemap.cpp — HDR→SDR 色调映射纯数学（video_tonemap 插件 / tonemap.h）
 *
 * 这类函数错了不会崩、不会报错，只会让 HDR 画面"过曝 / 发灰 / 暗部死黑"——
 * 属于最不容易被发现的一类缺陷，所以用已知输入逐条钉住：
 *   - PQ EOTF/OETF 的端点、往返一致性、单调性
 *   - HLG OETF+OOTF 逆的端点与已知中灰值
 *   - BT.2390 EETF 的段结构（中段 1:1、肩部端点、单调、输出被钳在显示范围）
 *   - BT.2020→BT.709 矩阵的"中性灰不变"不变式与饱和色降饱和方向
 *   - sRGB piecewise 的端点与已知值
 *   - gamut 软钳的界内不变式
 *   - 端到端 processPixel：strength=0 透传、已知 PQ/HLG 字节 → 期望 SDR 字节、单调性
 *   - auto 判据 wantTonemap 的真值表（含 Phase 2 边界 ④ 的显示端参与，
 *     以及"旧宿主无 video.display 服务时退回只看源"的向后兼容）
 *
 * 期望字节的来源：BT.2390 EETF 照参考实现（natural-harmonia-gropius/mpv-config 的
 * shaders/hdr-toys/tone-mapping/bt2390.glsl 里的 f(x, iw, ib, ow, ob)）另写了一份
 * 独立 Python 移植算出来的，不是把 C++ 的输出抄回来；容差 ±3 留给 8-bit 取整与
 * 实现间的浮点差。
 *
 * 本文件刻意不 include SDL/FFmpeg 头：tonemap.h 是纯数学（仅 <cmath>），
 * 这套用例在 Windows(MinGW) 与 Linux(g++) 上都能编能跑（Gitee Go 的 Ubuntu 环境）。
 */
#include "harness.h"
#include "../../src/plugins/video_tonemap/tonemap.h"

#include <cmath>

PP_TEST(tonemap, pq_endpoints) {
    PP_NEAR(tonemap::pqToLinear(0.0), 0.0, 1e-12);
    PP_NEAR(tonemap::pqToLinear(1.0), 1.0, 1e-12);
    PP_NEAR(tonemap::linearToPq(0.0), 0.0, 1e-12);
    PP_NEAR(tonemap::linearToPq(1.0), 1.0, 1e-12);
}

PP_TEST(tonemap, pq_roundtrip_with_oetf) {
    /* 往返：encode→decode 应回到原线性光。覆盖 0.01 nits ~ 10000 nits 全动态范围。 */
    const double levels[] = {1e-6, 1e-4, 0.001, 0.01, 0.1, 0.5, 0.9, 0.9999};
    for (double L : levels) {
        double back = tonemap::pqToLinear(tonemap::linearToPq(L));
        double tol = std::fmax(1e-7, L * 1e-4);
        PP_NEAR(back, L, tol);
    }
}

PP_TEST(tonemap, pq_monotonic) {
    PP_CHECK(tonemap::pqToLinear(0.2) < tonemap::pqToLinear(0.5));
    PP_CHECK(tonemap::pqToLinear(0.5) < tonemap::pqToLinear(0.8));
    PP_CHECK(tonemap::pqToLinear(0.8) < tonemap::pqToLinear(0.99));
    PP_CHECK(tonemap::linearToPq(0.1) < tonemap::linearToPq(0.5));
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

/* ---- BT.2390 EETF（peak=1000 / refWhite=100 时的锚点，见文件头） ----
 * ib=-0.001  iw=0.7518271  ob=0  ow=0.5080784
 * minLum=0.0013283  maxLum=0.6762222  SS=0.5143334  TS=0.0019925            */
static const double IB = -0.001;
static const double IW = 0.751827096;
static const double OB = 0.0;
static const double OW = 0.508078422;
static const double SS = 0.514333352;   /* 肩部起点，归一域 */
static const double TS = 0.001992489;   /* 趾部终点，归一域 */

PP_TEST(tonemap, eetf_middle_region_is_identity) {
    /* BT.2390 的 [TS, SS] 段是 1:1 直通（中段不做任何压缩）。
     * 注意 E3 趾部抬升会叠加一个极小偏移，它在归一域是 minLum·(1−n)⁴，
     * 折算回 PQ 域最大约 5.5e-4（越暗越大），所以用 1e-3 容差而不是精确相等。 */
    const double probes[] = {0.15, 0.25, 0.30, 0.35};
    for (double e1 : probes) {
        double n = (e1 - IB) / (IW - IB);
        PP_CHECK(n > TS && n < SS);                       /* 确实落在中段 */
        PP_NEAR(tonemap::eetf(e1, IB, IW, OB, OW), e1, 1e-3);
    }
}

PP_TEST(tonemap, eetf_shoulder_and_endpoints) {
    /* 肩部：SS 处 = SS（连续），输入 1.0 处 = maxLum（归一域），再反正归一后 = ow */
    double at_ss = tonemap::eetf(SS * (IW - IB) + IB, IB, IW, OB, OW);
    PP_NEAR(at_ss, SS * (IW - IB) + IB, 4e-4);
    /* 内容峰值（iw）与满码都必须落在显示白 ow 上（钳位生效） */
    PP_NEAR(tonemap::eetf(IW, IB, IW, OB, OW), OW, 1e-9);
    PP_NEAR(tonemap::eetf(1.0, IB, IW, OB, OW), OW, 1e-9);
    /* 超出内容峰值（源不该有，但要有界不爆） */
    PP_NEAR(tonemap::eetf(1.5, IB, IW, OB, OW), OW, 1e-9);
}

PP_TEST(tonemap, eetf_monotonic_and_keeps_sign_of_derivative) {
    /* 单调整调（非严格：肩部顶端趋于平台），且不得出现回折 */
    double prev = -1.0;
    for (int i = 0; i <= 100; ++i) {
        double e1 = i / 100.0;
        double e2 = tonemap::eetf(e1, IB, IW, OB, OW);
        PP_CHECK(e2 >= prev - 1e-12);
        prev = e2;
    }
    /* 切点两侧的斜率都必须是正数（无折角翻转） */
    double a = tonemap::eetf(SS * (IW - IB) + IB - 0.005, IB, IW, OB, OW);
    double b = tonemap::eetf(SS * (IW - IB) + IB + 0.005, IB, IW, OB, OW);
    PP_CHECK(b > a);
}

PP_TEST(tonemap, eetf_output_clamped_to_display_range) {
    for (int i = 0; i <= 255; ++i) {
        double e2 = tonemap::eetf(i / 255.0, IB, IW, OB, OW);
        PP_CHECK(e2 >= OB - 1e-12);
        PP_CHECK(e2 <= OW + 1e-12);
    }
}

PP_TEST(tonemap, eetf_degenerate_range_is_passthrough) {
    /* iw == ib（无内容范围）时不得除零，直接透传 */
    PP_NEAR(tonemap::eetf(0.42, 0.5, 0.5, 0.0, 1.0), 0.42, 1e-12);
}

/* ---- BT.2020 → BT.709 基色矩阵 ---- */
PP_TEST(tonemap, matrix_preserves_neutral_gray) {
    /* 每行和 = 1 ⇒ 中性灰（r=g=b）在矩阵下不变。
     * 这条是"白点没写错"的最强单点证据：写错任何一个系数都会立刻破坏它。
     * 公开系数只给到 7 位小数，行和因此是 1±1e-7，容差按此取 2e-7。 */
    for (int i = 0; i < 3; ++i) {
        double s = tonemap::BT2020_TO_BT709.m[i][0] + tonemap::BT2020_TO_BT709.m[i][1]
                 + tonemap::BT2020_TO_BT709.m[i][2];
        PP_NEAR(s, 1.0, 2e-7);
    }
    for (double v : {0.0, 0.05, 0.18, 0.5, 1.0}) {
        double r = v, g = v, b = v;
        tonemap::applyMatrix(tonemap::BT2020_TO_BT709, r, g, b);
        PP_NEAR(r, v, 2e-7);
        PP_NEAR(g, v, 2e-7);
        PP_NEAR(b, v, 2e-7);
    }
}

PP_TEST(tonemap, matrix_desaturates_bt2020_primary) {
    /* BT.2020 的饱和红落在 BT.709 色域外（G/B 会变负）。
     * 矩阵之后必须能看出"往色域外跑"，再由软钳拉回来——这正是修掉"饱和色偏淡"的那一步。 */
    double r = 1.0, g = 0.0, b = 0.0;
    tonemap::applyMatrix(tonemap::BT2020_TO_BT709, r, g, b);
    PP_CHECK(r > 1.0);                 /* 超出 709 红基色 */
    PP_CHECK(g < 0.0);
    PP_CHECK(b < 0.0);
    tonemap::softClip(r, g, b);
    PP_CHECK(r <= 1.0 + 1e-9 && r >= 0.0);
    PP_CHECK(g >= -1e-9 && b >= -1e-9);
    PP_CHECK(r > g && r > b);          /* 仍是红色调，没有翻成别的色相 */
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
    p.refWhiteNits = tonemap::REF_WHITE_NITS;
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
    /* PQ 码 130/255 ≈ 101.9 nits；peak=1000 / refWhite=100 →
     * BT.2390 EETF 给出 PQ 码 ≈0.4724 → ≈70.1 nits → SDR 白 0.70 → 字节 218。
     * （旧 Reinhard-extended 版是 174；改成 BT.2390 后中段更接近参考白，这是预期的修正。）
     * 期望值来自参考实现的独立 Python 移植，见文件头。 */
    tonemap::Params p;
    px(p, tonemap::TRANSFER_PQ, 1000.0, 1.0);
    unsigned char c[4] = {130, 130, 130, 255};
    tonemap::processPixel(c, p);
    PP_CHECK((int)c[0] >= 215 && (int)c[0] <= 221);
    PP_EQ((int)c[0], (int)c[1]);
    PP_EQ((int)c[1], (int)c[2]);
    /* 满码白：内容峰值之上被钳到显示白 ow → 255（不染色、不裁黑） */
    unsigned char w[4] = {255, 255, 255, 255};
    tonemap::processPixel(w, p);
    PP_EQ((int)w[0], 255);
    PP_EQ((int)w[1], 255);
    PP_EQ((int)w[2], 255);
}

PP_TEST(tonemap, pipeline_hlg_known_bytes) {
    /* HLG 码 128/255 → 已知值 ≈ 182（参考实现的独立 Python 移植） */
    tonemap::Params p;
    px(p, tonemap::TRANSFER_HLG, 1000.0, 1.0);
    unsigned char c[4] = {128, 128, 128, 255};
    tonemap::processPixel(c, p);
    PP_CHECK((int)c[0] >= 178 && (int)c[0] <= 186);
    /* HLG 满码（1000 nits 峰）→ 内容峰值 → 显示白 255 */
    unsigned char w[4] = {255, 255, 255, 255};
    tonemap::processPixel(w, p);
    PP_EQ((int)w[0], 255);
}

PP_TEST(tonemap, pipeline_monotonic_and_strength) {
    /* 更亮的输入必须得到更亮（或相等）的输出——色调映射不得翻转亮度序 */
    tonemap::Params p;
    px(p, tonemap::TRANSFER_PQ, 1000.0, 1.0);
    unsigned char lo[4] = {100, 100, 100, 255};
    unsigned char hi[4] = {160, 160, 160, 255};
    tonemap::processPixel(lo, p);
    tonemap::processPixel(hi, p);
    PP_CHECK((int)lo[0] <= (int)hi[0]);
    /* 逐字节全扫一遍，确保整体单调（暗部到亮部不出现回折） */
    int prev = -1;
    for (int b = 0; b <= 255; ++b) {
        unsigned char s[4] = {(unsigned char)b, (unsigned char)b, (unsigned char)b, 255};
        tonemap::processPixel(s, p);
        PP_CHECK((int)s[0] >= prev);
        prev = (int)s[0];
    }
    /* strength 混合：0.5 落在 透传值 与 全量值 之间 */
    unsigned char mid[4] = {130, 130, 130, 255};
    px(p, tonemap::TRANSFER_PQ, 1000.0, 0.5);
    tonemap::processPixel(mid, p);
    PP_CHECK((int)mid[0] > 130);                 /* 比透传亮（130 → 218 全量） */
    PP_CHECK((int)mid[0] < 218 + 3);             /* 比全量暗 */
}

PP_TEST(tonemap, pipeline_neutral_stays_neutral) {
    /* 中性灰经"EETF + 基色矩阵 + 软钳"整条链后必须仍是中性灰（R=G=B）。
     * 矩阵行和为 1 保证这一点；若哪天有人把矩阵写成经验值，这条会立刻红。 */
    tonemap::Params p;
    px(p, tonemap::TRANSFER_PQ, 1000.0, 1.0);
    for (int b : {8, 40, 90, 130, 190}) {
        unsigned char c[4] = {(unsigned char)b, (unsigned char)b, (unsigned char)b, 255};
        tonemap::processPixel(c, p);
        PP_EQ((int)c[0], (int)c[1]);
        PP_EQ((int)c[1], (int)c[2]);
    }
}

/* ---- auto 判据 wantTonemap（Phase 2 边界 ④：显示端参与决策） ---------------
 * 这几条覆盖的是"这一帧要不要挂帧链"的全部语义。它错了不会崩，只会
 * "该映射时没映射（HDR 内容在 SDR 屏上过曝）"或"不该映射时映射了（HDR 屏上
 * 被压暗）"—— 同样是只有肉眼能发现的一类缺陷，所以按真值表逐格钉住。 */

PP_TEST(tonemap, want_off_or_zero_strength_never_maps) {
    /* off 与 strength<=0 都是"透传"，源和显示端说什么都不映射 */
    PP_CHECK(!tonemap::wantTonemap(tonemap::MODE_OFF, 1.0, 1, tonemap::TRANSFER_PQ, 0));
    PP_CHECK(!tonemap::wantTonemap(tonemap::MODE_OFF, 1.0, 1, tonemap::TRANSFER_PQ, 1));
    PP_CHECK(!tonemap::wantTonemap(tonemap::MODE_AUTO, 0.0, 1, tonemap::TRANSFER_PQ, 0));
    PP_CHECK(!tonemap::wantTonemap(tonemap::MODE_ON, 0.0, 1, tonemap::TRANSFER_PQ, 0));
    /* 负强度（不该出现，但也不能让 <=0 溜进热路径） */
    PP_CHECK(!tonemap::wantTonemap(tonemap::MODE_AUTO, -0.5, 1, tonemap::TRANSFER_PQ, 0));
}

PP_TEST(tonemap, want_on_forces_map) {
    /* on 无条件：即使源是 SDR、显示端已是 HDR 也照样映射（测试/预览用） */
    PP_CHECK(tonemap::wantTonemap(tonemap::MODE_ON, 1.0, 0, -1, 1));
    PP_CHECK(tonemap::wantTonemap(tonemap::MODE_ON, 1.0, 1, tonemap::TRANSFER_PQ, 1));
}

PP_TEST(tonemap, want_auto_sdr_source_never_maps) {
    /* auto：源不是 HDR 就完全不挂链（保住 NV12/GPU 快路径） */
    PP_CHECK(!tonemap::wantTonemap(tonemap::MODE_AUTO, 1.0, 0, 2, 0));
    /* is_hdr 为真但传输函数不是 PQ/HLG（BT.2020 + SDR 曲线，少见但合法）：不映射 */
    PP_CHECK(!tonemap::wantTonemap(tonemap::MODE_AUTO, 1.0, 1, 2, 0));
    PP_CHECK(!tonemap::wantTonemap(tonemap::MODE_AUTO, 1.0, 1, -1, 0));
}

PP_TEST(tonemap, want_auto_hdr_source_maps_on_sdr_output) {
    /* 边界 ④ 的主路径：HDR 源 + 输出链路不是 HDR → 映射到 SDR */
    PP_CHECK(tonemap::wantTonemap(tonemap::MODE_AUTO, 1.0, 1, tonemap::TRANSFER_PQ, 0));
    PP_CHECK(tonemap::wantTonemap(tonemap::MODE_AUTO, 1.0, 1, tonemap::TRANSFER_HLG, 0));
}

PP_TEST(tonemap, want_auto_hdr_source_passes_through_on_hdr_output) {
    /* 边界 ④ 的另一半：源是 HDR 但输出链路已在 HDR 输出 → **不映射**。
     * 这是"显示端说了算"的核心不可退化点：把 HDR 直通交给显示端，
     * 而不是先压成 SDR 再交给 HDR 屏。 */
    PP_CHECK(!tonemap::wantTonemap(tonemap::MODE_AUTO, 1.0, 1, tonemap::TRANSFER_PQ, 1));
    PP_CHECK(!tonemap::wantTonemap(tonemap::MODE_AUTO, 1.0, 1, tonemap::TRANSFER_HLG, 1));
}

PP_TEST(tonemap, want_auto_unknown_display_service_falls_back_to_source_only) {
    /* 旧宿主没有 video.display 服务时插件传 displayHdrActive=0 →
     * 行为必须与 Phase 1 的"只看源"逐格一致（向后兼容，不会更差）。 */
    for (int h : {0, 1})
        for (int t : {int(-1), int(2), int(tonemap::TRANSFER_PQ), int(tonemap::TRANSFER_HLG)}) {
            bool expect = h && (t == tonemap::TRANSFER_PQ || t == tonemap::TRANSFER_HLG);
            PP_EQ((int)tonemap::wantTonemap(tonemap::MODE_AUTO, 1.0, h, t, 0), expect ? 1 : 0);
        }
}

PP_TEST(tonemap, want_is_total_over_full_input_grid) {
    /* 穷举整个输入空间，确认判据与规范式**逐格**等价：
     *   auto = strength>0 && srcHdr && trc∈{PQ,HLG} && !displayHdrActive
     *   on   = strength>0；  off = 恒假
     * 这是重构判据时的安全网 —— 日后有人再动这个函数，一格不符即红。 */
    for (int m = 0; m <= 2; ++m)
      for (double s : {0.0, 0.5, 1.0})
        for (int h = 0; h <= 1; ++h)
          for (int t : {int(-1), int(2), int(tonemap::TRANSFER_PQ), int(tonemap::TRANSFER_HLG)})
            for (int a = 0; a <= 1; ++a) {
                const bool want = (m == tonemap::MODE_OFF) ? false
                                : (m == tonemap::MODE_ON)  ? (s > 0.0)
                                : (s > 0.0 && h != 0 &&
                                   (t == tonemap::TRANSFER_PQ || t == tonemap::TRANSFER_HLG) && a == 0);
                PP_EQ((int)tonemap::wantTonemap(m, s, h, t, a), want ? 1 : 0);
            }
}
