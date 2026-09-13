/* player.cpp — 播放内核实现 */
#include "player.h"
#include "util.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cctype>
#include <fstream>

extern "C" {
#include <libavutil/time.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

namespace pp {

static const int64_t VIDEO_QUEUE_MAX_FRAMES = 16;
static const int64_t VIDEO_PACKET_MAX_BYTES = 24 * 1024 * 1024;
static const int64_t AUDIO_PACKET_MAX_BYTES = 8 * 1024 * 1024;
/* demux 连续读失败多少次后判定"读不下去了"（每次重试间隔 20ms，100 次 ≈ 2 秒） */
static const int kMaxReadErrors = 100;

/* ======================= PacketQueue ======================= */

void PacketQueue::abort() {
    std::lock_guard<std::mutex> lk(mu_);
    abort_ = true;
    cv_.notify_all();
    cvSpace_.notify_all();
}

void PacketQueue::restart() {
    std::lock_guard<std::mutex> lk(mu_);
    abort_ = false;
    cv_.notify_all();
}

void PacketQueue::flush() {
    std::lock_guard<std::mutex> lk(mu_);
    for (AVPacket* p : q_) av_packet_free(&p);
    q_.clear();
    pktSerials_.clear();
    bytes_ = 0;
    serial_++;
    cvSpace_.notify_all();
}

void PacketQueue::wakeAll() {
    std::lock_guard<std::mutex> lk(mu_);
    cv_.notify_all();
    cvSpace_.notify_all();
}

int PacketQueue::put(AVPacket* pkt) {
    std::unique_lock<std::mutex> lk(mu_);
    if (abort_) {
        lk.unlock();
        av_packet_free(&pkt);
        return -1;
    }
    q_.push_back(pkt);
    pktSerials_.push_back(serial_);
    bytes_ += pkt->size;
    cv_.notify_one();
    return 0;
}

int PacketQueue::get(AVPacket* out, int block, int* serial) {
    std::unique_lock<std::mutex> lk(mu_);
    for (;;) {
        if (abort_) return -1;
        if (!q_.empty()) {
            AVPacket* p = q_.front();
            q_.pop_front();
            int s = pktSerials_.front();
            pktSerials_.pop_front();
            bytes_ -= p->size;
            av_packet_move_ref(out, p);
            av_packet_free(&p);
            if (serial) *serial = s;   /* 该包入队时的序号 */
            cvSpace_.notify_one();
            return 1;
        }
        if (!block) return 0;
        cv_.wait_for(lk, std::chrono::milliseconds(50));
    }
}

int PacketQueue::serial() const { std::lock_guard<std::mutex> lk(mu_); return serial_; }
bool PacketQueue::aborted() const { std::lock_guard<std::mutex> lk(mu_); return abort_; }
size_t PacketQueue::count() const { std::lock_guard<std::mutex> lk(mu_); return q_.size(); }
int64_t PacketQueue::bytes() const { std::lock_guard<std::mutex> lk(mu_); return bytes_; }

/* ======================= AudioRing ======================= */

void AudioRing::init(size_t capacity) {
    std::lock_guard<std::mutex> lk(mu_);
    cap_ = capacity;
    buf_.assign(capacity, 0);
    r_ = w_ = size_ = 0;
    abort_ = false;
}

void AudioRing::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    r_ = w_ = size_ = 0;
    cv_.notify_all();
}

size_t AudioRing::write(const uint8_t* data, size_t n) {
    std::lock_guard<std::mutex> lk(mu_);
    size_t room = cap_ - size_;
    size_t k = std::min(n, room);
    for (size_t i = 0; i < k; ++i) {
        buf_[w_] = data[i];
        w_ = (w_ + 1) % cap_;
    }
    size_ += k;
    if (k) cv_.notify_all();
    return k;
}

size_t AudioRing::read(uint8_t* data, size_t n) {
    std::lock_guard<std::mutex> lk(mu_);
    size_t k = std::min(n, size_);
    for (size_t i = 0; i < k; ++i) {
        data[i] = buf_[r_];
        r_ = (r_ + 1) % cap_;
    }
    size_ -= k;
    if (k) cv_.notify_all();
    return k;
}

size_t AudioRing::avail() const { std::lock_guard<std::mutex> lk(mu_); return size_; }
size_t AudioRing::space() const { std::lock_guard<std::mutex> lk(mu_); return cap_ - size_; }
size_t AudioRing::capacity() const { std::lock_guard<std::mutex> lk(mu_); return cap_; }

void AudioRing::wake() {
    std::lock_guard<std::mutex> lk(mu_);
    cv_.notify_all();
}

bool AudioRing::waitData(int timeoutMs) {
    std::unique_lock<std::mutex> lk(mu_);
    if (size_ > 0 || abort_) return size_ > 0;
    cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs));
    return size_ > 0;
}

void AudioRing::setAbort(bool a) {
    std::lock_guard<std::mutex> lk(mu_);
    abort_ = a;
    cv_.notify_all();
}

/* ======================= Player ======================= */

Player::Player() {}
Player::~Player() { close(); }

int Player::interruptCb(void* opaque) {
    Player* p = (Player*)opaque;
    return (p->abort_.load() || p->seekReq_.load()) ? 1 : 0;
}

