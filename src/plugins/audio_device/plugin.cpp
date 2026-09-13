/* audio_device — 音频输出设备选择插件（P1-d）
 * ---------------------------------------------------------------------------
 * 架构红线（与字幕一致）：内核只提供**能力通道**（把可用音频输出设备以
 * audio.devices 事件暴露，把"切换设备"以 audio.device.set.req 事件交给内核执行
 * reopen 音频流），**不做设备选择 UI**。本插件做两件事：
 *   1. 订阅 audio.devices 事件 —— 缓存可用音频输出设备列表。
 *   2. 注册命令：
 *        audio.list           打印缓存的设备列表（JSON 数组）
 *        audio.set <id>      —— 把"切换设备"请求以 audio.device.set.req 事件发给内核
 *                              （内核真正 reopen 音频流，避免给 ABI 加方法）
 *
 * 注意：本文件只能调用 PP_HostApi 里的函数指针与自身代码 —— 插件不链接 SDL3，
 * 设备枚举由内核（listAudioDevicesJson）完成，本插件只缓存与转发。
 */
#include "player_plugin.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static PP_Host*          g_host = nullptr;
static const PP_HostApi* g_api  = nullptr;

/* 可用音频输出设备列表（由 audio.devices 事件更新；audio.list 命令打印它）。 */
static std::string g_devices;

/* ---- audio.devices：更新缓存 ---- */
static int onAudioDevices(PP_EventCtx* ctx) {
    if (ctx->json) g_devices = ctx->json;
    return 0;
}

/* ---- audio.list 命令：打印缓存的设备列表 ---- */
static int cmdAudioList(PP_Host* h, int argc, const char** argv, void* user) {
    (void)argc; (void)argv; (void)user;
    if (g_devices.empty()) {
        g_api->log(h, PP_LOG_WARN, "audio.list: 还没有设备列表（audio.devices 为空）");
        return -1;
    }
    printf("%s\n", g_devices.c_str());
    return 0;
}

/* ---- audio.set <id> 命令：把请求发给内核 ---- */
static int cmdAudioSet(PP_Host* h, int argc, const char** argv, void* user) {
    (void)user;
    if (argc < 1 || !argv[0] || !argv[0][0]) {
        g_api->log(h, PP_LOG_WARN, "audio.set: 用法 audio.set <device_id>");
        return -1;
    }
    int id = std::atoi(argv[0]);
    char req[64];
    std::snprintf(req, sizeof(req), "{\"id\":%d}", id);
    g_api->publish(h, PP_TOPIC_AUDIO_DEVICE_SET_REQ, req);
    return 0;
}

/* ---- 生命周期 ---- */
static int onLoad(PP_Host* host, const PP_HostApi* api) {
    if (api->abi_version != PP_ABI_VERSION || api->struct_size < sizeof(PP_HostApi)) {
        return -1;
    }
    g_host = host;
    g_api  = api;

    int s1 = api->subscribe(host, PP_TOPIC_AUDIO_DEVICES, &onAudioDevices, nullptr);
    api->register_command(host, "audio.list",
                          "audio.list              打印可用音频输出设备（JSON 数组）",
                          &cmdAudioList, nullptr);
    api->register_command(host, "audio.set",
                          "audio.set <device_id>   切换音频输出设备（内核 reopen 音频流）",
                          &cmdAudioSet, nullptr);
    api->log(host, PP_LOG_INFO, "audio_device 已加载 (audio.devices=%d)", s1);
    return 0;
}

static int onStart(PP_Host* host) {
    g_api->log(host, PP_LOG_INFO, "audio_device 启动");
    return 0;
}
static void onStop(PP_Host* host) { g_api->log(host, PP_LOG_INFO, "audio_device 停止"); }
static void onUnload(PP_Host* host) { g_api->log(host, PP_LOG_INFO, "audio_device 卸载"); }

static PP_Plugin g_plugin = {
    PP_ABI_VERSION,
    "audio_device",
    "1.0.0",
    "PluginPlayer 内置",
    "订阅 audio.devices 缓存设备列表，注册 audio.list/audio.set（内核 reopen 音频流执行切换）",
    0,
    &onLoad, &onStart, &onStop, &onUnload,
};

PP_PLUGIN_DECL(&g_plugin)
