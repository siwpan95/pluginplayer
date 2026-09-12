/* osd_stats — 播放信息叠加层插件
 * ---------------------------------------------------------------------------
 * 演示的插件能力：
 *   - 订阅 osd.draw 事件，在渲染后绘制叠加层（EMIT 语义）
 *   - 通过 host API 查询媒体信息与实时统计
 *   - 使用 OSD 基本图元（panel / text / rect / fill_rect）
 *   - 通过 get_service() 消费其它插件注册的服务（跨插件协作，对齐 Cordis 的 ctx.<key>）
 *   - 提供 "stats" 命令，输出文本形式的信息
 *
 * 注意：OSD 内置字体只覆盖可打印 ASCII，因此这里全部用英文，避免出现 '?' 方块。
 */
#include "player_plugin.h"

#include <cstdio>
#include <cstring>

static PP_Host*          g_host = nullptr;
static const PP_HostApi* g_api  = nullptr;

/* 跨插件服务：由 hotkey_ext 注册 */
typedef struct HotkeysService {
    const char* (*describe)(void);
    int         (*keyCount)(void);
} HotkeysService;

/* 画一个带底板的文字行，返回下一行的 y */
static float line(PP_Host* h, const PP_OsdApi* osd, float x, float y,
                  const char* text, PP_Color color, float scale) {
    osd->text(h, x, y, text, color, scale);
    return y + osd->line_height(h, scale) + 4.0f;
}

/* 画面外的可用空间（黑边）。插件据此把叠加层放进黑边、不遮画面：
 * 视频矩形由宿主渲染时算出（osd.video_rect），没有画面时四边都是 0，
 * 各绘制函数自然退回原来的画面内布局。 */
typedef struct Bands {
    float top;      /* 画面顶端到窗口顶端的距离 */
    float bottom;   /* 画面底端到窗口底端的距离 */
    float left;
    float right;
} Bands;

