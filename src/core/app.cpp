/* app.cpp — 应用层实现 */
#include "app.h"
#include "osd.h"
#include "font.h"
#include "util.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>

namespace pp {

/* 自检抓屏落盘的像素上限（4096x4096 ≈ 16.7M 像素 → BMP 约 50 MB）。
 * 超过则只验证"抓屏成功"，不写文件，避免写出 GB 级 BMP。 */
static const int64_t kMaxCapturePixels = 4096LL * 4096LL;

/* 清屏色：renderFrame 用它铺底，自检抓屏取证也拿它当**参照色**。
 * 写成常量而不是两处各写一遍 —— 两边一旦漂开，取证判据就静默失效了。 */
static const uint8_t kClearR = 16, kClearG = 16, kClearB = 20;

/* 自检取证：画面矩形内"恰好等于清屏色"的像素占比上限，超过即认为画面没被绘制。
 * 依据：纹理没画上去时该占比接近 1（本机实测 86.6%，其余是 OSD 叠加层）；
 * 真实画面哪怕是一整帧 YUV 有限范围的黑场，转成 RGBA 也是 (0,0,0) 附近，
 * 占比在 0 量级。阈值取在两者中间，两侧都留足余量。 */
static const double kMaxClearRatioInVideo = 0.80;

/* ======================= 初始化 ======================= */

/* P1-c：subtitles 插件的 subtitle.load 命令把请求以事件形式发过来（避免给 ABI 加方法），
 * 这里订阅并真正加载外部字幕。命令在主线程（RPC/自检）同步派发，所以回调也同步执行。 */
static int onSubtitleLoadReq(PP_EventCtx* ctx) {
    App* a = (App*)ctx->user;
    if (!a || !ctx->json) return 0;
    std::string path = jsonGetString(ctx->json, "path", "");
    if (path.empty()) { PP_WARN("subtitle.load: 缺少 path 参数"); return 0; }
    if (!fileExists(path)) { PP_WARN("subtitle.load: 文件不存在: %s", path.c_str()); return 0; }
    auto cues = Player::parseSrt(path);
    if (cues.empty()) { PP_WARN("subtitle.load: 未解析出任何字幕行: %s", path.c_str()); return 0; }
    a->player().loadExternalSubtitles(cues);
    PP_INFO("subtitle.load: 已加载 %d 条外部字幕: %s", (int)cues.size(), path.c_str());
    return 0;
}

/* P1-d：切轨请求（命令来自 track_selector 插件，内核执行 reopen，单一选择源） */
static int onTrackSelectReq(PP_EventCtx* ctx) {
    App* a = (App*)ctx->user;
    if (!a || !ctx->json) return 0;
    int type  = (int)jsonGetNumber(ctx->json, "type", -1);
    int index = (int)jsonGetNumber(ctx->json, "index", -1);
    if (type < 0 || index < 0) {
        PP_WARN("track.select: 缺少 type/index 参数");
        return 0;
    }
    if (!a->selectTrack(type, index)) {
        PP_WARN("track.select: 切换失败（type=%d index=%d 非法或媒体未打开）", type, index);
    }
    return 0;
}

/* P1-d：切音频设备请求（命令来自 audio_device 插件，内核 reopen 音频流，单一选择源） */
static int onAudioDeviceSetReq(PP_EventCtx* ctx) {
    App* a = (App*)ctx->user;
    if (!a || !ctx->json) return 0;
    double did = jsonGetNumber(ctx->json, "id", -1.0);
    if (did < 0) {
        PP_WARN("audio.device.set: 缺少 id 参数");
        return 0;
    }
    if (!a->setAudioDevice((SDL_AudioDeviceID)(int)did)) {
        PP_WARN("audio.device.set: 切换失败（id=%d）", (int)did);
    }
    return 0;
}

/* P2-L1：video.color 服务 —— 内核把当前视频帧色彩元数据暴露给插件（ABI 安全的
 * get_service 机制，不进 PP_ABI_VERSION）。SDL 不在播放器初始化时知道内容是否 HDR，
 * 所以这些值是"最近一次上传的帧"的快照，插件按需查询。 */
static pp::App* s_colorApp = nullptr;
static uint32_t svcVideoCurCs() { return s_colorApp ? s_colorApp->curVideoColorspace() : 0u; }
static int      svcVideoPri()   { return s_colorApp ? s_colorApp->curVideoPrimaries() : 2; }
static int      svcVideoTrc()   { return s_colorApp ? s_colorApp->curVideoTransfer()   : 2; }
static int      svcVideoMat()   { return s_colorApp ? s_colorApp->curVideoMatrix()     : 0; }
static int      svcVideoRange() { return s_colorApp ? s_colorApp->curVideoFullRange()  : 0; }
static int      svcVideoHdr()   { return s_colorApp ? (s_colorApp->curVideoIsHdr() ? 1 : 0) : 0; }
static PP_VideoColorService g_videoColorService = {
    &svcVideoCurCs, &svcVideoPri, &svcVideoTrc, &svcVideoMat, &svcVideoRange, &svcVideoHdr
};

bool App::init(int winW, int winH, bool hidden, bool audioEnabled) {
    /* 防御性钳位：即使调用方漏了校验，也不让越界尺寸流到 SDL。
     * 越界尺寸会在 SDL_RenderReadPixels / 纹理分配处触发 int32 溢出崩溃。 */
    if (winW < kMinWindowSize || winW > kMaxWindowSize) {
        PP_WARN("窗口宽度 %d 超出 %d..%d，回退为 1280", winW, kMinWindowSize, kMaxWindowSize);
        winW = 1280;
    }
    if (winH < kMinWindowSize || winH > kMaxWindowSize) {
        PP_WARN("窗口高度 %d 超出 %d..%d，回退为 720", winH, kMinWindowSize, kMaxWindowSize);
        winH = 720;
    }
    winW_ = winW;
    winH_ = winH;
    hidden_ = hidden;
    audioEnabled_ = audioEnabled;

    /* PP_NO_NV12=1 强制 RGBA 上传路径 —— 只用于同一份二进制上做 NV12/RGBA 的 A/B 基准。 */
    {
        const char* v = getenv("PP_NO_NV12");
        forceRgba_ = (v && *v && *v != '0');
        if (forceRgba_) PP_INFO("PP_NO_NV12=1：强制 RGBA 上传路径（基准对比用，非正常播放路径）");
    }

    SDL_InitFlags initFlags = SDL_INIT_VIDEO;
    if (audioEnabled) initFlags |= SDL_INIT_AUDIO;
    if (!SDL_Init(initFlags)) {
        lastError_ = format("SDL_Init 失败: %s", SDL_GetError());
        PP_ERROR("%s", lastError_.c_str());
        return false;
    }

    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;
    if (hidden) flags |= SDL_WINDOW_HIDDEN;
    win_ = SDL_CreateWindow("PluginPlayer — 插件版播放器", winW, winH, flags);
    if (!win_) {
        lastError_ = format("SDL_CreateWindow 失败: %s", SDL_GetError());
        PP_ERROR("%s", lastError_.c_str());
        return false;
    }
    ren_ = SDL_CreateRenderer(win_, nullptr);
    if (!ren_) {
        lastError_ = format("SDL_CreateRenderer 失败: %s", SDL_GetError());
        PP_ERROR("%s", lastError_.c_str());
        return false;
    }
    SDL_SetRenderVSync(ren_, 1);
    SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);

    /* 宿主与插件 */
    std::string appDir = exeDir();
    if (!host_.init(appDir)) return false;
    host_.setApp(this);
    host_.setPlayer(&player_);
    /* P2-L1：注册 video.color 服务（内核暴露当前视频色彩元数据；ABI 安全，不升版本号）。
     * 放在 host_.init 之后：api_ 此时已 buildApi 完成；插件经 get_service 在运行时取用。 */
    s_colorApp = this;
    host_.api()->register_service(host_.handle(), "video.color", "1.0.0", &g_videoColorService);
    /* P1-e：把字幕 ASS 事件（header / 每帧 dialogue）经事件总线转发出去，
     * 供 subtitles 插件用 libass 渲染（不进 ABI，单一字幕源在内核）。 */
    player_.setSubtitlePublisher([this](const char* t, const char* j) {
        host_.bus().publish(host_.handle(), t, j);
    });
    /* P1-c：订阅"加载外部字幕"请求（命令来自 subtitles 插件，内核执行加载，
     * 单一字幕源、渲染交给插件，避免给 ABI 加方法）。owner=0 表示宿主内置。 */
    host_.bus().subscribe("subtitle.load.req", &onSubtitleLoadReq, this, 0);
    /* P1-d：订阅"切换轨 / 切换音频设备"请求（命令来自 track_selector / audio_device 插件，
     * 内核执行 reopen，单一选择源、UI 由插件做，避免给 ABI 加方法）。owner=0 表示宿主内置。 */
    host_.bus().subscribe(PP_TOPIC_TRACK_SELECT_REQ, &onTrackSelectReq, this, 0);
    host_.bus().subscribe(PP_TOPIC_AUDIO_DEVICE_SET_REQ, &onAudioDeviceSetReq, this, 0);
    /* 所有宿主事件同时转发给控制通道（进程外插件） */
    host_.setEventSink([this](const std::string& t, const std::string& j) {
        rpc_.broadcastEvent(t, j);
    });

    /* P1-a 真字体：有字体就走真字形，没有就回退 SDL 调试点阵字（界面仍然可用，
     * 只是非 ASCII 会显示成 '?'）。加载失败不算致命 —— 播放器照样能播。 */
    {
        std::string fp;
        const char* envFont = getenv("PP_FONT");
        if (envFont && *envFont) fp = envFont;
        else fp = pathJoin(appDir, "assets/fonts/ZCOOLKuaiLe-Regular.ttf");
        if (!Font::instance().load(fp)) {
            PP_WARN("真字体未启用（%s），OSD 退回调试点阵字：中文会显示成 '?'。"
                    "可用 PP_FONT=<字体.ttf> 指定（必须是 TrueType 'glyf' 轮廓，OTF/CFF 不支持）",
                    fp.c_str());
        }
    }

    /* P1-d：init 末尾广播一次可用音频输出设备（供 audio_device 插件在打开媒体前
     * 就拿到设备列表）。与 openMedia 里的刷新互为补充。 */
    if (audioEnabled_) {
        publishEvent(PP_TOPIC_AUDIO_DEVICES, listAudioDevicesJson().c_str());
    }

    return true;
}

void App::shutdown() {
    rpc_.stop();
    closeAudio();
    plugins_.stopAll(&host_);
    plugins_.unloadAll(&host_);
    player_.close();
    host_.shutdown();
    if (tex_) { SDL_DestroyTexture(tex_); tex_ = nullptr; }
    Font::instance().shutdown();          /* 必须在销毁渲染器之前释放字形图集纹理 */
    if (ren_) { SDL_DestroyRenderer(ren_); ren_ = nullptr; }
    if (win_) { SDL_DestroyWindow(win_); win_ = nullptr; }
    SDL_Quit();
}

/* ======================= 音频 ======================= */

