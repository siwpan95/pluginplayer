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

private:
    /* 主循环分步 */
    void pollEvents();
    void handleKey(const SDL_Event& e);
    /** 取当前帧 → 跑帧处理链 → 上传到纹理。返回**是否真的上传成功**。
     *  force=true 跳过"同一帧不重复上传"的去重，供自检抓屏使用（那时的当前帧
     *  可能已经被别人"看过"、但屏幕上一次都没画过）。调用方必须保证同一帧
     *  只 force 一次，否则帧处理链会被跑两遍。 */
    bool pumpAndUpload(bool force = false);
    void renderFrame();
    void renderBuiltinOverlay();       /* 帮助/提示层（核心自带） */
    void stepAudioLead();
    void tickPlugins();

    /* 音频 */
    bool openAudio();
    void closeAudio();
    static void SDLCALL audioCallback(void* userdata, SDL_AudioStream* stream,
                                      int additional, int total);

    /* 工具 */
    void publishEvent(const char* topic, const char* json);
    void invokeOrLog(const char* cmd, int argc, const char** argv);
    std::string statusLine() const;

    SDL_Window*        win_ = nullptr;
    SDL_Renderer*      ren_ = nullptr;
    SDL_Texture*       tex_ = nullptr;
    int                texW_ = 0, texH_ = 0;
    int                winW_ = 1280, winH_ = 720;
    bool               hidden_ = false;
    bool               audioEnabled_ = true;
    bool               quit_ = false;
    bool               fullscreen_ = false;
    bool               showHelp_ = false;
    bool               osdHidden_ = false;   /* O 键：整体隐藏 OSD（插件叠加层 + 内置角标） */
    bool               completedFired_ = false;
    bool               allowNetworkInput_ = false;
    int64_t            lastUploadedIndex_ = -1;
    double             lastTickEmit_ = 0.0;
    /* 最近一次 renderFrame 算出的画面矩形（letterbox 之后的位置与尺寸），
     * 以及"这一轮到底有没有画面"。自检抓屏取证靠它把该是画面的区域圈出来，
     * 否则只能对整窗统计，会被 OSD 的占比淹掉。 */
    float              videoRectX_ = 0.0f, videoRectY_ = 0.0f;
    float              videoRectW_ = 0.0f, videoRectH_ = 0.0f;
    bool               videoRectValid_ = false;
    std::string        mediaPath_;
    std::string        lastError_;

    /* 音频输出 */
    SDL_AudioStream*   audioStream_ = nullptr;
    std::vector<uint8_t> pullBuf_;

    Host       host_;
    Player     player_;
    PluginHost plugins_;
    RpcServer  rpc_;
};

} // namespace pp