bool Player::open(const std::string& path) {
    close();
    path_ = path;
    /* 硬解开关：环境变量与项目里既有惯例一致（PP_WEB_REMOTE / PP_RPC_TOKEN 同款）。
     * 用途是排障与 A/B —— 硬解出问题时能一键确认"是不是硬解的锅"。 */
    {
        const char* v = getenv("PP_NO_HWDEC");
        hwRequested_ = !(v && *v && *v != '0');
    }

    int ret = avformat_open_input(&fmt_, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        char err[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(ret, err, sizeof(err));
        PP_ERROR("avformat_open_input 失败: %s (%s)", path.c_str(), err);
        fmt_ = nullptr;
        return false;
    }
    fmt_->interrupt_callback.callback = &Player::interruptCb;
    fmt_->interrupt_callback.opaque = this;

    /* 慢启动网络流的探测窗口。
     * 不设 analyzeduration 时它是 0 —— avformat 会在拿到**完整帧**之前就放弃：
     * 实测打开"刚开播的电台 /stream"（源还在拉、数据一滴滴来）时报
     *   "Could not find codec parameters for stream 0 (Audio: mp3, 0 channels, fltp):
     *    unspecified frame size"
     * 给 3 秒上限让它等到真正的音频帧。注意这是**上限不是固定等待**：本地文件与
     * 热流参数一齐就立刻返回，不会把"打开"拖慢 3 秒。 */
    /* 注意 FFmpeg 9 已把该字段改名为 max_analyze_duration（旧名 analyzeduration 已移除） */
    fmt_->max_analyze_duration = 3 * AV_TIME_BASE;
    fmt_->probesize = 8 * 1024 * 1024;    /* 默认 5 MB，稍放大以覆盖慢速流的首帧 */

    if (avformat_find_stream_info(fmt_, nullptr) < 0) {
        PP_ERROR("avformat_find_stream_info 失败");
        close();
        return false;
    }

    vStream_ = chooseStream(AVMEDIA_TYPE_VIDEO, prefVideo_);
    aStream_ = chooseStream(AVMEDIA_TYPE_AUDIO, prefAudio_);
    if (vStream_ < 0 && aStream_ < 0) {
        PP_ERROR("未找到可解码的音视频流: %s", path.c_str());
        close();
        return false;
    }

    if (vStream_ >= 0 && !openVideoCodec(vStream_)) { close(); return false; }
    if (aStream_ >= 0 && !openAudioCodec(aStream_)) { close(); return false; }

    /* 字幕流：能开就开，开不了也不致命（只是没字幕）。 */
    sStream_ = chooseStream(AVMEDIA_TYPE_SUBTITLE, prefSub_);
    if (sStream_ >= 0 && !openSubtitleCodec(sStream_)) {
        PP_WARN("字幕解码器打开失败，继续播放（无字幕）");
        sStream_ = -1;
    }

    /* P1-d：构建全部轨列表（供 media.tracks 事件与 player.tracks RPC）。
     * 必须在选流之后调用，selected 标记才正确。 */
    buildTrackList();

    duration_ = (fmt_->duration != AV_NOPTS_VALUE)
                    ? (double)fmt_->duration / AV_TIME_BASE
                    : 0.0;
    bitrate_ = fmt_->bit_rate;

    /* 无音频时，把输出参数设成默认值，避免 0 除 */
    if (aStream_ < 0) {
        outSampleRate_ = 48000;
        outChannels_ = 2;
    }
    bytesPerSec_ = outSampleRate_ * outChannels_ * 2;

    ring_.init((size_t)bytesPerSec_ * 3); /* 3 秒缓冲 */
    ring_.setAbort(false);

    {
        std::lock_guard<std::mutex> lk(clockMu_);
        audioBasePending_ = true;
        audioChunkPts_ = 0.0;
    }
    audioWrittenBytes_ = 0;
    audioConsumedBytes_ = 0;
    audioLeadBytes_ = 0;
    dropBefore_ = 0.0;
    lastAudioClock_ = 0.0;
    lastAudioTick_ = 0.0;
    nextAudioPts_ = 0.0;
    extPts_ = 0.0;
    extLastTick_ = (double)SDL_GetTicks() / 1000.0;
    /* 进入预热：解码器冷启动期间主时钟必须冻在 0，否则开头几帧一解出来就已经"迟到"。
     * 无视频轨时不必预热（没有帧可等）。 */
    prerollTick_ = extLastTick_;
    lastDecodeTickMs_ = (int64_t)SDL_GetTicks();
    prerollPending_ = (vStream_ >= 0);
    paused_ = false;
    eof_ = false;
    abort_ = false;
    seekReq_ = false;
    serial_ = 0;
    frameIndex_ = 0;
    /* 跨媒体状态必须逐个复位。这几个是"上一部片子留下的脏值"，漏掉哪一个都会
     * 让第二次 open 出问题；其中 demuxFinished_/drainSent_ 曾经就是漏的 ——
     * 于是 demux 线程开新片时直接卡在"已完成"分支一个包都不读，
     * 同时 finished() 当场为真把主时钟冻在 0 → 永久黑屏 + 立刻打"播放结束"。 */
    demuxFinished_ = false;
    drainSent_ = false;
    vDrained_ = false;
    aDrained_ = false;
    {
        std::lock_guard<std::mutex> lk(subMu_);
        subCues_.clear();
        subExternal_ = false;
    }
    {
        std::lock_guard<std::mutex> lk(statMu_);
        decodedFrames_ = displayedFrames_ = droppedFrames_ = 0;
        fpsWindowStart_ = 0.0;
        fpsWindowCount_ = 0;
        measuredFps_ = 0.0;
    }
    {
        std::lock_guard<std::mutex> lk(curMu_);
        current_.reset();
    }

    vq_.restart();
    aq_.restart();
    vq_.flush();
    aq_.flush();

    opened_ = true;

    demuxTh_ = std::thread(&Player::demuxLoop, this);
    if (vStream_ >= 0) vdecTh_ = std::thread(&Player::videoDecodeLoop, this);
    if (aStream_ >= 0) adecTh_ = std::thread(&Player::audioDecodeLoop, this);
    if (sStream_ >= 0) sdecTh_ = std::thread(&Player::subtitleDecodeLoop, this);

    PP_INFO("已打开媒体: %s", path.c_str());
    return true;
}

/* ---------------- 硬件解码（D3D11VA） ---------------- */

enum AVPixelFormat Player::hwGetFormatCb(AVCodecContext* ctx,
                                         const enum AVPixelFormat* fmt) {
    Player* p = (Player*)ctx->opaque;
    return p ? p->pickHwFormat(fmt) : fmt[0];
}

enum AVPixelFormat Player::pickHwFormat(const enum AVPixelFormat* fmt) const {
    /* 优先用我们准备好的硬件格式（D3D11）。 */
    for (const enum AVPixelFormat* f = fmt; *f != AV_PIX_FMT_NONE; ++f)
        if (*f == hwPixFmt_) return *f;
    /* 解码器不支持：退回第一个**软件**格式。
     * 注意不能直接用 fmt[0] —— 列表里可能全是硬件格式，
     * 那样解码器会拿到一个它无法输出的格式，avcodec_open2 之后每帧都失败。 */
    for (const enum AVPixelFormat* f = fmt; *f != AV_PIX_FMT_NONE; ++f) {
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(*f);
        if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *f;
    }
    PP_WARN("get_format 没能选出一个可用格式，交给解码器默认（fmt[0]=%d）", (int)fmt[0]);
    return fmt[0];
}

bool Player::openHwDevice() {
    /* 只尝试 D3D11VA：本项目是 Windows 独占，D3D11 是覆盖面最好的一个
     * （Intel/AMD/NVIDIA 都支持，且与 SDL3 的 d3d11 渲染后端同源）。 */
    int r = av_hwdevice_ctx_create(&hwDevCtx_, AV_HWDEVICE_TYPE_D3D11VA,
                                   nullptr, nullptr, 0);
    if (r < 0) {
        char err[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(r, err, sizeof(err));
        PP_INFO("硬件解码不可用（D3D11VA 设备创建失败: %s），改用软解", err);
        hwDevCtx_ = nullptr;
        return false;
    }
    hwPixFmt_ = AV_PIX_FMT_D3D11;
    PP_INFO("已创建 D3D11VA 解码设备，将尝试硬件解码");
    return true;
}

void Player::closeHwDevice() {
    if (hwXferFrame_) { av_frame_free(&hwXferFrame_); hwXferFrame_ = nullptr; }
    if (hwDevCtx_) { av_buffer_unref(&hwDevCtx_); hwDevCtx_ = nullptr; }
    hwPixFmt_ = AV_PIX_FMT_NONE;
    hwActive_ = false;
    hwName_ = "";
}

bool Player::openVideoCodec(int idx) {
    AVStream* st = fmt_->streams[idx];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        PP_ERROR("找不到视频解码器 (codec_id=%d)", (int)st->codecpar->codec_id);
        return false;
    }
    vCtx_ = avcodec_alloc_context3(codec);
    if (!vCtx_) return false;
    if (avcodec_parameters_to_context(vCtx_, st->codecpar) < 0) return false;
    vCtx_->pkt_timebase = st->time_base;

    /* 先试硬解。失败路径必须**无副作用**地退回软解：清掉 get_format/opaque，
     * 让解码器按自己的默认格式走。 */
    if (hwRequested_ && openHwDevice()) {
        vCtx_->opaque = this;
        vCtx_->get_format = &Player::hwGetFormatCb;
        vCtx_->hw_device_ctx = av_buffer_ref(hwDevCtx_);
        if (!vCtx_->hw_device_ctx) {
            PP_WARN("硬件解码上下文引用失败，改用软解");
            vCtx_->get_format = nullptr;
            vCtx_->opaque = nullptr;
            closeHwDevice();
        }
    }

    if (avcodec_open2(vCtx_, codec, nullptr) < 0) {
        PP_ERROR("avcodec_open2(视频) 失败");
        return false;
    }
    /* 解码器没调 get_format 就说明它不支持这套硬件帧 —— 把引用还回去，
     * 免得留一个"看起来在用硬解"的假状态（真用没用上以第一帧的格式为准）。 */
    if (vCtx_->hw_device_ctx && vCtx_->get_format != &Player::hwGetFormatCb) {
        vCtx_->get_format = nullptr;
        vCtx_->opaque = nullptr;
        av_buffer_unref(&vCtx_->hw_device_ctx);
        closeHwDevice();
    }
    srcW_ = vCtx_->width;
    srcH_ = vCtx_->height;
    sar_ = vCtx_->sample_aspect_ratio;
    if (sar_.num <= 0 || sar_.den <= 0) sar_ = AVRational{1, 1};

    AVRational fr = st->avg_frame_rate;
    if (fr.num <= 0 || fr.den <= 0) fr = st->r_frame_rate;
    fps_ = (fr.num > 0 && fr.den > 0) ? av_q2d(fr) : 0.0;
    return true;
}

bool Player::openAudioCodec(int idx) {
    AVStream* st = fmt_->streams[idx];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        PP_ERROR("找不到音频解码器 (codec_id=%d)", (int)st->codecpar->codec_id);
        return false;
    }
    aCtx_ = avcodec_alloc_context3(codec);
    if (!aCtx_) return false;
    if (avcodec_parameters_to_context(aCtx_, st->codecpar) < 0) return false;
    aCtx_->pkt_timebase = st->time_base;
    if (avcodec_open2(aCtx_, codec, nullptr) < 0) {
        PP_ERROR("avcodec_open2(音频) 失败");
        return false;
    }
    outSampleRate_ = aCtx_->sample_rate > 0 ? aCtx_->sample_rate : 48000;
    if (outSampleRate_ < 8000) outSampleRate_ = 8000;
    if (outSampleRate_ > 192000) outSampleRate_ = 192000;
    int ch = aCtx_->ch_layout.nb_channels > 0 ? aCtx_->ch_layout.nb_channels : 2;
    outChannels_ = ch >= 2 ? 2 : 1;
    return true;
}

/* ---------------- 字幕解码（P1-c 能力通道） ----------------
 *
 * 轻路线：只用 FFmpeg 自带的文本字幕解码（AV_CODEC_ID_SUBRIP / ASS），
 * 不引入 libass。解码出 SUBTITLE_TEXT / SUBTITLE_ASS 后只保留纯文本（剥离 ASS
 * 样式标签），时间轴以 pts + start/end_display_time 计算，存进 subCues_。
 * 渲染由 subtitles 插件在 osd.draw 阶段做 —— 内核在这里**只解码、不画**。 */

/** 轻路线：剥离 ASS / 字幕样式标签（花括号包围的 {\...}），只留文本与换行。
 *  顺带折叠 \r、去掉整段首尾空行，把多行字幕压成稳定的纯文本。 */
static std::string stripAssTags(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool inTag = false;
    for (char c : s) {
        if (c == '{') { inTag = true; continue; }
        if (c == '}') { inTag = false; continue; }
        if (!inTag) out.push_back(c);
    }
    std::string cleaned;
    cleaned.reserve(out.size());
    bool prevNl = false;
    for (char c : out) {
        if (c == '\r') continue;
        if (c == '\n') {
            if (!prevNl) cleaned.push_back('\n');
            prevNl = true;
        } else {
            prevNl = false;
            cleaned.push_back(c);
        }
    }
    size_t a = cleaned.find_first_not_of(" \t\n");
    if (a == std::string::npos) return "";
    size_t b = cleaned.find_last_not_of(" \t\n");
    return cleaned.substr(a, b - a + 1);
}

/* ---------------- P1-e：字幕 ASS 事件（供 libass 渲染，不进 ABI） ----------------
 * 内核只解码、不渲染：把字幕包转为 ASS 事件（subtitle.ass_header + subtitle.ass）
 * 经事件总线暴露给 subtitles 插件，由插件用 libass 真正画图。与既有 subtitle.line
 * （纯文本）并存；旧插件只用 subtitle.line 不受影响。 */

/* 秒数 -> ASS 时间戳 H:MM:SS.cc（厘秒）。 */
static std::string buildAssTime(double sec) {
    if (sec < 0) sec = 0;
    long long ms = (long long)(sec * 1000.0 + 0.5);
    long long h = ms / 3600000; ms %= 3600000;
    long long m = ms / 60000;    ms %= 60000;
    long long s = ms / 1000;     ms %= 1000;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld.%02lld", h, m, s, ms / 10);
    return buf;
}

