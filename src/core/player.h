#pragma once
/* player.h — 播放内核
 *
 * 架构参考 potplayer-master(playerdemo)/src/videoctl.cpp 的 ffplay 派设计：
 *   demux 线程 -> PacketQueue -> 解码线程 -> FrameQueue -> 主线程显示
 * 但按 FFmpeg 9 的新 API 与 SDL3 重写：
 *   - avcodec_send_packet / avcodec_receive_frame 取代已删除的 avcodec_decode_video2
 *   - AVChannelLayout 取代 int64_t channel_layout
 *   - SDL3 的 SDL_AudioStream + 回调取代 SDL2 的 SDL_OpenAudioDevice 回调
 *   - 音频作为主时钟（音频可用时），否则退化为墙钟
 */
#include <string>
#include <memory>
#include <functional>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <vector>
#include <cstdint>

#include <SDL3/SDL.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
}

#include "player_plugin.h"

struct SwsContext;
struct SwrContext;

namespace pp {

/** 一帧已转换好的像素。
 *
 * 解码线程产出 **NV12**（`nv12` + `nv12Pitch`）：渲染层用 NV12 纹理，YUV→RGB 交给 GPU，
 * CPU 不再逐像素转换，上传量也只有 RGBA 的 3/8（4K 每帧 33 MB → 12.4 MB）。
 *
 * `rgba` 是**按需物化**的 —— 插件的帧处理链与 acquire_frame 需要可写 RGBA，
 * 只有真的有人要时才由 Player::materializeRgba() 填上；`rgbaReady` 标记是否已填。 */
struct VideoFrame {
    std::vector<uint8_t> nv12;      /* Y 平面 pitch*h + UV 平面（交错），两块连续 */
    int      nv12Pitch = 0;         /* Y 平面每行字节数（偶数对齐），UV 平面行距相同 */
    std::vector<uint8_t> rgba;      /* 按需物化的 RGBA8888（给插件） */
    bool     rgbaReady = false;
    int      w = 0;
    int      h = 0;
    int      pitch = 0;             /* RGBA 行距；rgbaReady 之后才有效 */
    /* 这一帧的色彩标注（chooseYuvColor 的结果）。渲染层据此在建 NV12 纹理时
     * 把矩阵/范围告诉 SDL —— GPU 才知道该用哪套系数做 YUV→RGB。 */
    int      colorMatrix = 0;
    int      fullRange = 0;
    /* P2-L1：原色 / 传输函数（AVCOL_PRI_* / AVCOL_TRC_*，默认 UNSPECIFIED=2）。
     * 这两项 + 上面两项共同决定纹理该标哪个 SDL_Colorspace（HDR 判定全靠它俩）。 */
    int      colorPrimaries = 2;
    int      colorTrc = 2;
    double   pts = 0.0;
    double   duration = 0.0;
    int64_t  index = 0;
    int      serial = 0;
};

/* 字幕 cue（P1-c 能力通道的载体）：一段应显示的文本及其起止时间（秒）。
 * 内核解出/加载后只保留纯文本（轻路线剥离 ASS 样式标签，只留文本与换行），
 * 渲染交给 subtitles 插件——内核不做渲染（架构红线）。 */
struct SubCue {
    double    start = 0.0;   /* 应开始显示的 pts（秒） */
    double    end   = 0.0;   /* 应结束显示的 pts（秒） */
    std::string text;        /* 这一行的字幕文本（可能含 '\n' 表示多行） */
};

/* ------------------------- 数据包队列 ------------------------- */
class PacketQueue {
public:
    void flush();
    void abort();
    void restart();
    int  put(AVPacket* pkt);                     /* 接管所有权 */
    /** block!=0 时阻塞等待。返回 1 取到，0 无数据，-1 已中断。
     *  *serial 返回**该包入队时**的序号；调用方据此判断包是否已被 flush 作废。 */
    int  get(AVPacket* out, int block, int* serial);
    void wakeAll();

