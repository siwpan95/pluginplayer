/* app.cpp — 应用层实现 */
#include "app.h"
#include "osd.h"
#include "util.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>

namespace pp {

/* 自检抓屏落盘的像素上限（4096x4096 ≈ 16.7M 像素 → BMP 约 50 MB）。
 * 超过则只验证"抓屏成功"，不写文件，避免写出 GB 级 BMP。 */
static const int64_t kMaxCapturePixels = 4096LL * 4096LL;

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
    player_.setAudioActive(true);
    applyAudioParams();
    SDL_ResumeAudioStreamDevice(audioStream_);
    PP_INFO("音频输出已就绪: %d Hz / %d ch / S16", spec.freq, spec.channels);
    return true;
}

void App::closeAudio() {
    if (audioStream_) {
        SDL_DestroyAudioStream(audioStream_);
        audioStream_ = nullptr;
    }
    player_.setAudioActive(false);
    player_.setAudioFlushCallback(nullptr);
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

void App::pumpAndUpload() {
    if (!player_.isOpen()) return;
    player_.pumpFrame();

    /* currentFrame() 返回 shared_ptr 副本，插件/seek 并发改动 current_ 也不会悬空 */
    auto frame = player_.currentFrame();
    if (!frame) return;
    if (frame->index == lastUploadedIndex_) return;
    lastUploadedIndex_ = frame->index;

    /* 插件帧处理链（WATERFALL：可原地改写像素 / 中止链 / 丢弃帧） */
    PP_FrameView view{};
    view.data = frame->rgba.data();
    view.width = frame->w;
    view.height = frame->h;
    view.pitch = frame->pitch;
    view.format = PP_PIXFMT_RGBA8888;
    view.pts = frame->pts;
    view.index = frame->index;
    view.serial = frame->serial;

    int chainResult = host_.runFrameChain(&view);
    if (chainResult == PP_CHAIN_DROP) {
        PP_DEBUG("帧 #%lld 被插件丢弃", (long long)frame->index);
        return;
    }

    if (!tex_ || texW_ != frame->w || texH_ != frame->h) {
        if (tex_) SDL_DestroyTexture(tex_);
        tex_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_RGBA32,
                                 SDL_TEXTUREACCESS_STREAMING, frame->w, frame->h);
        if (!tex_) {
            PP_ERROR("创建纹理失败: %s", SDL_GetError());
            return;
        }
        texW_ = frame->w;
        texH_ = frame->h;
    }
    SDL_UpdateTexture(tex_, nullptr, frame->rgba.data(), frame->pitch);
}

/* ======================= 渲染 ======================= */

