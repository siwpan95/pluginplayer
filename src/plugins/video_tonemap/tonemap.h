/* tonemap.h — HDR→SDR 色调映射的纯数学（Phase 1：CPU 帧链实现）
 * ---------------------------------------------------------------------------
 * 零依赖（仅 <cmath>/<algorithm>），纯函数、无全局状态，可被单元测试直接引用。
 *
 * 管线（每通道）：
 *   byte/255 → [PQ EOTF 逆 | HLG OETF+OOTF 逆] → 线性光（0..1 = 10000 nits）
 *   → 按源峰值归一（1.0 = peakNits）→ BT.2390 族 EETF（Reinhard-extended）
 *   → 跨通道 gamut 软钳（soft clip，超 1 的部分向白点去饱和）
 *   → sRGB EOTF 逆（piecewise）→ 与原始字节按 strength 混合。
 *
 * 诚实边界（README §23 有完整说明）：
 *   - "BT.2390 族"指 Reinhard-extended 软膝 EETF，不是 BT.2390 报告的逐段精确实现；
 *   - 不转换 BT.2020→BT.709 基色（Phase 1 把 BT.2020 RGB 直接当显示 RGB 用，
 *     饱和色会偏淡）；完整 gamut map 与 GPU 加速是 Phase 2（libplacebo）。
 *   - 帧链像素是 8-bit RGBA（物化路径），PQ 在 8-bit 下有量化带隙，
 *     Phase 2 在真纹理/浮点上做。
 */
#pragma once
#include <algorithm>
#include <cmath>

