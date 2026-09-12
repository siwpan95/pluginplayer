#pragma once
/* osd.h — 叠加层绘制（文字 / 面板 / 矩形 / 直线 / 频谱柱）
 *
 * 文字使用 SDL3 内置调试字体（8x8 点阵），通过 SDL_SetRenderScale 做整数放大，
 * 因此不需要外挂字体文件。所有坐标单位为输出像素。
 */
#include <SDL3/SDL.h>
#include "player_plugin.h"

namespace pp {

class Osd {
public:
    static Osd& instance();

    /* 在渲染视频之后、Present 之前调用。video* 是当前画面矩形（letterbox 后的位置与
     * 尺寸；无画面时传整窗，插件据此算出的"黑边"为零、自然退回画面内布局）。 */
    void begin(SDL_Renderer* r, int outW, int outH,
               float videoX, float videoY, float videoW, float videoH);
    void end();

    void  text(float x, float y, const char* s, PP_Color c, float scale);
    float textWidth(const char* s, float scale) const;
    float lineHeight(float scale) const;
    void  panel(float x, float y, float w, float h, PP_Color bg, PP_Color border);
    void  fillRect(float x, float y, float w, float h, PP_Color c);
    void  rect(float x, float y, float w, float h, PP_Color c, float thickness);
    void  line(float x1, float y1, float x2, float y2, PP_Color c, float thickness);
    void  bars(float x, float y, float w, float h, const float* values, int count, PP_Color c);
    bool  size(float* w, float* h) const;
    /** 当前画面矩形（letterbox 后的 dst）；无可用画面时返回 false。 */
    bool  videoRect(float* x, float* y, float* w, float* h) const;

    /** 暴露给插件的 C ABI 函数表（进程内唯一）。 */
    static const PP_OsdApi* cApi();

private:
    Osd() = default;
    SDL_Renderer* r_ = nullptr;
    int outW_ = 0;
    int outH_ = 0;
    float vx_ = 0.0f, vy_ = 0.0f, vw_ = 0.0f, vh_ = 0.0f;
};

} // namespace pp
