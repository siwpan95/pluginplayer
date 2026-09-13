/* subtitles — libass 字幕渲染插件（P1-c → P1-e 升级）
 * ---------------------------------------------------------------------------
 * 架构红线（不变）：内核只解码、把字幕以事件总线暴露；渲染在本插件完成，不进 ABI。
 *
 * P1-e 起本插件用 libass 渲染（完整样式：字体/描边/阴影/对齐/覆盖标签 \an \b \c…）：
 *   1. 注册命令 subtitle.load <path.srt> —— 把"加载外部字幕"请求以事件
 *      subtitle.load.req 发给内核（内核真正加载并发 subtitle.ass 事件）。
 *   2. 订阅 subtitle.ass_header —— 内核每条媒体发一次（seek/换轨后重发）：
 *      新建/重置 libass track，并把完整 ASS 头喂进去（含 PlayRes 与默认样式）。
 *   3. 订阅 subtitle.ass —— 内核每解出一条字幕发一次：把 Dialogue 行喂进 track。
 *   4. 订阅 osd.draw —— 用 libass 渲染"当前播放位置"那一帧的字幕位图，
 *      经 overlay.set 叠加到视频画面（叠加层由内核合成，不碰内核渲染代码）。
 *   5. 订阅 subtitle.line（纯文本，旧通道）—— 仅作 libass 不可用时的回退渲染。
 *
 * 字体：随包 TTF（dist/assets/fonts/ZCOOLKuaiLe-Regular.ttf）由系统字体
 *   provider（DirectWrite / fontconfig）从 fonts 目录扫描加载——调用
 *   ass_set_fonts_dir + ASS_FONTPROVIDER_AUTODETECT，**不依赖 fontconfig 配置、
 *   也不使用 ass_add_font 内存字体**（本 libass 0.17.5/MSYS2 构建里内存字体在
 *   渲染时会 0xC0000005 崩溃，已实测验证）。内核 ASS 头统一用哨兵家族名
 *   PP_SUBTITLE_FONT_FAMILY("PluginPlayerSub")，本插件在收到 header 时替换为随包
 *   字体的真实 family 名（见 PP_BUNDLED_FONT_REAL），从而保持「内核只发中性哨兵、
 *   插件拥有真实字体」的解耦。字体缺失则降级为系统字体回退（CJK 可能缺字形）。
 *
 * 注意：本文件只能调用 PP_HostApi 里的函数指针与自身代码 —— 内核的
 * util.cpp 等被链进 exe 而非本 DLL，直接调会链接失败。所以 JSON 解析/拼接、
 * 分行、文件读取等都在本文件内自实现。
 */
#include "player_plugin.h"
#include <ass/ass.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <mutex>
#include <fstream>

static PP_Host*          g_host = nullptr;
static const PP_HostApi* g_api  = nullptr;

/* ---- libass 状态 ---- */
static ASS_Library*  g_lib   = nullptr;   /* libass 库句柄（进程级，一个即可） */
static ASS_Renderer* g_ren   = nullptr;   /* 渲染器 */
static ASS_Track*    g_track = nullptr;   /* 当前字幕轨（header 重置、ass 累积） */
static bool          g_libass_ok = false;/* libass 是否可用（否则走回退渲染） */
static int           g_overlay  = 0;     /* overlay 图层 id（0=未建） */
static std::mutex    g_track_mu;          /* 保护 g_track（解码线程与渲染线程并发） */

/* 回退渲染用的当前纯文本行（来自 subtitle.line）。 */
static std::string g_line;

/* 随包字体真实 family 名（ZCOOLKuaiLe-Regular.ttf 的 name 表里家族名；Mac-Roman
 * nameID 1 = "HappyZcool-2016"，与文件名 ZCOOLKuaiLe 不同，必须以 name 表为准）。
 * 内核 ASS 头统一用哨兵 PP_SUBTITLE_FONT_FAMILY("PluginPlayerSub")，本插件在收到
 * header 时把该哨兵替换为真实字体名，从而保持内核与具体字体解耦。
 * 若更换随包字体，必须同步更新本常量（或在此解析 TTF name 表得到真实名）。
 * 字体加载机制：ass_set_fonts_dir(<asset_dir>/fonts) + ASS_FONTPROVIDER_AUTODETECT，
 * 由系统 provider 扫描随包字体目录——本 libass 构建的 ass_add_font 内存字体路径
 * 在渲染时会 0xC0000005 崩溃，故严禁使用。 */
static const char* PP_BUNDLED_FONT_REAL = "HappyZcool-2016";

