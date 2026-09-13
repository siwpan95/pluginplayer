/* video_quality — 画质调整插件（亮度 / 对比度 / 饱和度 / HDR 增益）
 * ---------------------------------------------------------------------------
 * 与 video_effects 共享同一套「瀑布式帧处理链」机制（set_frame_processor /
 * remove_frame_processor + PP_CHAIN_CONTINUE），但做的是**连续色调调整**而非
 * 风格化效果：对插件链拿到的可写 RGBA8888 像素原地做亮度/对比度/饱和度/HDR 增益。
 *
 * 设计要点（与 video_effects 一致的不占链原则）：
 *   - 任何参数都还是默认值时（brightness=0/contrast=1/saturation=1/hdr=1），
 *     不挂帧处理器 —— 链为空时渲染层走 NV12/GPU 路线，避免每帧多一次 RGBA 物化。
 *   - 一旦任一参数偏离默认，才挂链；回到全默认立刻撤销。
 *   - 调整经 video_quality 服务暴露给其它插件（ABI 安全 get_service 机制）。
 */
#include "player_plugin.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <cfloat>

static PP_Host*          g_host = nullptr;
static const PP_HostApi* g_api  = nullptr;

/* 当前画质参数（默认值 = 不调整） */
static float g_brightness = 0.0f;   /* -1..1：加法偏移（乘以 255） */
static float g_contrast   = 1.0f;   /*  0..2：围绕 128 缩放 */
static float g_saturation = 1.0f;   /*  0..2：围绕亮度缩放 */
static float g_hdr        = 1.0f;   /* 0.5..3：乘性增益（HDR 内容提亮用） */

static int  g_procId = 0;
static long g_frameSeen = 0;
static long g_framesProcessed = 0;

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static bool qualityActive() {
    return g_brightness != 0.0f || g_contrast != 1.0f ||
           g_saturation != 1.0f || g_hdr != 1.0f;
}

/* ---------------- 像素调整（原地改写 RGBA） ---------------- */
static void adjustPixel(uint8_t* q, float bright, float contrast, float sat, float hdr) {
    float r = q[0], g = q[1], b = q[2];
    /* 对比度：围绕中灰 128 */
    r = (r - 128.0f) * contrast + 128.0f;
    g = (g - 128.0f) * contrast + 128.0f;
    b = (b - 128.0f) * contrast + 128.0f;
    /* 饱和度：围绕亮度（BT.601 权重 77/150/29 /256 ≈ 0.299/0.587/0.114） */
    float l = (r * 77.0f + g * 150.0f + b * 29.0f) / 256.0f;
    r = l + (r - l) * sat;
    g = l + (g - l) * sat;
    b = l + (b - l) * sat;
    /* 亮度：加法偏移（bright∈-1..1 → ±255） */
    r += bright * 255.0f;
    g += bright * 255.0f;
    b += bright * 255.0f;
    /* HDR 增益：乘性（HDR 内容提亮；SDR 用 1.0 即不变） */
    r *= hdr; g *= hdr; b *= hdr;
    /* 8-bit 管道钳到 [0,255]（HDR 真·浮点纹理超出部分由渲染器处理，这里只保护 8-bit 像素） */
    q[0] = (uint8_t)std::max(0.0f, std::min(255.0f, r));
    q[1] = (uint8_t)std::max(0.0f, std::min(255.0f, g));
    q[2] = (uint8_t)std::max(0.0f, std::min(255.0f, b));
}

/* ---------------- 帧处理链回调 ---------------- */
static int frameProcessor(PP_Host* h, PP_FrameView* v, void* user) {
    (void)h; (void)user;
    if (!v || !v->data) return PP_CHAIN_CONTINUE;
    if (!qualityActive()) return PP_CHAIN_CONTINUE;
    g_frameSeen++;
    for (int y = 0; y < v->height; ++y) {
        uint8_t* row = v->data + (size_t)y * v->pitch;
        for (int x = 0; x < v->width; ++x)
            adjustPixel(row + (size_t)x * 4, g_brightness, g_contrast, g_saturation, g_hdr);
    }
    g_framesProcessed++;
    return PP_CHAIN_CONTINUE;
}

