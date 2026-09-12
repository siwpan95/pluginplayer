/* smoke_link.cpp — 工具链冒烟测试：确认 g++ 能同时链接 SDL3 与 FFmpeg 9.0.1 导入库。
 * 这是构建前的关键去风险步骤：只要能编过、能跑、版本对得上，后续内核代码就有保障。 */
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#include <cstdio>

int main() {
    /* ---- SDL3 ---- */
    int sdlVer = SDL_GetVersion();
    printf("SDL3 version      : %d.%d.%d\n",
           SDL_VERSIONNUM_MAJOR(sdlVer), SDL_VERSIONNUM_MINOR(sdlVer), SDL_VERSIONNUM_MICRO(sdlVer));

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        printf("SDL_Init(VIDEO) failed: %s\n", SDL_GetError());
        return 1;
    }
    printf("SDL video driver  : %s\n", SDL_GetCurrentVideoDriver());
    int numDrivers = SDL_GetNumVideoDrivers();
    printf("SDL video drivers : %d\n", numDrivers);
    SDL_Quit();

    /* ---- FFmpeg ---- */
    printf("libavutil         : %s (%d)\n", av_version_info(), avutil_version());
    printf("libavformat       : %u\n", avformat_version());
    printf("libavcodec        : %u\n", avcodec_version());
    printf("libswscale        : %u\n", swscale_version());
    printf("libswresample     : %u\n", swresample_version());

    const AVCodec* h264 = avcodec_find_decoder(AV_CODEC_ID_H264);
    const AVCodec* vp9  = avcodec_find_decoder(AV_CODEC_ID_VP9);
    const AVCodec* aac  = avcodec_find_decoder(AV_CODEC_ID_AAC);
    printf("decoder h264/vp9/aac: %s / %s / %s\n",
           h264 ? h264->name : "MISSING",
           vp9 ? vp9->name : "MISSING",
           aac ? aac->name : "MISSING");

    if (!h264 || !aac) {
        printf("SMOKE TEST FAILED: essential decoders missing\n");
        return 2;
    }

    printf("SMOKE TEST OK\n");
    return 0;
}