void SDLCALL App::audioCallback(void* userdata, SDL_AudioStream* stream,
                                int additional, int total) {
    App* a = (App*)userdata;
    (void)total;
    if (additional <= 0 || !a) return;
    if (!a->player_.hasAudio()) return;

    if (a->player_.paused()) return;   /* 暂停时不喂数据，让它自然静音 */

    int want = additional;
    if (want > 1 << 20) want = 1 << 20;
    if ((int)a->pullBuf_.size() < want) a->pullBuf_.resize(want);

    size_t got = a->player_.audioRing().read(a->pullBuf_.data(), (size_t)want);
    if (got == 0) return;
    SDL_PutAudioStreamData(stream, a->pullBuf_.data(), (int)got);
    a->player_.addConsumedAudioBytes((int64_t)got);

    /* 告知内核：还有多少字节在 SDL 侧没播，用于校准音频时钟 */
    a->player_.setAudioLeadBytes((int64_t)SDL_GetAudioStreamQueued(stream));
}

bool App::openAudio() {
    closeAudio();
    if (!audioEnabled_ || !player_.hasAudio()) {
        player_.setAudioActive(false);
        return false;
    }
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16;
    spec.channels = player_.audioChannels();
    spec.freq = player_.audioSampleRate();

    audioStream_ = SDL_OpenAudioDeviceStream(audioDeviceId_,
                                             &spec, &App::audioCallback, this);
    if (!audioStream_) {
        PP_WARN("打开音频设备失败（将静音播放）: %s", SDL_GetError());
        player_.setAudioActive(false);
        return false;
    }
    player_.setAudioFlushCallback([this]() {
        if (audioStream_) SDL_ClearAudioStream(audioStream_);
    });
    /* seek 后预热：Player 已把 prerollPending_ 置位、主时钟冻在目标点。
     * 这里把音频设备暂停 —— 否则音频立刻按实时速率往前走，预热结束时音频时钟
     * 已经跑掉一秒多，视频第一帧（pts≈目标）反而"迟到"。等第一帧上屏，
     * stepAudioPreroll() 会 resume（与 open 预热共用同一条路）。 */
    player_.setSeekCallback([this]() {
        if (!player_.prerollPending()) return;
        audioAwaitPreroll_ = true;
        if (audioStream_) SDL_PauseAudioStreamDevice(audioStream_);
    });
    player_.setAudioActive(true);
    applyAudioParams();
    /* 预热：解码器还没吐出第一帧时**先不启动音频设备**。
     * 否则音频立刻按实时速率往前跑，主时钟（音频驱动）随之走掉 ~0.2 s，
     * 视频头几帧一解出来就已经迟到、被整批丢掉 —— 声音从 0 开始、画面却从 0.2 s 开始。
     * 推迟到第一帧上屏再启动，A/V 一起从 0 起跑（见 Player::prerollPending）。 */
    if (player_.prerollPending()) {
        audioAwaitPreroll_ = true;
        PP_INFO("音频设备已打开，等待第一帧上屏后再启动（预热）");
        return true;
    }
    SDL_ResumeAudioStreamDevice(audioStream_);
    PP_INFO("音频输出已就绪: %d Hz / %d ch / S16", spec.freq, spec.channels);
    return true;
}

void App::closeAudio() {
    audioAwaitPreroll_ = false;
    if (audioStream_) {
        SDL_DestroyAudioStream(audioStream_);
        audioStream_ = nullptr;
    }
    player_.setAudioActive(false);
    player_.setAudioFlushCallback(nullptr);
    player_.setSeekCallback(nullptr);
}

void App::stepAudioPreroll() {
    if (!audioAwaitPreroll_ || player_.prerollPending()) return;
    audioAwaitPreroll_ = false;
    if (audioStream_) {
        SDL_ResumeAudioStreamDevice(audioStream_);
        PP_INFO("预热结束（第一帧已上屏），音频设备已启动");
    }
}

void App::stepAudioLead() {
    if (!audioStream_) return;
    player_.setAudioLeadBytes((int64_t)SDL_GetAudioStreamQueued(audioStream_));
}

void App::applyAudioParams() {
    if (!audioStream_) return;
    /* 静音时增益恒为 0：这样调音量不会「悄悄解除静音」；
     * 用 Player 钳位后的值，避免 1e300 之类原始值产生 inf 增益。 */
    SDL_SetAudioStreamGain(audioStream_, player_.muted() ? 0.0f : player_.volume());
    SDL_SetAudioStreamFrequencyRatio(audioStream_, (float)player_.speed());
}

/* ===========================================================================
 * P1-d：多轨与音频设备选择（能力通道，内核 reopen 执行，不进 ABI）
 * =========================================================================== */

bool App::selectTrack(int avMediaType, int streamIndex) {
    if (!player_.isOpen()) { PP_WARN("selectTrack: 媒体未打开"); return false; }
    /* 写偏好（内核内部校验 index 属于该类型）；非法直接返回 false，绝不 reopen。 */
    if (!player_.setPreferredTrack(avMediaType, streamIndex)) {
        PP_WARN("selectTrack: 非法轨（type=%d index=%d），不切换", avMediaType, streamIndex);
        return false;
    }
    /* 保持当前播放位置，reopen 后 reseek 回去。 */
    double pos = player_.position();
    const std::string path = mediaPath_;
    closeAudio();
    if (!player_.open(path)) {
        PP_ERROR("selectTrack: reopen 失败: %s", path.c_str());
        return false;
    }
    openAudio();
    /* 广播新轨列表与新选中轨（供 track_selector 插件更新 UI）。 */
    publishEvent(PP_TOPIC_MEDIA_TRACKS, player_.listTracksJson().c_str());
    publishEvent(PP_TOPIC_MEDIA_TRACK_SEL,
                 format("{\"type\":%d,\"index\":%d}", avMediaType, streamIndex).c_str());
    /* 回到切换前的播放位置（reopen 会从头开始）。 */
    if (pos > 0.0) player_.seekAbsolute(pos);
    PP_INFO("已切换轨（type=%d index=%d），位置恢复到 %.2fs", avMediaType, streamIndex, pos);
    return true;
}

std::string App::listAudioDevicesJson() const {
    std::string out = "[";
    bool first = true;
    auto appendDevice = [&](SDL_AudioDeviceID id, bool isDefault) {
        const char* name = SDL_GetAudioDeviceName(id);
        char buf[640];
        std::snprintf(buf, sizeof(buf),
                      "{\"id\":%u,\"name\":\"%s\",\"default\":%s}",
                      (unsigned)id,
                      jsonEscape(name ? name : "").c_str(),
                      isDefault ? "true" : "false");
        if (!first) out += ",";
        first = false;
        out += buf;
    };
    /* 默认播放设备（sentinel，不在 SDL_GetAudioPlaybackDevices 列表里）。 */
    appendDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, true);
    int n = 0;
    SDL_AudioDeviceID* devs = SDL_GetAudioPlaybackDevices(&n);
    for (int i = 0; i < n; ++i) appendDevice(devs[i], false);
    if (devs) SDL_free(devs);
    out += "]";
    return out;
}

bool App::setAudioDevice(SDL_AudioDeviceID id) {
    audioDeviceId_ = id;
    /* reopen 音频流即可生效（音频环与解码线程不受影响，播放不中断）。 */
    closeAudio();
    openAudio();
    publishEvent(PP_TOPIC_AUDIO_DEVICE_CHG, format("{\"id\":%u}", (unsigned)id).c_str());
    PP_INFO("已切换音频输出设备: id=%u", (unsigned)id);
    return true;
}

/* ======================= 媒体 ======================= */

bool App::openMedia(const std::string& path) {
    /* 默认拒绝网络输入：控制通道与插件都能触发 openMedia，
     * 若默认放行就等于给本机装了一个「代任意来源出网」的原语（SSRF）。
     * 需要播放 http/rtsp/UNC 时用 --allow-network-input 显式开启。 */
    if (!allowNetworkInput_ && isNetworkInput(path)) {
        lastError_ = format("已拒绝网络输入（默认只允许本地文件）: %s", path.c_str());
        PP_WARN("%s；确需播放网络流或 UNC 路径请加 --allow-network-input", lastError_.c_str());
        publishEvent(PP_TOPIC_PLAY_ERROR,
                     format("{\"path\":\"%s\",\"message\":\"%s\"}",
                            jsonEscape(path).c_str(),
                            jsonEscape(lastError_).c_str()).c_str());
        return false;
    }
    completedFired_ = false;
    lastUploadedIndex_ = -1;
    lastSubLine_.clear();      /* 强制重播新媒体的首条字幕状态 */
    closeAudio();
    if (!player_.open(path)) {
        lastError_ = format("无法打开媒体: %s", path.c_str());
        PP_ERROR("%s", lastError_.c_str());
        publishEvent(PP_TOPIC_PLAY_ERROR,
                     format("{\"path\":\"%s\",\"message\":\"%s\"}",
                            jsonEscape(path).c_str(),
                            jsonEscape(lastError_).c_str()).c_str());
        return false;
    }
    mediaPath_ = path;
    openAudio();

    PP_MediaInfo mi{};
    player_.fillMediaInfo(&mi);
    std::string j = format(
        "{\"path\":\"%s\",\"format\":\"%s\",\"video\":\"%s\",\"audio\":\"%s\","
        "\"width\":%d,\"height\":%d,\"fps\":%.4f,\"duration\":%.4f,\"bitrate\":%lld,"
        "\"sample_rate\":%d,\"channels\":%d}",
        jsonEscape(path).c_str(), jsonEscape(mi.format).c_str(),
        jsonEscape(mi.video_codec).c_str(), jsonEscape(mi.audio_codec).c_str(),
        mi.width, mi.height, mi.fps, mi.duration, (long long)mi.bitrate,
        mi.audio_sample_rate, mi.audio_channels);
    publishEvent(PP_TOPIC_MEDIA_OPENED, j.c_str());
    PP_INFO("媒体信息: %s %dx%d %.2f fps %s / %s", mi.format, mi.width, mi.height,
            mi.fps, mi.video_codec, mi.audio_codec);
    /* P1-d：打开新媒体即广播其全部轨（供 track_selector 插件初始化 UI）。 */
    publishEvent(PP_TOPIC_MEDIA_TRACKS, player_.listTracksJson().c_str());
    /* P1-d：广播可用音频输出设备（供 audio_device 插件初始化设备列表）。
     * 设备列表与媒体无关，但随每次 open 刷新一次最稳妥（插拔设备后重开即更新）。 */
    if (audioEnabled_) {
        publishEvent(PP_TOPIC_AUDIO_DEVICES, listAudioDevicesJson().c_str());
    }
    return true;
}

void App::publishEvent(const char* topic, const char* json) {
    host_.publishTopic(topic, json);
}

void App::invokeOrLog(const char* cmd, int argc, const char** argv) {
    int r = host_.invokeCommand(cmd, argc, argv);
    if (r < 0) PP_WARN("命令不存在: %s", cmd);
    else PP_INFO("命令 %s 返回 %d", cmd, r);
}

/* ======================= 事件 ======================= */

void App::pollEvents() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_EVENT_QUIT:
                quit_ = true;
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                handleKey(e);
                break;
            /* P1-b 鼠标事件：与键盘同款，先给插件（BAIL，插件可吞掉） */
            case SDL_EVENT_MOUSE_MOTION:
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
            case SDL_EVENT_MOUSE_WHEEL:
                handleMouse(e);
                break;
            case SDL_EVENT_WINDOW_RESIZED:
                winW_ = e.window.data1;
                winH_ = e.window.data2;
                host_.bus().publish(host_.handle(), PP_TOPIC_UI_RESIZE,
                                    format("{\"width\":%d,\"height\":%d}",
                                           winW_, winH_).c_str());
                break;
            default:
                break;
        }
    }
}