/* ---------------- "video_quality" 服务（给其它插件查询） ---------------- */
static float svcBrightness(void) { return g_brightness; }
static float svcContrast(void)   { return g_contrast; }
static float svcSaturation(void) { return g_saturation; }
static float svcHdr(void)        { return g_hdr; }
static int   svcActive(void)     { return qualityActive() ? 1 : 0; }

typedef struct QualityService {
    float (*brightness)(void);
    float (*contrast)(void);
    float (*saturation)(void);
    float (*hdr)(void);
    int   (*active)(void);
} QualityService;

static QualityService g_service = {
    &svcBrightness, &svcContrast, &svcSaturation, &svcHdr, &svcActive
};

/* ---------------- 命令 ---------------- */
static void syncChain(PP_Host* h);
static void applyState(PP_Host* h);

static int cmdBrightness(PP_Host* h, int argc, const char** argv, void* user) {
    (void)user;
    if (argc < 1) { g_api->log(h, PP_LOG_WARN, "用法: quality.brightness < -1..1 >"); return -1; }
    g_brightness = clampf((float)atof(argv[0]), -1.0f, 1.0f);
    g_api->config_set(h, "brightness", argv[0]);
    applyState(h);
    g_api->log(h, PP_LOG_INFO, "亮度 = %.3f", g_brightness);
    return 0;
}
static int cmdContrast(PP_Host* h, int argc, const char** argv, void* user) {
    (void)user;
    if (argc < 1) { g_api->log(h, PP_LOG_WARN, "用法: quality.contrast < 0..2 >"); return -1; }
    g_contrast = clampf((float)atof(argv[0]), 0.0f, 2.0f);
    g_api->config_set(h, "contrast", argv[0]);
    applyState(h);
    g_api->log(h, PP_LOG_INFO, "对比度 = %.3f", g_contrast);
    return 0;
}
static int cmdSaturation(PP_Host* h, int argc, const char** argv, void* user) {
    (void)user;
    if (argc < 1) { g_api->log(h, PP_LOG_WARN, "用法: quality.saturation < 0..2 >"); return -1; }
    g_saturation = clampf((float)atof(argv[0]), 0.0f, 2.0f);
    g_api->config_set(h, "saturation", argv[0]);
    applyState(h);
    g_api->log(h, PP_LOG_INFO, "饱和度 = %.3f", g_saturation);
    return 0;
}
static int cmdHdr(PP_Host* h, int argc, const char** argv, void* user) {
    (void)user;
    if (argc < 1) { g_api->log(h, PP_LOG_WARN, "用法: quality.hdr < 0.5..3 >"); return -1; }
    g_hdr = clampf((float)atof(argv[0]), 0.5f, 3.0f);
    g_api->config_set(h, "hdr", argv[0]);
    applyState(h);
    g_api->log(h, PP_LOG_INFO, "HDR 增益 = %.3f", g_hdr);
    return 0;
}
static int cmdReset(PP_Host* h, int argc, const char** argv, void* user) {
    (void)argc; (void)argv; (void)user;
    g_brightness = 0.0f; g_contrast = 1.0f; g_saturation = 1.0f; g_hdr = 1.0f;
    g_api->config_set(h, "brightness", "0");
    g_api->config_set(h, "contrast", "1");
    g_api->config_set(h, "saturation", "1");
    g_api->config_set(h, "hdr", "1");
    applyState(h);
    g_api->log(h, PP_LOG_INFO, "画质参数已重置为默认");
    return 0;
}
static int cmdStatus(PP_Host* h, int argc, const char** argv, void* user) {
    (void)argc; (void)argv; (void)user;
    char buf[256];
    snprintf(buf, sizeof(buf),
             "brightness=%.3f contrast=%.3f saturation=%.3f hdr=%.3f active=%d seen=%ld processed=%ld procId=%d",
             g_brightness, g_contrast, g_saturation, g_hdr,
             qualityActive() ? 1 : 0, g_frameSeen, g_framesProcessed, g_procId);
    g_api->log(h, PP_LOG_INFO, "%s", buf);
    return 0;
}

