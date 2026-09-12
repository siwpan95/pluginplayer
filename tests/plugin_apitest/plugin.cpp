/* plugin_apitest — 插件 API 回归测试插件（**仅供测试，不随 dist/ 发布**）
 * ---------------------------------------------------------------------------
 * 专门验证一个曾经真实存在的缺陷：
 *   FrameChain 与 EventBus 各有自己的句柄计数器，**都从 1 开始**。
 *   subscribe() 早期直接把内部 id 返回给插件，于是同一插件订阅
 *   「video.frame」（走帧链）与任意普通主题（走事件总线）会拿到**相同**句柄；
 *   unsubscribe() 又会先扫帧链，结果撤错对象 —— 撤销帧订阅把主题订阅也干掉了。
 *
 * 用法：
 *   g++ -std=c++17 -O2 -shared -Isrc/sdk -static-libgcc -static-libstdc++ \
 *       -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic \
 *       tests/plugin_apitest/plugin.cpp -o build/plugin_apitest.dll
 *   把 DLL 放进 dist/plugins/，启动播放器后调用命令 `api.check`：
 *     返回 0  = 全部通过
 *     返回 >0 = 失败项数量（每项都会打日志说明）
 * 检查完毕后请把 DLL 从 dist/plugins/ 删除（它不属于发布内容）。
 */
#include "player_plugin.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

static PP_Host*          g_host = nullptr;
static const PP_HostApi* g_api  = nullptr;

static int g_frameSubId = 0;      /* subscribe(video.frame) 返回的句柄 */
static int g_tickSubId  = 0;      /* subscribe(app.tick)    返回的句柄 */
static int g_tickHits   = 0;
static int g_frameHits  = 0;

static int onTick(PP_EventCtx* ctx) { (void)ctx; g_tickHits++; return 0; }
static int onFrame(PP_EventCtx* ctx) { (void)ctx; g_frameHits++; return PP_CHAIN_CONTINUE; }

