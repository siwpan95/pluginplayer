/* subtitles — 字幕渲染插件（P1-c）
 * ---------------------------------------------------------------------------
 * 架构红线：内核只提供**能力通道**（把字幕包交给 FFmpeg 解码、把结果以
 * subtitle.line 事件暴露），**不做渲染**；渲染在本插件里完成。
 *
 * 本插件做三件事：
 *   1. 注册命令 subtitle.load <path.srt> —— 把"加载外部字幕"请求以事件
 *      subtitle.load.req 发给内核（内核真正加载，避免给 ABI 加方法）。
 *   2. 订阅 subtitle.line 事件 —— 内核每帧在字幕文本变化时广播当前行。
 *   3. 订阅 osd.draw —— 用 P1-a 真字体把当前字幕行画在画面底部黑边里。
 *
 * 轻路线：内核已经剥离了 ASS 样式标签，subtitle.line 里只有纯文本与 '\n'。
 *
 * 注意：本文件只能调用 PP_HostApi 里的函数指针与自身代码 —— 内核的
 * util.cpp 等被链进 exe 而非本 DLL，直接调会链接失败。所以 JSON 解析/拼接、
 * 分行等都在本文件内自实现。
 */
#include "player_plugin.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static PP_Host*          g_host = nullptr;
static const PP_HostApi* g_api  = nullptr;

/* 当前字幕行（由 subtitle.line 事件更新，osd.draw 时读取绘制）。 */
static std::string g_line;

/* ---- 小工具：JSON 解析/拼接/分行（不依赖内核，避免链接 exe 的符号） ---- */

/* 从 JSON 里取顶层字符串键的值，处理 \n \t \\ \" 等基本转义（够用，不追 Unicode）。 */
static std::string extractJsonText(const char* json, const char* key) {
    std::string out;
    if (!json || !key) return out;
    std::string k = std::string("\"") + key + "\"";
    const char* p = strstr(json, k.c_str());
    if (!p) return out;
    p += k.size();
    while (*p && *p != ':') p++;
    if (*p == ':') p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return out;
    p++;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            if (!*p) break;
            switch (*p) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                default:  out.push_back(*p); break;
            }
            p++;
        } else {
            out.push_back(*p);
            p++;
        }
    }
    return out;
}

/* 把本地路径拼成 {"path":"..."}（转义 " 与 \，路径里可能出现 \\）。 */
static void buildLoadReq(const char* path, std::string& out) {
    out = "{\"path\":\"";
    for (const char* p = path; *p; ++p) {
        if (*p == '"' || *p == '\\') { out.push_back('\\'); out.push_back(*p); }
        else if (*p == '\n') out += "\\n";
        else if (*p == '\t') out += "\\t";
        else out.push_back(*p);
    }
    out += "\"}";
}

