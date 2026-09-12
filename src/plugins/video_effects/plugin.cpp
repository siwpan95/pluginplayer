/* video_effects — 视频效果插件（WATERFALL 帧处理链演示）
 * ---------------------------------------------------------------------------
 * 这是理解插件「瀑布式事件」的最佳样本：
 *   宿主在显示每帧之前依次调用链上的帧处理器，处理器拿到的是同一块可写像素。
 *   返回值语义：
 *     PP_CHAIN_CONTINUE(0) 交给链上后续处理器（本插件就是多个效果串联）
 *     PP_CHAIN_BAIL(1)     终止链但照常显示
 *     PP_CHAIN_DROP(2)     丢弃该帧，不显示
 *
 * 演示的插件能力：
 *   - set_frame_processor() 注册 / remove_frame_processor() 撤销
 *   - 通过 register_service() 注册 "effects" 服务，供其它插件查询
 *   - 多个效果可叠加（位掩码），全部通过命令动态开关
 *   - 用 PP_CHAIN_DROP 实现「每 N 帧丢一帧」的链路控制演示
 */
#include "player_plugin.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <algorithm>

static PP_Host*          g_host = nullptr;
static const PP_HostApi* g_api  = nullptr;

enum Effect {
    FX_GRAY     = 1 << 0,
    FX_INVERT   = 1 << 1,
    FX_SEPIA    = 1 << 2,
    FX_POSTER   = 1 << 3,
    FX_EDGE     = 1 << 4,
    FX_MIRROR   = 1 << 5,
    FX_COUNT    = 6,
};

static const char* kEffectNames[FX_COUNT] = {
    "gray", "invert", "sepia", "posterize", "edge", "mirror"
};

static int  g_mask      = 0;      /* 已启用的效果位掩码 */
static int  g_procId    = 0;
static int  g_dropEvery = 0;      /* >0 时每 N 帧丢一帧 */
static long g_frameSeen = 0;
static long g_framesProcessed = 0;

static std::vector<uint8_t> g_scratch;   /* edge 用的源像素副本 */

/* ---------------- 各效果实现（原地改写 RGBA） ---------------- */
static void fxGray(PP_FrameView* v) {
    for (int y = 0; y < v->height; ++y) {
        uint8_t* row = v->data + (size_t)y * v->pitch;
        for (int x = 0; x < v->width; ++x) {
            uint8_t* q = row + (size_t)x * 4;
            int l = (q[0] * 77 + q[1] * 150 + q[2] * 29) >> 8;
            q[0] = q[1] = q[2] = (uint8_t)l;
        }
    }
}

static void fxInvert(PP_FrameView* v) {
    for (int y = 0; y < v->height; ++y) {
        uint8_t* row = v->data + (size_t)y * v->pitch;
        for (int x = 0; x < v->width; ++x) {
            uint8_t* q = row + (size_t)x * 4;
            q[0] = (uint8_t)(255 - q[0]);
            q[1] = (uint8_t)(255 - q[1]);
            q[2] = (uint8_t)(255 - q[2]);
        }
    }
}

static void fxSepia(PP_FrameView* v) {
    for (int y = 0; y < v->height; ++y) {
        uint8_t* row = v->data + (size_t)y * v->pitch;
        for (int x = 0; x < v->width; ++x) {
            uint8_t* q = row + (size_t)x * 4;
            int r = q[0], g = q[1], b = q[2];
            int nr = (r * 393 + g * 769 + b * 189) >> 10;
            int ng = (r * 349 + g * 686 + b * 168) >> 10;
            int nb = (r * 272 + g * 534 + b * 131) >> 10;
            q[0] = (uint8_t)std::min(255, nr);
            q[1] = (uint8_t)std::min(255, ng);
            q[2] = (uint8_t)std::min(255, nb);
        }
    }
}

