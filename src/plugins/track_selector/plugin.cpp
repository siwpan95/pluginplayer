/* track_selector — 多轨选择插件（P1-d）
 * ---------------------------------------------------------------------------
 * 架构红线（与字幕一致）：内核只提供**能力通道**（把全部轨以 media.tracks 事件暴露，
 * 把"切换某轨"以 track.select.req 事件交给内核执行 reopen），**不做轨选择 UI**。
 * 本插件做两件事：
 *   1. 订阅 media.tracks 事件 —— 缓存当前媒体的全部轨列表。
 *   2. 注册命令：
 *        track.list             打印缓存的轨列表（JSON）
 *        track.select <type> <index>
 *                              —— 把"切换某轨"请求以 track.select.req 事件发给内核
 *                                （内核真正 reopen，避免给 ABI 加方法）
 *
 * 注意：本文件只能调用 PP_HostApi 里的函数指针与自身代码 —— 内核的
 * util.cpp 等被链进 exe 而非本 DLL，直接调会链接失败。所以 JSON 解析/拼接
 * 都在本文件内自实现。插件也不链接 SDL3 / FFmpeg（同 subtitles 插件）。
 */
#include "player_plugin.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static PP_Host*          g_host = nullptr;
static const PP_HostApi* g_api  = nullptr;

/* 当前媒体的全部轨列表（由 media.tracks 事件更新；track.list 命令打印它）。 */
static std::string g_tracks;

/* ---- media.tracks：更新缓存 ---- */
static int onMediaTracks(PP_EventCtx* ctx) {
    if (ctx->json) g_tracks = ctx->json;
    return 0;
}

/* ---- track.list 命令：打印缓存的轨列表 ---- */
static int cmdTrackList(PP_Host* h, int argc, const char** argv, void* user) {
    (void)argc; (void)argv; (void)user;
    if (g_tracks.empty()) {
        g_api->log(h, PP_LOG_WARN, "track.list: 还没有打开媒体（media.tracks 为空）");
        return -1;
    }
    /* 打印到 stdout，供 cmd 调用方 / 控制台查看。 */
    printf("%s\n", g_tracks.c_str());
    return 0;
}

/* ---- track.select <type> <index> 命令：把请求发给内核 ---- */
static int cmdTrackSelect(PP_Host* h, int argc, const char** argv, void* user) {
    (void)user;
    if (argc < 2 || !argv[0] || !argv[1] || !argv[0][0] || !argv[1][0]) {
        g_api->log(h, PP_LOG_WARN, "track.select: 用法 track.select <type> <index>");
        return -1;
    }
    int type  = std::atoi(argv[0]);
    int index = std::atoi(argv[1]);
    char req[64];
    std::snprintf(req, sizeof(req), "{\"type\":%d,\"index\":%d}", type, index);
    g_api->publish(h, PP_TOPIC_TRACK_SELECT_REQ, req);
    return 0;
}

/* ---- 生命周期 ---- */
static int onLoad(PP_Host* host, const PP_HostApi* api) {
    if (api->abi_version != PP_ABI_VERSION || api->struct_size < sizeof(PP_HostApi)) {
        return -1;
    }
    g_host = host;
    g_api  = api;

    int s1 = api->subscribe(host, PP_TOPIC_MEDIA_TRACKS, &onMediaTracks, nullptr);
    api->register_command(host, "track.list",
                          "track.list              打印当前媒体的全部轨（video/audio/subtitle）",
                          &cmdTrackList, nullptr);
    api->register_command(host, "track.select",
                          "track.select <type> <index>   切换某类型轨（内核 reopen，type=AVMediaType）",
                          &cmdTrackSelect, nullptr);
    api->log(host, PP_LOG_INFO, "track_selector 已加载 (media.tracks=%d)", s1);
    return 0;
}

static int onStart(PP_Host* host) {
    g_api->log(host, PP_LOG_INFO, "track_selector 启动");
    return 0;
}
static void onStop(PP_Host* host) { g_api->log(host, PP_LOG_INFO, "track_selector 停止"); }
static void onUnload(PP_Host* host) { g_api->log(host, PP_LOG_INFO, "track_selector 卸载"); }

static PP_Plugin g_plugin = {
    PP_ABI_VERSION,
    "track_selector",
    "1.0.0",
    "PluginPlayer 内置",
    "订阅 media.tracks 缓存轨列表，注册 track.list/track.select（内核 reopen 执行切轨）",
    0,
    &onLoad, &onStart, &onStop, &onUnload,
};

PP_PLUGIN_DECL(&g_plugin)