    int      serial() const;
    bool     aborted() const;
    size_t   count() const;
    int64_t  bytes() const;

private:
    mutable std::mutex         mu_;
    std::condition_variable    cv_;
    std::condition_variable    cvSpace_;
    std::deque<AVPacket*>      q_;
    std::deque<int>            pktSerials_;
    int64_t                    bytes_ = 0;
    bool                       abort_ = true;
    int                        serial_ = 0;
};

/* ------------------------- 音频环形缓冲 ------------------------- */
class AudioRing {
public:
    void   init(size_t capacity);
    void   clear();
    size_t write(const uint8_t* data, size_t n);
    size_t read(uint8_t* data, size_t n);
    size_t avail() const;
    size_t space() const;
    size_t capacity() const;
    void   wake();
    bool   waitData(int timeoutMs);
    void   setAbort(bool a);

private:
    mutable std::mutex      mu_;
    std::condition_variable cv_;
    std::vector<uint8_t>    buf_;
    size_t                  r_ = 0, w_ = 0, size_ = 0, cap_ = 0;
    bool                    abort_ = false;
};

/* ------------------------- 播放器 ------------------------- */
class Player {
public:
    Player();
    ~Player();

    bool open(const std::string& path);
    void close();
    bool isOpen() const { return opened_; }
    const std::string& path() const { return path_; }

    /* --- 控制（线程安全） --- */
    void setPaused(bool p);
    void togglePause();
    bool paused() const { return paused_.load(); }
    bool seekAbsolute(double seconds);
    bool seekRelative(double delta);
    void setVolume(float v01);
    float volume() const { return volume_.load(); }
    void setMuted(bool m);
    bool muted() const { return muted_.load(); }
    bool setSpeed(double speed);
    double speed() const { return speed_.load(); }

    /* --- 状态 --- */
    double position() const;
    double duration() const { return duration_; }
    double masterClock();
    double audioClockValue() const;
    double videoClockValue();
    bool   eof() const { return eof_.load(); }
    /** 播放结束：demux 读完 **且解码器已排空**（收到 drain 包后再吐不出帧），
     *  并且包队列、帧队列与音频缓冲都已排空。
     *  四个条件缺一不可 —— 少了"解码器排空"就会在刚 open 时误判为已结束。 */
    bool   finished() const;
    bool   hasAudio() const { return aStream_ >= 0; }
    bool   hasVideo() const { return vStream_ >= 0; }
    /** 当前选中的流序号（供自检/P1-d 切轨验证；不涉及 ABI）。 */
    int    audioTrackIndex() const { return aStream_; }
    int    videoTrackIndex() const { return vStream_; }
    int    subtitleTrackIndex() const { return sStream_; }
    int64_t decodedFrames() const { return decodedFrames_; }
    /** 实际生效的硬件解码名（"d3d11va"），纯软解时返回空串。 */
    const char* hwdecName() const { return hwActive_ ? hwName_ : ""; }

    /** 预热态：open() 之后、第一帧真正上屏之前为 true。
     *
     *  为什么需要它：解码器（尤其 D3D11VA 硬件解码）冷启动要 ~0.2 s 才吐得出第一帧，
     *  而主时钟原本在 open() 里就开始走 —— 于是解出的头几帧一进队列就已经
     *  "迟到"（pts 落后于主时钟），被 pumpFrame 整批弹掉算作丢帧。
     *  实测：720p30 素材固定丢 6 帧，且**与素材长短无关**（3 s 丢 6 / 10 s 也丢 6），
     *  与有无音轨也无关（6 / 6）—— 说明是固定启动成本，不是解码跟不上。
     *  预热期间冻结主时钟，并让宿主推迟启动音频设备，播放开始时 A/V 一起从 0 起跑。 */
    bool   prerollPending() const { return prerollPending_.load(); }

    void   fillMediaInfo(PP_MediaInfo* out) const;
    void   fillStats(PP_PlaybackStats* out);

    /* --- 主线程帧泵 --- */
    bool pumpFrame();

    /** 返回当前显示帧的 shared_ptr **副本**：调用方持有期间对象一定存活，
     *  不会因为 seek 时 demux 线程重置 current_ 而变成空指针（曾经的 -O2 段错误根因）。 */
    std::shared_ptr<VideoFrame> currentFrame() const;
    std::shared_ptr<VideoFrame> copyCurrentFrame();
    void writeBackCurrentFrame(const std::shared_ptr<VideoFrame>& f);

