/* app.cpp — 应用层实现 */
#include "app.h"
#include "osd.h"
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
    /* 所有宿主事件同时转发给控制通道（进程外插件） */
    host_.setEventSink([this](const std::string& t, const std::string& j) {
        rpc_.broadcastEvent(t, j);
    });

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

    audioStream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
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

    /* OSD 阶段：把绘制权交给插件（O 键可整体隐藏，连 osd.draw 都不广播） */
    if (!osdHidden_) {
        Osd& osd = Osd::instance();
        osd.begin(ren_, rw, rh, vx, vy, vw, vh);
        host_.emitOsdDraw();
        renderBuiltinOverlay();
        osd.end();
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
