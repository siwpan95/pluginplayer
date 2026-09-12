/* snapshot — 截图 / 标记插件
 * ---------------------------------------------------------------------------
 * 演示的插件能力：
 *   - set_frame_processor()：注册帧处理链，在像素上原地绘制（WATERFALL）
 *   - acquire_frame() / release_frame()：取当前帧副本，用于另存 PNG
 *   - save_png()：宿主内置 PNG 写出，插件不需要任何图像库
 *   - config_get/config_set：把「截图张数、上次输出目录」持久化到 ini
 *   - register_command()：暴露 snapshot / snapshot.gray / stamp / snapshot.count
 */
#include "player_plugin.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

static PP_Host*          g_host = nullptr;
static const PP_HostApi* g_api  = nullptr;

static int   g_stampOn   = 0;   /* 是否给画面加标记框 */
static int   g_grayShot  = 0;
static int   g_counter   = 0;   /* 已截图张数（持久化） */
static int   g_stampProc = 0;   /* 帧处理器 id */
static char  g_outDir[512] = {0};

/* ---------------- 帧处理器：给画面画标记框 + 十字准星 ---------------- */
static int stampProcessor(PP_Host* h, PP_FrameView* v, void* user) {
    (void)h; (void)user;
    if (!g_stampOn || !v || !v->data) return PP_CHAIN_CONTINUE;

    const int t = 3;                                   /* 边框粗细 */
    const uint32_t color = 0xFF30C0FFu;                /* 0xAABBGGRR → 橙黄 */
    uint8_t* p = v->data;

    auto px = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= v->width || y >= v->height) return;
        uint8_t* q = p + (size_t)y * v->pitch + (size_t)x * 4;
        q[0] = (uint8_t)(color & 0xFF);
        q[1] = (uint8_t)((color >> 8) & 0xFF);
        q[2] = (uint8_t)((color >> 16) & 0xFF);
        q[3] = 255;
    };
    for (int i = 0; i < t; ++i) {
        for (int x = 0; x < v->width; ++x) { px(x, i); px(x, v->height - 1 - i); }
        for (int y = 0; y < v->height; ++y) { px(i, y); px(v->width - 1 - i, y); }
    }
    /* 中心十字 */
    int cx = v->width / 2, cy = v->height / 2, arm = 28;
    for (int i = -arm; i <= arm; ++i) { px(cx + i, cy); px(cx, cy + i); }
    return PP_CHAIN_CONTINUE;
}

/* ---------------- 工具 ---------------- */
static void ensureOutDir(PP_Host* h) {
    if (g_outDir[0]) return;
    const char* d = g_api->config_get(h, "outdir", "");
    if (d && d[0]) {
        snprintf(g_outDir, sizeof(g_outDir), "%s", d);
    } else {
        snprintf(g_outDir, sizeof(g_outDir), "%s", g_api->app_dir(h));
    }
}

static bool saveCurrentFrame(PP_Host* h, const char* dir, bool gray, char* outPath, int outCap) {
    PP_FrameView view{};
    if (g_api->acquire_frame(h, &view) != 0) {
        g_api->log(h, PP_LOG_WARN, "snapshot: 当前没有可用的视频帧");
        return false;
    }

    if (gray) {
        /* 原地灰度化（对副本操作，不影响屏幕） */
        for (int y = 0; y < view.height; ++y) {
            uint8_t* row = view.data + (size_t)y * view.pitch;
            for (int x = 0; x < view.width; ++x) {
                uint8_t* q = row + (size_t)x * 4;
                int luma = (q[0] * 77 + q[1] * 150 + q[2] * 29) >> 8;
                q[0] = q[1] = q[2] = (uint8_t)luma;
            }
        }
    } else {
        /* 加 2px 白框，证明像素确实被改写 */
        for (int y = 0; y < view.height; ++y) {
            uint8_t* row = view.data + (size_t)y * view.pitch;
            bool edgeRow = (y < 2 || y >= view.height - 2);
            for (int x = 0; x < view.width; ++x) {
                if (!edgeRow && x >= 2 && x < view.width - 2) continue;
                uint8_t* q = row + (size_t)x * 4;
                q[0] = q[1] = q[2] = 245;
                q[3] = 255;
            }
        }
    }

    uint64_t ts = g_api->ticks_ms(h);
    const char* base = g_outDir;
    snprintf(outPath, outCap, "%s/snapshot_%04d%s.png", dir && dir[0] ? dir : base,
             ++g_counter, gray ? "_gray" : "");

    int r = g_api->save_png(h, outPath, view.data, view.width, view.height, view.pitch);

    /* 回写：把带白框的版本写回当前帧，屏幕上也能看到（acquire/release 的对称语义） */
    g_api->release_frame(h, &view);

    if (r == 0) {
        char key[32];
        g_api->config_set(h, "last", outPath);
        snprintf(key, sizeof(key), "%d", g_counter);
        g_api->config_set(h, "count", key);
        g_api->log(h, PP_LOG_INFO, "已保存截图: %s (%dx%d)", outPath, view.width, view.height);
        return true;
    }
    g_api->log(h, PP_LOG_ERROR, "保存 PNG 失败: %s", outPath);
    return false;
}

