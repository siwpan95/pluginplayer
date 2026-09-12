/* hotkey_ext — 扩展快捷键插件（BAIL 事件语义演示）
 * ---------------------------------------------------------------------------
 * 演示的插件能力：
 *   - 订阅 ui.key，并通过 ctx->consumed = 1 表示「这个按键我处理了」（BAIL 语义）
 *     宿主的默认快捷键只有在没有插件消费时才会执行
 *   - register_service("hotkeys")：把自身能力暴露给其它插件查询（跨插件协作）
 *   - 通过 invoke_command() 调用别的插件注册的命令（插件间解耦调用）
 *   - config_get/config_set 持久化「是否启用」「已处理按键数」
 *   - write_text_file 导出状态
 *
 * 本插件不直接依赖 video_effects / snapshot，只通过"命令名"调用它们 —— 如果对方
 * 没加载，invoke_command 返回 -1，插件自行降级，不会崩。
 */
#include "player_plugin.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

static PP_Host*          g_host = nullptr;
static const PP_HostApi* g_api  = nullptr;

static int  g_enabled = 1;
static long g_handled = 0;
static int  g_fastMode = 0;
static char g_lastAction[128] = "-";

/* SDL 键码（值是 SDL3 的 SDLK_*，这里写死整数避免插件依赖 SDL 头文件） */
enum {
    K_Z = 'z', K_X = 'x', K_G = 'g', K_I = 'i', K_E = 'e', K_V = 'v',
    K_N = 'n', K_B = 'b', K_T = 't', K_A = 'a', K_Y = 'y', K_U = 'u',
};

/* ---------------- 供其它插件查询的服务 ---------------- */
static const char* svcDescribe(void) {
    static char buf[160];
    snprintf(buf, sizeof(buf), "%s  handled=%ld  last=%s",
             g_enabled ? "on" : "off", g_handled, g_lastAction);
    return buf;
}
static int svcKeyCount(void) { return 12; }

typedef struct HotkeysService {
    const char* (*describe)(void);
    int         (*keyCount)(void);
} HotkeysService;

static HotkeysService g_service = { &svcDescribe, &svcKeyCount };

static const char* kHelpText =
    "G=gray I=invert E=edge V=viz-cycle N=snapshot B=gray-shot T=stamp "
    "Z/X=-/+1s A=speed x3 Y=export U=toggle-plugin";

/* ---------------- 按键处理 ---------------- */
static int callCmd(const char* name, int argc, const char** argv) {
    int r = g_api->invoke_command(g_host, name, argc, argv);
    snprintf(g_lastAction, sizeof(g_lastAction), "%s -> %d", name, r);
    return r;
}

static int onKey(PP_EventCtx* ctx) {
    if (!g_enabled) return 0;
    PP_KeyEvent* ke = (PP_KeyEvent*)ctx->payload;
    if (!ke || !ke->down || ke->repeat) return 0;

    bool handled = true;
    switch (ke->keycode) {
        case K_G: { const char* a[1] = {"gray"}; callCmd("effect.toggle", 1, a); break; }
        case K_I: { const char* a[1] = {"invert"}; callCmd("effect.toggle", 1, a); break; }
        case K_E: { const char* a[1] = {"edge"}; callCmd("effect.toggle", 1, a); break; }
        case K_V: { const char* a[1] = {"cycle"}; callCmd("viz.mode", 1, a); break; }
        case K_N: callCmd("snapshot", 0, nullptr); break;
        case K_B: callCmd("snapshot.gray", 0, nullptr); break;
        case K_T: callCmd("stamp", 0, nullptr); break;
        case K_Z: g_api->seek_relative(g_host, -1.0);
                  snprintf(g_lastAction, sizeof(g_lastAction), "seek -1s"); break;
        case K_X: g_api->seek_relative(g_host, 1.0);
                  snprintf(g_lastAction, sizeof(g_lastAction), "seek +1s"); break;
        case K_A:
            g_fastMode = !g_fastMode;
            g_api->set_speed(g_host, g_fastMode ? 3.0 : 1.0);
            snprintf(g_lastAction, sizeof(g_lastAction), "speed %s", g_fastMode ? "3x" : "1x");
            break;
        case K_Y: {
            char path[600];
            snprintf(path, sizeof(path), "%s/hotkey_ext_state.txt", g_api->app_dir(g_host));
            char body[512];
            snprintf(body, sizeof(body),
                     "hotkey_ext v1.0.0\nenabled=%d\nhandled=%ld\nlast_action=%s\nkeys=%s\n",
                     g_enabled, g_handled, g_lastAction, kHelpText);
            int r = g_api->write_text_file(g_host, path, body);
            snprintf(g_lastAction, sizeof(g_lastAction), "export -> %d", r);
            g_api->log(g_host, PP_LOG_INFO, "热键状态已导出: %s (r=%d)", path, r);
            break;
        }
        case K_U:
            g_enabled = !g_enabled;
            g_api->config_set(g_host, "enabled", g_enabled ? "1" : "0");
            snprintf(g_lastAction, sizeof(g_lastAction), "self %s", g_enabled ? "on" : "off");
            break;
        default:
            handled = false;
            break;
    }

    if (!handled) return 0;

    g_handled++;
    ctx->consumed = 1;       /* BAIL：告诉宿主这个按键已被插件处理 */
    g_api->set_window_subtitle(g_host, g_lastAction);
    g_api->log(g_host, PP_LOG_INFO, "热键处理: key=%d -> %s", ke->keycode, g_lastAction);
    return 1;
}