/* ---- 小工具：JSON 解析（不依赖内核） ---- */

/* 从 JSON 里取顶层字符串键的值，处理 \n \t \\ \" 等基本转义。 */
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

/* 把本地路径拼成 {"path":"..."}（转义 " 与 \）。 */
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

/* ---- libass 初始化 / 清理 ---- */

static bool initLibass(PP_Host* h) {
    g_lib = ass_library_init();
    if (!g_lib) {
        g_api->log(h, PP_LOG_WARN, "subtitles: ass_library_init 失败，降级为 OSD 真字体渲染");
        return false;
    }
    g_ren = ass_renderer_init(g_lib);
    if (!g_ren) {
        g_api->log(h, PP_LOG_WARN, "subtitles: ass_renderer_init 失败，降级为 OSD 真字体渲染");
        ass_library_done(g_lib); g_lib = nullptr;
        return false;
    }
    /* 用系统字体 provider（DirectWrite / fontconfig）扫描随包字体目录。
     * 不用 ass_add_font 内存字体：本 libass(0.17.5/MSYS2) 构建里内存字体在渲染时
     * 会 0xC0000005 崩溃（已实测：family 名一旦命中内存字体即崩）。
     * 随包字体目录 = <asset_dir>/fonts，里面放 ZCOOLKuaiLe-Regular.ttf 等。 */
    const char* adir = g_api->asset_dir ? g_api->asset_dir(h) : nullptr;
    std::string fdir = adir ? adir : ".";
    fdir += "/fonts";
    ass_set_fonts_dir(g_lib, fdir.c_str());
    ass_set_fonts(g_ren, nullptr, PP_BUNDLED_FONT_REAL, ASS_FONTPROVIDER_AUTODETECT, nullptr, 1);

    /* 仅做存在性校验（字体由 provider 扫描，无需读入内存）。缺失不致命：
     * libass 回退到系统字体，只是 CJK 可能缺字形。 */
    std::string fp = fdir + "/ZCOOLKuaiLe-Regular.ttf";
    if (!std::ifstream(fp)) {
        g_api->log(h, PP_LOG_WARN,
                   "subtitles: 随包字体缺失 %s，将回退系统字体（CJK 可能缺字形）", fp.c_str());
    }
    g_api->log(h, PP_LOG_INFO, "subtitles: libass 就绪（fonts_dir=%s, 字体 %s）",
               fdir.c_str(), PP_BUNDLED_FONT_REAL);
    return true;
}

static void freeLibass() {
    if (g_track) { ass_free_track(g_track); g_track = nullptr; }
    if (g_ren)   { ass_renderer_done(g_ren); g_ren = nullptr; }
    if (g_lib)   { ass_library_done(g_lib); g_lib = nullptr; }
    g_libass_ok = false;
}

/* ---- libass 位图合成（ASS_Image.color = 0xRRGGBBAA，低字节 alpha 0=不透明）---- */

static void compositeImage(uint8_t* dst, int W, int H, const ASS_Image* im) {
    int iw = im->w, ih = im->h;
    if (iw <= 0 || ih <= 0) return;
    int dx = im->dst_x, dy = im->dst_y;
    uint32_t color = im->color;
    uint8_t R = (uint8_t)((color >> 24) & 0xFF);
    uint8_t G = (uint8_t)((color >> 16) & 0xFF);
    uint8_t B = (uint8_t)((color >> 8)  & 0xFF);
    uint8_t aCol = (uint8_t)(color & 0xFF);     /* 0=不透明 255=全透明 */
    const uint8_t* src = im->bitmap;
    for (int y = 0; y < ih; ++y) {
        int py = dy + y;
        if (py < 0 || py >= H) continue;
        const uint8_t* sp = src + (size_t)y * im->stride;
        uint8_t* dp = dst + ((size_t)py * (size_t)W) * 4;
        for (int x = 0; x < iw; ++x) {
            int px = dx + x;
            if (px < 0 || px >= W) continue;
            uint8_t cv = sp[x];                 /* 覆盖度 0..255 */
            if (!cv) continue;
            int aSrc = (cv * (255 - aCol)) / 255;  /* 有效直透 alpha */
            if (!aSrc) continue;
            uint8_t* d = dp + px * 4;
            int aDst = d[3];
            int aOut = aSrc + aDst * (255 - aSrc) / 255;
            if (aOut == 0) { d[0] = d[1] = d[2] = d[3] = 0; continue; }
            d[0] = (uint8_t)((R * aSrc + d[0] * aDst * (255 - aSrc) / 255) / aOut);
            d[1] = (uint8_t)((G * aSrc + d[1] * aDst * (255 - aSrc) / 255) / aOut);
            d[2] = (uint8_t)((B * aSrc + d[2] * aDst * (255 - aSrc) / 255) / aOut);
            d[3] = (uint8_t)aOut;
        }
    }
}