    /** 把 NV12 帧物化成 RGBA8888（供插件的帧处理链与 acquire_frame 使用）。
     *  已物化则直接返回 true。**必须在主线程调用**（与帧链同一线程）。 */
    bool materializeRgba(const std::shared_ptr<VideoFrame>& f);

    int  videoWidth() const { return srcW_; }
    int  videoHeight() const { return srcH_; }
    double sourceFps() const { return fps_; }

    /* --- 音频接入（宿主初始化 SDL 音频流用） --- */
    int   audioSampleRate() const { return outSampleRate_; }
    int   audioChannels() const { return outChannels_; }
    AudioRing& audioRing() { return ring_; }
    void  setAudioActive(bool a) { audioActive_ = a; }
    bool  audioActive() const { return audioActive_; }
    /** 补偿 SDL 音频流内部尚未播放的字节数，使音频时钟更贴近真实播放位置。 */
    void  setAudioLeadBytes(int64_t n) { audioLeadBytes_ = n; }
    /** 音频回调每从环形缓冲取走 n 字节就调用一次，用于推进音频时钟。 */
    void  addConsumedAudioBytes(int64_t n) { audioConsumedBytes_ += n; }
    void  setAudioFlushCallback(std::function<void()> cb) { onAudioFlush_ = std::move(cb); }
    /** seek 完成后回调（此时 prerollPending_ 已置位）：
     *  宿主用它把音频设备暂停、等第一帧上屏再恢复（A/V 对齐，同 open 预热）。 */
    void  setSeekCallback(std::function<void()> cb) { onSeek_ = std::move(cb); }
    /** 归一化到 [-1,1] 的最近音频快照（供频谱插件使用）。 */
    int   snapshotAudio(float* dst, int maxSamples) const;

    /* --- 字幕（P1-c，能力通道；渲染由插件做，内核不渲染） ---
     * 走事件总线暴露当前字幕行（subtitle.line），不进 ABI。 */
    bool  hasSubtitles() const;
    /** 取 pts 时刻应显示的字幕文本（无则返回空串）。主线程调用。 */
    void  currentSubtitle(double pts, std::string& out) const;
    /** 加载外部 .srt（覆盖内嵌字幕轨）。 */
    void  loadExternalSubtitles(const std::vector<SubCue>& cues);
    static std::vector<SubCue> parseSrt(const std::string& path);

    /* P1-e：把字幕 ASS 事件（header / 每帧 dialogue）转发出去，避免给 ABI 加方法。
     * App 构造时塞入 publish 闭包（host.bus().publish）；为空则不发（旧行为）。 */
    void  setSubtitlePublisher(std::function<void(const char*, const char*)> fn)
                { subPublish_ = std::move(fn); }

    /* --- 多轨（P1-d，能力通道；选择由内核 reopen 执行，渲染/UI 由插件做） --- */
    struct TrackInfo {
        int         index = -1;
        int         type = 0;        /* AVMediaType */
        std::string codec;
        std::string lang;
        std::string title;
        bool        selected = false;
    };
    /** 当前媒体的全部 视频/音频/字幕 轨（JSON 数组，对应 media.tracks 事件）。主线程调用。 */
    std::string listTracksJson() const;
    /** 设定偏好轨：验证 idx 属于该媒体类型后写入，返回 false 表示非法（不切换）。 */
    bool setPreferredTrack(int avMediaType, int streamIndex);
    /** 清除偏好轨（下次 open 退回自动选最佳）。 */
    void resetTrackPrefs();

private:
    void demuxLoop();
    void videoDecodeLoop();
    void audioDecodeLoop();
    bool openVideoCodec(int idx);
    bool openAudioCodec(int idx);
    bool openSubtitleCodec(int idx);
    void subtitleDecodeLoop();
    /** 按偏好选流：pref 合法（类型匹配且越界内）就用 pref，否则退回 av_find_best_stream。 */
    int  chooseStream(enum AVMediaType t, int pref);
    /** 构建 tracks_（当前媒体全部 视频/音频/字幕 轨）。 */
    void buildTrackList();
    void flushAfterSeek(double target);
    static int interruptCb(void* opaque);