static void fxPosterize(PP_FrameView* v) {
    for (int y = 0; y < v->height; ++y) {
        uint8_t* row = v->data + (size_t)y * v->pitch;
        for (int x = 0; x < v->width; ++x) {
            uint8_t* q = row + (size_t)x * 4;
            for (int c = 0; c < 3; ++c) q[c] = (uint8_t)((q[c] / 64) * 85);
        }
    }
}

/* Sobel 亮度梯度：需要源像素副本 */
static void fxEdge(PP_FrameView* v) {
    const int W = v->width, H = v->height;
    size_t need = (size_t)v->pitch * H;
    if (g_scratch.size() < need) g_scratch.resize(need);
    memcpy(g_scratch.data(), v->data, need);

    auto luma = [&](int x, int y) -> int {
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x >= W) x = W - 1;
        if (y >= H) y = H - 1;
        const uint8_t* q = g_scratch.data() + (size_t)y * v->pitch + (size_t)x * 4;
        return (q[0] * 77 + q[1] * 150 + q[2] * 29) >> 8;
    };

    for (int y = 0; y < H; ++y) {
        uint8_t* row = v->data + (size_t)y * v->pitch;
        for (int x = 0; x < W; ++x) {
            int gx = -luma(x-1,y-1) - 2*luma(x-1,y) - luma(x-1,y+1)
                     + luma(x+1,y-1) + 2*luma(x+1,y) + luma(x+1,y+1);
            int gy = -luma(x-1,y-1) - 2*luma(x,y-1) - luma(x+1,y-1)
                     + luma(x-1,y+1) + 2*luma(x,y+1) + luma(x+1,y+1);
            int m = (int)std::min(255.0, sqrt((double)(gx * gx + gy * gy)));
            uint8_t* q = row + (size_t)x * 4;
            q[0] = q[1] = q[2] = (uint8_t)m;
        }
    }
}

static void fxMirror(PP_FrameView* v) {
    for (int y = 0; y < v->height; ++y) {
        uint8_t* row = v->data + (size_t)y * v->pitch;
        for (int x = 0; x < v->width / 2; ++x) {
            uint8_t* a = row + (size_t)x * 4;
            uint8_t* b = row + (size_t)(v->width - 1 - x) * 4;
            for (int c = 0; c < 4; ++c) std::swap(a[c], b[c]);
        }
    }
}

/* ---------------- 帧处理链回调 ---------------- */
static int frameProcessor(PP_Host* h, PP_FrameView* v, void* user) {
    (void)h; (void)user;
    if (!v || !v->data) return PP_CHAIN_CONTINUE;
    g_frameSeen++;

    /* DROP 语义演示：每 N 帧丢弃一帧 */
    if (g_dropEvery > 0 && (g_frameSeen % g_dropEvery) == 0) {
        return PP_CHAIN_DROP;
    }

    if (g_mask == 0) return PP_CHAIN_CONTINUE;

    if (g_mask & FX_EDGE)   fxEdge(v);          /* 先说 edge：它需要原始像素 */
    if (g_mask & FX_GRAY)   fxGray(v);
    if (g_mask & FX_INVERT) fxInvert(v);
    if (g_mask & FX_SEPIA)  fxSepia(v);
    if (g_mask & FX_POSTER) fxPosterize(v);
    if (g_mask & FX_MIRROR) fxMirror(v);

    g_framesProcessed++;
    return PP_CHAIN_CONTINUE;
}

/* ---------------- "effects" 服务（给其它插件查询） ---------------- */
static char g_summary[256];
static const char* svcSummary(void) {
    if (g_mask == 0 && g_dropEvery == 0) {
        snprintf(g_summary, sizeof(g_summary), "none");
    } else {
        int off = 0;
        for (int i = 0; i < FX_COUNT; ++i) {
            if (g_mask & (1 << i)) {
                off += snprintf(g_summary + off, sizeof(g_summary) - off, "%s%s",
                                off ? "+" : "", kEffectNames[i]);
            }
        }
        if (g_dropEvery > 0) {
            snprintf(g_summary + off, sizeof(g_summary) - off, "%sdrop1/%d",
                     off ? "+" : "", g_dropEvery);
        }
    }
    return g_summary;
}
static int svcProcessed(void) { return (int)g_framesProcessed; }