/* ---- 回退渲染（libass 不可用时，沿用旧 OSD 真字体）---- */

typedef struct Bands { float top, bottom, left, right; } Bands;

static Bands bandsOf(PP_Host* h, const PP_OsdApi* osd) {
    Bands b{0, 0, 0, 0};
    float w = 0, hh = 0;
    if (!osd->size(h, &w, &hh)) return b;
    float x = 0, y = 0, vw = 0, vh = 0;
    if (osd->video_rect && osd->video_rect(h, &x, &y, &vw, &vh)) {
        b.top = y; b.bottom = hh - (y + vh); b.left = x; b.right = w - (x + vw);
    }
    return b;
}

static std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> v; std::string cur;
    for (char c : s) { if (c == '\n') { v.push_back(cur); cur.clear(); } else cur.push_back(c); }
    v.push_back(cur); return v;
}

static int renderFallback(PP_Host* h, const PP_OsdApi* osd) {
    if (g_line.empty()) return 0;
    float w = 0, hh = 0;
    if (!osd->size(h, &w, &hh)) return 0;
    float scale = 1.6f;
    std::vector<std::string> lines = splitLines(g_line);
    float lh = osd->line_height(h, scale);
    float totalH = lh * (int)lines.size() + 10.0f;
    Bands b = bandsOf(h, osd);
    float boxBottom = (b.bottom >= totalH + 8.0f) ? (hh - 4.0f) : (hh - 10.0f);
    float boxY = boxBottom - totalH;
    float maxW = 0;
    for (auto& l : lines) maxW = std::max(maxW, osd->text_width(h, l.c_str(), scale));
    float boxX = (w - maxW) * 0.5f - 16.0f;
    float boxW = maxW + 32.0f;
    osd->panel(h, boxX, boxY, boxW, totalH, PP_RGBA(0, 0, 0, 150), PP_RGBA(255, 255, 255, 0));
    float ty = boxY + 6.0f;
    for (auto& l : lines) {
        float tw = osd->text_width(h, l.c_str(), scale);
        osd->text(h, (w - tw) * 0.5f, ty, l.c_str(), PP_RGB(255, 255, 255), scale);
        ty += lh;
    }
    return 0;
}

/* ---- 事件回调 ---- */

/* subtitle.ass_header：重置 libass track 并喂入完整 ASS 头。 */
static int onAssHeader(PP_EventCtx* ctx) {
    std::string hdr = extractJsonText(ctx->json, "header");
    if (hdr.empty() || !g_libass_ok) return 0;
    /* 把内核哨兵家族名替换为随包字体真实名（保持内核与字体解耦）。
     * 用户自带 .ass 里的其它字体名不受影响，仍走系统 provider 解析/回退。 */
    static const std::string sentinel = PP_SUBTITLE_FONT_FAMILY;
    size_t pos = 0;
    while ((pos = hdr.find(sentinel, pos)) != std::string::npos) {
        hdr.replace(pos, sentinel.size(), PP_BUNDLED_FONT_REAL);
        pos += std::strlen(PP_BUNDLED_FONT_REAL);
    }
    std::lock_guard<std::mutex> lk(g_track_mu);
    if (g_track) ass_free_track(g_track);
    g_track = ass_new_track(g_lib);
    ass_process_data(g_track, hdr.c_str(), (int)hdr.size());
    if (g_overlay && g_api->overlay) g_api->overlay->set_visible(ctx->host, g_overlay, 0);
    return 0;
}

/* subtitle.ass：把一条 Dialogue 喂进 track（累积成完整字幕轨）。 */
static int onAssLine(PP_EventCtx* ctx) {
    std::string line = extractJsonText(ctx->json, "line");
    if (line.empty() || !g_libass_ok) return 0;
    std::lock_guard<std::mutex> lk(g_track_mu);
    if (!g_track) g_track = ass_new_track(g_lib);   /* 防御：header 迟到也先建 */
    ass_process_data(g_track, line.c_str(), (int)line.size());
    return 0;
}

/* subtitle.line：仅作回退渲染的当前文本。 */
static int onSubLine(PP_EventCtx* ctx) {
    g_line = extractJsonText(ctx->json, "text");
    return 0;
}

