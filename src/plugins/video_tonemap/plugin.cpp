/* video_tonemap — CPU 色调映射插件
 * ---------------------------------------------------------------------------
 * 把 HDR（PQ / HLG）内容在**帧链上**做 HDR→SDR 色调映射（**BT.2390 逐段精确 EETF**
 * + BT.2020→BT.709 基色矩阵 + gamut 软钳 + sRGB EOTF 逆，纯数学见 tonemap.h）。
 *
 * 与 video_quality 同一套按需挂链原则，但激活条件是**源 + 显示端**两边：
 *   - mode=auto：源是 HDR（BT.2020 + PQ/HLG，经内核 video.color 服务的快照）
 *     **且**输出链路当前不是 HDR（video.display 服务的 hdr_active）才挂帧处理器；
 *     SDR 源完全不挂 → 不破坏 NV12/GPU 快路径；显示端已在 HDR 输出时也完全不挂
 *     → 把 HDR 直通交给显示端，不做无谓的 CPU 逐像素。
 *   - mode=on：无条件映射（测试/预览用）。
 *   - mode=off：完全不映射。**默认就是 off**，理由见下。
 *   - 判据本体是 tonemap::wantTonemap()（纯函数，单测覆盖）。
 *
 * 为什么默认 off（而不是 Phase 1 原本设想的 auto）
 * ---------------------------------------------------------------------------
 * 本机实测（1280x720、PQ 源、正常播放路径，`stats` 读数，两次）：
 *   - tonemap=auto（挂链）: 0.42 / 1.00 fps，解码 175 / 显示 10 / 丢帧 144；
 *   - tonemap=off         : 30 fps 满帧（自检 decode_and_display 90/90、0 丢帧）。
 * 即 CPU 路径约 1~2.4 s/帧，是实时所需的 1~2 个数量级之外 —— 它作为**正确性参考实现**
 * 完全合格（数学由单测逐条钉住），但作为默认开启的播放路径不合格：默认 auto
 * 等于让 SDR 屏上的 HDR 内容变成幻灯片。所以默认 off，`tonemap.mode on` 可显式
 * 开启（单帧截图、离线转码、对照验证都够用），等 Phase 2 的 libplacebo GPU
 * 后端落地后再把默认改回 auto。这不是"把没做完的藏起来"：auto 的语义、显示端
 * 判据、单测真值表都已交付并被自检覆盖，缺的只是让它跑得动的那块 GPU。
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
/* 默认 off —— 见本文件头部「为什么默认 off」一节：CPU 逐像素路径实测 0.42 fps@720p，
 * 默认 auto 会让 HDR 内容在 SDR 屏上直接变成幻灯片。auto 的语义与单测都在，
 * 只是为了不破坏播放体验而不默认启用。 */
static std::atomic<int>   g_mode{tonemap::MODE_OFF};
static std::atomic<float> g_strength{1.0f};/* 0..1 与原始字节混合 */
static std::atomic<int>   g_peak{1000};    /* 源峰值 nits */

/* ---- 运行态 ---- */
static int  g_subTick = 0;
static int  g_procId  = 0;
static long g_frameSeen      = 0;
static long g_framesProcessed = 0;
static int  g_srcTrc = -1;    /* 最近一次 video.color 快照：AVColorTransferCharacteristic */
static int  g_srcHdr = 0;
/* 最近一次 video.display 快照（0 也可能是"宿主没这个服务"—— 两种情况的行为
 * 都是"当作显示端不是 HDR"，所以不必区分；下方 g_dispKnown 仅用于状态输出） */
static int  g_dispCapable  = 0;
static int  g_dispActive   = 0;
static int  g_dispKnown    = 0;

static int frameProcessor(PP_Host* h, PP_FrameView* v, void* user);

static bool tonemapWanted() {
    return tonemap::wantTonemap(g_mode.load(), (double)g_strength.load(),
                               g_srcHdr, g_srcTrc, g_dispActive);
}