/* ---------------- 命令 ---------------- */
static int cmdList(PP_Host* h, int argc, const char** argv, void* user) {
    (void)argc; (void)argv; (void)user;
    char buf[512];
    snprintf(buf, sizeof(buf), "扩展快捷键 (%s): %s", g_enabled ? "启用" : "停用", kHelpText);
    g_api->log(h, PP_LOG_INFO, "%s", buf);
    g_api->set_window_subtitle(h, g_enabled ? "hotkeys on" : "hotkeys off");
    return g_enabled ? (int)g_handled : 0;
}

static int cmdEnable(PP_Host* h, int argc, const char** argv, void* user) {
    (void)user;
    if (argc >= 1) {
        if (strcmp(argv[0], "on") == 0) g_enabled = 1;
        else if (strcmp(argv[0], "off") == 0) g_enabled = 0;
        else if (strcmp(argv[0], "toggle") == 0) g_enabled = !g_enabled;
    } else {
        g_enabled = !g_enabled;
    }
    g_api->config_set(h, "enabled", g_enabled ? "1" : "0");
    g_api->log(h, PP_LOG_INFO, "扩展快捷键: %s", g_enabled ? "启用" : "停用");
    return g_enabled;
}

/* ---------------- 生命周期 ---------------- */
static int onLoad(PP_Host* host, const PP_HostApi* api) {
    if (api->abi_version != PP_ABI_VERSION || api->struct_size < sizeof(PP_HostApi)) return -1;
    g_host = host;
    g_api = api;

    const char* e = api->config_get(host, "enabled", "1");
    g_enabled = (e && e[0] == '0') ? 0 : 1;

    int sub = api->subscribe(host, PP_TOPIC_UI_KEY, &onKey, nullptr);
    api->register_service(host, "hotkeys", "1.0.0", &g_service);
    api->register_command(host, "hotkey.list",   "hotkey.list           列出扩展快捷键", &cmdList, nullptr);
    api->register_command(host, "hotkey.enable", "hotkey.enable on|off|toggle  启用/停用扩展快捷键", &cmdEnable, nullptr);
    api->log(host, PP_LOG_INFO, "hotkey_ext 已加载 (ui.key sub_id=%d, enabled=%d, 历史处理 %ld 次)",
             sub, g_enabled, g_handled);
    return 0;
}

static int onStart(PP_Host* host) {
    g_api->log(host, PP_LOG_INFO, "hotkey_ext 启动: %s", kHelpText);
    return 0;
}

static void onStop(PP_Host* host) {
    char b[32];
    snprintf(b, sizeof(b), "%ld", g_handled);
    g_api->config_set(host, "handled_total", b);
    g_api->set_window_subtitle(host, nullptr);
    g_api->log(host, PP_LOG_INFO, "hotkey_ext 停止（本次处理 %ld 次按键）", g_handled);
}

static void onUnload(PP_Host* host) { g_api->log(host, PP_LOG_INFO, "hotkey_ext 卸载"); }

static PP_Plugin g_plugin = {
    PP_ABI_VERSION,
    "hotkey_ext",
    "1.0.0",
    "PluginPlayer 内置",
    "扩展快捷键：订阅 ui.key 并消费按键，暴露 hotkeys 服务，可调用其它插件的命令",
    0,
    &onLoad, &onStart, &onStop, &onUnload,
};

PP_PLUGIN_DECL(&g_plugin)