/* ---------------- 帧链按需挂载（不调整时不占链） ---------------- */
static void syncChain(PP_Host* h) {
    const bool need = qualityActive();
    if (need && !g_procId) {
        g_procId = g_api->set_frame_processor(h, &frameProcessor, nullptr, 100);
        g_api->log(h, PP_LOG_INFO, "已挂帧处理器 (id=%d)", g_procId);
    } else if (!need && g_procId) {
        const int old = g_procId;
        g_api->remove_frame_processor(h, old);
        g_procId = 0;
        g_api->log(h, PP_LOG_INFO, "画质已全默认，撤销帧处理器 (id=%d)", old);
    }
}

static void applyState(PP_Host* h) { syncChain(h); }

/* ---------------- 生命周期 ---------------- */
static int onLoad(PP_Host* host, const PP_HostApi* api) {
    if (api->abi_version != PP_ABI_VERSION || api->struct_size < sizeof(PP_HostApi)) return -1;
    g_host = host;
    g_api = api;

    /* 从持久化配置恢复（没有就保持默认 = 不调整） */
    const char* b = api->config_get(host, "brightness", "");
    const char* c = api->config_get(host, "contrast", "");
    const char* s = api->config_get(host, "saturation", "");
    const char* d = api->config_get(host, "hdr", "");
    if (b && *b) g_brightness = clampf((float)atof(b), -1.0f, 1.0f);
    if (c && *c) g_contrast   = clampf((float)atof(c), 0.0f, 2.0f);
    if (s && *s) g_saturation = clampf((float)atof(s), 0.0f, 2.0f);
    if (d && *d) g_hdr        = clampf((float)atof(d), 0.5f, 3.0f);

    api->register_service(host, "video_quality", "1.0.0", &g_service);
    api->register_command(host, "quality.brightness", "quality.brightness < -1..1 >  亮度加法偏移", &cmdBrightness, nullptr);
    api->register_command(host, "quality.contrast",   "quality.contrast < 0..2 >      对比度（围绕中灰）", &cmdContrast, nullptr);
    api->register_command(host, "quality.saturation", "quality.saturation < 0..2 >    饱和度（围绕亮度）", &cmdSaturation, nullptr);
    api->register_command(host, "quality.hdr",        "quality.hdr < 0.5..3 >         HDR 增益（乘性提亮）", &cmdHdr, nullptr);
    api->register_command(host, "quality.reset",      "quality.reset                重置全部画质参数为默认", &cmdReset, nullptr);
    api->register_command(host, "quality.status",     "quality.status               输出当前画质参数", &cmdStatus, nullptr);
    return 0;
}

static int onStart(PP_Host* host) {
    syncChain(host);
    g_api->log(host, PP_LOG_INFO, "video_quality 启动 (帧处理器 id=%d, 亮度=%.2f 对比度=%.2f 饱和度=%.2f hdr=%.2f)",
               g_procId, g_brightness, g_contrast, g_saturation, g_hdr);
    return 0;
}

static void onStop(PP_Host* host) {
    if (g_procId) { g_api->remove_frame_processor(host, g_procId); g_procId = 0; }
    g_api->log(host, PP_LOG_INFO, "video_quality 停止");
}

static void onUnload(PP_Host* host) {
    g_api->log(host, PP_LOG_INFO, "video_quality 卸载");
}

static PP_Plugin g_plugin = {
    PP_ABI_VERSION,
    "video_quality",
    "1.0.0",
    "PluginPlayer 内置",
    "亮度/对比度/饱和度/HDR 增益 四种画质调整，可组合，暴露 video_quality 服务",
    0,
    &onLoad, &onStart, &onStop, &onUnload,
};

PP_PLUGIN_DECL(&g_plugin)