static void syncChain(PP_Host* h) {
    const bool need = tonemapWanted();
    if (need && !g_procId) {
        g_procId = g_api->set_frame_processor(h, &frameProcessor, nullptr, 90);
        g_api->log(h, PP_LOG_INFO,
                   "video_tonemap: 已挂帧处理器 (id=%d, trc=%d hdr=%d dispActive=%d)",
                   g_procId, g_srcTrc, g_srcHdr, g_dispActive);
    } else if (!need && g_procId) {
        const int old = g_procId;
        g_api->remove_frame_processor(h, old);
        g_procId = 0;
        g_api->log(h, PP_LOG_INFO,
                   "video_tonemap: 撤销帧处理器 (id=%d, hdr=%d dispActive=%d)",
                   old, g_srcHdr, g_dispActive);
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
    p.refWhiteNits = tonemap::REF_WHITE_NITS;
    for (int y = 0; y < v->height; ++y) {
        uint8_t* row = v->data + (size_t)y * v->pitch;
        for (int x = 0; x < v->width; ++x)
            tonemap::processPixel(row + (size_t)x * 4, p);
    }
    g_framesProcessed++;
    return PP_CHAIN_CONTINUE;
}

/* ---------------- tick：刷新 video.color / video.display 快照并同步帧链 ---------------- */
static int onTick(PP_EventCtx* ctx) {
    bool changed = false;

    PP_VideoColorService* cs =
        (PP_VideoColorService*)g_api->get_service(ctx->host, "video.color");
    if (cs) {
        int trc = cs->transfer ? cs->transfer() : -1;
        int hdr = cs->is_hdr  ? cs->is_hdr()  : 0;
        if (trc != g_srcTrc || hdr != g_srcHdr) {
            g_srcTrc = trc;
            g_srcHdr = hdr;
            changed = true;
        }
    }

    /* P2-L2：显示端/输出链路能力。宿主没注册这个服务（旧宿主）时 svc == nullptr，
     * 快照保持 0 —— auto 于是退回"只看源"，与 Phase 1 行为一致，不会更差。 */
    PP_DisplayService* ds =
        (PP_DisplayService*)g_api->get_service(ctx->host, "video.display");
    if (ds) {
        int cap = ds->hdr_capable ? ds->hdr_capable() : 0;
        int act = ds->hdr_active  ? ds->hdr_active()  : 0;
        if (!g_dispKnown) {
            g_api->log(ctx->host, PP_LOG_INFO,
                       "video_tonemap: video.display 可用（显示器支持 HDR=%d，输出链路 HDR=%d）",
                       cap, act);
            g_dispKnown = 1;
        }
        if (cap != g_dispCapable || act != g_dispActive) {
            g_dispCapable = cap;
            g_dispActive  = act;
            changed = true;
        }
    }

    if (changed) syncChain(ctx->host);
    return 0;
}

/* ---------------- 命令 ---------------- */
static int cmdMode(PP_Host* h, int argc, const char** argv, void* user) {
    (void)user;
    if (argc < 1) { g_api->log(h, PP_LOG_WARN, "用法: tonemap.mode < auto | on | off >"); return -1; }
    int m = -1;
    if (!strcmp(argv[0], "auto")) m = tonemap::MODE_AUTO;
    else if (!strcmp(argv[0], "on")) m = tonemap::MODE_ON;
    else if (!strcmp(argv[0], "off")) m = tonemap::MODE_OFF;
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
    const int m = g_mode.load();
    const char* modeName = m == tonemap::MODE_AUTO ? "auto"
                         : m == tonemap::MODE_ON   ? "on" : "off";
    char buf[320];
    snprintf(buf, sizeof(buf),
             "mode=%s strength=%.3f peak=%d active=%d srcTrc=%d srcHdr=%d "
             "dispService=%d dispCapable=%d dispActive=%d seen=%ld processed=%ld procId=%d",
             modeName, (double)g_strength.load(), g_peak.load(),
             tonemapWanted() ? 1 : 0, g_srcTrc, g_srcHdr,
             g_dispKnown, g_dispCapable, g_dispActive,
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
        if (!strcmp(m, "on")) g_mode = tonemap::MODE_ON;
        else if (!strcmp(m, "off")) g_mode = tonemap::MODE_OFF;
        else if (!strcmp(m, "auto")) g_mode = tonemap::MODE_AUTO;
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
        "tonemap.mode < off | auto | on >  色调映射开关（默认 off：CPU 路径约 1~2.4s/帧；auto=源 HDR 且显示端非 HDR 才映射）", &cmdMode, nullptr);
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
    "CPU 色调映射：HDR(PQ/HLG) → SDR 的 BT.2390 逐段精确 EETF + BT.2020→BT.709 基色矩阵 + gamut 软钳，帧链原地处理；默认 off（CPU 约 1~2.4s/帧，仅供对照/离线），auto=源 HDR 且输出链路非 HDR 才挂链；Phase 2 余下部分为 libplacebo GPU 后端",
    0,
    &onLoad, &onStart, &onStop, &onUnload,
};

PP_PLUGIN_DECL(&g_plugin)
