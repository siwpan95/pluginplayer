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

/** 一帧已转换好的 RGBA8888 像素。 */
struct VideoFrame {
    std::vector<uint8_t> rgba;
    int      w = 0;
    int      h = 0;
    int      pitch = 0;
    double   pts = 0.0;
    double   duration = 0.0;
    int64_t  index = 0;
    int      serial = 0;
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

    void   fillMediaInfo(PP_MediaInfo* out) const;
    void   fillStats(PP_PlaybackStats* out);

    /* --- 主线程帧泵 --- */
    bool pumpFrame();

    /** 返回当前显示帧的 shared_ptr **副本**：调用方持有期间对象一定存活，
     *  不会因为 seek 时 demux 线程重置 current_ 而变成空指针（曾经的 -O2 段错误根因）。 */
    std::shared_ptr<VideoFrame> currentFrame() const;
    std::shared_ptr<VideoFrame> copyCurrentFrame();
    void writeBackCurrentFrame(const std::shared_ptr<VideoFrame>& f);

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
    /** 归一化到 [-1,1] 的最近音频快照（供频谱插件使用）。 */
    int   snapshotAudio(float* dst, int maxSamples) const;

private:
    void demuxLoop();
    void videoDecodeLoop();
    void audioDecodeLoop();
    bool openVideoCodec(int idx);
    bool openAudioCodec(int idx);
    void flushAfterSeek(double target);
    static int interruptCb(void* opaque);

    /* --- 媒体 --- */
    AVFormatContext* fmt_ = nullptr;
    int              vStream_ = -1;
    int              aStream_ = -1;
    AVCodecContext*  vCtx_ = nullptr;
    AVCodecContext*  aCtx_ = nullptr;
    SwsContext*      sws_ = nullptr;
    SwrContext*      swr_ = nullptr;
    int              swsW_ = 0, swsH_ = 0, swsFmt_ = -1;
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