    /* --- 硬件解码（D3D11VA）---
     * 只在 openVideoCodec 里尝试；任何一步失败都不报错，只打日志并退回软解 ——
     * "能硬解就硬解、不能就软解"必须是自动的，不能让用户去配。 */
    static enum AVPixelFormat hwGetFormatCb(AVCodecContext* ctx,
                                           const enum AVPixelFormat* fmt);
    enum AVPixelFormat pickHwFormat(const enum AVPixelFormat* fmt) const;
    bool openHwDevice();
    void closeHwDevice();
    bool  hwRequested_ = true;                 /* PP_NO_HWDEC=1 时置 false */
    bool  hwActive_ = false;                   /* 真的解出了硬件帧才置 true */
    const char* hwName_ = "";
    AVBufferRef*   hwDevCtx_ = nullptr;        /* AVHWDeviceContext(D3D11VA) */
    AVPixelFormat  hwPixFmt_ = AV_PIX_FMT_NONE;/* 硬解帧的格式（AV_PIX_FMT_D3D11） */
    AVFrame*       hwXferFrame_ = nullptr;     /* av_hwframe_transfer_data 的落地帧（复用） */

    /* --- 媒体 --- */
    AVFormatContext* fmt_ = nullptr;
    int              vStream_ = -1;
    int              aStream_ = -1;
    /* --- 字幕（P1-c 能力通道）---
     * 内嵌字幕流交给 FFmpeg 解码（AV_CODEC_ID_SUBRIP / ASS），解出纯文本后存进 subCues_；
     * 外部 .srt 经 subtitle.load 命令 → core 加载后也并入 subCues_。渲染由 subtitles 插件做，
     * 内核只通过 subtitle.line 事件暴露当前行（不进 ABI）。 */
    int              sStream_ = -1;
    AVCodecContext*  sCtx_ = nullptr;
    PacketQueue      sq_;
    std::thread      sdecTh_;
    /* P1-d 轨道偏好：selectTrack 写入，open() 据此选流；resetTrackPrefs 清空（退回自动）。
     * 注意 open() 不重置它们——否则 selectTrack 触发的 reopen 会把刚写好的偏好冲掉。 */
    std::vector<TrackInfo> tracks_;   /* 当前媒体全部轨（open 时构建，供 media.tracks 事件） */
    int              prefVideo_ = -1, prefAudio_ = -1, prefSub_ = -1;
    std::vector<SubCue> subCues_;        /* 当前活跃字幕（内嵌累计 + 外部覆盖） */
    mutable std::mutex  subMu_;
    /* P1-e：字幕 ASS 事件转发闭包（App 注入）。持有后由内核在解出字幕时调用，
     * 把 header / 每帧 dialogue 以事件总线暴露给 subtitles 插件（libass 渲染）。 */
    std::function<void(const char*, const char*)> subPublish_;
    bool             subExternal_ = false;
    AVCodecContext*  vCtx_ = nullptr;
    AVCodecContext*  aCtx_ = nullptr;
    SwsContext*      sws_ = nullptr;
    /* NV12 → RGBA 的第二个转换上下文：只在**有人要 RGBA** 时才用
     * （插件帧链 / acquire_frame），不进常规路径。 */
    SwsContext*      swsRgba_ = nullptr;
    int              swsRgbaW_ = 0, swsRgbaH_ = 0;
    int              swsRgbaCs_ = -1, swsRgbaRange_ = -1;
    SwrContext*      swr_ = nullptr;
    int              swsW_ = 0, swsH_ = 0, swsFmt_ = -1;
    /* 当前 swscale 上下文所用的色彩矩阵与范围（enum YuvMatrix / 0|1）。
     * 它们是**重建判据的一部分**：帧上的色彩标注变了就必须重建上下文，
     * 否则会拿旧矩阵去还原新画面（合流、拼接、跨片段切轨时会发生）。 */
    int              swsCs_ = -1, swsRange_ = -1;
    std::string      path_;
    bool             opened_ = false;

    int              srcW_ = 0, srcH_ = 0;
    AVRational       sar_{1, 1};
    double           duration_ = 0.0;
    double           fps_ = 0.0;
    int64_t          bitrate_ = 0;
    int              outSampleRate_ = 48000;
    int              outChannels_ = 2;
    double           nextAudioPts_ = 0.0;

    /* --- 队列与线程 --- */
    PacketQueue  vq_, aq_;
    AudioRing    ring_;
    std::deque<std::shared_ptr<VideoFrame>> vframes_;
    mutable std::mutex        vfMu_;
    std::condition_variable   vfCv_;

