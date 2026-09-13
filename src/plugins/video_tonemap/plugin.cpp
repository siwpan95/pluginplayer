/* video_tonemap — CPU 色调映射插件（Phase 1）
 * ---------------------------------------------------------------------------
 * 把 HDR（PQ / HLG）内容在**帧链上**做 HDR→SDR 色调映射（BT.2390 族 Reinhard-
 * extended EETF + gamut 软钳 + sRGB EOTF 逆，纯数学见 tonemap.h）。
 *
 * 与 video_quality 同一套按需挂链原则，但激活条件不是参数、而是**源是否 HDR**：
 *   - 默认 mode=auto：只有源是 HDR（BT.2020 + PQ/HLG，经内核 video.color 服务
 *     的快照判断）才挂帧处理器；SDR 源完全不挂 → 不破坏 NV12/GPU 快路径。
 *   - mode=on：无条件映射（测试/预览用）；mode=off：完全不映射
 *     （真实 HDR 显示器想走直通时用 off；显示端 HDR 直通与 libplacebo GPU
 *     加速是 Phase 2，见 README §23）。
 *
 * 帧链优先级 90（video_quality 为 100，链按 priority 升序执行）→ 先做
 * HDR→SDR 映射，画质调整再作用于已映射的显示参考值。
 */
#include "player_plugin.h"
#include "tonemap.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static PP_Host*          g_host = nullptr;
static const PP_HostApi* g_api  = nullptr;

/* ---- 状态（命令改，持久化到 ini） ---- */
static std::atomic<int>   g_mode{0};       /* 0=auto 1=on 2=off */
static std::atomic<float> g_strength{1.0f};/* 0..1 与原始字节混合 */
static std::atomic<int>   g_peak{1000};    /* 源峰值 nits */

/* ---- 运行态 ---- */
static int  g_subTick = 0;
static int  g_procId  = 0;
static long g_frameSeen      = 0;
static long g_framesProcessed = 0;
static int  g_srcTrc = -1;    /* 最近一次 video.color 快照：AVColorTransferCharacteristic */
static int  g_srcHdr = 0;

static int frameProcessor(PP_Host* h, PP_FrameView* v, void* user);

static bool tonemapWanted() {
    if (g_mode.load() == 2) return false;                    /* off */
    if (g_strength.load() <= 0.0f) return false;
    if (g_mode.load() == 1) return true;                     /* on：强制 */
    /* auto：源是 HDR（PQ/HLG）才映射；SDR 源不挂链 */
    return g_srcHdr &&
           (g_srcTrc == tonemap::TRANSFER_PQ || g_srcTrc == tonemap::TRANSFER_HLG);
}

static void syncChain(PP_Host* h) {
    const bool need = tonemapWanted();
    if (need && !g_procId) {
        g_procId = g_api->set_frame_processor(h, &frameProcessor, nullptr, 90);
        g_api->log(h, PP_LOG_INFO, "video_tonemap: 已挂帧处理器 (id=%d, trc=%d hdr=%d)",
                   g_procId, g_srcTrc, g_srcHdr);
    } else if (!need && g_procId) {
        const int old = g_procId;
        g_api->remove_frame_processor(h, old);
        g_procId = 0;
        g_api->log(h, PP_LOG_INFO, "video_tonemap: 撤销帧处理器 (id=%d)", old);
    }
}

/* ---------------- 帧处理链回调 ---------------- */
static int frameProcessor(PP_Host* h, PP_FrameView* v, void* user) {
    (void)h; (void)user;
    if (!v || !v->data) return PP_CHAIN_CONTINUE;
    g_frameSeen++;
    if (!tonemapWanted()) return PP_CHAIN_CONTINUE;
    tonemap::Params p;
    p.transfer = (g_srcTrc == tonemap::TRANSFER_HLG) ? tonemap::TRANSFER_HLG
                                                     : tonemap::TRANSFER_PQ;
    p.peakNits = (double)(g_peak.load() > 0 ? g_peak.load() : 1000);
    p.strength = (double)g_strength.load();
    p.knee     = tonemap::EETF_KNEE;
    for (int y = 0; y < v->height; ++y) {
        uint8_t* row = v->data + (size_t)y * v->pitch;
        for (int x = 0; x < v->width; ++x)
            tonemap::processPixel(row + (size_t)x * 4, p);
    }
    g_framesProcessed++;
    return PP_CHAIN_CONTINUE;
}

/* ---------------- tick：刷新 video.color 快照并同步帧链 ---------------- */
static int onTick(PP_EventCtx* ctx) {
    PP_VideoColorService* cs =
        (PP_VideoColorService*)g_api->get_service(ctx->host, "video.color");
    if (cs) {
        int trc = cs->transfer ? cs->transfer() : -1;
        int hdr = cs->is_hdr  ? cs->is_hdr()  : 0;
        if (trc != g_srcTrc || hdr != g_srcHdr) {
            g_srcTrc = trc;
            g_srcHdr = hdr;
            syncChain(ctx->host);
        }
    }
    return 0;
}

/* ---------------- 命令 ---------------- */
static int cmdMode(PP_Host* h, int argc, const char** argv, void* user) {
    (void)user;
    if (argc < 1) { g_api->log(h, PP_LOG_WARN, "用法: tonemap.mode < auto | on | off >"); return -1; }
    int m = -1;
    if (!strcmp(argv[0], "auto")) m = 0;
    else if (!strcmp(argv[0], "on")) m = 1;
    else if (!strcmp(argv[0], "off")) m = 2;
    if (m < 0) { g_api->log(h, PP_LOG_WARN, "tonemap.mode 只接受 auto/on/off: %s", argv[0]); return -1; }
    g_mode = m;
    g_api->config_set(h, "mode", argv[0]);
    syncChain(h);
    g_api->log(h, PP_LOG_INFO, "video_tonemap: mode = %s", argv[0]);
    return 0;
}