static void say(const char* fmt, ...) {
    char body[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    if (g_api && g_host) g_api->log(g_host, PP_LOG_INFO, "[apitest] %s", body);
}

/* 命令：api.check —— 返回失败项数量（0 = 通过） */
static int cmdCheck(PP_Host* h, int argc, const char** argv, void* user) {
    (void)argc; (void)argv; (void)user;
    int fails = 0;

    /* 1) 两类订阅句柄必须来自同一命名空间且互不相同 */
    if (g_frameSubId == 0 || g_tickSubId == 0) {
        say("FAIL 订阅失败: frameSubId=%d tickSubId=%d", g_frameSubId, g_tickSubId);
        return ++fails;
    }
    if (g_frameSubId == g_tickSubId) {
        say("FAIL 帧订阅与主题订阅句柄撞号 (#%d) —— 命名空间未隔离", g_frameSubId);
        fails++;
    } else {
        say("PASS 句柄唯一: video.frame=#%d  app.tick=#%d", g_frameSubId, g_tickSubId);
    }

    /* 2) 撤销帧订阅后，主题订阅必须仍然有效 */
    {
        int before = g_tickHits;
        g_api->unsubscribe(h, g_frameSubId);
        g_api->publish(h, PP_TOPIC_APP_TICK, "{}");
        if (g_tickHits <= before) {
            say("FAIL 撤销帧订阅后主题订阅也失效了（unsubscribe 撤错对象）");
            fails++;
        } else {
            say("PASS 撤销帧订阅后主题订阅仍生效 (%d -> %d)", before, g_tickHits);
        }
    }

    /* 3) 撤销一个未知句柄不得影响任何正常订阅 */
    {
        int before = g_tickHits;
        g_api->unsubscribe(h, 999999);
        g_api->publish(h, PP_TOPIC_APP_TICK, "{}");
        if (g_tickHits <= before) {
            say("FAIL 未知句柄的 unsubscribe 影响了正常订阅");
            fails++;
        } else {
            say("PASS 未知句柄被安全忽略");
        }
    }

    /* 4) 重复撤销同一句柄必须幂等且不误伤 */
    {
        int before = g_tickHits;
        g_api->unsubscribe(h, g_frameSubId);   /* 第二次撤同一个（已撤销） */
        g_api->publish(h, PP_TOPIC_APP_TICK, "{}");
        if (g_tickHits <= before) {
            say("FAIL 重复撤销帧订阅影响了主题订阅");
            fails++;
        } else {
            say("PASS 重复撤销幂等");
        }
    }

    /* 5) 主题订阅本身仍可正常撤销 */
    {
        int before = g_tickHits;
        g_api->unsubscribe(h, g_tickSubId);
        g_api->publish(h, PP_TOPIC_APP_TICK, "{}");
        if (g_tickHits != before) {
            say("FAIL 撤销主题订阅后回调仍被调用 (%d -> %d)", before, g_tickHits);
            fails++;
        } else {
            say("PASS 主题订阅撤销生效");
        }
    }

    /* 6) set_topic_mode：自定义主题应能声明非 EMIT 模式（旧版无处可声明） */
    {
        const char* kTopic = "apitest.custom";
        int before = g_api->topic_mode(h, kTopic);
        int rc = g_api->set_topic_mode(h, kTopic, PP_DISPATCH_WATERFALL);
        int after = g_api->topic_mode(h, kTopic);
        if (rc != 0 || after != PP_DISPATCH_WATERFALL) {
            say("FAIL set_topic_mode 未生效（rc=%d, before=%d, after=%d）", rc, before, after);
            fails++;
        } else {
            say("PASS set_topic_mode 生效（%d -> %d）", before, after);
        }
        /* 非法模式必须被拒 */
        if (g_api->set_topic_mode(h, kTopic, 999) == 0) {
            say("FAIL 非法模式值竟然被接受");
            fails++;
        } else {
            say("PASS 非法模式值被拒");
        }
    }

    /* 7) 写入白名单：写 exeDir 之外必须失败（宿主强制，插件无法绕过） */
    {
        const char* outside = "C:/Windows/Temp/pp_apitest_should_fail.png";
        static uint8_t px[4 * 4 * 4] = {0};
        int rc = g_api->save_png(h, outside, px, 4, 4, 16);
        if (rc == 0) {
            say("FAIL 越界写文件竟然成功: %s", outside);
            fails++;
        } else {
            say("PASS 越界写文件被拒");
        }
    }

    say("api.check 结果: %s（失败 %d 项，帧回调 %d 次）",
        fails == 0 ? "ALL PASS" : "FAILED", fails, g_frameHits);
    return fails;
}

static int onLoad(PP_Host* host, const PP_HostApi* api) {
    if (!api || api->abi_version != PP_ABI_VERSION ||
        api->struct_size < sizeof(PP_HostApi)) return -1;
    g_host = host;
    g_api = api;

    /* 关键：同一插件先后订阅两类主题（老版本会拿到同一个句柄） */
    g_frameSubId = api->subscribe(host, PP_TOPIC_VIDEO_FRAME, &onFrame, nullptr);
    g_tickSubId  = api->subscribe(host, PP_TOPIC_APP_TICK,  &onTick,  nullptr);
    say("onLoad: video.frame=#%d app.tick=#%d", g_frameSubId, g_tickSubId);

    api->register_command(host, "api.check",
                         "api.check  插件 API 回归检查（返回 0 = 通过）",
                         &cmdCheck, nullptr);
    return 0;
}

static void onUnload(PP_Host* host) {
    if (g_api) g_api->log(host, PP_LOG_INFO, "[apitest] 卸载");
}

static PP_Plugin g_plugin = {
    PP_ABI_VERSION,
    "apitest",
    "1.0.0",
    "PluginPlayer 测试",
    "插件 API 回归测试：订阅句柄命名空间隔离 + unsubscribe 精确性（不随发布物分发）",
    0,
    &onLoad, nullptr, nullptr, &onUnload,
};

PP_PLUGIN_DECL(&g_plugin)
