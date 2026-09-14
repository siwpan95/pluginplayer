/* tonemap.h — HDR→SDR 色调映射的纯数学（Phase 1：CPU 帧链实现）
 * ---------------------------------------------------------------------------
 * 零依赖（仅 <cmath>/<algorithm>），纯函数、无全局状态，可被单元测试直接引用。
 *
 * 管线（每通道）：
 *   byte/255 → [PQ EOTF 逆 | HLG OETF+OOTF 逆] → 线性光（0..1 = 10000 nits，BT.2020）
 *   → 重新编码为**绝对 PQ 码字**（BT.2390 的 EETF 定义在 PQ 域）→ BT.2390 EETF
 *   → PQ EOTF 解回线性光 → 归一到参考白 → BT.2020→BT.709 基色矩阵
 *   → 跨通道 gamut 软钳 → sRGB EOTF 逆 → 与原始字节按 strength 混合。
 *
 * 与 Phase 1 首版的两处实质改动（对应 README §23 的四条边界中的 ①②）：
 *   ① EETF 换成 **BT.2390 逐段精确实现**（原为 Reinhard-extended 软膝，不是 BT.2390）：
 *      SS = 1.5·maxLum − 0.5、TS = 1.5·minLum，[TS,SS] 段 1:1 直通，肩部/趾部各用一个
 *      Hermite 样条，且在切点处**值与一阶导都连续**（无可见折角）。
 *      严格按定义实现，参考实现：
 *      natural-harmonia-gropius/mpv-config → shaders/hdr-toys/tone-mapping/bt2390.glsl
 *      的 f(x, iw, ib, ow, ob)（BT.2390 Annex EETF，含 E3 趾部抬升）。公式本身见
 *      ITU-R BT.2390 的 EETF 定义（Dolby 专利 US11582490 亦有逐式复述）。
 *      归一化沿用该参考的**内容相对**约定：iw = 内容峰值、ow = 参考白（默认 100 nits），
 *      即"内容峰值 → 显示参考白"，这与本插件原有的语义（peak 映到 SDR 白）一致。
 *   ② 补 **BT.2020→BT.709 基色矩阵**（原缺，导致 BT.2020 的饱和色被当显示 RGB 用、
 *      整体偏淡）。矩阵元素为 D65 下 BT.2020 / BT.709 基色推导的标准值，
 *      每行和 = 1（中性灰在矩阵下不变，单测钉住这条不变式）。
 *
 * 边界 ④ 已在 Phase 2 补齐（显示端探测）：
 *   判据抽成纯函数 wantTonemap()（本文件末尾），auto 不再"只看源" —— 只有
 *   「源是 HDR」且「输出链路当前不是 HDR」时才映射；显示端已在 HDR 输出时直通。
 *   显示端状态由宿主经 video.display 服务提供（见 sdk/player_plugin.h 的
 *   PP_DisplayService：为什么不扩充 PP_VideoColorService，那份注释里有理由）。
 *
 * 仍然存在的边界（README §23 的 ③，属 Phase 2 的 GPU 部分）：
 *   - ③ 帧链像素是 8-bit RGBA（物化路径），PQ 在 8-bit 下有量化带隙
 *     （映射本身全程 double，量化只发生在读入与写回两处）；要真正消掉
 *     必须走浮点纹理，那是 GPU 后端的事。
 */
#pragma once
#include <algorithm>
#include <cmath>