/* P1-b：鼠标事件进插件链（BAIL）。被吞掉时不执行内置的"点进度条跳转"。 */
void App::handleMouse(const SDL_Event& e) {
    PP_MouseEvent me{};
    switch (e.type) {
        case SDL_EVENT_MOUSE_MOTION:
            me.type = PP_MOUSE_MOTION;
            me.x = e.motion.x; me.y = e.motion.y;
            me.dx = e.motion.xrel; me.dy = e.motion.yrel;
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            me.type = (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) ? PP_MOUSE_DOWN : PP_MOUSE_UP;
            me.x = e.button.x; me.y = e.button.y;
            /* SDL3 的 button 字段：1=左 2=中 3=右，与 PP_MouseEvent 口径一致 */
            me.button = (int)e.button.button;
            me.clicks = (int)e.button.clicks;
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL:
            me.type = PP_MOUSE_WHEEL;
            me.x = e.wheel.mouse_x; me.y = e.wheel.mouse_y;
            me.dx = e.wheel.x; me.dy = e.wheel.y;
            break;
        default:
            return;
    }
    me.mod = 0;
    if (e.type != SDL_EVENT_MOUSE_WHEEL) {
        SDL_Keymod m = SDL_GetModState();
        if (m & SDL_KMOD_SHIFT) me.mod |= PP_MOD_SHIFT;
        if (m & SDL_KMOD_CTRL)  me.mod |= PP_MOD_CTRL;
        if (m & SDL_KMOD_ALT)   me.mod |= PP_MOD_ALT;
    }
    me.pts = player_.isOpen() ? player_.masterClock() : 0.0;

    int consumed = host_.bus().dispatch(host_.handle(), PP_TOPIC_UI_MOUSE, &me, (int)sizeof(me));
    if (consumed) { PP_DEBUG("鼠标事件被插件消费: type=%d", me.type); return; }

    /* 内置响应：左键点击画面区域 → 按横向位置跳转（与键盘一样是"能用"，不是"好用"） */
    if (me.type == PP_MOUSE_DOWN && me.button == 1 && player_.isOpen() && player_.duration() > 0) {
        if (videoRectValid_ && videoRectW_ > 0) {
            float rel = (me.x - videoRectX_) / videoRectW_;
            if (rel >= 0.0f && rel <= 1.0f) {
                player_.seekAbsolute((double)rel * player_.duration());
                publishEvent(PP_TOPIC_PLAY_SEEKED, "{}");
            }
        }
    }
}

/* ---------------- P1-b 通用叠加层 ---------------- */

static bool overlayBelongs(const App::Overlay& o, int owner, int id) {
    return o.id == id && (owner == 0 || o.owner == owner);
}

int App::overlaySet(int owner, int id, const uint8_t* rgba, int w, int h,
                    float x, float y, float dw, float dh, float alpha) {
    if (!ren_ || !rgba || w <= 0 || h <= 0) return 0;
    Overlay* o = nullptr;
    for (auto& it : overlays_) {
        if (overlayBelongs(it, owner, id)) { o = &it; break; }
    }
    if (!o) {
        if (id != 0) return 0;            /* 指定的 id 不存在 */
        overlays_.push_back(Overlay{});
        o = &overlays_.back();
        o->id = nextOverlayId_++;
        o->owner = owner;
    }
    if (!o->tex || o->w != w || o->h != h) {
        if (o->tex) SDL_DestroyTexture(o->tex);
        o->tex = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_RGBA32,
                                   SDL_TEXTUREACCESS_STATIC, w, h);
        if (!o->tex) { PP_WARN("叠加层纹理创建失败: %s", SDL_GetError()); return 0; }
        SDL_SetTextureBlendMode(o->tex, SDL_BLENDMODE_BLEND);
        o->w = w; o->h = h;
    }
    if (!SDL_UpdateTexture(o->tex, nullptr, rgba, w * 4)) {
        PP_WARN("叠加层像素上传失败: %s", SDL_GetError());
        return 0;
    }
    o->x = x; o->y = y;
    o->dw = dw > 0 ? dw : (float)w;
    o->dh = dh > 0 ? dh : (float)h;
    o->alpha = (alpha < 0.0f) ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
    o->visible = true;
    return o->id;
}

int App::overlayMove(int owner, int id, float x, float y, float dw, float dh, float alpha) {
    for (auto& o : overlays_) {
        if (!overlayBelongs(o, owner, id)) continue;
        o.x = x; o.y = y;
        if (dw > 0) o.dw = dw;
        if (dh > 0) o.dh = dh;
        o.alpha = (alpha < 0.0f) ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
        return 1;
    }
    return 0;
}

int App::overlayVisible(int owner, int id, bool visible) {
    for (auto& o : overlays_) {
        if (!overlayBelongs(o, owner, id)) continue;
        o.visible = visible;
        return 1;
    }
    return 0;
}

int App::overlayRemove(int owner, int id) {
    for (size_t i = 0; i < overlays_.size(); i++) {
        if (!overlayBelongs(overlays_[i], owner, id)) continue;
        if (overlays_[i].tex) SDL_DestroyTexture(overlays_[i].tex);
        overlays_.erase(overlays_.begin() + i);
        return 1;
    }
    return 0;
}

int App::overlayClear(int owner) {
    int n = 0;
    for (size_t i = 0; i < overlays_.size();) {
        if (owner == 0 || overlays_[i].owner == owner) {
            if (overlays_[i].tex) SDL_DestroyTexture(overlays_[i].tex);
            overlays_.erase(overlays_.begin() + i);
            n++;
        } else i++;
    }
    return n;
}

void App::clearAllOverlays() {
    for (auto& o : overlays_) if (o.tex) SDL_DestroyTexture(o.tex);
    overlays_.clear();
}

void App::renderOverlays() {
    if (!ren_ || overlays_.empty()) return;
    for (auto& o : overlays_) {
        if (!o.visible || !o.tex || o.alpha <= 0.0f) continue;
        SDL_SetTextureAlphaMod(o.tex, (Uint8)(o.alpha * 255.0f));
        SDL_FRect dst{o.x, o.y, o.dw, o.dh};
        SDL_RenderTexture(ren_, o.tex, nullptr, &dst);
    }
    for (auto& o : overlays_) if (o.tex) SDL_SetTextureAlphaMod(o.tex, 255);
}

void App::handleKey(const SDL_Event& e) {
    PP_KeyEvent ke{};
    ke.keycode = (int)e.key.key;
    ke.scancode = (int)e.key.scancode;
    ke.keymod = (int)e.key.mod;
    ke.down = e.key.down ? 1 : 0;
    ke.repeat = e.key.repeat ? 1 : 0;

    /* 先给插件（BAIL 语义：插件可以吞掉按键） */
    int consumed = host_.bus().dispatch(host_.handle(), PP_TOPIC_UI_KEY, &ke, (int)sizeof(ke));
    if (consumed) {
        PP_DEBUG("按键被插件消费: key=%d", ke.keycode);
        return;
    }
    if (!ke.down) return;

    const bool shift = (ke.keymod & SDL_KMOD_SHIFT) != 0;
    switch (ke.keycode) {
        case SDLK_ESCAPE:
        case SDLK_Q:
            if (fullscreen_) {
                SDL_SetWindowFullscreen(win_, false);
                fullscreen_ = false;
            } else {
                quit_ = true;
            }
            break;
        case SDLK_SPACE:
            player_.togglePause();
            publishEvent(player_.paused() ? PP_TOPIC_PLAY_PAUSED : PP_TOPIC_PLAY_RESUMED, "{}");
            break;
        case SDLK_LEFT:
            player_.seekRelative(shift ? -30.0 : -5.0);
            publishEvent(PP_TOPIC_PLAY_SEEKED, "{}");
            break;
        case SDLK_RIGHT:
            player_.seekRelative(shift ? 30.0 : 5.0);
            publishEvent(PP_TOPIC_PLAY_SEEKED, "{}");
            break;
        case SDLK_UP:
            player_.setVolume(player_.volume() + 0.05f);
            applyAudioParams();
            break;
        case SDLK_DOWN:
            player_.setVolume(player_.volume() - 0.05f);
            applyAudioParams();
            break;
        case SDLK_M: {
            player_.setMuted(!player_.muted());
            applyAudioParams();
            break;
        }
        case SDLK_F:
            fullscreen_ = !fullscreen_;
            SDL_SetWindowFullscreen(win_, fullscreen_);
            break;
        case SDLK_H:
            showHelp_ = !showHelp_;
            break;
        case SDLK_O:
            /* 整体隐藏 OSD：黑边很窄时（例如全屏 16:9 播 16:9）叠加层仍会压住画面，
             * 这一键给"零干扰观看"兜底。隐藏时连 osd.draw 都不广播，插件也省下绘制。 */
            osdHidden_ = !osdHidden_;
            PP_INFO("OSD 叠加层: %s（按 O 切换）", osdHidden_ ? "隐藏" : "显示");
            break;
        case SDLK_LEFTBRACKET:
            player_.setSpeed(player_.speed() - 0.25);
            applyAudioParams();
            break;
        case SDLK_RIGHTBRACKET:
            player_.setSpeed(player_.speed() + 0.25);
            applyAudioParams();
            break;
        case SDLK_0: case SDLK_1: case SDLK_2: case SDLK_3: case SDLK_4:
        case SDLK_5: case SDLK_6: case SDLK_7: case SDLK_8: case SDLK_9: {
            double pct = (double)(ke.keycode - SDLK_0) / 10.0;
            double d = player_.duration();
            if (d > 0) {
                player_.seekAbsolute(d * pct);
                publishEvent(PP_TOPIC_PLAY_SEEKED, "{}");
            }
            break;
        }
        case SDLK_S:
            invokeOrLog("snapshot", 0, nullptr);
            break;
        case SDLK_P: {
            std::string rep = plugins_.report(true);
            PP_INFO("插件状态:\n%s", rep.c_str());
            break;
        }
        case SDLK_TAB: {
            std::string c = host_.commandsText();
            PP_INFO("%s", c.c_str());
            break;
        }
        default:
            break;
    }
}

/* ======================= 帧泵 + 上传 ======================= */

bool App::makeFrameView(const std::shared_ptr<VideoFrame>& f, PP_FrameView* view) {
    if (!f || !view) return false;
    if (!player_.materializeRgba(f)) return false;
    view->data = f->rgba.data();
    view->width = f->w;
    view->height = f->h;
    view->pitch = f->pitch;
    view->format = PP_PIXFMT_RGBA8888;
    view->pts = f->pts;
    view->index = f->index;
    view->serial = f->serial;
    return true;
}