/* 构建一条媒体只发一次的 ASS 头：含 PlayRes 与默认样式。
 * 默认样式字体名 "PluginPlayerSub" 必须与插件注册进 libass 的家族名一致
 * （见 player_plugin.h 的 PP_SUBTITLE_FONT_FAMILY —— 内核这里硬编码镜像，避免引插件 SDK 头）。
 * 字号按 PlayResY 等比（约 1/18），保证不同分辨率下观感一致。 */
static std::string buildAssHeader(int w, int h) {
    if (w <= 0) w = 1280;
    if (h <= 0) h = 720;
    int fs = h / 18; if (fs < 12) fs = 12;
    char buf[1152];
    std::snprintf(buf, sizeof(buf),
        "[Script Info]\n"
        "ScriptType: v4.00+\n"
        "PlayResX: %d\n"
        "PlayResY: %d\n"
        "WrapStyle: 2\n"
        "ScaledBorderAndShadow: yes\n"
        "\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,PluginPlayerSub,%d,&H00FFFFFF,&H000000FF,&H00000000,&H64000000,0,0,0,0,100,100,0,0,1,1.5,1.2,2,20,20,20,1\n"
        "\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n",
        w, h, fs);
    return buf;
}

/* 从 FFmpeg 解出的字幕包（rct->ass 是完整 Dialogue 行，或 rct->text 纯文本）抽取
 * "文本内容"字段（Dialogue 第 9 个逗号之后），保留其中的 ASS 覆盖标签（\an \b \c…）。
 * 非 Dialogue 的纯文本整体当内容，并把 \n 换成 \N（ASS 换行）。 */
static std::string extractAssText(const std::string& raw) {
    if (raw.rfind("Dialogue:", 0) != 0) {
        std::string out; out.reserve(raw.size());
        for (char c : raw) { if (c == '\n') out += "\\N"; else if (c != '\r') out.push_back(c); }
        return out;
    }
    int commas = 0;
    size_t p = 0;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == ',') { if (++commas == 9) { p = i + 1; break; } }
    }
    if (commas < 9) return raw;          /* 字段不足，整体当文本兜底 */
    std::string content = raw.substr(p);
    while (!content.empty() && (content.back() == '\r' || content.back() == '\n')) content.pop_back();
    size_t a = content.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    return content.substr(a);
}

bool Player::openSubtitleCodec(int idx) {
    AVStream* st = fmt_->streams[idx];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        PP_WARN("找不到字幕解码器 (codec_id=%d)", (int)st->codecpar->codec_id);
        return false;
    }
    sCtx_ = avcodec_alloc_context3(codec);
    if (!sCtx_) return false;
    if (avcodec_parameters_to_context(sCtx_, st->codecpar) < 0) {
        avcodec_free_context(&sCtx_);
        return false;
    }
    sCtx_->pkt_timebase = st->time_base;
    if (avcodec_open2(sCtx_, codec, nullptr) < 0) {
        PP_WARN("avcodec_open2(字幕) 失败");
        avcodec_free_context(&sCtx_);
        return false;
    }
    PP_INFO("已打开字幕解码器: %s", codec->name);
    return true;
}

void Player::subtitleDecodeLoop() {
    AVStream* st = fmt_->streams[sStream_];
    AVRational tb = st->time_base;
    AVPacket* pkt = av_packet_alloc();
    AVSubtitle sub{};
    double nextPts = 0.0;
    int lastQSerial = -1;
    bool needHeader = true;          /* P1-e：下一跳有效字幕前先发一次 ASS 头（seek 后重置） */
    while (!abort_) {
        int serial = 0;
        int r = sq_.get(pkt, 1, &serial);
        if (r < 0) break;
        if (r == 0) continue;

        /* 序号变化 = seek/flush：由本线程安全地重置解码器（与音视频解码一致）。 */
        if (serial != lastQSerial) {
            avcodec_flush_buffers(sCtx_);
            lastQSerial = serial;
            needHeader = true;        /* P1-e：换轴/secre 后重发 ASS 头，插件据此重置 libass track */
        }
        const bool isDrain = (pkt->data == nullptr);

        int got = 0;
        if (!isDrain)
            avcodec_decode_subtitle2(sCtx_, &sub, &got, pkt);
        else
            avcodec_decode_subtitle2(sCtx_, &sub, &got, nullptr);  /* drain：逼出剩余字幕 */

        if (got) {
            double base = (sub.pts != AV_NOPTS_VALUE)
                              ? (double)sub.pts * av_q2d(tb)
                              : nextPts;
            for (unsigned i = 0; i < sub.num_rects; ++i) {
                AVSubtitleRect* rct = sub.rects[i];
                std::string raw;
                if (rct->type == SUBTITLE_TEXT && rct->text) raw = rct->text;
                else if (rct->type == SUBTITLE_ASS && rct->ass) raw = rct->ass;
                else continue;
                std::string text = stripAssTags(raw);
                if (text.empty()) continue;
                double s = base + (double)sub.start_display_time / 1000.0;
                double e = base + (double)sub.end_display_time / 1000.0;
                if (e <= s) e = s + 2.0;   /* 防御：没有时长时给 2 秒 */
                /* seek/flush 之后作废在途旧包的字幕，避免把旧时间轴混进新轴。
                 * 外部字幕加载后（subExternal_）忽略内嵌，避免两套字幕打架。 */
                if (serial == sq_.serial()) {
                    std::lock_guard<std::mutex> lk(subMu_);
                    if (!subExternal_) subCues_.push_back({s, e, text});
                    /* P1-e：把带样式的 ASS（header + 每帧 dialogue）转发出去供 libass 渲染。
                     * 与上方 subCues_（纯文本）并存；旧插件只用 subtitle.line 不受影响。 */
                    if (subPublish_) {
                        std::string content = extractAssText(raw);
                        if (!content.empty()) {
                            if (needHeader) {
                                int vW = videoWidth()  > 0 ? videoWidth()  : 1280;
                                int vH = videoHeight() > 0 ? videoHeight() : 720;
                                std::string hp = "{\"header\":\""
                                              + jsonEscape(buildAssHeader(vW, vH)) + "\"}";
                                subPublish_("subtitle.ass_header", hp.c_str());
                                needHeader = false;
                            }
                            std::string line = std::string("Dialogue: 0,")
                                             + buildAssTime(s) + "," + buildAssTime(e)
                                             + ",Default,,0,0,0,," + content;
                            std::string lp = "{\"line\":\"" + jsonEscape(line) + "\"}";
                            subPublish_("subtitle.ass", lp.c_str());
                        }
                    }
                }
            }
            avsubtitle_free(&sub);
            nextPts = base;
        }

        if (!isDrain) av_packet_unref(pkt);
        if (isDrain) break;   /* 排空即结束本线程 */
    }
    av_packet_free(&pkt);
}

bool Player::hasSubtitles() const {
    if (subExternal_) return true;
    std::lock_guard<std::mutex> lk(subMu_);
    return !subCues_.empty();
}

void Player::currentSubtitle(double pts, std::string& out) const {
    out.clear();
    std::lock_guard<std::mutex> lk(subMu_);
    for (const auto& c : subCues_) {
        if (pts >= c.start && pts < c.end) { out = c.text; return; }
    }
}

void Player::loadExternalSubtitles(const std::vector<SubCue>& cues) {
    {
        std::lock_guard<std::mutex> lk(subMu_);
        subCues_ = cues;
        subExternal_ = !cues.empty();
    }
    /* P1-e：外部 .srt 也走 libass 渲染通道。header 与内嵌走同一套样式，
     * 字体名必须与插件注册进 libass 的家族名一致（见 player_plugin.h 的
     * PP_SUBTITLE_FONT_FAMILY，这里硬编码镜像，避免内核引入插件 SDK 头）。 */
        if (subPublish_ && !cues.empty()) {
            int vW = videoWidth()  > 0 ? videoWidth()  : 1280;
            int vH = videoHeight() > 0 ? videoHeight() : 720;
            std::string hdr = "{\"header\":\"" + jsonEscape(buildAssHeader(vW, vH)) + "\"}";
            /* 主题名必须与 player_plugin.h 的 PP_TOPIC_SUBTITLE_ASS_HEADER / _ASS 一致
             * （内核不引插件 SDK 头，这里硬编码镜像）。 */
            subPublish_("subtitle.ass_header", hdr.c_str());
            for (const auto& c : cues) {
                std::string out;
                out.reserve(c.text.size());
                for (char ch : c.text) {        /* 外部 .srt 是纯文本，\n -> \N（ASS 换行） */
                    if (ch == '\n') out += "\\N";
                    else if (ch != '\r') out.push_back(ch);
                }
                if (out.empty()) continue;
                std::string line = std::string("Dialogue: 0,") + buildAssTime(c.start) + ","
                                 + buildAssTime(c.end) + ",Default,,0,0,0,," + out;
                std::string p2 = "{\"line\":\"" + jsonEscape(line) + "\"}";
                subPublish_("subtitle.ass", p2.c_str());
            }
        }
}

static bool parseSrtTime(const std::string& t, double& out) {
    int h = 0, m = 0, s = 0, ms = 0;
    char sep = 0;
    if (std::sscanf(t.c_str(), "%d:%d:%d%c%d", &h, &m, &s, &sep, &ms) >= 5) {
        out = (double)h * 3600.0 + (double)m * 60.0 + (double)s + (double)ms / 1000.0;
        return true;
    }
    return false;
}

