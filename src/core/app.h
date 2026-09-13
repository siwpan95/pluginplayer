#pragma once
/* app.h — 应用层：SDL3 窗口 / 渲染 / 音频输出 / 主循环 / 自检模式 */
#include <string>
#include <vector>
#include <atomic>
#include <cstdint>

#include "host.h"
#include "player.h"
#include "pluginhost.h"
#include "control.h"

namespace pp {

/* 窗口尺寸硬边界：8192 已覆盖 8K(7680x4320)。
 * main.cpp 的 --size 校验与 App::init 的防御性钳位共用这一对常量。 */
constexpr int kMinWindowSize = 160;
constexpr int kMaxWindowSize = 8192;

class App {
public:
    bool init(int winW, int winH, bool hidden, bool audioEnabled);
    void shutdown();

    bool openMedia(const std::string& path);
    void requestQuit() { quit_ = true; }

    /** 正常播放主循环。 */
    void run();

    /** 无人工干预的自检：解码若干帧、执行 seek、调用插件命令、产出 JSON 报告与帧 PNG。 */
    int selfTest(const std::string& media, int frames, const std::string& outDir);

    Host&       host() { return host_; }
    Player&     player() { return player_; }
    PluginHost& plugins() { return plugins_; }

    /** 控制通道（供 host-js/Cordis 等进程外插件接入）；port<=0 表示不启动。
     *  token 为空时由 RpcServer 自动生成（控制通道强制令牌认证）。 */
    bool startRpc(int port, const std::string& token = std::string()) {
        return rpc_.start(&host_, port, token);
    }
    RpcServer& rpc() { return rpc_; }

    SDL_AudioStream* audioStream() const { return audioStream_; }
    const std::string& lastError() const { return lastError_; }

    /** 把 Player 的音量/静音/倍速下发到 SDL 音频流。
     *  插件 API、控制通道、快捷键三条路径统一走这里，避免「状态变了但听不出来」。 */
    void applyAudioParams();

    /** 是否允许打开网络输入（scheme:// 或 UNC）。
     *  默认 false：控制通道/插件能触发 openMedia，若默认放行就等于给了
     *  一个「本机代你出网」的 SSRF 原语。需要时用 --allow-network-input 显式开启。 */
    void setAllowNetworkInput(bool b) { allowNetworkInput_ = b; }
    bool allowNetworkInput() const { return allowNetworkInput_; }

    /* P1-d：多轨与音频设备选择（能力通道；选择由内核 reopen 执行，不进 ABI） */
    /** 切换某类型轨：写入偏好并 reopen 媒体（保持当前播放位置），成功返回 true。
     *  avMediaType 传 AVMediaType 值；streamIndex 为该类型流在 fmt 中的序号。 */
    bool selectTrack(int avMediaType, int streamIndex);
    /** 列出可用音频输出设备（JSON 数组，对应 audio.devices 事件）。 */
    std::string listAudioDevicesJson() const;
    /** 切换音频输出设备：写 audioDeviceId_ 并 reopen 音频流，成功返回 true。 */
    bool setAudioDevice(SDL_AudioDeviceID id);
    SDL_AudioDeviceID audioDeviceId() const { return audioDeviceId_; }

    /* P2-L1：video.color 服务用的色彩元数据 getter（只读快照，最近一次上传帧） */
    uint32_t curVideoColorspace() const { return curVideoCs_; }
    int      curVideoPrimaries()  const { return curPrimaries_; }
    int      curVideoTransfer()   const { return curTrc_; }
    int      curVideoMatrix()     const { return curMatrix_; }
    int      curVideoFullRange()  const { return curFullRange_; }
    bool     curVideoIsHdr()      const { return curIsHdr_; }

private:
    /* 主循环分步 */
    void pollEvents();
    void handleKey(const SDL_Event& e);
    /** P1-b：鼠标事件先给插件（BAIL，插件可吞掉），再走内置的"点画面跳转"。 */
    void handleMouse(const SDL_Event& e);
    /** 取当前帧 → 跑帧处理链 → 上传到纹理。返回**是否真的上传成功**。
     *  force=true 跳过"同一帧不重复上传"的去重，供自检抓屏使用（那时的当前帧
     *  可能已经被别人"看过"、但屏幕上一次都没画过）。调用方必须保证同一帧
     *  只 force 一次，否则帧处理链会被跑两遍。 */
    bool pumpAndUpload(bool force = false);
    /** 构造给插件帧处理链的 RGBA 视图：**按需**把 NV12 物化成 RGBA。
     *  返回 false 表示物化失败（这一帧不能给插件，调用方应照常显示并跳过帧链）。
     *  必须与帧链在同一线程（主线程）调用。 */
    bool makeFrameView(const std::shared_ptr<VideoFrame>& f, PP_FrameView* view);
    /** P2-L1：显示器支持 HDR 时把渲染器重建为 HDR 输出（SRGB_LINEAR 色彩空间），
     *  否则维持 SDR（SDL 自动做 HDR→SDR 色调映射）。只重建一次（hdrOutputConfigured_ 守卫）。 */
    void ensureHdrOutput_();
    void renderFrame();
    void renderBuiltinOverlay();       /* 帮助/提示层（核心自带） */
    void stepAudioLead();
    void tickPlugins();