bool App::pumpAndUpload(bool force) {
    if (!player_.isOpen()) return false;

    stepAudioPreroll();
    player_.pumpFrame();

    /* currentFrame() 返回 shared_ptr 副本，插件/seek 并发改动 current_ 也不会悬空 */
    auto frame = player_.currentFrame();
    if (!frame) return false;
    /* 去重：主循环每轮都会调本函数，没有这道检查就会空转重传同一帧。
     * force 跳过它 —— 自检抓屏要的正是"把当前帧真的画上去一次"，而那一帧
     * 可能已经被 seek 步骤"看过"（index 落在 lastUploadedIndex_ 上），
     * 但屏幕上一次都没画过。 */
    if (!force && frame->index == lastUploadedIndex_) return false;
    lastUploadedIndex_ = frame->index;

    /* 取纹理数据：默认走 **NV12**（GPU 做 YUV→RGB，CPU 不逐像素转换，
     * 上传量只有 RGBA 的 3/8 —— 4K 每帧 33 MB → 12.4 MB）；满足下面任一条件
     * 就改走 RGBA 路径：
     *   ① 有插件挂了帧处理器 —— 它会在链上原地改 RGBA 像素；
     *   ② 这一帧的 RGBA 已经物化过（rgbaReady）—— 说明有人（如 acquire_frame）
     *      已经拿它处理甚至写回过，此时 RGBA 才是"权威内容"，
     *      继续用 NV12 会把插件的改动丢掉。
     * 这两个条件都要求"插件不启用时不占帧链"（见 ARCHITECTURE §6b）。
     *
     * 另有一个调试开关：PP_NO_NV12=1 强制走 RGBA 上传路径。
     * 它存在的意义是**同一份二进制**上做 A/B —— 否则"NV12 比 RGBA 快多少"
     * 只能拿两次不同构建的数字比，版本一变就没法复核（见 docs/DEVELOPMENT 的基准一节）。 */
    const bool chained = forceRgba_ || (host_.chain().count() > 0) || frame->rgbaReady;
    bool useRgba = false;

    if (chained) {
        PP_FrameView view{};
        if (makeFrameView(frame, &view)) {
            int chainResult = host_.runFrameChain(&view);
            if (chainResult == PP_CHAIN_DROP) {
                PP_DEBUG("帧 #%lld 被插件丢弃", (long long)frame->index);
                return false;
            }
            useRgba = true;
        } else {
            PP_WARN("NV12→RGBA 物化失败，本帧跳过帧处理链（照常显示原画面）");
        }
    }

    const uint32_t fmt = useRgba ? SDL_PIXELFORMAT_RGBA32 : SDL_PIXELFORMAT_NV12;
    const void* pixels = useRgba ? (const void*)frame->rgba.data()
                                 : (const void*)frame->nv12.data();
    const int pitch = useRgba ? frame->pitch : frame->nv12Pitch;
    /* 色彩空间必须在**进判据之前**算出来。
     * 早期写法把它放在"建纹理"的分支里，于是判据里的 cs 恒为初值 0，
     * `texCs_ != cs` 永远成立 —— NV12 纹理每帧都被销毁重建（4K 每帧 12 MB 反复分配）。
     * 这个 bug 只影响 NV12 分支（RGBA 分支该判据被 !useRgba 短路），
     * 也就是会**专门把 NV12 的账算坏**。 */
    int cs = 0;
    if (!useRgba) {
        /* 矩阵与范围必须告诉 SDL，否则 GPU 会用默认（JPEG/全范围 BT.601）
         * 去还原 —— 顺序反了就是整幅偏色。 */
        YuvColorChoice cc;
        cc.matrix = frame->colorMatrix;
        cc.fullRange = frame->fullRange;
        int fellBack = 0;
        cs = sdlColorspaceFor(cc, &fellBack);
        if (fellBack && !texCsFellBackWarned_) {
            texCsFellBackWarned_ = true;
            PP_WARN("该色彩矩阵在 SDL 里没有对应枚举（FCC/SMPTE240M），"
                    "NV12 纹理按 BT.601 处理");
        }
        /* P2-L1：把这一帧的色彩元数据记下来，供 video.color 服务暴露给插件；
         * 同时 HDR 内容首次出现时最好努力把渲染器输出切到 HDR（仅当显示器真的
         * 支持 HDR —— 否则不重建、维持 SDR，SDL 自动做 HDR→SDR 色调映射）。 */
        curVideoCs_ = (uint32_t)cs;
        curPrimaries_ = cc.primaries;
        curTrc_ = cc.trc;
        curMatrix_ = cc.matrix;
        curFullRange_ = cc.fullRange;
        curIsHdr_ = isHdrColorChoice(cc);
        if (curIsHdr_) ensureHdrOutput_();
    }

    if (!tex_ || texW_ != frame->w || texH_ != frame->h || texFmt_ != fmt ||
        (!useRgba && texCs_ != cs)) {
        if (tex_) { SDL_DestroyTexture(tex_); tex_ = nullptr; }
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, (Sint64)fmt);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER,
                              SDL_TEXTUREACCESS_STREAMING);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, frame->w);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, frame->h);
        if (!useRgba)
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER, (Sint64)cs);
        tex_ = SDL_CreateTextureWithProperties(ren_, props);
        SDL_DestroyProperties(props);
        if (!tex_) {
            PP_ERROR("创建纹理失败: %s", SDL_GetError());
            texW_ = texH_ = 0;   /* 别留下"已经有纹理"的假状态，下一轮还能重建 */
            texFmt_ = 0;
            texCs_ = -1;
            return false;
        }
        texW_ = frame->w;
        texH_ = frame->h;
        texFmt_ = fmt;
        texCs_ = useRgba ? -1 : cs;
    }
    /* 上传失败必须如实返回失败：早期版本忽略返回值，于是"纹理存在但内容没更新"
     * 会被自检当成上传成功。 */
    if (!SDL_UpdateTexture(tex_, nullptr, pixels, pitch)) {
        PP_ERROR("上传帧到纹理失败: %s", SDL_GetError());
        return false;
    }
    return true;
}

/* ======================= 渲染 ======================= */

void App::ensureHdrOutput_() {
    if (hdrOutputConfigured_) return;
    hdrOutputConfigured_ = true;   /* 只尝试一次，无论成败都不再重入 */

    /* 显示器不支持 HDR：维持默认 SDR 输出（纹理已标 HDR10，SDL 自动做 HDR→SDR
     * 色调映射回退）。这是绝大多数环境（dummy 驱动 / 普通 SDR 屏）的落点。 */
    SDL_DisplayID disp = SDL_GetDisplayForWindow(win_);
    SDL_PropertiesID dp = SDL_GetDisplayProperties(disp);
    bool hdrDisp = SDL_GetBooleanProperty(dp, SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN, false);
    SDL_DestroyProperties(dp);
    if (!hdrDisp) {
        PP_INFO("显示器不支持 HDR：维持 SDR 输出（HDR 纹理由 SDL 做 HDR→SDR 色调映射）");
        return;
    }
    /* 已是 HDR 输出则跳过。 */
    SDL_PropertiesID rprops = SDL_GetRendererProperties(ren_);
    Uint32 cur = (Uint32)SDL_GetNumberProperty(rprops, SDL_PROP_RENDERER_OUTPUT_COLORSPACE_NUMBER, 0);
    SDL_DestroyProperties(rprops);
    if (cur == PP_SDL_CS_SRGB_LINEAR) return;

    PP_INFO("显示器支持 HDR：重建渲染器以启用 HDR 输出 (SRGB_LINEAR)");
    /* 先把现有纹理与叠加层作废，让它们随下一帧 / 下一调用重建（renderOverlays
     * 对空 tex 有保护，不会在重建间隙崩）。 */
    if (tex_) { SDL_DestroyTexture(tex_); tex_ = nullptr; }
    texW_ = texH_ = texFmt_ = 0; texCs_ = -1;
    for (auto& o : overlays_) if (o.tex) { SDL_DestroyTexture(o.tex); o.tex = nullptr; }
    SDL_DestroyRenderer(ren_); ren_ = nullptr;

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, win_);
    SDL_SetNumberProperty(props, SDL_PROP_RENDERER_CREATE_OUTPUT_COLORSPACE_NUMBER, (Sint64)PP_SDL_CS_SRGB_LINEAR);
    SDL_SetNumberProperty(props, SDL_PROP_RENDERER_CREATE_PRESENT_VSYNC_NUMBER, 1);
    SDL_Renderer* r = SDL_CreateRendererWithProperties(props);
    SDL_DestroyProperties(props);
    if (!r) {
        PP_WARN("HDR 渲染器创建失败（%s），回退默认 SDR 渲染器", SDL_GetError());
        r = SDL_CreateRenderer(win_, nullptr);
        if (!r) { lastError_ = format("SDL_CreateRenderer 失败: %s", SDL_GetError()); return; }
    }
    ren_ = r;
    SDL_SetRenderVSync(ren_, 1);
    SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
}

void App::renderFrame() {
    /* 渲染几何一律按「渲染像素」计算：SDL_GetWindowSize 给的是窗口逻辑尺寸，在高
     * DPI 下与渲染目标不一致（实测：逻辑 1280x900 的窗口，渲染目标约 1038x727），
     * 会让画面位置算错、右侧与底部的 OSD 被整块裁掉。窗口隐藏（自检）时窗口仍在，
     * 同一个调用同样拿到真实的目标尺寸。 */
    int rw = 0, rh = 0;
    SDL_GetWindowSizeInPixels(win_, &rw, &rh);
    if (rw <= 0 || rh <= 0) {
        if (!hidden_) SDL_GetWindowSize(win_, &rw, &rh);
        if (rw <= 0 || rh <= 0) { rw = 960; rh = 540; }
    }
    winW_ = rw;
    winH_ = rh;

    SDL_SetRenderDrawColor(ren_, kClearR, kClearG, kClearB, 255);
    SDL_RenderClear(ren_);

    /* 画面矩形（letterbox 后的 dst）：OSD 插件据此避开画面、把叠加层画进黑边。
     * 无画面时整窗视为画面 —— 插件算出的黑边为零，自然退回画面内布局。 */
    float vx = 0.0f, vy = 0.0f, vw = (float)rw, vh = (float)rh;
    if (tex_) {
        double srcAspect = (double)texW_ / (double)texH_;
        /* 用窗口比例和视频比例求「含黑边」的显示矩形 */
        double dstAspect = (double)rw / (double)rh;
        float w, h;
        if (srcAspect > dstAspect) {
            w = (float)rw;
            h = (float)(rw / srcAspect);
        } else {
            h = (float)rh;
            w = (float)(rh * srcAspect);
        }
        SDL_FRect dst{(float)((rw - w) * 0.5), (float)((rh - h) * 0.5), w, h};
        SDL_RenderTexture(ren_, tex_, nullptr, &dst);
        vx = dst.x;
        vy = dst.y;
        vw = dst.w;
        vh = dst.h;
    }

    /* 记下画面矩形与"这一轮有没有画面"：自检抓屏取证要用它把该是画面的区域
     * 圈出来（对整窗统计会被 OSD 的占比淹掉）。无画面时上面已把整窗当作画面，
     * 此时 videoRectValid_ 为 false，取证会明确报"无法取证"而不是给个假数字。 */
    videoRectX_ = vx;
    videoRectY_ = vy;
    videoRectW_ = vw;
    videoRectH_ = vh;
    videoRectValid_ = (tex_ != nullptr);

    /* 字幕能力通道（P1-c）：每帧算出当前应显示的字幕行，仅在文本变化时广播。
     * 渲染由 subtitles 插件在 osd.draw 里做 —— 内核不渲染（架构红线）。
     * 走普通字符串主题 "subtitle.line"，不进 ABI。 */
    if (player_.isOpen() && player_.hasSubtitles()) {
        std::string line;
        player_.currentSubtitle(player_.masterClock(), line);
        if (line != lastSubLine_) {
            lastSubLine_ = line;
            if (line.empty())
                publishEvent("subtitle.line", "{\"text\":\"\"}");
            else
                publishEvent("subtitle.line",
                             format("{\"text\":\"%s\"}", jsonEscape(line).c_str()).c_str());
        }
    }

    /* OSD 阶段：把绘制权交给插件（O 键可整体隐藏，连 osd.draw 都不广播） */
    if (!osdHidden_) {
        Osd& osd = Osd::instance();
        osd.begin(ren_, rw, rh, vx, vy, vw, vh);
        host_.emitOsdDraw();
        renderBuiltinOverlay();
        osd.end();
        /* P1-b：插件叠加层在 OSD 之上、Present 之下合成。
         * osdHidden_ 为真时连同内置 OSD 一起隐藏（见 app.h 注释语义）。 */
        renderOverlays();
    }
}