namespace tonemap {

/* AVColorTransferCharacteristic 取值（FFmpeg 枚举，勿改）：
 * AVCOL_TRC_SMPTE2084 = 16（PQ / ST 2084，HDR10 的传输函数）
 * AVCOL_TRC_ARIB_STD_B67 = 18（HLG / BT.2100） */
enum { TRANSFER_PQ = 16, TRANSFER_HLG = 18 };

/* ---- SMPTE ST 2084 (PQ) 常量 ---- */
constexpr double PQ_M1 = 0.1593017578125;   /* 2610/16384 */
constexpr double PQ_M2 = 78.84375;          /* 2523/32   */
constexpr double PQ_C1 = 0.8359375;         /* 3424/4096 */
constexpr double PQ_C2 = 18.8515625;        /* 2413/128  */
constexpr double PQ_C3 = 18.6875;           /* 2392/128  */

/* ---- BT.2100 HLG OETF 常量（OETF 逆用） ---- */
constexpr double HLG_A = 0.17883277;
constexpr double HLG_B = 0.28466892;
constexpr double HLG_C = 0.55991073;
/* HLG OOTF：显示光 = 场景光^1.2（1000 cd/m2 系统伽马）；归一进 0..1=10000 nits 尺度时 ×0.1 */
constexpr double HLG_OOTF_GAMMA = 1.2;
constexpr double HLG_PEAK_SCALE = 0.1;

/* ---- EETF 软膝 ---- */
constexpr double EETF_KNEE = 0.18;   /* 与峰值的比值；越大中灰越暗、高光越柔和 */

/** PQ EOTF（ST 2084 解码）：编码 0..1 → 线性光 0..1（1.0 = 10000 nits）。 */
inline double pqToLinear(double f) {
    if (f <= 0.0) return 0.0;
    if (f >= 1.0) return 1.0;
    double fp = std::pow(f, 1.0 / PQ_M2);
    double num = fp - PQ_C1;
    if (num < 0.0) num = 0.0;
    double den = PQ_C2 - PQ_C3 * fp;
    if (den <= 0.0) return 1.0;
    return std::pow(num / den, 1.0 / PQ_M1);
}

/** HLG OETF 逆 + OOTF：编码 0..1 → 线性光 0..1（1.0 = 10000 nits）。 */
inline double hlgToLinear(double f) {
    if (f <= 0.0) return 0.0;
    if (f >= 1.0) return HLG_PEAK_SCALE;
    double scene;                      /* 场景光 0..1 */
    if (f <= 0.5) {
        scene = (f * f) / 3.0;
    } else {
        scene = (std::exp((f - HLG_C) / HLG_A) + HLG_B) / 12.0;
    }
    if (scene < 0.0) scene = 0.0;
    /* OOTF：显示光 = 场景光^γ，再按 1000→10000 nits 尺度归一并界内 */
    double disp = std::pow(scene, HLG_OOTF_GAMMA) * HLG_PEAK_SCALE;
    if (disp > 1.0) disp = 1.0;
    return disp;
}

/** BT.2390 族 Reinhard-extended EETF：Ln（0..1，1=源峰值）→ SDR 显示线性 0..1。
 *  Ld = Ln·(1+k)/(Ln+k)；Ln=0→0、Ln=1→1，单调、对 Ln>1 亦有界((1+k)/(1+k/…))软滚降。 */
inline double eetf(double ln, double knee = EETF_KNEE) {
    if (ln <= 0.0) return 0.0;
    return ln * (1.0 + knee) / (ln + knee);
}

/** sRGB EOTF 逆（piecewise）：显示线性 0..1 → 编码 0..1。 */
inline double linearToSrgb(double l) {
    if (l <= 0.0) return 0.0;
    if (l >= 1.0) return 1.0;
    if (l <= 0.0031308) return 12.92 * l;
    return 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
}

/** gamut 软钳：超出 [0,1] 时沿亮度方向向白点（上限）/黑点（下限）去饱和，
 *  保持亮度感、避免硬钳导致的色相偏移。BT.2020 亮度权重。 */
inline void softClip(double& r, double& g, double& b) {
    double mx = std::max({r, g, b});
    double mn = std::min({r, g, b});
    if (mx <= 1.0 && mn >= 0.0) return;
    double l = 0.2627 * r + 0.6780 * g + 0.0593 * b;
    if (mx > 1.0) {
        if (mx > l + 1e-9) {
            double t = (1.0 - l) / (mx - l);
            r = l + (r - l) * t;
            g = l + (g - l) * t;
            b = l + (b - l) * t;
        } else {
            r = g = b = 1.0;
        }
        mx = std::max({r, g, b});
        mn = std::min({r, g, b});
        l = 0.2627 * r + 0.6780 * g + 0.0593 * b;
    }
    if (mn < 0.0) {
        if (l > mn + 1e-9) {
            double t = l / (l - mn);
            r = l + (r - l) * t;
            g = l + (g - l) * t;
            b = l + (b - l) * t;
        } else {
            r = g = b = 0.0;
        }
    }
}

/* ---- 端到端管线 ---- */
struct Params {
    int    transfer;   /* TRANSFER_PQ 或 TRANSFER_HLG */
    double peakNits;   /* 源峰值（nits），默认 1000；决定 EETF 前归一 */
    double strength;   /* 0..1 与原始字节的混合（0=透传 1=全量） */
    double knee;       /* EETF 软膝，默认 EETF_KNEE */
};

/** 对单个 RGBA 像素原地做色调映射（alpha 不动）。 */
inline void processPixel(unsigned char* px, const Params& p) {
    if (!px) return;
    if (p.strength <= 0.0) return;
    const double src[3] = {px[0] / 255.0, px[1] / 255.0, px[2] / 255.0};
    double lin[3];
    const bool hlg = (p.transfer == TRANSFER_HLG);
    for (int i = 0; i < 3; ++i)
        lin[i] = hlg ? hlgToLinear(src[i]) : pqToLinear(src[i]);
    /* 归一：1.0 = 源峰值。PQ 解码尺度是 0..1=10000 nits，故 ×10000/peak。 */
    double scale = 10000.0 / (p.peakNits > 0.0 ? p.peakNits : 1000.0);
    double knee = p.knee > 0.0 ? p.knee : EETF_KNEE;
    double r = eetf(lin[0] * scale, knee);
    double g = eetf(lin[1] * scale, knee);
    double b = eetf(lin[2] * scale, knee);
    softClip(r, g, b);
    unsigned char out[3];
    out[0] = (unsigned char)(linearToSrgb(r) * 255.0 + 0.5);
    out[1] = (unsigned char)(linearToSrgb(g) * 255.0 + 0.5);
    out[2] = (unsigned char)(linearToSrgb(b) * 255.0 + 0.5);
    if (p.strength >= 1.0) {
        px[0] = out[0]; px[1] = out[1]; px[2] = out[2];
        return;
    }
    double s = p.strength;
    px[0] = (unsigned char)(src[0] * 255.0 * (1.0 - s) + out[0] * s + 0.5);
    px[1] = (unsigned char)(src[1] * 255.0 * (1.0 - s) + out[1] * s + 0.5);
    px[2] = (unsigned char)(src[2] * 255.0 * (1.0 - s) + out[2] * s + 0.5);
}

} /* namespace tonemap */
