/* audio_viz — 音频频谱可视化插件
 * ---------------------------------------------------------------------------
 * 演示的插件能力：
 *   - 订阅 audio.samples 事件，拿到宿主喂来的最近一段 PCM（只读负载）
 *   - 自己实现 FFT（插件不链接任何第三方库，包括 FFmpeg/SDL）
 *   - 在 osd.draw 阶段用 OSD 图元绘制频谱柱与波形
 *   - 用命令控制模式与增益
 *
 * 两个事件（audio.samples 取数 + osd.draw 绘制）分开处理，是因为取数发生在
 * 音频线程/主循环，而绘制必须发生在渲染阶段。
 */
#include "player_plugin.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>

static PP_Host*          g_host = nullptr;
static const PP_HostApi* g_api  = nullptr;

static const int   FFT_N   = 512;         /* 必须是 2 的幂 */
static const int   BARS    = 40;
static float       g_bars[BARS];
static float       g_peaks[BARS];
static float       g_wave[256];

static int    g_mode = 1;                 /* 0=off 1=bars 2=wave 3=both */
static float  g_gain = 1.0f;
static int    g_frames = 0;

/* ---------------- FFT（迭代式 Cooley-Tukey，radix-2） ---------------- */
static void fft(float* re, float* im, int n) {
    const double PI = 3.14159265358979323846;
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2.0 * PI / (double)len;
        double wr = cos(ang), wi = sin(ang);
        for (int i = 0; i < n; i += len) {
            double cr = 1.0, ci = 0.0;
            for (int k = 0; k < len / 2; ++k) {
                int a = i + k, b = i + k + len / 2;
                double vr = re[b] * cr - im[b] * ci;
                double vi = re[b] * ci + im[b] * cr;
                double ur = re[a], ui = im[a];
                re[a] = (float)(ur + vr); im[a] = (float)(ui + vi);
                re[b] = (float)(ur - vr); im[b] = (float)(ui - vi);
                double ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

static void analyze(const float* samples, int count, int channels) {
    if (count < FFT_N * channels || channels <= 0) return;

    static float re[FFT_N], im[FFT_N];
    const float* src = samples + (count - FFT_N * channels);

    /* 取左声道 + Hann 窗，并去直流 */
    double mean = 0.0;
    for (int i = 0; i < FFT_N; ++i) mean += src[i * channels];
    mean /= FFT_N;
    for (int i = 0; i < FFT_N; ++i) {
        double w = 0.5 - 0.5 * cos(2.0 * 3.14159265358979323846 * i / (FFT_N - 1));
        double v = (src[i * channels] - mean) * w;
        re[i] = (float)(v * g_gain);
        im[i] = 0.0f;
    }
    fft(re, im, FFT_N);

    /* 只取前半谱，按对数分组压成 BARS 根柱子 */
    const int half = FFT_N / 2;
    for (int b = 0; b < BARS; ++b) {
        double lo = pow((double)half, (double)b / BARS);
        double hi = pow((double)half, (double)(b + 1) / BARS);
        int i0 = (int)lo;
        int i1 = (int)hi;
        if (i1 <= i0) i1 = i0 + 1;
        if (i1 > half) i1 = half;
        double sum = 0.0;
        for (int i = i0; i < i1; ++i) sum += sqrt((double)(re[i] * re[i] + im[i] * im[i]));
        double mag = sum / (i1 - i0);
        /* 转成 0..1 的显示量（经验对数压缩） */
        double db = 20.0 * log10(mag + 1e-6);
        double norm = (db + 60.0) / 60.0;
        if (norm < 0) norm = 0;
        if (norm > 1) norm = 1;
        /* 平滑：上升快，下落慢 */
        float target = (float)norm;
        if (target > g_bars[b]) g_bars[b] = g_bars[b] * 0.4f + target * 0.6f;
        else g_bars[b] = g_bars[b] * 0.86f + target * 0.14f;
        if (g_bars[b] > g_peaks[b]) g_peaks[b] = g_bars[b];
        else g_peaks[b] = std::max(0.0f, g_peaks[b] - 0.008f);
    }

    /* 波形：抽 256 个点 */
    int step = (count / channels) / 256;
    if (step < 1) step = 1;
    for (int i = 0; i < 256; ++i) {
        int idx = i * step;
        if (idx >= count / channels) idx = count / channels - 1;
        g_wave[i] = samples[idx * channels] * g_gain;
    }
}

/* ---------------- 事件回调 ---------------- */
static int onAudioSamples(PP_EventCtx* ctx) {
    if (!ctx->payload) return 0;
    const PP_AudioTap* tap = (const PP_AudioTap*)ctx->payload;
    if (!tap->samples || tap->count <= 0) return 0;
    g_frames++;
    analyze(tap->samples, tap->count, tap->channels);
    return 0;
}

static void drawViz(PP_Host* h, const PP_OsdApi* osd) {
    if (g_mode == 0) return;
    float W = 0, H = 0;
    if (!osd->size(h, &W, &H)) return;

    /* 布局：优先放进底部黑边（不遮画面）。插件之间没有布局管理器，约定「底部
     * 48px 是 osd_stats 进度条的区域」（那边的注释写了同样一句），频谱让开它；
     * 黑边不够高或没有黑边时，退回原来画面内右下角的布局。 */
    float bottomBand = 0.0f;
    {
        float bx = 0, by = 0, bw = 0, bh = 0;
        if (osd->video_rect && osd->video_rect(h, &bx, &by, &bw, &bh))
            bottomBand = H - (by + bh);
    }

    const float panelW = W * 0.42f;
    float panelH = 110.0f;
    float px = W - panelW - 16.0f;
    float py = H - panelH - 60.0f;             /* 回退布局：让开底部进度条 */
    if (bottomBand >= 96.0f) {
        panelH = bottomBand - 50.0f;           /* 贴进黑边，顶部留 2px、底部让开进度条 */
        if (panelH > 110.0f) panelH = 110.0f;
        py = H - bottomBand + 2.0f;
    }

    osd->panel(h, px, py, panelW, panelH, PP_RGBA(0, 0, 0, 150), PP_RGBA(120, 255, 190, 190));

    /* 内部布局按面板高度取比例：扁版（黑边里）和原尺寸共用同一套代码。 */
    const float titleH = panelH * 0.24f;
    const float plotY = py + titleH;
    const float plotH = panelH - titleH - 6.0f;

    if (g_mode & 1) {
        osd->bars(h, px + 10.0f, plotY, panelW - 20.0f, plotH, g_bars, BARS,
                  PP_RGB(90, 230, 170));
        /* 峰值保持线 */
        for (int i = 0; i < BARS; ++i) {
            float bw = (panelW - 20.0f) / BARS;
            float y = plotY + plotH * (1.0f - g_peaks[i]);
            osd->fill_rect(h, px + 10.0f + i * bw, y, bw > 2 ? bw - 1 : bw, 2.0f,
                           PP_RGB(255, 220, 120));
        }
    }
    if (g_mode & 2) {
        /* 用垂直线段近似波形 */
        const float base = plotY + plotH * 0.5f;
        const float amp = plotH * 0.45f;
        const float dx = (panelW - 20.0f) / 256.0f;
        for (int i = 0; i < 256; ++i) {
            float v = g_wave[i];
            if (v > 1) v = 1;
            if (v < -1) v = -1;
            float x = px + 10.0f + i * dx;
            osd->line(h, x, base, x, base + v * amp, PP_RGB(120, 200, 255), 1.0f);
        }
    }

    char tag[96];
    snprintf(tag, sizeof(tag), "FFT%d x%g  events %d", FFT_N, (double)g_gain, g_frames);
    osd->text(h, px + 10.0f, py + titleH * 0.22f, tag, PP_RGB(190, 255, 220), 1.0f);
}

static int onOsdDraw(PP_EventCtx* ctx) {
    drawViz(ctx->host, g_api->osd);
    return 0;
}

/* ---------------- 命令 ---------------- */
static int cmdMode(PP_Host* h, int argc, const char** argv, void* user) {
    (void)user;
    if (argc < 1) return -1;
    if (strcmp(argv[0], "off") == 0) g_mode = 0;
    else if (strcmp(argv[0], "bars") == 0) g_mode = 1;
    else if (strcmp(argv[0], "wave") == 0) g_mode = 2;
    else if (strcmp(argv[0], "both") == 0) g_mode = 3;
    else if (strcmp(argv[0], "cycle") == 0) g_mode = (g_mode + 1) & 3;
    else { g_api->log(h, PP_LOG_WARN, "用法: viz.mode <off|bars|wave|both|cycle>"); return -1; }
    char b[16];
    snprintf(b, sizeof(b), "%d", g_mode);
    g_api->config_set(h, "mode", b);
    g_api->log(h, PP_LOG_INFO, "频谱模式 -> %d", g_mode);
    return g_mode;
}

static int cmdGain(PP_Host* h, int argc, const char** argv, void* user) {
    (void)user;
    if (argc >= 1) g_gain = (float)atof(argv[0]);
    if (g_gain < 0.1f) g_gain = 0.1f;
    if (g_gain > 20.0f) g_gain = 20.0f;
    g_api->log(h, PP_LOG_INFO, "频谱增益 -> %.2f", (double)g_gain);
    return 0;
}

/* ---------------- 生命周期 ---------------- */
static int onLoad(PP_Host* host, const PP_HostApi* api) {
    if (api->abi_version != PP_ABI_VERSION || api->struct_size < sizeof(PP_HostApi)) return -1;
    g_host = host;
    g_api = api;

    const char* m = api->config_get(host, "mode", "1");
    g_mode = (m && m[0]) ? atoi(m) : 1;
    if (g_mode < 0 || g_mode > 3) g_mode = 1;

    memset(g_bars, 0, sizeof(g_bars));
    memset(g_peaks, 0, sizeof(g_peaks));
    memset(g_wave, 0, sizeof(g_wave));

    api->subscribe(host, PP_TOPIC_AUDIO_SAMPLES, &onAudioSamples, nullptr);
    api->subscribe(host, PP_TOPIC_OSD_DRAW, &onOsdDraw, nullptr);
    api->register_command(host, "viz.mode", "viz.mode <off|bars|wave|both|cycle>  频谱显示模式", &cmdMode, nullptr);
    api->register_command(host, "viz.gain", "viz.gain <x>                         频谱增益", &cmdGain, nullptr);
    return 0;
}

static int onStart(PP_Host* host) {
    g_api->log(host, PP_LOG_INFO, "audio_viz 启动 (mode=%d)", g_mode);
    return 0;
}

static void onStop(PP_Host* host) {
    char b[16];
    snprintf(b, sizeof(b), "%d", g_mode);
    g_api->config_set(host, "mode", b);
    g_api->log(host, PP_LOG_INFO, "audio_viz 停止");
}

static void onUnload(PP_Host* host) { g_api->log(host, PP_LOG_INFO, "audio_viz 卸载"); }

static PP_Plugin g_plugin = {
    PP_ABI_VERSION,
    "audio_viz",
    "1.0.0",
    "PluginPlayer 内置",
    "自实现 FFT 的音频频谱/波形可视化，订阅 audio.samples 与 osd.draw",
    0,
    &onLoad, &onStart, &onStop, &onUnload,
};

PP_PLUGIN_DECL(&g_plugin)
