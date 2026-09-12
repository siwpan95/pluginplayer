/* hello_plugin — 最小原生插件示例（docs/PLUGIN_GUIDE.md 里逐行讲解的那个）
 *
 * 编译（在工程根目录执行）:
 *   F:\mingw64\mingw64\bin\g++.exe -std=c++17 -O2 -shared -Isrc/sdk \
 *     examples/hello_plugin/plugin.cpp -o dist/plugins/hello_plugin.dll \
 *     -static-libgcc -static-libstdc++ -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic
 *
 * 演示：
 *   - 帧处理链（set_frame_processor）：直接原地改像素
 *   - 注册命令（register_command）：hello.status
 *   - 生命周期四件套与 ABI 校验
 */
#include "player_plugin.h"

#include <cstdio>

static const PP_HostApi* g_api = nullptr;
static int g_frames = 0;

/* 帧处理链回调：拿到的就是即将显示的像素（RGBA8888，内存里是 R,G,B,A） */
static int onFrame(PP_Host* h, PP_FrameView* v, void* user) {
    (void)h; (void)user;
    g_frames++;
    /* 每 60 帧在左上角画一个 32x32 的红块，演示「直接改像素」 */
    if (g_frames % 60 == 0 && v && v->data) {
        for (int y = 0; y < 32 && y < v->height; ++y) {
            uint8_t* row = v->data + (size_t)y * v->pitch;
            for (int x = 0; x < 32 && x < v->width; ++x) {
                uint8_t* q = row + (size_t)x * 4;
                q[0] = 240; q[1] = 40; q[2] = 40; q[3] = 255;   /* R G B A：q[0]=R */
            }
        }
    }
    return PP_CHAIN_CONTINUE;   /* 也可以 BAIL(1) / DROP(2) */
}

/* 命令：hello.status */
static int cmdStatus(PP_Host* h, int argc, const char** argv, void* user) {
    (void)argc; (void)argv; (void)user;
    char buf[128];
    snprintf(buf, sizeof(buf), "hello_plugin: 已处理 %d 帧", g_frames);
    g_api->log(h, PP_LOG_INFO, "%s", buf);
    g_api->set_window_subtitle(h, buf);
    return g_frames;
}

/* 生命周期 */
static int onLoad(PP_Host* host, const PP_HostApi* api) {
    if (api->abi_version != PP_ABI_VERSION || api->struct_size < sizeof(PP_HostApi))
        return -1;                      /* ABI 不匹配，宿主会拒绝加载 */
    g_api = api;
    api->register_command(host, "hello.status", "hello.status  查询处理帧数", &cmdStatus, nullptr);
    api->log(host, PP_LOG_INFO, "hello_plugin 已加载");
    return 0;
}

static int onStart(PP_Host* host) {
    /* 优先级小的先跑；内置的 video_effects 是 100、snapshot 的标记框是 200 */
    g_api->set_frame_processor(host, &onFrame, nullptr, 50);
    return 0;
}

static void onStop(PP_Host* host)   { g_api->log(host, PP_LOG_INFO, "hello_plugin 停止"); }
static void onUnload(PP_Host* host) { g_api->log(host, PP_LOG_INFO, "hello_plugin 卸载"); }

static PP_Plugin g_plugin = {
    PP_ABI_VERSION, "hello_plugin", "1.0.0", "示例作者",
    "最小原生插件示例：帧处理链 + 注册命令",
    0, &onLoad, &onStart, &onStop, &onUnload,
};

PP_PLUGIN_DECL(&g_plugin)   /* 导出 pp_plugin_query / pp_plugin_abi_version */