static int cmdStrength(PP_Host* h, int argc, const char** argv, void* user) {
    (void)user;
    if (argc < 1) { g_api->log(h, PP_LOG_WARN, "用法: tonemap.strength < 0..1 >"); return -1; }
    float s = (float)atof(argv[0]);
    if (s < 0.0f) s = 0.0f;
    if (s > 1.0f) s = 1.0f;
    g_strength = s;
    char buf[32];
    snprintf(buf, sizeof(buf), "%.3f", s);
    g_api->config_set(h, "strength", buf);
    syncChain(h);
    g_api->log(h, PP_LOG_INFO, "video_tonemap: strength = %.3f", s);
    return 0;
}

static int cmdPeak(PP_Host* h, int argc, const char** argv, void* user) {
    (void)user;
    if (argc < 1) { g_api->log(h, PP_LOG_WARN, "用法: tonemap.peak < 100..10000 >(nits)"); return -1; }
    int p = atoi(argv[0]);
    if (p < 100) p = 100;
    if (p > 10000) p = 10000;
    g_peak = p;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", p);
    g_api->config_set(h, "peak", buf);
    g_api->log(h, PP_LOG_INFO, "video_tonemap: peak = %d nits", p);
    return 0;
}

static int cmdStatus(PP_Host* h, int argc, const char** argv, void* user) {
    (void)argc; (void)argv; (void)user;
    const char* modeName = g_mode.load() == 0 ? "auto"
                         : g_mode.load() == 1 ? "on" : "off";
    char buf[256];
    snprintf(buf, sizeof(buf),
             "mode=%s strength=%.3f peak=%d active=%d srcTrc=%d srcHdr=%d seen=%ld processed=%ld procId=%d",
             modeName, (double)g_strength.load(), g_peak.load(),
             tonemapWanted() ? 1 : 0, g_srcTrc, g_srcHdr,
             g_frameSeen, g_framesProcessed, g_procId);
    g_api->log(h, PP_LOG_INFO, "%s", buf);
    return 0;
}

/* ---------------- 生命周期 ---------------- */
static int onLoad(PP_Host* host, const PP_HostApi* api) {
    if (api->abi_version != PP_ABI_VERSION || api->struct_size < sizeof(PP_HostApi)) return -1;
    g_host = host;
    g_api = api;

    const char* m = api->config_get(host, "mode", "");
    const char* s = api->config_get(host, "strength", "");
    const char* p = api->config_get(host, "peak", "");
    if (m && *m) {
        if (!strcmp(m, "on")) g_mode = 1;
        else if (!strcmp(m, "off")) g_mode = 2;
    }
    if (s && *s) {
        float v = (float)atof(s);
        if (v >= 0.0f && v <= 1.0f) g_strength = v;
    }
    if (p && *p) {
        int v = atoi(p);
        if (v >= 100 && v <= 10000) g_peak = v;
    }

    api->register_command(host, "tonemap.mode",
        "tonemap.mode < auto | on | off >  色调映射开关（auto=源 HDR 才映射）", &cmdMode, nullptr);
    api->register_command(host, "tonemap.strength",
        "tonemap.strength < 0..1 >          与原始画面的混合强度（0=透传 1=全量）", &cmdStrength, nullptr);
    api->register_command(host, "tonemap.peak",
        "tonemap.peak < 100..10000 >        源峰值亮度 nits（默认 1000）", &cmdPeak, nullptr);
    api->register_command(host, "tonemap.status",
        "tonemap.status                     输出色调映射当前状态", &cmdStatus, nullptr);
    return 0;
}

static int onStart(PP_Host* host) {
    g_subTick = g_api->subscribe(host, PP_TOPIC_APP_TICK, &onTick, nullptr);
    syncChain(host);
    g_api->log(host, PP_LOG_INFO,
               "video_tonemap 启动 (mode=%s strength=%.2f peak=%d, 订阅 tick=%d)",
               g_mode.load() == 0 ? "auto" : g_mode.load() == 1 ? "on" : "off",
               (double)g_strength.load(), g_peak.load(), g_subTick);
    return 0;
}

static void onStop(PP_Host* host) {
    if (g_subTick) { g_api->unsubscribe(host, g_subTick); g_subTick = 0; }
    if (g_procId)  { g_api->remove_frame_processor(host, g_procId); g_procId = 0; }
    g_api->log(host, PP_LOG_INFO, "video_tonemap 停止");
}

static void onUnload(PP_Host* host) {
    g_api->log(host, PP_LOG_INFO, "video_tonemap 卸载");
}

static PP_Plugin g_plugin = {
    PP_ABI_VERSION,
    "video_tonemap",
    "1.0.0",
    "PluginPlayer 内置",
    "CPU 色调映射（Phase 1）：HDR(PQ/HLG) → SDR 的 BT.2390 族 EETF + gamut 软钳，帧链原地处理；Phase 2 换 libplacebo GPU 后端",
    0,
    &onLoad, &onStart, &onStop, &onUnload,
};

PP_PLUGIN_DECL(&g_plugin)