    /* 音频 */
    bool openAudio();
    void closeAudio();
    /** 预热结束就把被推迟的音频设备补启动。
     *  主循环与自检循环都要调 —— 自检是直接调 player_.pumpFrame() 的，
     *  少了这一步自检里音频设备永远不会启动，音频环也永远排不空。 */
    void stepAudioPreroll();
    static void SDLCALL audioCallback(void* userdata, SDL_AudioStream* stream,
                                      int additional, int total);

    /* P1-b 通用叠加层：插件推位图，内核只保管与合成（功能仍全在插件里）。
     * owner 用于"插件卸载时自动清干净"，与 registry 的撤销口径一致。
     * 这些方法对 Host 公开：宿主要把插件的 overlay API 调用转派到 App。 */
public:
    struct Overlay {
        int          id = 0;
        int          owner = 0;
        SDL_Texture* tex = nullptr;
        int          w = 0, h = 0;
        float        x = 0, y = 0, dw = 0, dh = 0, alpha = 1.0f;
        bool         visible = true;
    };
    int  overlaySet(int owner, int id, const uint8_t* rgba, int w, int h,
                    float x, float y, float dw, float dh, float alpha);
    int  overlayMove(int owner, int id, float x, float y, float dw, float dh, float alpha);
    int  overlayVisible(int owner, int id, bool visible);
    int  overlayRemove(int owner, int id);
    int  overlayClear(int owner);
    int  overlayCount() const { return (int)overlays_.size(); }
    void renderOverlays();             /* 在 OSD 之后、Present 之前合成 */
    void clearAllOverlays();

private:
    /* 工具 */
    void publishEvent(const char* topic, const char* json);
    void invokeOrLog(const char* cmd, int argc, const char** argv);
    std::string statusLine() const;

    SDL_Window*        win_ = nullptr;
    SDL_Renderer*      ren_ = nullptr;
    SDL_Texture*       tex_ = nullptr;
    int                texW_ = 0, texH_ = 0;
    /* 纹理的格式与色彩空间也进重建判据：NV12 纹理的矩阵/范围**只能在创建时**给
     * （SDL 3.4.16 没有 SetTextureColorSpace），换标注就必须重建。 */
    uint32_t           texFmt_ = 0;
    int                texCs_ = -1;
    bool               texCsFellBackWarned_ = false;
    /* PP_NO_NV12=1：强制 RGBA 上传路径（同一二进制上的 A/B 基准开关，见 pumpAndUpload） */
    bool               forceRgba_ = false;
    std::vector<Overlay> overlays_;      /* P1-b 插件叠加层 */
    int                nextOverlayId_ = 1;
    int                winW_ = 1280, winH_ = 720;
    bool               hidden_ = false;
    bool               audioEnabled_ = true;
    /* 音频设备已打开但因"预热"而故意没启动（等第一帧视频上屏，见 Player::prerollPending）。
     * 这类设备必须在主循环里补一次 SDL_ResumeAudioStreamDevice。 */
    bool               audioAwaitPreroll_ = false;
    bool               quit_ = false;
    bool               fullscreen_ = false;
    bool               showHelp_ = false;
    bool               osdHidden_ = false;   /* O 键：整体隐藏 OSD（插件叠加层 + 内置角标） */
    bool               completedFired_ = false;
    bool               allowNetworkInput_ = false;
    /* P2-L1：最近一次上传视频帧的色彩元数据快照（video.color 服务用） */
    uint32_t curVideoCs_ = 0x120005a0u; /* =PP_SDL_CS_SRGB（默认 SDR 输出）*/
    int      curPrimaries_ = 2;
    int      curTrc_ = 2;
    int      curMatrix_ = 0;
    int      curFullRange_ = 0;
    bool     curIsHdr_ = false;
    bool     hdrOutputConfigured_ = false;
    int64_t            lastUploadedIndex_ = -1;
    double             lastTickEmit_ = 0.0;
    /* 最近一次 renderFrame 算出的画面矩形（letterbox 之后的位置与尺寸），
     * 以及"这一轮到底有没有画面"。自检抓屏取证靠它把该是画面的区域圈出来，
     * 否则只能对整窗统计，会被 OSD 的占比淹掉。 */
    float              videoRectX_ = 0.0f, videoRectY_ = 0.0f;
    float              videoRectW_ = 0.0f, videoRectH_ = 0.0f;
    bool               videoRectValid_ = false;
    /* 字幕能力通道（P1-c）：上一帧广播过的 subtitle.line 文本，用于"只在变化时广播"，
     * 避免每帧都发事件。 */
    std::string        lastSubLine_;
    std::string        mediaPath_;
    std::string        lastError_;

    /* 音频输出 */
    SDL_AudioStream*   audioStream_ = nullptr;
    /* P1-d：目标音频输出设备。默认 SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK；
     * setAudioDevice 改写后由 openAudio() 用于打开指定设备。 */
    SDL_AudioDeviceID  audioDeviceId_ = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    std::vector<uint8_t> pullBuf_;

    Host       host_;
    Player     player_;
    PluginHost plugins_;
    RpcServer  rpc_;
};

} // namespace pp