void App::renderBuiltinOverlay() {
    Osd& osd = Osd::instance();
    const float W = (float)winW_;
    const float Hh = (float)winH_;

    if (!player_.isOpen()) {
        osd.panel(24, 24, W - 48, 96, PP_RGBA(0, 0, 0, 170), PP_RGBA(90, 90, 110, 220));
        osd.text(40, 40, "PluginPlayer — 插件版播放器", PP_RGB(255, 255, 255), 2.0f);
        osd.text(40, 72, "用法: pluginplayer.exe <媒体文件>   (按 H 查看快捷键)", PP_RGB(200, 200, 210), 1.0f);
        if (!lastError_.empty())
            osd.text(40, 90, lastError_.c_str(), PP_RGB(255, 140, 140), 1.0f);
        return;
    }

    /* 暂停时给个角标，避免"看起来卡住"（优先放进顶部黑边，不压画面） */
    if (player_.paused()) {
        float px = 0, py = 0, pw = 0, ph = 0;
        float y = 14.0f;
        const float lh = osd.lineHeight(1.5f);
        if (osd.videoRect(&px, &py, &pw, &ph) && py >= lh + 8.0f) y = py - lh - 6.0f;
        osd.text(16, y, "|| PAUSED", PP_RGB(255, 210, 120), 1.5f);
    }

    if (showHelp_) {
        static const char* lines[] = {
            "PluginPlayer 快捷键",
            "  SPACE        播放 / 暂停",
            "  LEFT/RIGHT   后退 / 前进 5 秒 (Shift 为 30 秒)",
            "  UP/DOWN      音量 +/- 5%",
            "  M            静音开关",
            "  0-9          跳转到 0%~90%",
            "  [ / ]        减速 / 加速 0.25x",
            "  F            全屏切换",
            "  S            执行 snapshot 命令（插件提供）",
            "  TAB          列出全部已注册命令",
            "  P            输出插件加载报告",
            "  O            显示 / 隐藏 OSD 叠加层",
            "  H            显示 / 隐藏本帮助",
            "  Q / ESC      退出（全屏时先退出全屏）",
        };
        const int n = (int)(sizeof(lines) / sizeof(lines[0]));
        float lh = osd.lineHeight(1.0f) + 6;
        float pw = 420, ph = lh * n + 24;
        osd.panel(24, 24, pw, ph, PP_RGBA(0, 0, 0, 200), PP_RGBA(120, 120, 150, 230));
        for (int i = 0; i < n; ++i) {
            osd.text(40, 36 + i * lh, lines[i],
                     i == 0 ? PP_RGB(120, 220, 255) : PP_RGB(230, 230, 235), 1.0f);
        }
    }
    (void)Hh;
}

/* ======================= 主循环 ======================= */

void App::tickPlugins() {
    double now = (double)SDL_GetTicks() / 1000.0;
    if (now - lastTickEmit_ >= 0.25) {
        lastTickEmit_ = now;
        PP_PlaybackStats st{};
        player_.fillStats(&st);
        std::string j = format(
            "{\"time\":%.3f,\"position\":%.3f,\"duration\":%.3f,\"paused\":%d,"
            "\"speed\":%.3f,\"fps\":%.2f,\"decoded\":%lld,\"displayed\":%lld,\"dropped\":%d}",
            now, st.position, st.duration, st.paused, st.speed, st.current_fps,
            (long long)st.decoded_frames, (long long)st.displayed_frames, st.dropped_frames);
        publishEvent(PP_TOPIC_APP_TICK, j.c_str());
    }

    /* 音频快照（供频谱类插件） */
    if (player_.hasAudio() && host_.bus().subscriberCount(PP_TOPIC_AUDIO_SAMPLES) > 0) {
        static float tap[4096];
        int n = player_.snapshotAudio(tap, 4096);
        if (n > 0) {
            PP_AudioTap at{};
            at.samples = tap;
            at.count = n;
            at.sample_rate = player_.audioSampleRate();
            at.channels = player_.audioChannels();
            at.pts = player_.position();
            /* 只走 dispatch（带 PP_AudioTap 二进制负载），不发 JSON 空负载 */
            host_.bus().dispatch(host_.handle(), PP_TOPIC_AUDIO_SAMPLES, &at, (int)sizeof(at));
        }
    }
}

void App::run() {
    while (!quit_) {
        /* 控制通道的请求在主线程执行：必须在每帧开头把队列抽干，
         * 否则 RPC 客户端会等到超时（见 RpcServer::pumpMainThread）。 */
        rpc_.pumpMainThread();
        pollEvents();
        pumpAndUpload();
        stepAudioLead();
        tickPlugins();
        renderFrame();
        SDL_RenderPresent(ren_);

        if (player_.isOpen() && player_.finished() && !completedFired_) {
            completedFired_ = true;
            publishEvent(PP_TOPIC_MEDIA_EOF, "{}");
            publishEvent(PP_TOPIC_PLAY_COMPLETED, "{}");
            PP_INFO("播放结束");
        }

        /* 标题栏跟随插件想要显示的字幕 */
        if (host_.takeSubtitleDirty()) {
            std::string t = "PluginPlayer";
            if (!mediaPath_.empty()) t = baseName(mediaPath_) + " — PluginPlayer";
            const std::string& sub = host_.subtitle();
            if (!sub.empty()) t += "  [" + sub + "]";
            SDL_SetWindowTitle(win_, t.c_str());
        }
    }
}

/* ======================= 自检 ======================= */