namespace tonemap {

/* AVColorTransferCharacteristic 取值（FFmpeg 枚举，勿改）：
 * AVCOL_TRC_SMPTE2084 = 16（PQ / ST 2084，HDR10 的传输函数）
 * AVCOL_TRC_ARIB_STD_B67 = 18（HLG / BT.2100） */
enum { TRANSFER_PQ = 16, TRANSFER_HLG = 18 };

/* 插件工作模式（与 tonemap.mode 命令行取值一一对应）。 */
enum { MODE_AUTO = 0, MODE_ON = 1, MODE_OFF = 2 };

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

/* ---- BT.2390 EETF / 显示端常量 ---- */
constexpr double REF_WHITE_NITS = 100.0;    /* SDR 参考白：EETF 的 ow */
constexpr double CONTENT_MIN_NITS = 0.001;  /* 内容黑：EETF 的 ib（参考实现的 get_min_i 默认） */
constexpr double NITS_SCALE = 10000.0;      /* PQ 满码 = 10000 nits */

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

/** PQ OETF（ST 2084 编码，pqToLinear 的逆）：线性光 0..1（1.0 = 10000 nits）→ 编码 0..1。
 *  BT.2390 的 EETF 定义在 PQ 码字上，所以需要重新编码一次。 */
inline double linearToPq(double l) {
    if (l <= 0.0) return 0.0;
    if (l >= 1.0) return 1.0;
    double p = std::pow(l, PQ_M1);
    return std::pow((PQ_C1 + PQ_C2 * p) / (1.0 + PQ_C3 * p), PQ_M2);
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

/** BT.2390 EETF 的 Hermite 样条（P(B) = (2T³−3T²+1)·Y1 + (T³−2T²+T)·(X2−X1) + (−2T³+3T²)·Y2）。
 *  T 由调用方给定，避免在热路径上重复算除法。 */
inline double hermite(double t, double y1, double x2mx1, double y2) {
    double t2 = t * t;
    double t3 = t2 * t;
    return (2.0 * t3 - 3.0 * t2 + 1.0) * y1
         + (t3 - 2.0 * t2 + t) * x2mx1
         + (-2.0 * t3 + 3.0 * t2) * y2;
}

/** BT.2390 EETF（逐段精确）：绝对 PQ 码字 → 绝对 PQ 码字。
 *
 *  iw/ib = 内容的 PQ 码字白/黑，ow/ob = 显示端的 PQ 码字白/黑。
 *  段： [TS, SS] 1:1 直通；[SS, 1] 肩部 Hermite；[0, TS] 趾部 Hermite；最后叠加趾部抬升。
 *  与参考实现的对应：E1 归一 → E2 肩部 → E3 趾部抬升 → E4 反正归一 → clamp[ob, ow]。 */
inline double eetf(double x, double ib, double iw, double ob, double ow) {
    if (iw - ib <= 1e-12) return x;
    const double minLum = (ob - ib) / (iw - ib);
    const double maxLum = (ow - ib) / (iw - ib);
    const double ss = 1.5 * maxLum - 0.5;      /* 肩部起点（= knee start） */
    const double ts = 1.5 * minLum;            /* 趾部终点 */

    double n = (x - ib) / (iw - ib);           /* E1：归一到 [0,1] */

    if (n <= ts) {                             /* 趾部：0 → minLum，TS → TS */
        double t = (ts > 1e-12) ? n / ts : 0.0;
        n = hermite(t, 0.0, ts, minLum);
    } else if (n >= ss) {                      /* 肩部：SS → SS，1 → maxLum */
        double t = (1.0 - ss > 1e-12) ? (n - ss) / (1.0 - ss) : 0.0;
        n = hermite(t, ss, 1.0 - ss, maxLum);
    }                                          /* 中间段：n 原样直通 */

    if (n >= 0.0) n = n + minLum * std::pow(1.0 - n, 4.0);   /* E3：趾部抬升 */

    double out = n * (iw - ib) + ib;                           /* E4：反正归一 */
    return std::min(std::max(out, ob), ow);
}

/** sRGB EOTF 逆（piecewise）：显示线性 0..1 → 编码 0..1。 */
inline double linearToSrgb(double l) {
    if (l <= 0.0) return 0.0;
    if (l >= 1.0) return 1.0;
    if (l <= 0.0031308) return 12.92 * l;
    return 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
}

/** BT.2020 → BT.709 基色矩阵（D65，线性光）。每行和 = 1，故中性灰不变。
 *  元素由两套基色的 RGB→XYZ 矩阵推导（BT.2020→XYZ→BT.709），非经验拟合。 */
struct Rgb2Rgb {
    double m[3][3];
};
constexpr Rgb2Rgb BT2020_TO_BT709 = {{
    { 1.6604910, -0.5876411, -0.0728500 },
    { -0.1245505, 1.1328999, -0.0083494 },
    { -0.0181508, -0.1005789, 1.1187297 },
}};

/** 用 3×3 矩阵变换线性 RGB（原地）。 */
inline void applyMatrix(Rgb2Rgb m, double& r, double& g, double& b) {
    double nr = m.m[0][0] * r + m.m[0][1] * g + m.m[0][2] * b;
    double ng = m.m[1][0] * r + m.m[1][1] * g + m.m[1][2] * b;
    double nb = m.m[2][0] * r + m.m[2][1] * g + m.m[2][2] * b;
    r = nr; g = ng; b = nb;
}

/** gamut 软钳：超出 [0,1] 时沿亮度方向向白点（上限）/黑点（下限）去饱和，
 *  保持亮度感、避免硬钳导致的色相偏移。亮度权重按 BT.709（矩阵之后即为 709 域）。 */
inline void softClip(double& r, double& g, double& b) {
    double mx = std::max({r, g, b});
    double mn = std::min({r, g, b});
    if (mx <= 1.0 && mn >= 0.0) return;
    double l = 0.2126 * r + 0.7152 * g + 0.0722 * b;
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
        l = 0.2126 * r + 0.7152 * g + 0.0722 * b;
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
    int    transfer;      /* TRANSFER_PQ 或 TRANSFER_HLG */
    double peakNits;      /* 源峰值（nits），默认 1000；EETF 的 iw */
    double strength;      /* 0..1 与原始字节的混合（0=透传 1=全量） */
    double refWhiteNits;  /* 显示参考白（nits），默认 REF_WHITE_NITS；EETF 的 ow */
};

/* ---- 开关判据（纯函数，便于单测：这是 auto 的全部语义所在） ---- */

/** 决定这一帧要不要走色调映射。
 *
 *  mode=off，或 strength<=0        → 不映射（透传）
 *  mode=on                        → 映射（强制，忽略源与显示端）
 *  mode=auto                      → 仅当「源是 HDR（BT.2020 + PQ/HLG）」且
 *                                   「输出链路当前不是 HDR 输出」才映射
 *
 *  最后那个条件就是 Phase 1 遗留的边界 ④。语义上它是"**显示端说了算**"：
 *  源是 HDR 不必然要映射 —— 显示端已经真的在以 HDR（SRGB_LINEAR）输出时，
 *  映射反而是错的（把 HDR 压成 SDR 再交给 HDR 屏）。所以这里用的是
 *  **输出链路是否已激活 HDR**（displayHdrActive），而不是"显示器是否支持 HDR"：
 *  支持但重建失败（回退 SDR 渲染器）的场合，直通会把 HDR 喂给 SDR 路径。
 *
 *  displayHdrActive 来自 video.display 服务；旧宿主没有该服务时调用方传 0，
 *  行为退回 Phase 1 的"只看源"，不会更差。
 *
 *  srcTrc 只在 srcHdr 为真时被检查：内核的 is_hdr 已包含"BT.2020 原色"，这里
 *  再确认传输函数确为 PQ/HLG，避免 BT.2020+SDR 曲线（少见但合法）被误映射。 */
inline bool wantTonemap(int mode, double strength, int srcHdr, int srcTrc, int displayHdrActive) {
    if (mode == MODE_OFF) return false;
    if (!(strength > 0.0)) return false;
    if (mode == MODE_ON) return true;
    if (!srcHdr) return false;
    if (srcTrc != TRANSFER_PQ && srcTrc != TRANSFER_HLG) return false;
    return !displayHdrActive;
}

/** 对单个 RGBA 像素原地做色调映射（alpha 不动）。 */
inline void processPixel(unsigned char* px, const Params& p) {
    if (!px) return;
    if (p.strength <= 0.0) return;

    const double refWhite = p.refWhiteNits > 0.0 ? p.refWhiteNits : REF_WHITE_NITS;
    const double peak     = p.peakNits > 0.0 ? p.peakNits : 1000.0;
    const bool   hlg      = (p.transfer == TRANSFER_HLG);

    /* 1) 源码字 → 绝对 PQ 码字（EETF 定义在 PQ 域；HLG 需先解出线性光再编码） */
    double q[3];
    for (int i = 0; i < 3; ++i) {
        double code = px[i] / 255.0;
        double lin  = hlg ? hlgToLinear(code) : pqToLinear(code);
        q[i] = linearToPq(std::min(lin, 1.0));
    }

    /* 2) EETF 的四个锚点（PQ 码字） */
    const double ob = linearToPq(0.0);
    const double ow = linearToPq(std::min(refWhite / NITS_SCALE, 1.0));
    const double iw = std::max(linearToPq(std::min(peak / NITS_SCALE, 1.0)), ow + 1e-3);
    const double ib = std::min(linearToPq(CONTENT_MIN_NITS / NITS_SCALE), ob - 1e-3);

    /* 3) EETF → 线性光 → 归一到参考白 → BT.2020→BT.709 */
    double r = pqToLinear(eetf(q[0], ib, iw, ob, ow)) * NITS_SCALE / refWhite;
    double g = pqToLinear(eetf(q[1], ib, iw, ob, ow)) * NITS_SCALE / refWhite;
    double b = pqToLinear(eetf(q[2], ib, iw, ob, ow)) * NITS_SCALE / refWhite;
    applyMatrix(BT2020_TO_BT709, r, g, b);
    softClip(r, g, b);

    unsigned char out[3];
    out[0] = (unsigned char)(linearToSrgb(r) * 255.0 + 0.5);
    out[1] = (unsigned char)(linearToSrgb(g) * 255.0 + 0.5);
    out[2] = (unsigned char)(linearToSrgb(b) * 255.0 + 0.5);
    if (p.strength >= 1.0) {
        px[0] = out[0]; px[1] = out[1]; px[2] = out[2];
        return;
    }
    const double s = p.strength;
    px[0] = (unsigned char)(px[0] * (1.0 - s) + out[0] * s + 0.5);
    px[1] = (unsigned char)(px[1] * (1.0 - s) + out[1] * s + 0.5);
    px[2] = (unsigned char)(px[2] * (1.0 - s) + out[2] * s + 0.5);
}

} /* namespace tonemap */