static Bands bandsOf(PP_Host* h, const PP_OsdApi* osd) {
    Bands b{0.0f, 0.0f, 0.0f, 0.0f};
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

static void drawTopLeft(PP_Host* h, const PP_OsdApi* osd) {
    PP_MediaInfo mi{};
    g_api->get_media_info(h, &mi);
    PP_PlaybackStats st{};
    g_api->get_stats(h, &st);

    const char* fname = mi.path;
    const char* slash = strrchr(mi.path, '/');
    const char* bslash = strrchr(mi.path, '\\');
    if (slash) fname = slash + 1;
    if (bslash && bslash + 1 > fname) fname = bslash + 1;

    char l1[256], l2[192], l3[192];
    snprintf(l1, sizeof(l1), "%s", fname);
    snprintf(l2, sizeof(l2), "%s  %dx%d  %.2f fps  %s",
             mi.format, mi.width, mi.height, mi.fps, mi.video_codec);
    snprintf(l3, sizeof(l3), "audio: %s %d Hz x%d   bitrate: %lld kbps",
             mi.has_audio ? mi.audio_codec : "(none)",
             mi.audio_sample_rate, mi.audio_channels,
             (long long)(mi.bitrate / 1000));

    float w = 0, hh = 0;
    osd->size(h, &w, &hh);
    float pw = osd->text_width(h, l2, 1.0f) + 24.0f;
    float pw3 = osd->text_width(h, l3, 1.0f) + 24.0f;
    if (pw3 > pw) pw = pw3;

    /* 面板高 76：顶部黑边放得下就贴进黑边，放不下退回画面内左上角。 */
    const float kPanelH = 76.0f;
    Bands b = bandsOf(h, osd);
    float y = (b.top >= kPanelH + 6.0f) ? (b.top - kPanelH - 4.0f) : 14.0f;
    osd->panel(h, 14.0f, y, pw, kPanelH, PP_RGBA(0, 0, 0, 165), PP_RGBA(80, 200, 255, 200));
    float ty = y + 8.0f;
    ty = line(h, osd, 26.0f, ty, l1, PP_RGB(255, 235, 150), 1.0f);
    ty = line(h, osd, 26.0f, ty, l2, PP_RGB(225, 225, 235), 1.0f);
    ty = line(h, osd, 26.0f, ty, l3, PP_RGB(180, 200, 215), 1.0f);
    (void)st;
}

static void drawTopRight(PP_Host* h, const PP_OsdApi* osd) {
    PP_PlaybackStats st{};
    g_api->get_stats(h, &st);

    char l[6][96];
    snprintf(l[0], sizeof(l[0]), "decoded %lld", (long long)st.decoded_frames);
    snprintf(l[1], sizeof(l[1]), "shown %lld", (long long)st.displayed_frames);
    snprintf(l[2], sizeof(l[2]), "dropped %d", st.dropped_frames);
    snprintf(l[3], sizeof(l[3]), "fps %.1f", st.current_fps);
    snprintf(l[4], sizeof(l[4]), "vqueue %lld", (long long)st.video_queue_depth);
    snprintf(l[5], sizeof(l[5]), "abuf %lld KB", (long long)(st.audio_buffer_bytes / 1024));

    float w = 0, hh = 0;
    osd->size(h, &w, &hh);
    /* 两列 × 三行：顶部黑边通常只有几十像素高，纵向铺 6 行放不进去。 */
    float colw[2] = {0.0f, 0.0f};
    for (int i = 0; i < 6; i++) {
        float tw = osd->text_width(h, l[i], 1.0f);
        if (tw > colw[i / 3]) colw[i / 3] = tw;
    }
    const float lh = osd->line_height(h, 1.0f);
    const float pw = colw[0] + colw[1] + 34.0f;
    const float ph = 3 * (lh + 4.0f) + 10.0f;
    const float px = w - pw - 14.0f;
    Bands b = bandsOf(h, osd);
    const float py = (b.top >= ph + 6.0f) ? (b.top - ph - 4.0f) : 14.0f;
    osd->panel(h, px, py, pw, ph, PP_RGBA(0, 0, 0, 165), PP_RGBA(255, 160, 90, 200));
    for (int i = 0; i < 6; i++) {
        PP_Color c = (i == 3) ? PP_RGB(140, 255, 170) : PP_RGB(230, 230, 235);
        float cx = px + 11.0f + (i / 3) * (colw[0] + 12.0f);
        float cy = py + 6.0f + (i % 3) * (lh + 4.0f);
        osd->text(h, cx, cy, l[i], c, 1.0f);
    }
}

/* 顶部中间：跨插件服务信息（若 hotkey_ext 已注册 hotkeys 服务） */
static void drawServiceInfo(PP_Host* h, const PP_OsdApi* osd) {
    HotkeysService* hk = (HotkeysService*)g_api->get_service(h, "hotkeys");
    const char* txt = hk && hk->describe ? hk->describe() : nullptr;
    if (!txt) return;

    float w = 0, hh = 0;
    osd->size(h, &w, &hh);
    char buf[192];
    snprintf(buf, sizeof(buf), "hotkeys svc: %s", txt);
    float tw = osd->text_width(h, buf, 1.0f);
    float px = (w - tw) * 0.5f - 10.0f;
    const float ph = osd->line_height(h, 1.0f) + 12.0f;
    Bands b = bandsOf(h, osd);
    float py = (b.top >= ph + 4.0f) ? (b.top - ph - 3.0f) : 14.0f;
    osd->panel(h, px, py, tw + 20.0f, ph,
               PP_RGBA(0, 0, 0, 160), PP_RGBA(180, 140, 255, 190));
    osd->text(h, px + 10.0f, py + 6.0f, buf, PP_RGB(220, 200, 255), 1.0f);
}

static void drawBottomBar(PP_Host* h, const PP_OsdApi* osd) {
    PP_PlaybackStats st{};
    g_api->get_stats(h, &st);

    float w = 0, hh = 0;
    osd->size(h, &w, &hh);

    const float margin = 16.0f;
    const float barH = 8.0f;
    /* 底部黑边放得下（信息行 + 进度条 ≈ 44px）就把整条挪进黑边；audio_viz 的
     * 频谱面板按同一约定让开这 48px（见那边的注释）。 */
    Bands b = bandsOf(h, osd);
    float barY = (b.bottom >= 44.0f) ? (hh - 4.0f - barH) : (hh - 34.0f);
    float barW = w - margin * 2;

    /* 打印信息行 */
    char info[256];
    int total = (int)st.duration;
    int cur = (int)st.position;
    snprintf(info, sizeof(info), "%02d:%02d:%02d / %02d:%02d:%02d   vol %.0f%%   speed %.2fx%s",
             cur / 3600, (cur / 60) % 60, cur % 60,
             total / 3600, (total / 60) % 60, total % 60,
             g_api->get_volume(h) * 100.0f, st.speed,
             g_api->is_muted(h) ? "  [MUTED]" : "");
    float tw = osd->text_width(h, info, 1.0f);
    osd->panel(h, margin, barY - osd->line_height(h, 1.0f) - 12.0f,
               tw + 20.0f, osd->line_height(h, 1.0f) + 10.0f,
               PP_RGBA(0, 0, 0, 165), PP_RGBA(80, 200, 255, 170));
    osd->text(h, margin + 10.0f, barY - osd->line_height(h, 1.0f) - 7.0f,
              info, PP_RGB(235, 235, 240), 1.0f);

    /* 进度条 */
    osd->fill_rect(h, margin, barY, barW, barH, PP_RGBA(40, 40, 50, 190));
    float ratio = st.duration > 0 ? (float)(st.position / st.duration) : 0.0f;
    if (ratio < 0) ratio = 0;
    if (ratio > 1) ratio = 1;
    osd->fill_rect(h, margin, barY, barW * ratio, barH, PP_RGB(90, 200, 255));
    /* 缓冲进度（音频缓冲代表的领先量） */
    if (st.duration > 0) {
        float lead = (float)(st.audio_buffer_bytes / (double)(st.audio_sample_rate * st.audio_channels * 2 + 1));
        float bratio = ratio + (float)(lead / st.duration);
        if (bratio > 1) bratio = 1;
        osd->rect(h, margin, barY, barW * bratio, barH, PP_RGBA(255, 200, 90, 200), 1.0f);
    }
    osd->rect(h, margin, barY, barW, barH, PP_RGBA(150, 150, 170, 220), 1.0f);
}

static int onOsdDraw(PP_EventCtx* ctx) {
    PP_Host* h = ctx->host;
    const PP_OsdApi* osd = g_api->osd;
    if (!osd || !osd->size) return 0;

    PP_MediaInfo mi{};
    if (g_api->get_media_info(h, &mi) != 0) return 0;
    if (!mi.has_video && !mi.has_audio) return 0;

    drawTopLeft(h, osd);
    drawTopRight(h, osd);
    drawServiceInfo(h, osd);
    drawBottomBar(h, osd);
    return 0;
}

/* ---- stats 命令：把统计写成文本 ---- */
static int cmdStats(PP_Host* h, int argc, const char** argv, void* user) {
    (void)user;
    PP_MediaInfo mi{};
    PP_PlaybackStats st{};
    g_api->get_media_info(h, &mi);
    g_api->get_stats(h, &st);

    char buf[2048];
    snprintf(buf, sizeof(buf),
             "file      : %s\n"
             "container : %s\n"
             "video     : %s %dx%d %.3f fps\n"
             "audio     : %s %d Hz %d ch\n"
             "duration  : %.3f s   bitrate %.0f kbps\n"
             "position  : %.3f s (master %.3f / video %.3f)\n"
             "frames    : decoded %lld  displayed %lld  dropped %d\n"
             "queue     : video %lld frames  audio %lld bytes\n"
             "speed     : %.2fx   paused=%d  muted=%d  volume=%.2f\n",
             mi.path, mi.format, mi.video_codec, mi.width, mi.height, mi.fps,
             mi.has_audio ? mi.audio_codec : "(none)", mi.audio_sample_rate, mi.audio_channels,
             mi.duration, mi.bitrate / 1000.0,
             st.position, st.master_clock, st.video_clock,
             (long long)st.decoded_frames, (long long)st.displayed_frames, st.dropped_frames,
             (long long)st.video_queue_depth, (long long)st.audio_buffer_bytes,
             st.speed, st.paused, g_api->is_muted(h), g_api->get_volume(h));

    g_api->log(h, PP_LOG_INFO, "%s", buf);

    /* 如果命令带参数，则同时把文本导出到文件（演示 write_text_file） */
    if (argc >= 1 && argv[0] && argv[0][0]) {
        int r = g_api->write_text_file(h, argv[0], buf);
        g_api->log(h, PP_LOG_INFO, "stats 已导出到 %s (r=%d)", argv[0], r);
        return r;
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

    int sub = api->subscribe(host, PP_TOPIC_OSD_DRAW, &onOsdDraw, nullptr);
    api->register_command(host, "stats", "stats [outfile]   打印/导出播放统计", &cmdStats, nullptr);
    api->log(host, PP_LOG_INFO, "osd_stats 已加载 (osd.draw sub_id=%d)", sub);
    return 0;
}

static int onStart(PP_Host* host) {
    g_api->log(host, PP_LOG_INFO, "osd_stats 启动");
    return 0;
}

static void onStop(PP_Host* host) { g_api->log(host, PP_LOG_INFO, "osd_stats 停止"); }
static void onUnload(PP_Host* host) { g_api->log(host, PP_LOG_INFO, "osd_stats 卸载"); }

static PP_Plugin g_plugin = {
    PP_ABI_VERSION,
    "osd_stats",
    "1.0.0",
    "PluginPlayer 内置",
    "叠加显示媒体信息、实时统计、进度条与跨插件服务状态",
    0,
    &onLoad, &onStart, &onStop, &onUnload,
};

PP_PLUGIN_DECL(&g_plugin)
