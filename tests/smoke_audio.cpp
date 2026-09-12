/* smoke_audio.cpp — 判定「SDL3 音频设备在本机能否正常打开并回调」
 * 目的：把「打开音频设备就崩」这件事的原因定位到环境还是内核代码。
 * 回调只做最保守的事：往流里写静音。
 */
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>

static int g_calls = 0;
static long g_bytes = 0;

static void SDLCALL cb(void* userdata, SDL_AudioStream* stream, int additional, int total) {
    (void)userdata; (void)total;
    g_calls++;
    if (additional <= 0) return;
    if (additional > 1 << 20) additional = 1 << 20;
    static uint8_t zeros[1 << 20];
    memset(zeros, 0, (size_t)additional);
    if (SDL_PutAudioStreamData(stream, zeros, additional)) g_bytes += additional;
}

int main() {
    printf("step1: SDL_Init(VIDEO|AUDIO)\n"); fflush(stdout);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    printf("step2: audio driver = %s\n", SDL_GetCurrentAudioDriver()); fflush(stdout);

    int ndev = 0;
    SDL_AudioDeviceID* devs = SDL_GetAudioPlaybackDevices(&ndev);
    printf("step3: playback devices = %d\n", ndev);
    for (int i = 0; i < ndev; ++i) {
        const char* nm = SDL_GetAudioDeviceName(devs[i]);
        printf("        [%u] %s\n", (unsigned)devs[i], nm ? nm : "(null)");
    }
    if (devs) SDL_free(devs);
    fflush(stdout);

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq = 48000;

    printf("step4: SDL_OpenAudioDeviceStream ...\n"); fflush(stdout);
    SDL_AudioStream* st = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                                   &spec, &cb, nullptr);
    printf("step5: stream = %p  err=%s\n", (void*)st, SDL_GetError()); fflush(stdout);
    if (!st) return 2;

    printf("step6: resume\n"); fflush(stdout);
    SDL_ResumeAudioStreamDevice(st);

    for (int i = 0; i < 60; ++i) {
        SDL_Delay(20);
        if (i % 15 == 0) {
            printf("  t=%dms calls=%d bytes=%ld queued=%d\n",
                   i * 20, g_calls, g_bytes, SDL_GetAudioStreamQueued(st));
            fflush(stdout);
        }
    }

    printf("step7: destroy\n"); fflush(stdout);
    SDL_DestroyAudioStream(st);
    SDL_Quit();
    printf("AUDIO SMOKE OK (calls=%d bytes=%ld)\n", g_calls, g_bytes);
    return 0;
}