typedef struct EffectsService {
    const char* (*summary)(void);
    int         (*processedFrames)(void);
} EffectsService;

static EffectsService g_service = { &svcSummary, &svcProcessed };

/* ---------------- 命令 ---------------- */
static int findEffect(const char* name) {
    if (!name) return -1;
    if (strcmp(name, "none") == 0 || strcmp(name, "clear") == 0) return 0;
    for (int i = 0; i < FX_COUNT; ++i)
        if (strcmp(name, kEffectNames[i]) == 0) return 1 << i;
    return -1;
}

/* 帧处理器的**按需挂载**（2026-09-12）。
 *
 * 为什么不再"启动就挂、常驻到卸载"：帧处理器一旦挂上，宿主**每帧**都会调用它 ——
 * 哪怕此刻一个效果都没开。代价不只是那点空回调：只要链非空，渲染层就必须走
 * "CPU RGBA"这条路（插件的契约是可写 RGBA 像素），于是 NV12/GPU 路线下每帧还会
 * 多一次 NV12→RGBA 物化。**不启用时不占链**，链才可能是空的。
 * 顺带让"链上有几个帧处理器"成为一个诚实的观测点（见 effect.status 的 procId）。 */
static void syncChain(PP_Host* h) {
    const bool need = (g_mask != 0) || (g_dropEvery > 0);
    if (need && !g_procId) {
        g_procId = g_api->set_frame_processor(h, &frameProcessor, nullptr, 100);
        g_api->log(h, PP_LOG_INFO, "已挂帧处理器 (id=%d)", g_procId);
    } else if (!need && g_procId) {
        const int old = g_procId;
        g_api->remove_frame_processor(h, old);
        g_procId = 0;
        g_api->log(h, PP_LOG_INFO, "效果已全关，撤销帧处理器 (id=%d)", old);
    }
}

static void updateSubtitle(PP_Host* h) {
    const char* s = svcSummary();
    if (strcmp(s, "none") == 0) g_api->set_window_subtitle(h, nullptr);
    else g_api->set_window_subtitle(h, s);
}

/** 任何改变效果状态的地方都走这里：同步帧链 + 标题。 */
static void applyState(PP_Host* h) {
    syncChain(h);
    updateSubtitle(h);
}

static int cmdSet(PP_Host* h, int argc, const char** argv, void* user) {
    (void)user;
    if (argc < 1) {
        g_api->log(h, PP_LOG_WARN, "用法: effect.set <gray|invert|sepia|posterize|edge|mirror|none>");
        return -1;
    }
    int bit = findEffect(argv[0]);
    if (bit < 0) {
        g_api->log(h, PP_LOG_WARN, "未知效果: %s", argv[0]);
        return -1;
    }
    g_mask = bit;
    g_api->config_set(h, "mask", argv[0]);
    applyState(h);
    g_api->log(h, PP_LOG_INFO, "效果已设为: %s", svcSummary());
    return 0;
}
static int cmdToggle(PP_Host* h, int argc, const char** argv, void* user) {
    (void)user;
    if (argc < 1) return -1;
    int bit = findEffect(argv[0]);
    if (bit < 0) { g_api->log(h, PP_LOG_WARN, "未知效果: %s", argv[0]); return -1; }
    if (bit == 0) g_mask = 0;
    else g_mask ^= bit;
    applyState(h);
    g_api->log(h, PP_LOG_INFO, "效果开关后: %s", svcSummary());
    return 0;
}

static int cmdClear(PP_Host* h, int argc, const char** argv, void* user) {
    (void)argc; (void)argv; (void)user;
    g_mask = 0;
    g_dropEvery = 0;
    applyState(h);
    g_api->log(h, PP_LOG_INFO, "已清空全部效果");
    return 0;
}