static std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> v;
    std::string cur;
    for (char c : s) {
        if (c == '\n') { v.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    v.push_back(cur);
    return v;
}

/* 画面外的可用空间（黑边）。同 osd_stats 的做法：把字幕放进底部黑边，不遮画面。 */
typedef struct Bands { float top, bottom, left, right; } Bands;

static Bands bandsOf(PP_Host* h, const PP_OsdApi* osd) {
    Bands b{0, 0, 0, 0};
    float w = 0, hh = 0;
    if (!osd->size(h, &w, &hh)) return b;
    float x = 0, y = 0, vw = 0, vh = 0;
    if (osd->video_rect && osd->video_rect(h, &x, &y, &vw, &vh)) {
        b.top = y;
        b.bottom = hh - (y + vh);
        b.left = x;
        b.right = w - (x + vw);
    }
    return b;
}

/* ---- subtitle.line：更新当前行 ---- */
static int onSubLine(PP_EventCtx* ctx) {
    g_line = extractJsonText(ctx->json, "text");
    return 0;
}

/* ---- subtitle.load 命令：把请求发给内核 ---- */
static int cmdSubtitleLoad(PP_Host* h, int argc, const char** argv, void* user) {
    (void)user;
    if (argc < 1 || !argv[0] || !argv[0][0]) {
        g_api->log(h, PP_LOG_WARN, "subtitle.load: 用法 subtitle.load <path.srt>");
        return -1;
    }
    std::string req;
    buildLoadReq(argv[0], req);
    g_api->publish(h, "subtitle.load.req", req.c_str());
    return 0;
}

/* ---- osd.draw：画当前字幕行 ---- */
static int onOsdDraw(PP_EventCtx* ctx) {
    PP_Host* h = ctx->host;
    const PP_OsdApi* osd = g_api->osd;
    if (!osd || !osd->size) return 0;
    if (g_line.empty()) return 0;          /* 没有字幕就不画 */

    float w = 0, hh = 0;
    if (!osd->size(h, &w, &hh)) return 0;

    float scale = 1.6f;
    std::vector<std::string> lines = splitLines(g_line);
    float lh = osd->line_height(h, scale);
    float totalH = lh * (int)lines.size() + 10.0f;

    Bands b = bandsOf(h, osd);
    /* 字幕贴在画面底边上方：底部黑边放得下就落进黑边，否则压在画面最下方。 */
    float boxBottom = (b.bottom >= totalH + 8.0f) ? (hh - 4.0f) : (hh - 10.0f);
    float boxY = boxBottom - totalH;

    float maxW = 0;
    for (auto& l : lines) maxW = std::max(maxW, osd->text_width(h, l.c_str(), scale));
    float boxX = (w - maxW) * 0.5f - 16.0f;
    float boxW = maxW + 32.0f;

    /* 半透明底板，提升可读性 */
    osd->panel(h, boxX, boxY, boxW, totalH, PP_RGBA(0, 0, 0, 150), PP_RGBA(255, 255, 255, 0));

    /* 逐行白字居中 */
    float ty = boxY + 6.0f;
    for (auto& l : lines) {
        float tw = osd->text_width(h, l.c_str(), scale);
        osd->text(h, (w - tw) * 0.5f, ty, l.c_str(), PP_RGB(255, 255, 255), scale);
        ty += lh;
    }
    return 0;
}

/* ---- 生命周期 ---- */
static int onLoad(PP_Host* host, const PP_HostApi* api) {
    if (api->abi_version != PP_ABI_VERSION || api->struct_size < sizeof(PP_HostApi)) {
        return -1;
    }
    g_host = host;
    g_api = api;

    int s1 = api->subscribe(host, "subtitle.line", &onSubLine, nullptr);
    int s2 = api->subscribe(host, PP_TOPIC_OSD_DRAW, &onOsdDraw, nullptr);
    api->register_command(host, "subtitle.load",
                          "subtitle.load <path.srt>   加载外部字幕（内核解码，本插件渲染）",
                          &cmdSubtitleLoad, nullptr);
    api->log(host, PP_LOG_INFO, "subtitles 已加载 (subtitle.line=%d, osd.draw=%d)", s1, s2);
    return 0;
}

static int onStart(PP_Host* host) {
    g_api->log(host, PP_LOG_INFO, "subtitles 启动");
    return 0;
}
static void onStop(PP_Host* host) { g_api->log(host, PP_LOG_INFO, "subtitles 停止"); }
static void onUnload(PP_Host* host) { g_api->log(host, PP_LOG_INFO, "subtitles 卸载"); }

static PP_Plugin g_plugin = {
    PP_ABI_VERSION,
    "subtitles",
    "1.0.0",
    "PluginPlayer 内置",
    "订阅 subtitle.line 事件，用真字体把字幕画在画面底部（内核只解码、不渲染）",
    0,
    &onLoad, &onStart, &onStop, &onUnload,
};

PP_PLUGIN_DECL(&g_plugin)