std::vector<SubCue> Player::parseSrt(const std::string& path) {
    std::vector<SubCue> cues;
    std::ifstream f(path, std::ios::binary);
    if (!f) return cues;
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    if (content.empty()) return cues;

    /* 统一成行数组（去 \r） */
    std::vector<std::string> lines;
    {
        std::string cur;
        for (char c : content) {
            if (c == '\r') continue;
            if (c == '\n') { lines.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        if (!cur.empty()) lines.push_back(cur);
    }

    size_t i = 0;
    while (i < lines.size()) {
        while (i < lines.size() && lines[i].empty()) i++;
        if (i >= lines.size()) break;
        /* 序号行（纯数字）可选：跳过 */
        size_t idx = i;
        bool allNum = true;
        for (char c : lines[idx]) if (!std::isdigit((unsigned char)c)) { allNum = false; break; }
        if (allNum) idx++;
        if (idx >= lines.size()) break;
        const std::string& tl = lines[idx];
        size_t arrow = tl.find("-->");
        if (arrow == std::string::npos) { i = idx + 1; continue; }
        double start = 0, end = 0;
        if (!parseSrtTime(trim(tl.substr(0, arrow)), start) ||
            !parseSrtTime(trim(tl.substr(arrow + 3)), end)) { i = idx + 1; continue; }
        std::string text;
        size_t j = idx + 1;
        while (j < lines.size() && !lines[j].empty()) {
            if (!text.empty()) text += '\n';
            text += lines[j];
            j++;
        }
        if (!text.empty()) cues.push_back({start, end, text});
        i = j;
    }
    return cues;
}

/* ===========================================================================
 * P1-d：多轨能力通道
 * 选择（setPreferredTrack）→ 由调用方 reopen 媒体；内核不进 ABI。
 * =========================================================================== */

int Player::chooseStream(enum AVMediaType t, int pref) {
    /* pref 合法：在流数量范围内、且类型确实匹配 → 直接用偏好轨。 */
    if (pref >= 0 && fmt_ && pref < (int)fmt_->nb_streams) {
        AVStream* st = fmt_->streams[pref];
        if (st && st->codecpar && st->codecpar->codec_type == t) {
            return pref;
        }
        PP_WARN("P1-d chooseStream: 偏好轨 %d 类型不匹配或越界，退回自动选择",
                pref);
    }
    /* 否则退回 FFmpeg 自动最优流。 */
    return av_find_best_stream(fmt_, t, -1, -1, nullptr, 0);
}

void Player::buildTrackList() {
    tracks_.clear();
    if (!fmt_) return;
    for (unsigned i = 0; i < fmt_->nb_streams; ++i) {
        AVStream* st = fmt_->streams[i];
        if (!st || !st->codecpar) continue;
        const enum AVMediaType mt = st->codecpar->codec_type;
        /* 只收录 视频/音频/字幕 三类；数据/附件等轨对外无意义。 */
        if (mt != AVMEDIA_TYPE_VIDEO && mt != AVMEDIA_TYPE_AUDIO &&
            mt != AVMEDIA_TYPE_SUBTITLE) {
            continue;
        }
        TrackInfo ti;
        ti.index = (int)i;
        ti.type  = (int)mt;
        const AVCodec* c = avcodec_find_decoder(st->codecpar->codec_id);
        ti.codec = c ? c->name : avcodec_get_name(st->codecpar->codec_id);
        /* 语言 / 标题：从流的 metadata 字典取，取不到留空串。 */
        if (st->metadata) {
            AVDictionaryEntry* lang = av_dict_get(st->metadata, "language", nullptr, 0);
            if (lang && lang->value) ti.lang = lang->value;
            AVDictionaryEntry* title = av_dict_get(st->metadata, "title", nullptr, 0);
            if (title && title->value) ti.title = title->value;
        }
        /* selected：当前 open() 实际选中的那条流。 */
        ti.selected = (i == (unsigned)vStream_ && mt == AVMEDIA_TYPE_VIDEO) ||
                      (i == (unsigned)aStream_ && mt == AVMEDIA_TYPE_AUDIO) ||
                      (i == (unsigned)sStream_ && mt == AVMEDIA_TYPE_SUBTITLE);
        tracks_.push_back(std::move(ti));
    }
}

std::string Player::listTracksJson() const {
    /* 手工拼 JSON（不引入第三方库），media.tracks 事件载荷。
     * 形如：{"video":[...],"audio":[...],"subtitle":[...]}。 */
    std::string out = "{\"video\":[";
    bool first = true;
    for (const auto& t : tracks_) {
        if (t.type != AVMEDIA_TYPE_VIDEO) continue;
        if (!first) out += ",";
        first = false;
        char buf[384];
        std::snprintf(buf, sizeof(buf),
            "{\"index\":%d,\"codec\":\"%s\",\"lang\":\"%s\",\"title\":\"%s\",\"selected\":%s}",
            t.index,
            jsonEscape(t.codec).c_str(),
            jsonEscape(t.lang).c_str(),
            jsonEscape(t.title).c_str(),
            t.selected ? "true" : "false");
        out += buf;
    }
    out += "],\"audio\":[";
    first = true;
    for (const auto& t : tracks_) {
        if (t.type != AVMEDIA_TYPE_AUDIO) continue;
        if (!first) out += ",";
        first = false;
        char buf[384];
        std::snprintf(buf, sizeof(buf),
            "{\"index\":%d,\"codec\":\"%s\",\"lang\":\"%s\",\"title\":\"%s\",\"selected\":%s}",
            t.index,
            jsonEscape(t.codec).c_str(),
            jsonEscape(t.lang).c_str(),
            jsonEscape(t.title).c_str(),
            t.selected ? "true" : "false");
        out += buf;
    }
    out += "],\"subtitle\":[";
    first = true;
    for (const auto& t : tracks_) {
        if (t.type != AVMEDIA_TYPE_SUBTITLE) continue;
        if (!first) out += ",";
        first = false;
        char buf[384];
        std::snprintf(buf, sizeof(buf),
            "{\"index\":%d,\"codec\":\"%s\",\"lang\":\"%s\",\"title\":\"%s\",\"selected\":%s}",
            t.index,
            jsonEscape(t.codec).c_str(),
            jsonEscape(t.lang).c_str(),
            jsonEscape(t.title).c_str(),
            t.selected ? "true" : "false");
        out += buf;
    }
    out += "]}";
    return out;
}

bool Player::setPreferredTrack(int avMediaType, int streamIndex) {
    /* 验证：streamIndex 属于该媒体类型且在范围内才写入。 */
    if (!fmt_ || streamIndex < 0 || streamIndex >= (int)fmt_->nb_streams)
        return false;
    AVStream* st = fmt_->streams[streamIndex];
    if (!st || !st->codecpar ||
        st->codecpar->codec_type != (enum AVMediaType)avMediaType) {
        return false;
    }
    switch (avMediaType) {
        case AVMEDIA_TYPE_VIDEO:    prefVideo_ = streamIndex; break;
        case AVMEDIA_TYPE_AUDIO:    prefAudio_ = streamIndex; break;
        case AVMEDIA_TYPE_SUBTITLE: prefSub_   = streamIndex; break;
        default:                    return false;
    }
    return true;
}

void Player::resetTrackPrefs() {
    prefVideo_ = -1;
    prefAudio_ = -1;
    prefSub_   = -1;
}

void Player::close() {
    if (!opened_ && !fmt_) return;
    abort_ = true;
    seekReq_ = false;
    vq_.abort();
    aq_.abort();
    ring_.setAbort(true);
    ring_.wake();
    vfCv_.notify_all();
    /* 原先这里有一对 `{ lock_guard clockMu_; }` 的空临界区 —— 什么都不保护，
     * 只是一次无意义的加解锁。真正需要的同步由下面的线程 join 保证，故删除。 */

    if (demuxTh_.joinable()) demuxTh_.join();
    if (vdecTh_.joinable())  vdecTh_.join();
    if (adecTh_.joinable())  adecTh_.join();
    if (sdecTh_.joinable())   sdecTh_.join();

    vq_.flush();
    aq_.flush();

    {
        std::lock_guard<std::mutex> lk(vfMu_);
        vframes_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(curMu_);
        current_.reset();
    }
    ring_.clear();

    if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }
    if (swsRgba_) { sws_freeContext(swsRgba_); swsRgba_ = nullptr; }
    swsRgbaW_ = swsRgbaH_ = 0;
    swsRgbaCs_ = swsRgbaRange_ = -1;
    if (swr_) { swr_free(&swr_); swr_ = nullptr; }
    if (vCtx_) avcodec_free_context(&vCtx_);
    closeHwDevice();      /* avcodec_free_context 会放开它自己那份 hw_device_ctx 引用 */
    if (aCtx_) avcodec_free_context(&aCtx_);
    if (sCtx_) avcodec_free_context(&sCtx_);
    if (fmt_)  avformat_close_input(&fmt_);

    swsW_ = swsH_ = 0;
    swsFmt_ = -1;
    swsCs_ = swsRange_ = -1;
    vStream_ = aStream_ = sStream_ = -1;
    opened_ = false;
    eof_ = false;
    PP_INFO("已关闭媒体");
}

/* ---------------- demux ---------------- */

void Player::demuxLoop() {
    /* 连续读失败计数：用于给"损坏输入"一个终止条件（见下面的失败分支） */
    int readErrors = 0;
    while (!abort_) {
        if (seekReq_.load()) {
            double target = 0;
            {
                std::lock_guard<std::mutex> lk(seekMu_);
                target = seekTarget_;
                seekReq_ = false;
            }
            flushAfterSeek(target);
            drainSent_ = false;
            demuxFinished_ = false;
            eof_ = false;
            readErrors = 0;

            int64_t ts = (int64_t)(target * AV_TIME_BASE);
            int ret = av_seek_frame(fmt_, -1, ts, AVSEEK_FLAG_BACKWARD);
            if (ret < 0) {
                AVStream* st = (vStream_ >= 0) ? fmt_->streams[vStream_]
                                               : fmt_->streams[aStream_];
                int64_t sts = av_rescale_q(ts, AV_TIME_BASE_Q, st->time_base);
                ret = av_seek_frame(fmt_, -1, sts, AVSEEK_FLAG_BACKWARD);
            }
            PP_INFO("seek -> %.3f s (ret=%d)", target, ret);
            /* 清掉 seek 期间可能残留的 EAGAIN 状态 */
            continue;
        }

        /* EOF 之后停在这里等 seek / abort；不再反复读文件 */
        if (demuxFinished_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        if (vq_.bytes() > VIDEO_PACKET_MAX_BYTES || aq_.bytes() > AUDIO_PACKET_MAX_BYTES) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        /* 每个包都新分配：put() 之后所有权已交给队列，绝不能再复用同一个 AVPacket，
         * 否则 av_read_frame 会覆写解码线程正在读的内存（曾导致 h264 解析出错 + 堆损坏）。 */
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }

        int ret = av_read_frame(fmt_, pkt);
        if (ret < 0) {
            av_packet_free(&pkt);
            if (abort_) break;
            if (ret == AVERROR_EOF) {
                if (!drainSent_) {
                    /* 送空包让解码器把残留帧吐完 */
                    /* av_packet_alloc 失败（OOM）时不能 put(nullptr)：
                     * 解码线程把 data==nullptr 当作"drain 空包"，会造成提前收尾。 */
                    if (vStream_ >= 0) { AVPacket* e = av_packet_alloc(); if (e) vq_.put(e); }
                    if (aStream_ >= 0) { AVPacket* e = av_packet_alloc(); if (e) aq_.put(e); }
                    if (sStream_ >= 0) { AVPacket* e = av_packet_alloc(); if (e) sq_.put(e); }
                    drainSent_ = true;
                    demuxFinished_ = true;
                    eof_ = true;
                    PP_INFO("demux 到达文件末尾");
                }
                continue;
            }
            char err[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_strerror(ret, err, sizeof(err));
            /* 给"永远读不出东西"一个终止条件：损坏文件 / 设备掉线时
             * av_read_frame 会一直返回非 EOF 的错误，旧实现只是 sleep+continue，
             * 文件既不到 EOF 也不结束 —— 对外表现就是"卡死"。
             * 这里连续失败到上限就主动按结束收尾（并让解码器 drain）。 */
            if (++readErrors >= kMaxReadErrors) {
                PP_ERROR("av_read_frame 连续失败 %d 次（%s），放弃读取并按播放结束处理",
                         readErrors, err);
                if (!drainSent_) {
                    /* av_packet_alloc 失败（OOM）时不能 put(nullptr)：
                     * 解码线程把 data==nullptr 当作"drain 空包"，会造成提前收尾。 */
                    if (vStream_ >= 0) { AVPacket* e = av_packet_alloc(); if (e) vq_.put(e); }
                    if (aStream_ >= 0) { AVPacket* e = av_packet_alloc(); if (e) aq_.put(e); }
                    if (sStream_ >= 0) { AVPacket* e = av_packet_alloc(); if (e) sq_.put(e); }
                    drainSent_ = true;
                    demuxFinished_ = true;
                    eof_ = true;
                }
                break;
            }
            if (readErrors == 1 || readErrors % 10 == 0)
                PP_WARN("av_read_frame 出错（第 %d 次）: %s", readErrors, err);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        readErrors = 0;      /* 读到包就清零 */

        if (pkt->stream_index == vStream_) {
            vq_.put(pkt);
        } else if (pkt->stream_index == aStream_) {
            aq_.put(pkt);
        } else if (sStream_ >= 0 && pkt->stream_index == sStream_) {
            sq_.put(pkt);
        } else {
            av_packet_free(&pkt);
        }
    }
}

bool Player::finished() const {
    if (!opened_) return false;
    if (!demuxFinished_.load()) return false;
    /* 解码器必须真的排空。只看"队列此刻为空"是不够的 —— 那会把两种情况误判成
     * "播放结束"：① 刚 open、解码线程还没喂上第一批数据；② 主线程恰好把帧取空。
     * 误判的代价是主时钟当场冻结（position 永远停在 0），并且立刻对外发
     * media.eof / player.completed，于是插件（例如自动续播）会以为片子放完了。 */
    if (vStream_ >= 0 && !vDrained_.load()) return false;
    if (aStream_ >= 0 && !aDrained_.load()) return false;
    if (vq_.count() > 0 || aq_.count() > 0) return false;
    {
        std::lock_guard<std::mutex> lk(vfMu_);
        if (!vframes_.empty()) return false;
    }
    if (aStream_ >= 0 && ring_.avail() > 0) return false;
    return true;
}

void Player::flushAfterSeek(double target) {
    /* 只清队列/缓冲，不碰 AVCodecContext：
     * avcodec_flush_buffers 必须由**拥有该上下文的解码线程**自己调用，
     * 否则会与正在进行的 avcodec_send/receive_frame 并发访问同一个 context
     * （FFmpeg codec context 非线程安全）—— 这是之前 -O2 下偶发段错误的根因。
     * 解码线程通过「包序号变化」检测到 seek 后自行 flush。 */
    vq_.flush();
    aq_.flush();
    {
        std::lock_guard<std::mutex> lk(vfMu_);
        vframes_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(curMu_);
        current_.reset();
    }
    ring_.clear();
    if (onAudioFlush_) onAudioFlush_();

    serial_ = serial_.load() + 1;
    eof_ = false;
    /* seek 之后解码器被 flush，排空状态不再成立，必须重新等待新的 drain 包；
     * demux 线程随后会把 demuxFinished_/drainSent_ 也置回 false（见 demuxLoop）。 */
    vDrained_ = false;
    aDrained_ = false;
    nextAudioPts_ = target;
    dropBefore_ = target > 0.10 ? target - 0.10 : 0.0;
    {
        std::lock_guard<std::mutex> lk(clockMu_);
        audioBasePending_ = true;
        audioChunkPts_ = target;
    }
    audioWrittenBytes_ = 0;
    audioConsumedBytes_ = 0;
    audioLeadBytes_ = 0;
    extPts_ = target;
    extLastTick_ = (double)SDL_GetTicks() / 1000.0;
    /* seek 后进入预热：目标处要重新解码（4K 硬解实测 ~1.2 s 才出第一帧），
     * 这期间主时钟若照走，解出来的帧全部落后时钟一秒多、被 pumpFrame 整批弹掉
     * （实测一次弹掉 24 帧，落点比目标晚 ~2 s）。与 open() 的预热同一机制，
     * 基准点是 seek 目标：时钟冻在 target，第一帧一到（pts≈target−0.1s，已被
     * dropBefore_ 放过）立即上屏，落点就是目标本身。
     * 先置位再回调 —— 宿主在回调里要按 prerollPending_ 决定是否暂停音频设备。 */
    prerollTick_ = extLastTick_;
    lastDecodeTickMs_ = (int64_t)SDL_GetTicks();
    prerollPending_ = (vStream_ >= 0);
    if (onSeek_) onSeek_();
}

/* ---------------- 视频解码 ---------------- */

void Player::videoDecodeLoop() {
    AVStream* st = fmt_->streams[vStream_];
    AVRational tb = st->time_base;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* fr = av_frame_alloc();
    double nextPts = 0.0;
    int lastQSerial = -1;   /* 上一个已处理的包序号，用于检测 seek 并自行 flush */

    while (!abort_) {
        int serial = 0;
        int r = vq_.get(pkt, 1, &serial);
        if (r < 0) break;
        if (r == 0) continue;

        /* 序号变化 = 发生过 seek/flush：由本线程安全地重置解码器 */
        if (serial != lastQSerial) {
            avcodec_flush_buffers(vCtx_);
            lastQSerial = serial;
        }
        /* 把解码器里已就绪的帧全部取出并处理。
         * 返回 false 表示本次包的处理应当中止（abort / seek / 序号变化）。 */
        auto drainDecoder = [&]() -> bool {
            for (;;) {
                int rret = avcodec_receive_frame(vCtx_, fr);
                if (rret == AVERROR(EAGAIN)) break;
                if (rret == AVERROR_EOF) break;
                if (rret < 0) break;

                /* seek/flush 之后，队列序号会递增；在途的旧包解出来的帧一律作废，
                 * 否则会把 seek 前的画面/时间戳混进新时间轴。 */
                if (serial != vq_.serial()) { av_frame_unref(fr); continue; }
                double pts = nextPts;
                if (fr->best_effort_timestamp != AV_NOPTS_VALUE) {
                    pts = (double)fr->best_effort_timestamp * av_q2d(tb);
                }
                double dur = (fr->duration > 0) ? (double)fr->duration * av_q2d(tb)
                                                : (fps_ > 0 ? 1.0 / fps_ : 1.0 / 30.0);
                nextPts = pts + dur;

                /* seek 后丢弃目标点之前的画面，保证跳到用户指定位置 */
                if (pts < dropBefore_.load()) { av_frame_unref(fr); continue; }

                /* 硬件帧先下载到系统内存再往下走。
                 * 这一步是"半零拷贝"：解码在 GPU、后续处理在内存。真正的零拷贝
                 * （把 ID3D11Texture2D 直接包成 SDL 纹理）留到 P0-4 的后续批次 ——
                 * 它要求 FFmpeg 与 SDL 共用同一个 D3D11 设备，改动面更大。 */
                AVFrame* src = fr;
                if (hwPixFmt_ != AV_PIX_FMT_NONE && fr->format == hwPixFmt_) {
                    if (!hwXferFrame_) hwXferFrame_ = av_frame_alloc();
                    if (!hwXferFrame_ || av_hwframe_transfer_data(hwXferFrame_, fr, 0) < 0) {
                        PP_WARN("硬件帧下载失败，丢弃该帧（第 %lld 帧）",
                                (long long)frameIndex_.load());
                        av_frame_unref(fr);
                        continue;
                    }
                    src = hwXferFrame_;
                    if (!hwActive_) {
                        hwActive_ = true;
                        hwName_ = "d3d11va";
                        PP_INFO("硬件解码已生效: d3d11va（帧格式 %s → 下载为 %s）",
                                av_get_pix_fmt_name(hwPixFmt_),
                                av_get_pix_fmt_name((AVPixelFormat)src->format));
                    }
                }

                /* 目标尺寸：按 coded 尺寸做像素格式转换，方向比交给渲染层 */
                int w = src->width > 0 ? src->width : srcW_;
                int h = src->height > 0 ? src->height : srcH_;
                if (w <= 0 || h <= 0) { av_frame_unref(src); av_frame_unref(fr); continue; }

                /* 色彩矩阵与电平范围：必须按标注来，不能靠 swscale 猜。
                 * 取值优先级链：
                 *   ① fr->colorspace / fr->color_range —— 最具体（逐帧，H.265/AV1 会逐帧给）
                 *   ② stream->codecpar->color_space / color_range —— 容器/流级标注
                 * ② 是保险层：解复用器把容器标注（mp4 的 colr box、mkv 的 Colour）写进
                 * codecpar，`avcodec_parameters_to_context()` 再带进解码器上下文，正常情况下
                 * ① 就能拿到（本机用"容器标 bt709、码流无 VUI"的素材实测：日志显示来源=帧）。
                 * 但这条链不是所有编解码器/容器都成立 —— 例如 h264 解码器会用 SPS VUI 覆盖
                 * avctx：colorspace 仅当 colour_description_present_flag 存在时才覆盖
                 * （见 libavcodec/h264_slice.c:1119），而 color_range 只要
                 * video_signal_type_present_flag 存在就覆盖。所以码流若声明了范围、容器另有
                 * 说法，以码流为准；反之码流什么都没说时，才回到容器这一层。
                 * codecpar 归解复用器所有，解码器覆盖不到，是这类情况下唯一的可靠来源。 */
                int csId = (int)fr->colorspace;
                int rgId = (int)fr->color_range;
                const char* csSrc = "帧";
                if (csId == 2 /* AVCOL_SPC_UNSPECIFIED */ ||
                    rgId == 0 /* AVCOL_RANGE_UNSPECIFIED */) {
                    const AVCodecParameters* par = fmt_->streams[vStream_]->codecpar;
                    if (csId == 2) {
                        csId = (int)par->color_space;
                        if (csId != 2) csSrc = "容器";
                    }
                    if (rgId == 0) rgId = (int)par->color_range;
                }
                if (csId == 2) csSrc = "按分辨率推测";
                /* P2-L1：原色（primaries）与传输函数（trc）也要读 —— 这是 HDR 的唯一判据
                 * （BT.2020 原色 + PQ/HLG 传输 = HDR10）。取值优先级同 colorspace：
                 * 先逐帧 fr->*，未标注(2=UNSPECIFIED)回退流级 codecpar->*。 */
                int prId = (int)fr->color_primaries;
                int trcId = (int)fr->color_trc;
                {
                    const AVCodecParameters* par = fmt_->streams[vStream_]->codecpar;
                    if (prId == 2) prId = (int)par->color_primaries;
                    if (trcId == 2) trcId = (int)par->color_trc;
                }
                YuvColorChoice cc = chooseYuvColor(csId, rgId, w, h);

                /* 这两项也进重建条件 —— 素材中途换色彩标注（合流/拼接）时要重建上下文。
                 * 注意用 src->format：硬解帧下载后是 NV12，sws 的输入格式以它为准
                 * （写 fr->format 会拿硬件格式去比较，导致每帧都重建 sws）。 */
                if (!sws_ || swsW_ != w || swsH_ != h || swsFmt_ != src->format ||
                    swsCs_ != cc.matrix || swsRange_ != cc.fullRange) {
                    if (sws_) sws_freeContext(sws_);
                    /* 目标是 NV12（不是 RGBA）：这一步只做像素格式重排、不做 YUV→RGB ——
                     * 矩阵与范围由渲染层的 NV12 纹理交给 GPU 做（见 app.cpp 的纹理创建）。 */
                    sws_ = sws_getContext(w, h, (AVPixelFormat)src->format,
                                          w, h, AV_PIX_FMT_NV12,
                                          SWS_BILINEAR, nullptr, nullptr, nullptr);
                    swsW_ = w; swsH_ = h; swsFmt_ = src->format;
                    swsCs_ = cc.matrix; swsRange_ = cc.fullRange;
                    if (!sws_) {
                        PP_ERROR("sws_getContext 失败 (fmt=%d)", src->format);
                        av_frame_unref(src);
                        av_frame_unref(fr);
                        continue;
                    }
                    /* sws_getContext() 不接受色彩信息，只能在建好之后补上。
                     * 参数依次是：输入系数表 / 输入范围 / 输出系数表 / 输出范围。
                     * 这里是 YUV→YUV 重排，**两端范围必须都是素材自己的范围** ——
                     * 写 1(全范围) 会把有限范围的值错误展开，画面就废了。
                     * 矩阵只影响 YUV→RGB，重排用不上，但一起给保持一致。 */
                    const int* coeffs = sws_getCoefficients(cc.matrix);
                    int cr = sws_setColorspaceDetails(sws_, coeffs, cc.fullRange,
                                                     coeffs, cc.fullRange, 0, 1 << 16, 1 << 16);
                    PP_INFO("色彩: 矩阵=%s 范围=%s（来源=%s，cs=%d range=%d，%dx%d）",
                            cc.matrix == YUV_MATRIX_BT709 ? "BT.709" :
                            cc.matrix == YUV_MATRIX_BT2020 ? "BT.2020" :
                            cc.matrix == YUV_MATRIX_SMPTE240M ? "SMPTE240M" :
                            cc.matrix == YUV_MATRIX_FCC ? "FCC" : "BT.601",
                            cc.fullRange ? "全范围" : "有限范围",
                            csSrc, csId, rgId, w, h);
                    if (cr < 0)
                        PP_WARN("sws_setColorspaceDetails 失败(%d)：将按 swscale 默认矩阵转换", cr);
                }

                /* NV12 缓冲：Y 平面 nvPitch*h，紧随其后是交错的 UV 平面。
                 * 两块连续存放是有意的 —— SDL 的 NV12 上传按
                 * `UVplane = Yplane + rect->h * pitch` 取 UV，与之一致。
                 * pitch 取偶数对齐：SDL 用 (pitch+1)&~1 算 UV 行距，
                 * 奇数宽度会让写入与读取的行距对不上。 */
                const int nvPitch = (w + 1) & ~1;
                auto vf = std::make_shared<VideoFrame>();
                vf->w = w;
                vf->h = h;
                vf->nv12Pitch = nvPitch;
                vf->nv12.resize((size_t)nvPitch * h + (size_t)nvPitch * ((h + 1) / 2));
                vf->colorMatrix = cc.matrix;
                vf->fullRange = cc.fullRange;
                vf->pts = pts;
                vf->duration = dur;
                vf->serial = serial;
                vf->index = frameIndex_.fetch_add(1);

                uint8_t* dst[4] = {vf->nv12.data(),
                                   vf->nv12.data() + (size_t)nvPitch * h,
                                   nullptr, nullptr};
                int dstLs[4] = {nvPitch, nvPitch, 0, 0};
                sws_scale(sws_, src->data, src->linesize, 0, h, dst, dstLs);
                if (src != fr) av_frame_unref(src);   /* 硬件帧下载缓冲，复用不释放 */
                av_frame_unref(fr);

                /* 入队；队列满则等待（seek/abort 时立刻放弃） */
                {
                    std::unique_lock<std::mutex> lk(vfMu_);
                    vfCv_.wait_for(lk, std::chrono::milliseconds(50), [&] {
                        return abort_.load() || seekReq_.load() ||
                               (int64_t)vframes_.size() < VIDEO_QUEUE_MAX_FRAMES;
                    });
                    if (abort_.load() || seekReq_.load()) return false;
                    if (serial != vq_.serial()) return false;   /* flush 作废 */
                    vframes_.push_back(vf);
                }
                /* 刷新"最近出帧"时刻：预热超时兜底据此区分
                 * "解码器在追赶（冻时钟）"与"解码器真卡死（放开时钟）"。 */
                lastDecodeTickMs_ = (int64_t)SDL_GetTicks();
                {
                    std::lock_guard<std::mutex> lk(statMu_);
                    decodedFrames_++;
                }
            }
            return true;
        };

        /* 正确的送包顺序：先抽干已就绪的帧，再送（必要时重发）。
         * 旧实现在 EAGAIN 时直接 av_packet_unref 丢包 —— 那是错的处理方式：
         * EAGAIN 只表示解码器输入缓冲满，必须先 receive 出帧腾出空间，
         * 然后**重发同一个包**。旧写法会丢帧与时间戳（尤其高码率/低帧率素材）。 */
        const bool isDrain = (pkt->data == nullptr);
        if (!drainDecoder()) continue;
        int sret = avcodec_send_packet(vCtx_, isDrain ? nullptr : pkt);
        if (sret == AVERROR(EAGAIN)) {
            if (!drainDecoder()) {
                if (!isDrain) av_packet_unref(pkt);
                continue;
            }
            sret = avcodec_send_packet(vCtx_, isDrain ? nullptr : pkt);
        }
        if (!isDrain) av_packet_unref(pkt);
        if (sret < 0 && sret != AVERROR(EAGAIN) && sret != AVERROR_EOF)
            PP_DEBUG("avcodec_send_packet 返回 %d（本包被丢弃）", sret);
        if (!drainDecoder()) continue;
        /* drain 包处理到这里 = 解码器再没有帧可吐（receive_frame 已返回 EOF）。
         * 这是 finished() 判定"真的放完了"的必要条件之一（见 player.h）。 */
        if (isDrain) vDrained_ = true;
    }
    av_frame_free(&fr);
    av_packet_free(&pkt);
}

/* ---------------- 音频解码 ---------------- */

void Player::audioDecodeLoop() {
    AVStream* st = fmt_->streams[aStream_];
    AVRational tb = st->time_base;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* fr = av_frame_alloc();
    std::vector<uint8_t> outBuf;
    double nextPts = 0.0;
    int lastQSerial = -1;
    /* swr 的输入侧参数（格式/采样率/声道数）会被记下来，变更时重建。
     * 旧实现只判断 `!swr_`，所以素材中途换格式（部分容器/拼接流）会继续用旧
     * 转换上下文，导致音高/速度错误。 */
    int  swrInFmt  = -1;
    int  swrInRate = 0;
    int  swrInCh   = 0;

    while (!abort_) {
        int serial = 0;
        int r = aq_.get(pkt, 1, &serial);
        if (r < 0) break;
        if (r == 0) continue;

        /* 序号变化 = 发生过 seek/flush：由本线程安全地重置解码器与重采样器 */
        if (serial != lastQSerial) {
            avcodec_flush_buffers(aCtx_);
            if (swr_) swr_init(swr_);
            lastQSerial = serial;
        }
        const bool isDrain = (pkt->data == nullptr);
        if (isDrain) {
            avcodec_send_packet(aCtx_, nullptr);
        } else {
            int sret = avcodec_send_packet(aCtx_, pkt);
            av_packet_unref(pkt);
            if (sret < 0 && sret != AVERROR(EAGAIN)) continue;
        }

        for (;;) {
            int rret = avcodec_receive_frame(aCtx_, fr);
            if (rret == AVERROR(EAGAIN)) break;
            if (rret == AVERROR_EOF) {
                /* drain 包处理完 = 解码器再吐不出帧；音频侧排空（见 finished()） */
                if (isDrain) aDrained_ = true;
                break;
            }
            if (rret < 0) break;

            /* 同视频：seek 后作废在途旧包，避免把旧时间戳当成新基准 */
            if (serial != aq_.serial()) { av_frame_unref(fr); continue; }

            double pts = nextPts;
            if (fr->best_effort_timestamp != AV_NOPTS_VALUE) {
                pts = (double)fr->best_effort_timestamp * av_q2d(tb);
            }

            /* seek 后丢弃目标点之前的音频，避免把旧声音灌进环形缓冲 */
            if (pts < dropBefore_.load()) { av_frame_unref(fr); continue; }

            /* 输入布局 */
            AVChannelLayout inLayout{};
            if (fr->ch_layout.nb_channels > 0) {
                av_channel_layout_copy(&inLayout, &fr->ch_layout);
            } else {
                av_channel_layout_default(&inLayout, outChannels_);
            }
            AVChannelLayout outLayout{};
            av_channel_layout_default(&outLayout, outChannels_);

            /* 输入采样率可能是 0（畸形文件 / 部分编码器）。建 swr 时做了保护，
             * 但下面的 swr_get_delay(swr_, 0) 与 av_rescale_rnd(..., 0, ...)
             * 会**除零**。这里统一取一个有效值，三处保持一致。 */
            const int inRate = fr->sample_rate > 0 ? fr->sample_rate : outSampleRate_;
            if (inRate <= 0) { av_frame_unref(fr); continue; }

            /* 输入侧参数变了就重建 swr（不是只在第一次建） */
            if (swr_ && (swrInFmt != (int)fr->format || swrInRate != inRate ||
                         swrInCh != inLayout.nb_channels)) {
                swr_free(&swr_);
            }
            if (!swr_) {
                if (swr_alloc_set_opts2(&swr_, &outLayout, AV_SAMPLE_FMT_S16, outSampleRate_,
                                        &inLayout, (AVSampleFormat)fr->format,
                                        inRate,
                                        0, nullptr) < 0 || swr_init(swr_) < 0) {
                    PP_ERROR("swr 初始化失败");
                    swr_free(&swr_);
                    av_channel_layout_uninit(&inLayout);
                    av_channel_layout_uninit(&outLayout);
                    av_frame_unref(fr);
                    continue;
                }
                swrInFmt  = (int)fr->format;
                swrInRate = inRate;
                swrInCh   = inLayout.nb_channels;
            }

            int maxOut = (int)av_rescale_rnd(swr_get_delay(swr_, inRate) + fr->nb_samples,
                                             outSampleRate_, inRate, AV_ROUND_UP) + 64;
            size_t need = (size_t)maxOut * outChannels_ * 2;
            if (outBuf.size() < need) outBuf.resize(need);

            uint8_t* op = outBuf.data();
            int got = swr_convert(swr_, &op, maxOut,
                                  (const uint8_t* const*)fr->extended_data, fr->nb_samples);
            av_channel_layout_uninit(&inLayout);
            av_channel_layout_uninit(&outLayout);
            av_frame_unref(fr);
            if (got <= 0) continue;

            size_t bytes = (size_t)got * outChannels_ * 2;

            /* 环形缓冲写入（满则等待） */
            size_t off = 0;
            while (off < bytes && !abort_ && !seekReq_.load()) {
                size_t n = ring_.write(outBuf.data() + off, bytes - off);
                if (n == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                off += n;
            }
            if (off < bytes) continue;
            if (serial != aq_.serial()) continue;   /* 写入途中被 flush 作废 */

            double chunkDur = (double)got / (double)outSampleRate_;
            {
                std::lock_guard<std::mutex> lk(clockMu_);
                if (audioBasePending_) {
                    audioChunkPts_ = pts;
                    audioWrittenBytes_ = 0;
                    audioConsumedBytes_ = 0;
                    audioBasePending_ = false;
                }
            }
            audioWrittenBytes_ += (int64_t)bytes;
            nextPts = pts + chunkDur;

            /* 音频快照（频谱插件用） */
            {
                const int16_t* s = (const int16_t*)outBuf.data();
                int n = got * outChannels_;
                std::lock_guard<std::mutex> lk(tapMu_);
                if ((int)audioTap_.size() < 8192) audioTap_.assign(8192, 0.0f);
                for (int i = 0; i < n; i++) {
                    audioTap_[audioTapWrite_] = (float)s[i] / 32768.0f;
                    audioTapWrite_ = (audioTapWrite_ + 1) % (int)audioTap_.size();
                    if (audioTapCount_ < (int)audioTap_.size()) audioTapCount_++;
                }
            }
        }
    }
    av_frame_free(&fr);
    av_packet_free(&pkt);
}

/* ---------------- 时钟 ---------------- */

double Player::audioClockValue() const {
    if (!audioActive_) return 0.0;
    std::lock_guard<std::mutex> lk(clockMu_);
    int64_t consumed = audioConsumedBytes_.load() - audioLeadBytes_.load();
    if (consumed < 0) consumed = 0;
    double t = audioChunkPts_ + (double)consumed / (double)(bytesPerSec_ > 0 ? bytesPerSec_ : 1);
    /* 已写入但还没播完的数据不能算进去 */
    int64_t written = audioWrittenBytes_.load();
    double maxT = audioChunkPts_ + (double)written / (double)(bytesPerSec_ > 0 ? bytesPerSec_ : 1);
    if (t > maxT) t = maxT;
    return t;
}

double Player::videoClockValue() {
    std::lock_guard<std::mutex> lk(curMu_);
    return current_ ? current_->pts : 0.0;
}

double Player::masterClock() {
    double now = (double)SDL_GetTicks() / 1000.0;

    /* 预热期：主时钟冻住不动，等第一帧到齐再从 0 起跑。
     * 只在冷启动用（open()），seek 不走这条路 —— seek 的"快出画"靠的是
     * flushAfterSeek 里的 dropBefore_，语义不同，不能混。 */
    if (prerollPending_.load()) {
        int64_t nowMs = (int64_t)SDL_GetTicks();
        /* 兜底要判"解码器真卡死"而不是"冻得太久"：seek 后从关键帧追赶解码
         * 可能持续数秒（4K 关键帧间隔可达 8 s），期间时钟必须继续冻着。
         * 只有"连续 2 秒连一帧都没产出"才放开（坏码流/解码器崩溃）。 */
        if (nowMs - lastDecodeTickMs_.load() > 2000) {
            PP_WARN("预热超时（2 秒无新帧产出），放开主时钟");
            prerollPending_ = false;
        } else {
            extLastTick_ = now;
            return extPts_;
        }
    }

    /* 播放结束（demux 读完且缓冲排空）后不再推进时钟，避免 position 超过时长 */
    if (finished()) {
        extLastTick_ = now;
        if (duration_ > 0 && extPts_ > duration_) extPts_ = duration_;
        return extPts_;
    }

    if (paused_.load()) {
        extLastTick_ = now;
        if (audioActive_ && audioWrittenBytes_.load() > 0) return audioClockValue();
        return extPts_;
    }
    double sp = speed_.load();
    if (sp <= 0.0) sp = 1.0;

    if (audioActive_ && audioWrittenBytes_.load() > 0) {
        double a = audioClockValue();
        if (a > lastAudioClock_ + 1e-9) {
            lastAudioClock_ = a;
            lastAudioTick_ = now;
        }
        /* 只有音频时钟确实在推进时才拿它当主时钟。
         * 否则（设备已打开但不出声，例如默认设备是未连接蓝牙耳机、
         * 或音频回调被系统挂起）退回墙钟，避免整个播放卡在 0 秒。 */
        if (now - lastAudioTick_ < 0.35) {
            extPts_ = a;            /* 让墙钟跟随音频，切换时无跳变 */
            extLastTick_ = now;
            return a;
        }
    }
    extPts_ += (now - extLastTick_) * sp;
    extLastTick_ = now;
    return extPts_;
}

/* ---------------- 主线程帧泵 ---------------- */

bool Player::pumpFrame() {
    double master = masterClock();
    double nowTick = (double)SDL_GetTicks() / 1000.0;
    std::shared_ptr<VideoFrame> pick;
    int dropped = 0;

    {
        std::lock_guard<std::mutex> lk(vfMu_);
        while (!vframes_.empty() && vframes_.front()->pts <= master + 0.004) {
            if (pick) dropped++;
            pick = vframes_.front();
            vframes_.pop_front();
        }
    }
    vfCv_.notify_one();

    /* 成批丢帧是可诊断事件：一次弹掉多帧只有两种成因 ——
     *   ① 主时钟两次调用之间跳过了整帧间隔（本线程被长时间阻塞）；
     *   ② 解码供不上，帧积在队列里，主时钟追上来把旧的整批挤掉。
     * 记下"距上次 pump 的间隔"与主时钟领先量，两者一比就能区分。 */
    if (dropped > 0) {
        double gapMs = (prevPumpTick_ > 0.0) ? (nowTick - prevPumpTick_) * 1000.0 : -1.0;
        /* 限流 1 条/秒：真跟不上时这段会每帧都命中，不做限流会把日志刷爆。 */
        if (nowTick - lastDropLogTick_ >= 1.0) {
            lastDropLogTick_ = nowTick;
            PP_WARN("丢帧 %d（主时钟 %.3f，本帧 pts %.3f，距上次取帧 %.1f ms，队列深 %d/%d）",
                    dropped, master, pick ? pick->pts : -1.0, gapMs,
                    (int)vframes_.size(), (int)VIDEO_QUEUE_MAX_FRAMES);
        }
    }
    prevPumpTick_ = nowTick;

    if (!pick) {
        /* 没有到点可显示的帧：本轮什么都不做。
         * 原先这里有一段"EOF 判定"死代码：在**锁外**读 vframes_（数据竞争），
         * 算出的 demuxDone 又用 (void) 直接丢弃。真正的结束判定在 finished()。 */
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(statMu_);
        displayedFrames_++;
        droppedFrames_ += dropped;
        fpsWindowCount_++;
        double now = (double)SDL_GetTicks() / 1000.0;
        if (fpsWindowStart_ == 0.0) fpsWindowStart_ = now;
        if (now - fpsWindowStart_ >= 1.0) {
            measuredFps_ = (double)fpsWindowCount_ / (now - fpsWindowStart_);
            fpsWindowCount_ = 0;
            fpsWindowStart_ = now;
        }
    }
    {
        std::lock_guard<std::mutex> lk(curMu_);
        current_ = pick;
    }
    /* 第一帧上屏 = 预热结束，主时钟从这一刻开始走（宿主随即启动音频设备）。 */
    if (prerollPending_.load()) prerollPending_ = false;
    return true;
}

std::shared_ptr<VideoFrame> Player::currentFrame() const {
    std::lock_guard<std::mutex> lk(curMu_);
    return current_;
}

std::shared_ptr<VideoFrame> Player::copyCurrentFrame() {
    std::lock_guard<std::mutex> lk(curMu_);
    if (!current_) return nullptr;
    /* 插件（acquire_frame）拿到的必须是 RGBA：在这里按需物化，
     * 而不是让插件去理解 NV12（PP_FrameView 的契约仍是 RGBA8888）。 */
    materializeRgba(current_);
    auto c = std::make_shared<VideoFrame>(*current_);
    return c;
}

bool Player::materializeRgba(const std::shared_ptr<VideoFrame>& f) {
    if (!f) return false;
    if (f->rgbaReady) return true;
    if (f->w <= 0 || f->h <= 0 || f->nv12.empty()) return false;

    const int pitch = f->w * 4;
    if (f->rgba.size() != (size_t)pitch * (size_t)f->h)
        f->rgba.resize((size_t)pitch * (size_t)f->h);

    if (!swsRgba_ || swsRgbaW_ != f->w || swsRgbaH_ != f->h ||
        swsRgbaCs_ != f->colorMatrix || swsRgbaRange_ != f->fullRange) {
        if (swsRgba_) sws_freeContext(swsRgba_);
        swsRgba_ = sws_getContext(f->w, f->h, AV_PIX_FMT_NV12,
                                  f->w, f->h, AV_PIX_FMT_RGBA,
                                  SWS_BILINEAR, nullptr, nullptr, nullptr);
        swsRgbaW_ = f->w; swsRgbaH_ = f->h;
        swsRgbaCs_ = f->colorMatrix; swsRgbaRange_ = f->fullRange;
        if (!swsRgba_) { PP_ERROR("NV12→RGBA 上下文创建失败"); return false; }
        /* 输出是 RGBA → dstRange 取 1（全范围）。与解码侧同款：
         * inv_table 与 srcRange 必须同时给。 */
        const int* coeffs = sws_getCoefficients(f->colorMatrix);
        sws_setColorspaceDetails(swsRgba_, coeffs, f->fullRange,
                                 coeffs, 1, 0, 1 << 16, 1 << 16);
    }

    const uint8_t* src[4] = {f->nv12.data(),
                             f->nv12.data() + (size_t)f->nv12Pitch * (size_t)f->h,
                             nullptr, nullptr};
    int srcLs[4] = {f->nv12Pitch, f->nv12Pitch, 0, 0};
    uint8_t* dst[4] = {f->rgba.data(), nullptr, nullptr, nullptr};
    int dstLs[4] = {pitch, 0, 0, 0};
    sws_scale(swsRgba_, src, srcLs, 0, f->h, dst, dstLs);
    f->pitch = pitch;
    f->rgbaReady = true;
    return true;
}

void Player::writeBackCurrentFrame(const std::shared_ptr<VideoFrame>& f) {
    if (!f) return;
    std::lock_guard<std::mutex> lk(curMu_);
    current_ = f;
}

/* ---------------- 控制 ---------------- */

void Player::setPaused(bool p) {
    if (!opened_) return;
    paused_ = p;
    if (!p) extLastTick_ = (double)SDL_GetTicks() / 1000.0;
}

void Player::togglePause() { setPaused(!paused_.load()); }

bool Player::seekAbsolute(double seconds) {
    if (!opened_) return false;
    if (seconds < 0) seconds = 0;
    if (duration_ > 0 && seconds > duration_) seconds = duration_ - 0.1;
    {
        std::lock_guard<std::mutex> lk(seekMu_);
        seekTarget_ = seconds;
        seekReq_ = true;
    }
    /* 乐观更新时钟：demux 线程真正执行 seek 需要一点时间，
     * 期间如果时钟还停在旧位置，主线程就会一直等不到 pts 匹配的新帧。 */
    {
        std::lock_guard<std::mutex> lk(clockMu_);
        audioBasePending_ = true;
        audioChunkPts_ = seconds;
    }
    audioWrittenBytes_ = 0;
    audioConsumedBytes_ = 0;
    audioLeadBytes_ = 0;
    lastAudioClock_ = seconds;
    lastAudioTick_ = (double)SDL_GetTicks() / 1000.0;
    extPts_ = seconds;
    extLastTick_ = lastAudioTick_;
    /* 精确 seek：丢掉目标点之前的解码输出（见 dropBefore_ 注释） */
    dropBefore_ = seconds > 0.10 ? seconds - 0.10 : 0.0;
    eof_ = false;

    vq_.wakeAll();
    aq_.wakeAll();
    ring_.wake();
    vfCv_.notify_all();
    return true;
}

bool Player::seekRelative(double delta) {
    return seekAbsolute(position() + delta);
}

void Player::setVolume(float v) {
    volume_ = std::max(0.0f, std::min(1.0f, v));
}

void Player::setMuted(bool m) { muted_ = m; }

bool Player::setSpeed(double s) {
    if (s < 0.1) s = 0.1;
    if (s > 8.0) s = 8.0;
    speed_ = s;
    return true;
}

double Player::position() const {
    if (!opened_) return 0.0;
    return ((Player*)this)->masterClock();
}

void Player::fillMediaInfo(PP_MediaInfo* out) const {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));
    std::snprintf(out->path, sizeof(out->path), "%s", path_.c_str());
    if (fmt_ && fmt_->iformat && fmt_->iformat->name)
        std::snprintf(out->format, sizeof(out->format), "%s", fmt_->iformat->name);
    if (vCtx_ && vCtx_->codec && vCtx_->codec->name)
        std::snprintf(out->video_codec, sizeof(out->video_codec), "%s", vCtx_->codec->name);
    if (aCtx_ && aCtx_->codec && aCtx_->codec->name)
        std::snprintf(out->audio_codec, sizeof(out->audio_codec), "%s", aCtx_->codec->name);
    out->width = srcW_;
    out->height = srcH_;
    out->fps = fps_;
    out->duration = duration_;
    out->bitrate = bitrate_;
    out->audio_channels = aStream_ >= 0 ? outChannels_ : 0;
    out->audio_sample_rate = aStream_ >= 0 ? outSampleRate_ : 0;
    out->has_video = vStream_ >= 0 ? 1 : 0;
    out->has_audio = aStream_ >= 0 ? 1 : 0;
}

void Player::fillStats(PP_PlaybackStats* out) {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));
    out->position = position();
    out->duration = duration_;
    out->master_clock = masterClock();
    out->audio_clock = audioActive_ ? audioClockValue() : 0.0;
    out->video_clock = videoClockValue();
    out->speed = speed_.load();
    out->paused = paused_.load() ? 1 : 0;
    out->eof = eof_.load() ? 1 : 0;
    out->audio_sample_rate = outSampleRate_;
    out->audio_channels = outChannels_;
    out->audio_buffer_bytes = (int64_t)ring_.avail();
    {
        std::lock_guard<std::mutex> lk(statMu_);
        out->decoded_frames = decodedFrames_;
        out->displayed_frames = displayedFrames_;
        out->dropped_frames = (int)droppedFrames_;
        out->current_fps = measuredFps_;
    }
    {
        std::lock_guard<std::mutex> lk(vfMu_);
        out->video_queue_depth = (int64_t)vframes_.size();
    }
}

int Player::snapshotAudio(float* dst, int maxSamples) const {
    std::lock_guard<std::mutex> lk(tapMu_);
    if (audioTap_.empty()) return 0;
    int cap = (int)audioTap_.size();
    int n = std::min(maxSamples, audioTapCount_);
    for (int i = 0; i < n; ++i) {
        int idx = (audioTapWrite_ - n + i + cap) % cap;
        dst[i] = audioTap_[idx];
    }
    return n;
}

} // namespace pp