static int cmdCycle(PP_Host* h, int argc, const char** argv, void* user) {
    (void)argc; (void)argv; (void)user;
    /* none -> gray -> invert -> sepia -> posterize -> edge -> mirror -> none */
    static int cycle = 0;
    cycle = (cycle + 1) % (FX_COUNT + 1);
    g_mask = (cycle == 0) ? 0 : (1 << (cycle - 1));
    applyState(h);
    g_api->log(h, PP_LOG_INFO, "效果轮转: %s", svcSummary());
    return 0;
}

static int cmdDrop(PP_Host* h, int argc, const char** argv, void* user) {
    (void)user;
    int n = (argc >= 1) ? atoi(argv[0]) : 0;
    g_dropEvery = (n > 1) ? n : 0;
    applyState(h);
    g_api->log(h, PP_LOG_INFO, "丢帧设置: %s", g_dropEvery ? svcSummary() : "关闭");
    return g_dropEvery;
}

static int cmdStatus(PP_Host* h, int argc, const char** argv, void* user) {
    (void)argc; (void)argv; (void)user;
    char buf[256];
    snprintf(buf, sizeof(buf), "effects=%s  seen=%ld  processed=%ld  procId=%d",
             svcSummary(), g_frameSeen, g_framesProcessed, g_procId);
    g_api->log(h, PP_LOG_INFO, "%s", buf);
    if (argc >= 1 && argv[0] && argv[0][0]) return g_api->write_text_file(h, argv[0], buf);
    return 0;
}

/* ---------------- 生命周期 ---------------- */
static int onLoad(PP_Host* host, const PP_HostApi* api) {
    if (api->abi_version != PP_ABI_VERSION || api->struct_size < sizeof(PP_HostApi)) return -1;
    g_host = host;
    g_api = api;

    const char* m = api->config_get(host, "mask", "");
    int bit = findEffect(m);
    g_mask = bit > 0 ? bit : 0;

    api->register_service(host, "effects", "1.0.0", &g_service);
    api->register_command(host, "effect.set",    "effect.set <name>      设置单一效果", &cmdSet, nullptr);
    api->register_command(host, "effect.toggle", "effect.toggle <name>   叠加/去掉一个效果", &cmdToggle, nullptr);
    api->register_command(host, "effect.clear",  "effect.clear           清空效果", &cmdClear, nullptr);
    api->register_command(host, "effect.cycle",  "effect.cycle           轮转效果", &cmdCycle, nullptr);
    api->register_command(host, "effect.drop",   "effect.drop <N>        每 N 帧丢一帧（CHAIN_DROP 演示）", &cmdDrop, nullptr);
    api->register_command(host, "effect.status", "effect.status [file]   输出效果状态", &cmdStatus, nullptr);
    return 0;
}

static int onStart(PP_Host* host) {
    /* 只有配置里真的存着效果/丢帧才挂链 —— 默认不占链（见 syncChain 的注释）。 */
    syncChain(host);
    g_api->log(host, PP_LOG_INFO, "video_effects 启动 (帧处理器 id=%d, 初始效果=%s)",
               g_procId, svcSummary());
    return 0;
}

static void onStop(PP_Host* host) {
    if (g_procId) { g_api->remove_frame_processor(host, g_procId); g_procId = 0; }
    g_api->set_window_subtitle(host, nullptr);
    g_api->log(host, PP_LOG_INFO, "video_effects 停止");
}

static void onUnload(PP_Host* host) {
    g_api->log(host, PP_LOG_INFO, "video_effects 卸载");
}

static PP_Plugin g_plugin = {
    PP_ABI_VERSION,
    "video_effects",
    "1.0.0",
    "PluginPlayer 内置",
    "灰度/反色/棕褐/色调分离/边缘/镜像 六种效果，可叠加，暴露 effects 服务",
    0,
    &onLoad, &onStart, &onStop, &onUnload,
};

PP_PLUGIN_DECL(&g_plugin)