int App::selfTest(const std::string& media, int frames, const std::string& outDir) {
    makeDirs(outDir);
    double t0 = (double)SDL_GetTicks() / 1000.0;

    std::string reportPath = pathJoin(outDir, "selftest_report.json");
    std::string log;

    PP_INFO("==== 自检开始 ====");

    struct StepResult { std::string name; bool ok; bool skipped; std::string detail; };
    std::vector<StepResult> steps;
    auto addStep = [&](const std::string& n, bool ok, const std::string& d) {
        steps.push_back({n, ok, false, d});
        PP_INFO("[%s] %s — %s", ok ? "PASS" : "FAIL", n.c_str(), d.c_str());
    };
    /* 不适用（而非"通过"）：单独标记 skipped，绝不混进 PASS 计数。
     * 早期版本把「媒体时长短、seek 校验跳过」直接计为 PASS，等于让自检
     * 在最需要它的短用例上失去把关能力。 */
    auto skipStep = [&](const std::string& n, const std::string& d) {
        steps.push_back({n, false, true, d});
        PP_WARN("[SKIP] %s — %s", n.c_str(), d.c_str());
    };

    addStep("sdl_init", win_ != nullptr && ren_ != nullptr,
            format("SDL3 video driver=%s", SDL_GetCurrentVideoDriver()));

    addStep("plugins_loaded", plugins_.loadedCount() > 0,
            format("加载 %d 个插件，失败 %d 个", plugins_.loadedCount(), plugins_.failedCount()));

    addStep("services_registered", host_.services().count() > 0,
            format("服务 %d 个", host_.services().count()));

    addStep("commands_registered", host_.commands().count() > 0,
            format("命令 %d 个", host_.commands().count()));

    /* --- 打开媒体 --- */
    bool opened = openMedia(media);
    PP_MediaInfo mi{};
    if (opened) player_.fillMediaInfo(&mi);
    addStep("open_media", opened,
            opened ? format("%s %dx%d %.2f fps dur=%.2f s codec=%s/%s",
                            mi.format, mi.width, mi.height, mi.fps, mi.duration,
                            mi.video_codec, mi.audio_codec)
                   : lastError_);

    if (!opened) {
        /* 仍然产出一份报告，方便定位 */
        std::string j = "{\n  \"result\": \"FAIL\",\n  \"reason\": \"" +
                        jsonEscape(lastError_) + "\"\n}\n";
        FILE* f = fopen(reportPath.c_str(), "wb");
        if (f) { fwrite(j.data(), 1, j.size(), f); fclose(f); }
        return 2;
    }

    /* --- 解码/显示 N 帧 ---
     * 断言必须真的能判失败：目标是「frames 与媒体实际可用帧数中的较小者」，
     * 并且超时也要判失败。早期版本只要求 displayed >= min(frames,5)，
     * 等于「显示 5 帧就算过」。 */
    int displayed = 0;
    int savedPngs = 0;
    int chainDropped = 0;
    /* 待落盘的证据帧（延后到测量循环之后再写，见下面注释）。 */
    struct PendingShot { std::shared_ptr<VideoFrame> frame; std::string path; };
    std::vector<PendingShot> shots;
    int framesAvail = frames;
    /* 自检自己数"看到过哪些帧"用的序号。**不能**拿 lastUploadedIndex_ 当它用：
     * 那个成员的不变量是"只有 pumpAndUpload 会写"，自检去写就会把去重判据搞坏 ——
     * 后面的抓屏步骤于是以为"这一帧已经在屏幕上了"而跳过上传，抓出来的
     * 证据图里只剩清屏色与 OSD（这正是 P0-1 要修的那个 bug）。 */
    int64_t lastSeenIdx = -1;
    if (mi.has_video && mi.duration > 0 && mi.fps > 0.001)
        framesAvail = (int)(mi.duration * mi.fps);
    if (framesAvail < 1) framesAvail = 1;
    const int frameTarget = std::min(frames, framesAvail);

    if (!mi.has_video) {
        /* 无视频轨：自检的核心是视频解码路径，这里不能假装通过，
         * 也不该硬等 60 秒 —— 直接快速判定失败。 */
        addStep("decode_and_display", false,
                format("媒体没有视频轨（音频编码 %s），自检需要视频轨道，无法验证解码/渲染链路",
                       mi.audio_codec));
    } else {
        double budget = mi.duration > 0 ? mi.duration * 2.0 + 10.0 : 60.0;
        if (budget > 60.0) budget = 60.0;
        if (budget < 10.0) budget = 10.0;
        double deadline = (double)SDL_GetTicks() / 1000.0 + budget;

        /* 可接受下限：frameTarget-1。
         * 最后一帧常常在 EOF 边界被 demux/decoder 冲掉，这不是缺陷；
         * 但少 2 帧以上说明解码链路真有问题 —— 所以不能像早期版本那样
         * 只要显示 5 帧就算通过。 */
        const int frameMin = std::max(1, frameTarget - 1);
        int stall = 0;
        bool brokeOnDrain = false;
        while (displayed < frameTarget && (double)SDL_GetTicks() / 1000.0 < deadline) {
            rpc_.pumpMainThread();
            stepAudioPreroll();
            player_.pumpFrame();
            auto frame = player_.currentFrame();
            if (frame && frame->index != lastSeenIdx) {
                stall = 0;
                lastSeenIdx = frame->index;

                /* 只有"链上有处理器"或"这一帧要存 PNG 证据"时才需要 RGBA。
                 * **不要无条件物化** —— 那会让自检本身退化成慢路径，于是 2160p 上的
                 * decode_and_display 量的就成了"物化开销"而不是播放管线：实测那种口径下
                 * 同一份构建会被判 FAIL（284/299），而它的真实播放能跑到 ~29 fps。 */
                const int nextDisplayed = displayed + 1;
                const bool savePng = (savedPngs + (int)shots.size() < 3 &&
                    (nextDisplayed == 1 || nextDisplayed == frameTarget / 2 ||
                     nextDisplayed == frameTarget));
                const bool needChain = host_.chain().count() > 0;
                PP_FrameView view{};
                bool haveView = false;
                if (needChain || savePng) {
                    haveView = makeFrameView(frame, &view);
                    if (!haveView) PP_WARN("本帧无法物化成 RGBA，跳过帧链与证据落盘");
                }
                if (needChain && haveView) {
                    int cr = host_.runFrameChain(&view);
                    if (cr == PP_CHAIN_DROP) { chainDropped++; continue; }
                }

                displayed++;

                /* 证据帧先攥在手里，**等测量循环结束再落盘**。
                 * 边测边写是错的：写一张 720p PNG 要 ~137 ms（4K 更久），这期间主循环
                 * 完全停摆而主时钟照走，回到循环时队列里三四帧都已"到点"，
                 * pumpFrame 只能整批弹掉、只留最后一帧 —— 实测**每写一张证据图固定丢 3 帧**，
                 * 于是 decode_and_display 量的其实是"写 PNG 的开销"而非播放管线。
                 * 帧对象是引用计数的独立对象（解码线程每帧新建），留着 shared_ptr 即安全。 */
                if (savePng && haveView)
                    shots.push_back({frame, pathJoin(outDir,
                        format("frame_%03d.png", savedPngs + (int)shots.size() + 1))});
            } else {
                stall++;
            }
            /* 已到媒体末尾且约 0.5 秒没新帧：不必白等到 deadline */
            if (player_.finished() && stall > 250) { brokeOnDrain = true; break; }
            SDL_Delay(2);
        }
        /* 测量结束，这才把证据图写出去（写盘开销不再计入播放口径）。 */
        for (const auto& s : shots) {
            PP_FrameView v{};
            if (makeFrameView(s.frame, &v) &&
                writePngRGBA(s.path, v.data, v.width, v.height, v.pitch)) {
                savedPngs++;
                PP_INFO("已保存帧快照: %s (%dx%d, pts=%.3f)",
                        s.path.c_str(), v.width, v.height, v.pts);
            }
        }
        shots.clear();
        bool hitDeadline = (double)SDL_GetTicks() / 1000.0 >= deadline;
        /* 只报"被插件丢弃"是不够的：播放器自己会因为**帧晚于主时钟**而整批跳过
         * （pumpFrame 把 pts <= master 的帧一次性弹空，只留最后一帧）。少帧时到底是
         * 解码慢、时钟跑飞、还是插件丢帧，必须三样分开看 —— 否则"显示 284/300"
         * 这种结论没法定位，早期版本就是因此反复误判。 */
        PP_PlaybackStats st{};
        player_.fillStats(&st);
        addStep("decode_and_display", displayed >= frameMin,
                format("显示 %d 帧（目标 %d，可接受下限 %d；媒体约 %d 帧可用）%s%s，"
                       "解码 %lld / 播放器丢帧 %d / 插件丢帧 %d，主时钟 %.3f 视频时钟 %.3f",
                       displayed, frameTarget, frameMin, framesAvail,
                       hitDeadline ? "（触达时限）" : "",
                       brokeOnDrain ? "（队列已排空即退出）" : "",
                       (long long)st.decoded_frames, st.dropped_frames, chainDropped,
                       st.master_clock, st.video_clock));
    }

    /* --- seek --- */
    bool seekOk = false;
    double posAfterSeek = 0.0;
    if (player_.duration() > 2.0) {
        double target = player_.duration() * 0.6;
        player_.seekAbsolute(target);
        int got = 0;
        double dl = (double)SDL_GetTicks() / 1000.0 + 20.0;
        double lastIdx = (double)lastSeenIdx;   /* seek 之前看到的最后一帧序号 */
        if (mi.has_video) {
            while (got < 5 && (double)SDL_GetTicks() / 1000.0 < dl) {
                rpc_.pumpMainThread();
                stepAudioPreroll();
                player_.pumpFrame();
                auto f = player_.currentFrame();
                if (f && f->index != lastSeenIdx) { lastSeenIdx = f->index; got++; }
                SDL_Delay(2);
            }
        }
        posAfterSeek = player_.position();
        if (mi.has_video) {
            seekOk = (got >= 5) && (std::fabs(posAfterSeek - target) < 3.0);
            addStep("seek", seekOk,
                    format("目标 %.2f s，seek 后位置 %.2f s，新显示 %d 帧 (上一帧 index=%.0f)",
                           target, posAfterSeek, got, lastIdx));
        } else {
            /* 无视频轨时像素层面无法校验，只能校验位置——如实说明，不冒充完整校验 */
            seekOk = std::fabs(posAfterSeek - target) < 3.0;
            addStep("seek", seekOk,
                    format("目标 %.2f s，seek 后位置 %.2f s（媒体无视频轨，仅校验位置，未校验帧输出）",
                           target, posAfterSeek));
        }
    } else {
        skipStep("seek", format("媒体时长 %.2f s <= 2.0 s，不适用 seek 校验（未验证）",
                                player_.duration()));
    }

    /* --- 调用插件命令（约定：argv 只含实参，不含命令名） ---
     * 返回码必须一起断言。早期版本只统计"调用了几条命令"，于是
     * `snapshot -> -1`（写文件被白名单拒了）也照样 PASS —— 这一步在最需要它
     * 的时候失去把关能力，还把"命令链路通了"的错觉写进了验证记录。 */
    const char* snapArgv[1] = { outDir.c_str() };
    int cmdsRun = 0, cmdsFailed = 0;
    std::string cmdDetail, cmdHint;
    auto runCmd = [&](const char* name, const char* label, int argc, const char** argv) {
        int r = host_.invokeCommand(name, argc, argv);
        cmdsRun++;
        cmdDetail += format("%s -> %d  ", label, r);
        if (r == 0) return;
        cmdsFailed++;
        if (strcmp(name, "snapshot") == 0) {
            cmdHint = format("：snapshot 返回 %d —— 要么当前没有可用帧，要么写文件被拒"
                             "（插件只能往 exeDir(=%s) 写；输出目录 %s 不在白名单内时，"
                             "用 --write-dir %s 追加，或把 --out 放到 dist/ 下）",
                             r, exeDir().c_str(), outDir.c_str(), outDir.c_str());
        } else {
            cmdHint = format("：%s 返回 %d（约定 0 = 成功）", label, r);
        }
    };
    if (host_.commands().has("snapshot")) runCmd("snapshot", "snapshot", 1, snapArgv);
    if (host_.commands().has("stats")) runCmd("stats", "stats", 0, nullptr);
    if (host_.commands().has("effect.toggle")) {
        const char* a1[1] = {"gray"};
        runCmd("effect.toggle", "effect.toggle gray", 1, a1);
        /* 开一下再关掉，确认链路可逆 */
        host_.invokeCommand("effect.toggle", 1, a1);
    }
    addStep("plugin_commands", cmdsRun > 0 && cmdsFailed == 0,
            cmdsRun ? (cmdDetail + cmdHint) : std::string("没有任何插件注册命令（异常）"));

    /* --- P1-d：多轨切换（reopen 路径） ---
     * 在当前媒体上重新选择音频轨（同一轨，验证 reopen + reseek 不破坏播放链路）。
     * 断言：selectTrack 返回成功，且切换点之后 decoded_frames 继续增长（解码没断）。
     * 注意：reopen 会把 decodedFrames_ 归零并 reseek 到原位置，所以必须以「切换点」
     * 为基准比对，不能拿切换前的累计值（短片的尾巴帧数远少于之前累计的全片帧数）。 */
    if (player_.hasAudio()) {
        bool switched = selectTrack(AVMEDIA_TYPE_AUDIO, player_.audioTrackIndex());
        /* 切完 reopen 会归零 decodedFrames_ 并 reseek 到原位置；以切换点为基准。 */
        int64_t decAtSwitch = player_.decodedFrames();
        double dl = (double)SDL_GetTicks() / 1000.0 + 8.0;
        while ((double)SDL_GetTicks() / 1000.0 < dl) {
            rpc_.pumpMainThread();
            stepAudioPreroll();
            player_.pumpFrame();
            SDL_Delay(2);
        }
        int64_t decAfter = player_.decodedFrames();
        addStep("track_switch", switched && decAfter > decAtSwitch,
                format("selectTrack(audio,%d)=%d，切换点解码 %lld / 切换后 %lld（增长 %lld）",
                       player_.audioTrackIndex(), switched,
                       (long long)decAtSwitch, (long long)decAfter,
                       (long long)(decAfter - decAtSwitch)));
    } else {
        skipStep("track_switch", "媒体没有音频轨，不适用多轨切换校验");
    }

    /* --- 渲染一次并抓屏，然后**验证抓屏里真的有画面** ---
     * 这一项以前只断言 `surf != nullptr`，等于"SDL 没报错就算过"。实测抓到的
     * 图里根本没有画面：86.6% 的像素恰好是清屏色 RGB(16,16,20)，其余全是 OSD，
     * 纯黑占 0.00% —— 一张不含画面的"证据图"却写进了验证记录，等于没验证。
     * 现在断言三件事：
     *   ① force 上传真的成功（不去重）；
     *   ② 抓屏成功；
     *   ③ 画面矩形内"恰好等于清屏色"的像素占比低于阈值。
     * 判据见 util.h 的 rgbaColorRatioInRect 与上面的 kMaxClearRatioInVideo：
     * "图是黑的"说明不了问题（黑场是合法内容），"整片是清屏色"才说明没画上去。
     * 抓屏产物只是自检证据，不是运行时功能。对超大帧主动跳过落盘，
     * 否则会写出 GB 级 BMP（历史实测 --size 20000 写出过 1.6 GB）。
     * 读回必须发生在 Present 之前：Present 之后后备缓冲内容未定义。 */
    bool uploaded = pumpAndUpload(true);
    renderFrame();
    SDL_Surface* surf = SDL_RenderReadPixels(ren_, nullptr);
    SDL_RenderPresent(ren_);
    bool captured = (surf != nullptr);
    bool shot = false;
    double clearRatio = -1.0;
    std::string capDetail;
    if (surf) {
        /* 读回缓冲的字节序由渲染后端决定（本机 D3D11 上实测是 RGBA，但不能假定），
         * 统一转成 RGBA32 再取样 —— 否则在 BGRA 后端上会把 R 与 B 读反，
         * 取证判据会得出一个"看着合理"的错结论。 */
        SDL_Surface* rgba = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_RGBA32);
        if (rgba && videoRectValid_)
            clearRatio = rgbaColorRatioInRect((const uint8_t*)rgba->pixels, rgba->pitch,
                                              rgba->w, rgba->h,
                                              (int)videoRectX_, (int)videoRectY_,
                                              (int)videoRectW_, (int)videoRectH_,
                                              kClearR, kClearG, kClearB);
        if (rgba) SDL_DestroySurface(rgba);

        const int64_t px = (int64_t)surf->w * (int64_t)surf->h;
        if (px > kMaxCapturePixels) {
            capDetail = format("渲染抓屏成功（%dx%d = %lld 像素），超过 %lld 上限，跳过落盘以免写出超大 BMP",
                               surf->w, surf->h, (long long)px, (long long)kMaxCapturePixels);
            PP_WARN("%s", capDetail.c_str());
        } else {
            /* 日志文字必须与真实落盘文件名一致：SDL_SaveBMP 写出的是 .bmp，不是 .png */
            std::string p = pathJoin(outDir, "render_output.bmp");
            if (SDL_SaveBMP(surf, p.c_str())) {
                shot = true;
                capDetail = format("渲染帧已抓屏保存: %s (%dx%d)", p.c_str(), surf->w, surf->h);
                PP_INFO("%s", capDetail.c_str());
            } else {
                capDetail = format("抓屏文件写出失败: %s", SDL_GetError());
            }
        }
        SDL_DestroySurface(surf);
    } else {
        capDetail = format("抓屏失败: %s", SDL_GetError());
    }
    (void)shot;

    if (!uploaded) capDetail += "；帧未被上传到纹理（屏幕上不会有画面）";
    if (clearRatio < 0.0) {
        capDetail += "；未能取证（没有画面矩形，或读回缓冲转换失败）";
    } else {
        capDetail += format("；画面矩形 %dx%d@(%d,%d) 内清屏色占比 %.2f%%（上限 %.0f%%）",
                            (int)videoRectW_, (int)videoRectH_, (int)videoRectX_, (int)videoRectY_,
                            clearRatio * 100.0, kMaxClearRatioInVideo * 100.0);
    }
    bool evidenceOk = captured && uploaded && clearRatio >= 0.0 &&
                      clearRatio < kMaxClearRatioInVideo;
    addStep("render_and_capture", evidenceOk, capDetail);

    /* --- P1-a 真字体：中文字形渲染（像素级断言） ---
     * 判据必须能证伪，不能只看"字体加载成功"：
     * 把同一个汉字和一个 '?' 用**同一条 OSD 路径**、同样的字号画在相邻位置，
     * 抓屏后逐像素比较两块区域。若中文被降级成 '?'（字体没生效/不是 TrueType 轮廓），
     * 两块会**逐像素相同** → 断言失败。这正是要抓的失效模式。
     * 字体不可用时 SKIP（跳过 ≠ 通过，与既有口径一致）。 */
    {
        Font& f = Font::instance();
        if (!f.loaded()) {
            skipStep("font_render", "真字体未加载，跳过中文字形校验（未验证，UI 已回退调试点阵字）");
        } else {
            const int bx = 60, by = 60, side = 40;
            const int qx = 160;
            SDL_SetRenderDrawColor(ren_, 16, 16, 20, 255);
            SDL_RenderClear(ren_);
            {
                Osd& osd = Osd::instance();
                osd.begin(ren_, winW_, winH_, 0, 0, (float)winW_, (float)winH_);
                /* 对照画**三个** '?'：汉字"播"的 UTF-8 是 3 字节，一旦被调试字体
                 * 逐字节降级，画出来的正是 3 个 '?' —— 只有对照数量一致，
                 * "降级"才会让两块区域逐像素相同，断言才抓得住。
                 * （早期版本对照只画 1 个 '?'，于是降级后仍有 48 像素差异而误判 PASS。） */
                osd.text((float)bx, (float)by, "\xE6\x92\xAD", 0xFFFFFFFFu, 4.0f);  /* 播 */
                osd.text((float)qx, (float)by, "???", 0xFFFFFFFFu, 4.0f);
                osd.end();
            }
            /* **不要 Present** —— Present 之后 backbuffer 内容未定义，读到的会是
             * 上一帧的视频画面（实测：两块区域都变成"整片亮"，断言形同虚设）。
             * 直接 ReadPixels 读当前渲染目标，拿到的才是刚画的字形。 */
            SDL_Surface* s = SDL_RenderReadPixels(ren_, nullptr);
            if (!s) {
                addStep("font_render", false, format("抓屏失败: %s", SDL_GetError()));
            } else {
                int diff = 0, inkCn = 0, inkQ = 0;
                for (int y = by; y < by + side && y < s->h; y++) {
                    for (int x = bx; x < bx + side && x < s->w; x++) {
                        const uint8_t* a = (const uint8_t*)s->pixels + y * s->pitch + x * 4;
                        const uint8_t* b = (const uint8_t*)s->pixels + y * s->pitch + (x + qx - bx) * 4;
                        if (a[0] != b[0] || a[1] != b[1] || a[2] != b[2]) diff++;
                        if (a[0] > 60 || a[1] > 60 || a[2] > 60) inkCn++;   /* 非背景的"墨迹" */
                        if (b[0] > 60 || b[1] > 60 || b[2] > 60) inkQ++;
                    }
                }
                SDL_DestroySurface(s);
                /* 两个条件缺一不可：
                 *   ① 中文区真的有墨迹（否则是画了个空）；
                 *   ② 与 '?' 的形状不同（否则说明中文被降级了）。 */
                bool ok = (inkCn >= 30) && (inkQ >= 5) && (diff >= 30);
                addStep("font_render", ok,
                        format("中文'播'与'?'逐像素差异 %d 个像素（阈值 %d）；"
                               "中文区墨迹 %d px、'?' 区墨迹 %d px —— 差异为 0 即说明中文被降级成 '?'",
                               diff, 30, inkCn, inkQ));
            }
        }
    }

    /* --- P1-b 通用叠加层：渲染 + owner 撤销不残留（像素级断言） ---
     * 判据：推一个红色方块图层（owner=1）→ 渲染后该区域应出现红色像素；
     * 调 overlayClear(owner) 后重渲染，红色像素必须归零、图层数归零。
     * 这直接验证"插件卸载时内核自动清掉它的图层"这条架构红线（Host::revokeAll
     * 走的就是 overlayClear(owner)），以及 overlay 真的被合成到画面。 */
    {
        const int OWNER = 1;
        const int SIDE = 40;
        const int OX = 80, OY = 80;
        std::vector<uint8_t> px((size_t)SIDE * SIDE * 4, 0);
        for (size_t i = 0; i < px.size(); i += 4) {
            px[i] = 230; px[i + 1] = 30; px[i + 2] = 30; px[i + 3] = 255;   /* 红 */
        }
        int id = overlaySet(OWNER, 0, px.data(), SIDE, SIDE, (float)OX, (float)OY, 0, 0, 1.0f);
        bool ok = (id > 0) && (overlayCount() == 1);
        std::string detail;
        if (!ok) {
            detail = format("overlaySet 失败（id=%d, 图层数=%d）", id, overlayCount());
        } else {
            auto redIn = [this, OX, OY, SIDE](int& redTex, int& redCtrl) {
                redTex = 0; redCtrl = 0;
                SDL_SetRenderDrawColor(ren_, 16, 16, 20, 255);
                SDL_RenderClear(ren_);
                renderOverlays();                       /* 纹理叠加层（80,80） */
                /* 对照：同一渲染器直接 FillRect 红方块（200,200），用于区分
                 * "纹理渲染本身失效" 还是 "读回/区域判断失效"。 */
                SDL_SetRenderDrawColor(ren_, 230, 30, 30, 255);
                SDL_FRect ctrl{(float)(OX + 120), (float)OY, (float)SIDE, (float)SIDE};
                SDL_RenderFillRect(ren_, &ctrl);
                /* 读回必须发生在 Present 之前（与生产路径 renderFrame 末尾的 Present
                 * 不同）—— Present 之后后备缓冲内容未定义，D3D11 上读回可能全黑。
                 * 读回缓冲的字节序由后端决定（D3D11 本机实测是 BGRA），必须统一
                 * 转成 SDL_PIXELFORMAT_RGBA32 再取样，否则会把 R 与 B 读反，导致
                 * "纹理红=0 对照红=0" 的假阴性。该归一化与 render_and_capture 一致。 */
                SDL_Surface* s = SDL_RenderReadPixels(ren_, nullptr);
                SDL_RenderPresent(ren_);
                if (!s) return false;
                SDL_Surface* rgba = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
                if (!rgba) { SDL_DestroySurface(s); return false; }
                for (int y = OY; y < OY + SIDE && y < rgba->h; y++)
                    for (int x = OX; x < OX + SIDE && x < rgba->w; x++) {
                        const uint8_t* p = (const uint8_t*)rgba->pixels + y * rgba->pitch + x * 4;
                        if (p[0] > 150 && p[1] < 80 && p[2] < 80) redTex++;
                    }
                for (int y = OY; y < OY + SIDE && y < rgba->h; y++)
                    for (int x = OX + 120; x < OX + 120 + SIDE && x < rgba->w; x++) {
                        const uint8_t* p = (const uint8_t*)rgba->pixels + y * rgba->pitch + x * 4;
                        if (p[0] > 150 && p[1] < 80 && p[2] < 80) redCtrl++;
                    }
                SDL_DestroySurface(rgba);
                SDL_DestroySurface(s);
                return true;
            };
            int red1 = 0, red2 = 0, rc1 = 0, rc2 = 0;
            bool rd1 = redIn(red1, rc1);
            overlayClear(OWNER);                       /* 模拟插件卸载 */
            bool rd2 = redIn(red2, rc2);
            PP_WARN("overlay debug: id=%d count=%d | 渲染后 纹理红=%d 对照红=%d | 撤销后 纹理红=%d 对照红=%d",
                    id, overlayCount(), red1, rc1, red2, rc2);
            ok = rd1 && rd2 &&
                 red1 > SIDE * SIDE / 2 &&
                 rc1 > SIDE * SIDE / 2 &&
                 red2 == 0 &&
                 rc2 > SIDE * SIDE / 2 &&           /* 对照在撤销后也应仍在（它不依赖图层） */
                 overlayCount() == 0;
            detail = format("渲染后 纹理红=%d 对照红=%d（阈值 %d）；owner 撤销后 纹理红=%d 对照红=%d，"
                            "图层数 %d -> %d",
                            red1, rc1, SIDE * SIDE / 2, red2, rc2, 1, overlayCount());
        }
        addStep("overlay_owner_revoke", ok, detail);
    }

    /* --- P1-c 字幕渲染（像素级断言） ---
     * 验证"内核暴露 subtitle.line 事件 → subtitles 插件用 P1-a 真字体把字画到屏幕"
     * 这条 P1-c 通道真的把字渲染出来（不是只把事件发出去）。
     * 做法：临时写一段覆盖整个时长的外部 .srt，用 subtitle.load 命令加载；
     * 渲染一帧后到【底部黑边带】找近白色字幕像素。
     * 命令缺失 / .srt 写不出 / 抓屏失败 → SKIP（跳过 ≠ 通过），不冒充验证。 */
    {
        std::string subPath = pathJoin(outDir, "selftest_sub.srt");
        std::string srt = "1\n00:00:00,000 --> 23:59:59,999\nSUBTITLE 字幕测试 你好世界\n";
        {
            FILE* f = fopen(subPath.c_str(), "wb");
            if (f) { fwrite(srt.data(), 1, srt.size(), f); fclose(f); }
        }
        const char* slArgv[1] = { subPath.c_str() };
        bool subLoaded = fileExists(subPath) && host_.commands().has("subtitle.load") &&
                          host_.invokeCommand("subtitle.load", 1, slArgv) == 0;
        if (!subLoaded) {
            skipStep("subtitle_render", "无法加载外部字幕（命令缺失或 .srt 写出失败），跳过字幕渲染校验");
        } else {
            /* 推进一帧让主时钟走动，并让 subtitle.line 事件发出当前行（仅文本变化时广播） */
            rpc_.pumpMainThread();
            stepAudioPreroll();
            player_.pumpFrame();
            pumpAndUpload(true);
            renderFrame();
            SDL_Surface* s = SDL_RenderReadPixels(ren_, nullptr);
            SDL_RenderPresent(ren_);
            if (!s) {
                skipStep("subtitle_render", "字幕渲染后抓屏失败，跳过校验");
            } else {
                /* 存盘以便人工/取证核对（这帧到底有没有字幕） */
                std::string bmpPath = pathJoin(outDir, "subtitle_render.bmp");
                SDL_SaveBMP(s, bmpPath.c_str());
                SDL_Surface* rgba = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
                int whiteAll = 0, whiteBand = 0, yMin = rgba ? rgba->h : 0, yMax = 0;
                const int y0 = (int)(winH_ * 0.74);
                if (rgba) {
                    for (int y = 0; y < rgba->h; y++)
                        for (int x = 0; x < rgba->w; x++) {
                            const uint8_t* p = (const uint8_t*)rgba->pixels +
                                               y * rgba->pitch + x * 4;
                            if (p[0] > 180 && p[1] > 180 && p[2] > 180) {
                                whiteAll++;
                                if (y > yMax) yMax = y;
                                if (y < yMin) yMin = y;
                                if (y >= y0) whiteBand++;
                            }
                        }
                    SDL_DestroySurface(rgba);
                }
                SDL_DestroySurface(s);
                bool ok = whiteBand >= 150;
                addStep("subtitle_render", ok,
                        format("近白像素 全帧 %d / 底部带 %d（阈值 150）；白区 y∈[%d,%d]；已存 %s%s",
                               whiteAll, whiteBand, yMin, yMax, bmpPath.c_str(),
                               ok ? "" : "；底部带为 0 说明 subtitle.line → 插件渲染通道未把字画到屏幕"));
            }
        }
    }

    /* --- 卸载/重载往返测试（验证注册撤销干净） --- */
    int beforeSubs = host_.bus().totalSubscriptions();
    int beforeCmds = host_.commands().count();
    int beforeSvcs = host_.services().count();
    int beforeProc = host_.chain().count();
    bool reloadOk = true;
    std::string reloadDetail;
    if (!plugins_.slots().empty()) {
        reloadOk = plugins_.reloadOne(&host_, 0);
        int afterSubs = host_.bus().totalSubscriptions();
        int afterCmds = host_.commands().count();
        int afterSvcs = host_.services().count();
        int afterProc = host_.chain().count();
        bool noLeak = (afterSubs == beforeSubs) && (afterCmds == beforeCmds) &&
                      (afterSvcs == beforeSvcs) && (afterProc == beforeProc);
        reloadDetail = format("重载后 订阅 %d->%d, 命令 %d->%d, 服务 %d->%d, 帧处理器 %d->%d%s",
                              beforeSubs, afterSubs, beforeCmds, afterCmds,
                              beforeSvcs, afterSvcs, beforeProc, afterProc,
                              noLeak ? "（无泄漏）" : "（数量不一致：注册未干净撤销）");
        /* 泄漏必须判失败：早期版本只断言 commands>0，收集了泄漏证据却从不断言，
         * 等价于这项检查永远为绿。 */
        reloadOk = reloadOk && afterCmds > 0 && noLeak;
    } else {
        reloadDetail = "没有插件可重载";
    }
    addStep("plugin_hot_reload", reloadOk, reloadDetail);

    /* --- 统计快照 ---
     * 位置很关键：它在下面的 reopen_media 回归**之前**取。报告里的 playback
     * 描述的是「首次打开 → 解码 → seek → 调插件命令 → 抓屏」这段会话，
     * 不能被二次打开的结果覆盖。 */
    PP_PlaybackStats st{};
    player_.fillStats(&st);

    /* --- 二次打开同一媒体（回归：open 必须复位全部跨媒体状态） ---
     * 历史缺陷（2026-09-12 复现并修掉）：Player::open() 漏复位 demuxFinished_ /
     * drainSent_。第一个媒体到 EOF 时它们被置真，于是第二次打开后 demux 线程
     * 一进循环就命中"已完成"分支、一个包都不读；同时 finished() 当场为真，
     * 主时钟被冻在 0 —— 表现就是**永久黑屏无画面**，日志里「已打开媒体」紧跟着
     * 一行「播放结束」。App 层的状态（lastUploadedIndex_ / completedFired_）
     * 当时是复位了的，漏的是 Player 层，所以这个缺陷此前一直没被自检碰到。
     * 这里重新打开同一媒体并解码若干帧，断言真的出画。 */
    bool reopenOk = false;
    std::string reopenDetail;
    {
        const int rTarget = std::min(10, framesAvail);
        const int rMin = std::max(1, rTarget - 1);
        int rFrames = 0;
        int rChainDrop = 0;
        bool rOpened = openMedia(media);
        int64_t rSeen = -1;
        double rDeadline = (double)SDL_GetTicks() / 1000.0 + 15.0;
        while (rOpened && rFrames < rTarget && (double)SDL_GetTicks() / 1000.0 < rDeadline) {
            rpc_.pumpMainThread();
            stepAudioPreroll();
            player_.pumpFrame();
            auto f = player_.currentFrame();
            if (f && f->index != rSeen) {
                rSeen = f->index;
                /* 同上：链为空就不必物化（这里也不需要 PNG 证据）。 */
                PP_FrameView view{};
                if (host_.chain().count() > 0 && makeFrameView(f, &view)) {
                    int cr = host_.runFrameChain(&view);
                    if (cr == PP_CHAIN_DROP) { rChainDrop++; continue; }
                }
                rFrames++;
            }
            SDL_Delay(2);
        }
        /* 下限与 decode_and_display 同一口径：目标减 1（容忍 EOF 边界丢的最后一帧），
         * 而不是"出几帧就算过"。 */
        reopenOk = rOpened && rFrames >= rMin;
        reopenDetail = rOpened
            ? format("二次打开后解码并显示 %d 帧（目标 %d，可接受下限 %d）%s，被插件丢弃 %d 帧",
                     rFrames, rTarget, rMin,
                     rFrames >= rMin ? "" : "；一次都没出画 = 第二次 open 后播放没起来",
                     rChainDrop)
            : format("二次打开失败: %s", lastError_);
    }
    addStep("reopen_media", reopenOk, reopenDetail);

    /* --- 产出 --- */
    double elapsed = (double)SDL_GetTicks() / 1000.0 - t0;

    int pass = 0, counted = 0, skippedN = 0;
    for (const auto& s : steps) {
        if (s.skipped) { skippedN++; continue; }   /* 不适用 ≠ 通过 */
        counted++;
        if (s.ok) pass++;
    }

    std::string j;
    j += "{\n";
    j += "  \"tool\": \"pluginplayer selftest\",\n";
    j += format("  \"result\": \"%s\",\n", pass == counted ? "PASS" : "FAIL");
    j += format("  \"pass\": %d,\n  \"total\": %d,\n  \"skipped\": %d,\n",
                pass, counted, skippedN);
    j += format("  \"elapsed_sec\": %.3f,\n", elapsed);
    j += format("  \"media\": {\"path\":\"%s\",\"format\":\"%s\",\"video_codec\":\"%s\","
                "\"audio_codec\":\"%s\",\"width\":%d,\"height\":%d,\"fps\":%.4f,"
                "\"duration\":%.4f,\"bitrate\":%lld,\"sample_rate\":%d,\"channels\":%d},\n",
                jsonEscape(mi.path).c_str(), jsonEscape(mi.format).c_str(),
                jsonEscape(mi.video_codec).c_str(), jsonEscape(mi.audio_codec).c_str(),
                mi.width, mi.height, mi.fps, mi.duration, (long long)mi.bitrate,
                mi.audio_sample_rate, mi.audio_channels);
    j += format("  \"playback\": {\"displayed\":%lld,\"decoded\":%lld,\"dropped\":%d,"
                "\"position\":%.3f,\"master_clock\":%.3f,\"video_clock\":%.3f,"
                "\"audio_buffer_bytes\":%lld},\n",
                (long long)st.displayed_frames, (long long)st.decoded_frames,
                st.dropped_frames, st.position, st.master_clock, st.video_clock,
                (long long)st.audio_buffer_bytes);
    j += format("  \"registry\": {\"subscriptions\":%d,\"services\":%d,\"commands\":%d,"
                "\"frame_processors\":%d},\n",
                host_.bus().totalSubscriptions(), host_.services().count(),
                host_.commands().count(), host_.chain().count());
    j += format("  \"plugins\": %s,\n", plugins_.jsonSummary().c_str());
    j += "  \"steps\": [\n";
    for (size_t i = 0; i < steps.size(); ++i) {
        j += format("    {\"name\":\"%s\",\"ok\":%s,\"skipped\":%s,\"detail\":\"%s\"}%s\n",
                    jsonEscape(steps[i].name).c_str(), steps[i].ok ? "true" : "false",
                    steps[i].skipped ? "true" : "false",
                    jsonEscape(steps[i].detail).c_str(),
                    i + 1 < steps.size() ? "," : "");
    }
    j += "  ]\n}\n";

    FILE* f = fopen(reportPath.c_str(), "wb");
    if (f) { fwrite(j.data(), 1, j.size(), f); fclose(f); }
    PP_INFO("自检报告已写入: %s", reportPath.c_str());
    if (skippedN > 0)
        PP_WARN("有 %d 项检查不适用而跳过（未验证，不计入通过数）", skippedN);
    PP_INFO("==== 自检结束: %d/%d 项通过，跳过 %d 项，用时 %.2f s ====",
            pass, counted, skippedN, elapsed);

    return pass == counted ? 0 : 1;
}

std::string App::statusLine() const {
    PP_PlaybackStats st{};
    ((Player&)player_).fillStats(&st);
    return format("%s / %s  %.0f fps  vol=%.0f%%  speed=%.2fx",
                  fmtTime(st.position).c_str(), fmtTime(st.duration).c_str(),
                  st.current_fps, st.master_clock * 0 + (player_.volume() * 100.0f),
                  st.speed);
}

} // namespace pp