/* subtitle.load 命令：把请求发给内核。 */
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

/* osd.draw：用 libass 渲染当前位置的字幕位图，经 overlay 叠加到视频。 */
static int onOsdDraw(PP_EventCtx* ctx) {
    PP_Host* h = ctx->host;
    const PP_OsdApi* osd = g_api->osd;
    if (!osd || !osd->size) return 0;

    float w = 0, hh = 0;
    if (!osd->size(h, &w, &hh)) return 0;
    float vx = 0, vy = 0, vw = 0, vh = 0;
    bool hasVideo = osd->video_rect && osd->video_rect(h, &vx, &vy, &vw, &vh);

    /* libass 不可用、或没有视频画面（没地方叠字幕）→ 回退 OSD 真字体渲染。 */
    if (!g_libass_ok || !g_track || !hasVideo || vw < 1.0f || vh < 1.0f) {
        return renderFallback(h, osd);
    }

    int W = (int)vw, H = (int)vh;
    ass_set_frame_size(g_ren, W, H);
    int64_t now_ms = (int64_t)(g_api->get_position(h) * 1000.0);

    ASS_Image* img = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_track_mu);
        int change = 0;
        img = ass_render_frame(g_ren, g_track, now_ms, &change);
    }
    if (!img) {                       /* 当前位置无可见字幕：隐藏图层，避免残留 */
        if (g_overlay && g_api->overlay) g_api->overlay->set_visible(h, g_overlay, 0);
        return 0;
    }

    std::vector<uint8_t> buf((size_t)W * (size_t)H * 4, 0);
    {
        std::lock_guard<std::mutex> lk(g_track_mu);
        for (ASS_Image* p = img; p; p = p->next)
            compositeImage(buf.data(), W, H, p);
    }
    if (g_overlay == 0)
        g_overlay = g_api->overlay->set(h, 0, buf.data(), W, H, vx, vy, vw, vh, 1.0f);
    else
        g_api->overlay->set(h, g_overlay, buf.data(), W, H, vx, vy, vw, vh, 1.0f);
    g_api->overlay->set_visible(h, g_overlay, 1);
    return 0;
}

/* ---- 生命周期 ---- */
static int onLoad(PP_Host* host, const PP_HostApi* api) {
    if (api->abi_version != PP_ABI_VERSION || api->struct_size < sizeof(PP_HostApi)) {
        return -1;
    }
    g_host = host;
    g_api = api;

    g_libass_ok = initLibass(host);

    int s1 = api->subscribe(host, PP_TOPIC_SUBTITLE_ASS_HEADER, &onAssHeader, nullptr);
    int s2 = api->subscribe(host, PP_TOPIC_SUBTITLE_ASS, &onAssLine, nullptr);
    int s3 = api->subscribe(host, "subtitle.line", &onSubLine, nullptr);
    int s4 = api->subscribe(host, PP_TOPIC_OSD_DRAW, &onOsdDraw, nullptr);
    api->register_command(host, "subtitle.load",
                          "subtitle.load <path.srt>   加载外部字幕（内核解码，libass 渲染）",
                          &cmdSubtitleLoad, nullptr);
    api->log(host, PP_LOG_INFO,
             "subtitles 已加载 (ass_header=%d, ass=%d, line=%d, osd.draw=%d, libass=%s)",
             s1, s2, s3, s4, g_libass_ok ? "yes" : "no(fallback)");
    return 0;
}

static int onStart(PP_Host* host) {
    g_api->log(host, PP_LOG_INFO, "subtitles 启动");
    return 0;
}
static void onStop(PP_Host* host) { g_api->log(host, PP_LOG_INFO, "subtitles 停止"); }
static void onUnload(PP_Host* host) {
    if (g_overlay && g_api->overlay) g_api->overlay->clear(host);
    freeLibass();
    g_overlay = 0;
    g_line.clear();
    g_api->log(host, PP_LOG_INFO, "subtitles 卸载");
}

static PP_Plugin g_plugin = {
    PP_ABI_VERSION,
    "subtitles",
    "1.1.0",
    "PluginPlayer 内置",
    "订阅 subtitle.ass(_header) 事件，用 libass 把字幕（含样式）渲染到视频叠加层；"
    "libass 不可用时回退 OSD 真字体（内核只解码、不渲染）",
    0,
    &onLoad, &onStart, &onStop, &onUnload,
};

PP_PLUGIN_DECL(&g_plugin)