void App::renderFrame() {
    if (!hidden_) SDL_GetWindowSize(win_, &winW_, &winH_);
    else { winW_ = 960; winH_ = 540; }

    SDL_SetRenderDrawColor(ren_, 16, 16, 20, 255);
    SDL_RenderClear(ren_);

    if (tex_) {
        double srcAspect = (double)texW_ / (double)texH_;
        /* 用窗口比例和视频比例求「含黑边」的显示矩形 */
        double dstAspect = (double)winW_ / (double)winH_;
        float w, h;
        if (srcAspect > dstAspect) {
            w = (float)winW_;
            h = (float)(winW_ / srcAspect);
        } else {
            h = (float)winH_;
            w = (float)(winH_ * srcAspect);
        }
        SDL_FRect dst{(float)((winW_ - w) * 0.5), (float)((winH_ - h) * 0.5), w, h};
        SDL_RenderTexture(ren_, tex_, nullptr, &dst);
    }

    /* OSD 阶段：把绘制权交给插件 */
    Osd& osd = Osd::instance();
    osd.begin(ren_, winW_, winH_);
    host_.emitOsdDraw();
    renderBuiltinOverlay();
    osd.end();
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

    /* 暂停时给个角标，避免"看起来卡住" */
    if (player_.paused()) {
        osd.text(16, 14, "|| PAUSED", PP_RGB(255, 210, 120), 1.5f);
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
    int framesAvail = frames;
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
        while (displayed < frameTarget && (double)SDL_GetTicks() / 1000.0 < deadline) {
            rpc_.pumpMainThread();
            player_.pumpFrame();
            auto frame = player_.currentFrame();
            if (frame && frame->index != lastUploadedIndex_) {
                stall = 0;
                lastUploadedIndex_ = frame->index;

                PP_FrameView view{};
                view.data = frame->rgba.data();
                view.width = frame->w;
                view.height = frame->h;
                view.pitch = frame->pitch;
                view.format = PP_PIXFMT_RGBA8888;
                view.pts = frame->pts;
                view.index = frame->index;
                view.serial = frame->serial;
                int cr = host_.runFrameChain(&view);
                if (cr == PP_CHAIN_DROP) { chainDropped++; continue; }

                displayed++;

                /* 存 3 张帧 PNG 作为可视证据 */
                if (savedPngs < 3 &&
                    (displayed == 1 || displayed == frameTarget / 2 || displayed == frameTarget)) {
                    std::string p = pathJoin(outDir, format("frame_%03d.png", savedPngs + 1));
                    if (writePngRGBA(p, frame->rgba.data(), frame->w, frame->h, frame->pitch)) {
                        savedPngs++;
                        PP_INFO("已保存帧快照: %s (%dx%d, pts=%.3f)",
                                p.c_str(), frame->w, frame->h, frame->pts);
                    }
                }
            } else {
                stall++;
            }
            /* 已到媒体末尾且约 0.5 秒没新帧：不必白等到 deadline */
            if (player_.finished() && stall > 250) break;
            SDL_Delay(2);
        }
        bool hitDeadline = (double)SDL_GetTicks() / 1000.0 >= deadline;
        addStep("decode_and_display", displayed >= frameMin,
                format("显示 %d 帧（目标 %d，可接受下限 %d；媒体约 %d 帧可用）%s，被插件丢弃 %d 帧",
                       displayed, frameTarget, frameMin, framesAvail,
                       hitDeadline ? "（触达时限）" : "", chainDropped));
    }

    /* --- seek --- */
    bool seekOk = false;
    double posAfterSeek = 0.0;
    if (player_.duration() > 2.0) {
        double target = player_.duration() * 0.6;
        player_.seekAbsolute(target);
        int got = 0;
        double dl = (double)SDL_GetTicks() / 1000.0 + 20.0;
        double lastIdx = (double)lastUploadedIndex_;
        if (mi.has_video) {
            while (got < 5 && (double)SDL_GetTicks() / 1000.0 < dl) {
                rpc_.pumpMainThread();
                player_.pumpFrame();
                auto f = player_.currentFrame();
                if (f && f->index != lastUploadedIndex_) { lastUploadedIndex_ = f->index; got++; }
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

    /* --- 调用插件命令（约定：argv 只含实参，不含命令名） --- */
    const char* snapArgv[1] = { outDir.c_str() };
    int cmdsRun = 0;
    std::string cmdDetail;
    if (host_.commands().has("snapshot")) {
        int r = host_.invokeCommand("snapshot", 1, snapArgv);
        cmdsRun++;
        cmdDetail += format("snapshot %s -> %d  ", outDir.c_str(), r);
    }
    if (host_.commands().has("stats")) {
        int r = host_.invokeCommand("stats", 0, nullptr);
        cmdsRun++;
        cmdDetail += format("stats -> %d  ", r);
    }
    if (host_.commands().has("effect.toggle")) {
        const char* a1[1] = {"gray"};
        int r = host_.invokeCommand("effect.toggle", 1, a1);
        cmdsRun++;
        cmdDetail += format("effect.toggle gray -> %d  ", r);
        /* 开一下再关掉，确认链路可逆 */
        host_.invokeCommand("effect.toggle", 1, a1);
    }
    addStep("plugin_commands", cmdsRun > 0,
            cmdsRun ? cmdDetail : "没有任何插件注册命令（异常）");

    /* --- 渲染一次并抓屏（验证渲染管线可用） ---
     * 抓屏产物只是自检证据，不是运行时功能。对超大帧主动跳过落盘，
     * 否则会写出 GB 级 BMP（历史实测 --size 20000 写出过 1.6 GB）。 */
    pumpAndUpload();
    renderFrame();
    SDL_RenderPresent(ren_);
    SDL_Surface* surf = SDL_RenderReadPixels(ren_, nullptr);
    bool captured = (surf != nullptr);
    bool shot = false;
    std::string capDetail;
    if (surf) {
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
    addStep("render_and_capture", captured, capDetail);

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

    /* --- 统计与产出 --- */
    double elapsed = (double)SDL_GetTicks() / 1000.0 - t0;
    PP_PlaybackStats st{};
    player_.fillStats(&st);

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