/* ---------------- 命令 ---------------- */
static int cmdSnapshot(PP_Host* h, int argc, const char** argv, void* user) {
    (void)user;
    ensureOutDir(h);
    char path[640] = {0};
    if (saveCurrentFrame(h, argc >= 1 ? argv[0] : g_outDir, false, path, sizeof(path))) {
        g_api->set_window_subtitle(h, "snapshot saved");
        return 0;
    }
    return -1;
}

static int cmdSnapshotGray(PP_Host* h, int argc, const char** argv, void* user) {
    (void)user;
    ensureOutDir(h);
    char path[640] = {0};
    if (saveCurrentFrame(h, argc >= 1 ? argv[0] : g_outDir, true, path, sizeof(path))) {
        g_api->set_window_subtitle(h, "gray snapshot saved");
        return 0;
    }
    return -1;
}

/* 标记框的帧处理器**按需挂载**（2026-09-12）：`stamp off`（默认）时不占帧链。
 * 理由与 video_effects 的 syncChain 相同 —— 链非空会迫使渲染层走 CPU RGBA 路线
 * （插件的契约是可写 RGBA 像素），而默认状态下这条链本该是空的。 */
static void syncStamp(PP_Host* h) {
    if (g_stampOn && !g_stampProc) {
        /* 优先级 200：排在效果类插件之后，保证标记框画在最终画面上 */
        g_stampProc = g_api->set_frame_processor(h, &stampProcessor, nullptr, 200);
        if (g_stampProc)
            g_api->log(h, PP_LOG_INFO, "已挂标记框帧处理器 (id=%d)", g_stampProc);
    } else if (!g_stampOn && g_stampProc) {
        const int old = g_stampProc;
        g_api->remove_frame_processor(h, old);
        g_stampProc = 0;
        g_api->log(h, PP_LOG_INFO, "标记框已关，撤销帧处理器 (id=%d)", old);
    }
}

static int cmdStamp(PP_Host* h, int argc, const char** argv, void* user) {
    (void)user;
    if (argc >= 1) {
        if (strcmp(argv[0], "on") == 0) g_stampOn = 1;
        else if (strcmp(argv[0], "off") == 0) g_stampOn = 0;
        else if (strcmp(argv[0], "toggle") == 0) g_stampOn = !g_stampOn;
    } else {
        g_stampOn = !g_stampOn;
    }
    syncStamp(h);
    g_api->config_set(h, "stamp", g_stampOn ? "1" : "0");
    g_api->set_window_subtitle(h, g_stampOn ? "STAMP ON" : nullptr);
    g_api->log(h, PP_LOG_INFO, "标记框: %s", g_stampOn ? "开" : "关");
    return 0;
}

static int cmdCount(PP_Host* h, int argc, const char** argv, void* user) {
    (void)argc; (void)argv; (void)user;
    char buf[256];
    const char* last = g_api->config_get(h, "last", "(none)");
    snprintf(buf, sizeof(buf), "snapshot: 本次会话累计 %d 张，上次输出 %s", g_counter, last);
    g_api->log(h, PP_LOG_INFO, "%s", buf);
    g_api->set_window_subtitle(h, buf);
    return g_counter;
}

/* ---------------- 生命周期 ---------------- */
static int onLoad(PP_Host* host, const PP_HostApi* api) {
    if (api->abi_version != PP_ABI_VERSION || api->struct_size < sizeof(PP_HostApi)) return -1;
    g_host = host;
    g_api = api;

    /* 读回持久化配置 */
    const char* cnt = api->config_get(host, "count", "0");
    g_counter = cnt ? atoi(cnt) : 0;
    const char* stamp = api->config_get(host, "stamp", "0");
    g_stampOn = stamp && stamp[0] == '1' ? 1 : 0;
    const char* od = api->config_get(host, "outdir", "");
    if (od && od[0]) snprintf(g_outDir, sizeof(g_outDir), "%s", od);

    api->register_command(host, "snapshot",      "snapshot [dir]       保存当前帧为 PNG", &cmdSnapshot, nullptr);
    api->register_command(host, "snapshot.gray", "snapshot.gray [dir]  保存灰度版当前帧", &cmdSnapshotGray, nullptr);
    api->register_command(host, "stamp",         "stamp on|off|toggle  给画面加/去标记框", &cmdStamp, nullptr);
    api->register_command(host, "snapshot.count", "snapshot.count      查询累计截图数", &cmdCount, nullptr);
    return 0;
}

static int onStart(PP_Host* host) {
    /* 只有配置里存着 "stamp on" 才挂链 —— 默认不占链。 */
    syncStamp(host);
    g_api->log(host, PP_LOG_INFO, "snapshot 启动 (帧处理器 id=%d, 累计 %d 张)", g_stampProc, g_counter);
    return 0;
}

static void onStop(PP_Host* host) {
    if (g_stampProc) { g_api->remove_frame_processor(host, g_stampProc); g_stampProc = 0; }
    g_api->config_set(host, "outdir", g_outDir);
    g_api->log(host, PP_LOG_INFO, "snapshot 停止");
}

static void onUnload(PP_Host* host) { g_api->log(host, PP_LOG_INFO, "snapshot 卸载"); }

static PP_Plugin g_plugin = {
    PP_ABI_VERSION,
    "snapshot",
    "1.0.0",
    "PluginPlayer 内置",
    "截图与画面标记：帧处理链 + 当前帧副本 + 宿主 PNG 写出 + 配置持久化",
    0,
    &onLoad, &onStart, &onStop, &onUnload,
};

PP_PLUGIN_DECL(&g_plugin)