    std::thread demuxTh_, vdecTh_, adecTh_;
    std::atomic<bool> abort_{false};
    /** demux 线程已读到文件末尾并把 drain 空包投进队列。**注意它只说明"读完了"**，
     *  不代表解码线程已经把帧吐干净 —— 判断播放结束必须再等 *_Drained_。 */
    std::atomic<bool> demuxFinished_{false};
    bool              drainSent_ = false;
    /** 解码线程已收到 drain 空包且解码器再也吐不出帧（receive_frame 返回 EOF）。
     *  与 demuxFinished_ 一起才能构成"播放真的结束了"。
     *  只在两处置真：处理完 drain 包之后；只在两处置假：open() 与 flushAfterSeek()。 */
    std::atomic<bool> vDrained_{false};
    std::atomic<bool> aDrained_{false};

    /* --- seek --- */
    mutable std::mutex seekMu_;
    std::atomic<bool>  seekReq_{false};
    double             seekTarget_ = 0.0;
    std::atomic<int>   serial_{0};

    /* --- 时钟 --- */
    mutable std::mutex clockMu_;
    double   audioChunkPts_ = 0.0;    /* 音频流「第 0 字节」对应的 pts */
    bool     audioBasePending_ = true;
    std::atomic<int64_t> audioWrittenBytes_{0};
    std::atomic<int64_t> audioConsumedBytes_{0};
    std::atomic<int64_t> audioLeadBytes_{0};
    double   extPts_ = 0.0;
    double   extLastTick_ = 0.0;
    /* 预热：第一帧上屏前冻结主时钟（见 prerollPending()）。 */
    std::atomic<bool> prerollPending_{false};
    double   prerollTick_ = 0.0;
    /* 最近一次解码出帧的时刻（毫秒）。预热超时兜底用它判"真卡死"：
     * 时钟冻住 ≠ 卡死 —— seek 后从关键帧追赶解码可能持续数秒（4K x264 关键帧
     * 间隔可长达 8 s），期间必须继续冻结。只有"2 秒连一帧都没产出"才放开。 */
    std::atomic<int64_t> lastDecodeTickMs_{0};
    double   prevPumpTick_ = 0.0;   /* 上次 pumpFrame 的时刻，用于诊断成批丢帧 */
    double   lastDropLogTick_ = 0.0;/* 丢帧日志限流（1 条/秒） */
    int      bytesPerSec_ = 0;
    /* 音频时钟停滞检测：设备打开但不出声（例如未连接蓝牙耳机）时退回墙钟 */
    double   lastAudioClock_ = 0.0;
    double   lastAudioTick_ = 0.0;

    /* --- 控制量 --- */
    std::atomic<bool>   paused_{false};
    std::atomic<float>  volume_{1.0f};
    std::atomic<bool>   muted_{false};
    std::atomic<double> speed_{1.0};
    std::atomic<bool>   eof_{false};
    std::atomic<bool>   audioActive_{false};
    /** seek 精确对齐：解码输出早于该时刻的一律丢弃。
     *  关键帧稀疏的媒体上 av_seek_frame 只能回到更早的关键帧，
     *  这一步把画面/声音真正推进到用户指定的时间点（mpv --hr-seek 的做法）。 */
    std::atomic<double> dropBefore_{0.0};
    std::function<void()> onAudioFlush_;
    std::function<void()> onSeek_;

    /* --- 统计 --- */
    mutable std::mutex statMu_;
    int64_t  decodedFrames_ = 0;
    int64_t  displayedFrames_ = 0;
    int64_t  droppedFrames_ = 0;
    double   fpsWindowStart_ = 0.0;
    int64_t  fpsWindowCount_ = 0;
    double   measuredFps_ = 0.0;
    std::atomic<int64_t> frameIndex_{0};

    /* --- 当前帧 --- */
    std::shared_ptr<VideoFrame> current_;
    mutable std::mutex          curMu_;

    /* --- 音频快照（频谱用） --- */
    mutable std::mutex  tapMu_;
    std::vector<float>  audioTap_;
    int                 audioTapWrite_ = 0;
    int                 audioTapCount_ = 0;
};

} // namespace pp
