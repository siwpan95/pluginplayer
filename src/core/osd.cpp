/* osd.cpp */
#include "osd.h"
#include "util.h"
#include "font.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace pp {

Osd& Osd::instance() {
    static Osd inst;
    return inst;
}

void Osd::begin(SDL_Renderer* r, int outW, int outH,
                float videoX, float videoY, float videoW, float videoH) {
    r_ = r;
    outW_ = outW;
    outH_ = outH;
    vx_ = videoX;
    vy_ = videoY;
    vw_ = videoW;
    vh_ = videoH;
    if (r_) SDL_SetRenderDrawBlendMode(r_, SDL_BLENDMODE_BLEND);
}

void Osd::end() {
    if (r_) {
        SDL_SetRenderScale(r_, 1.0f, 1.0f);
        SDL_SetRenderDrawColor(r_, 0, 0, 0, 255);
    }
    r_ = nullptr;
}

static inline void setColor(SDL_Renderer* r, PP_Color c, Uint8* outA = nullptr) {
    Uint8 a = (Uint8)((c >> 24) & 0xFF);
    SDL_SetRenderDrawColor(r, (Uint8)(c & 0xFF), (Uint8)((c >> 8) & 0xFF),
                           (Uint8)((c >> 16) & 0xFF), a);
    if (outA) *outA = a;
}

/** 调试字体只支持可打印 ASCII，其余统一降级为 '?'，避免出现乱码方块。 */
static std::string sanitize(const char* s) {
    std::string o;
    if (!s) return o;
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
        if (*p == '\t') { o += "    "; continue; }
        if (*p >= 0x20 && *p < 0x7F) o.push_back((char)*p);
        else if (*p == '\n') o.push_back(' ');
        else o.push_back('?');
    }
    return o;
}

/* OSD 的 scale 语义是"调试点阵字(8px)的放大倍数"，插件已经按这个口径在传。
 * 真字体沿用同一口径换算成逻辑像素，这样换字体不用改任何插件调用。 */
static int pxForScale(float scale) {
    if (scale < 0.5f) scale = 0.5f;
    int px = (int)(SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * scale + 0.5f);
    if (px < 6) px = 6;
    return px;
}

void Osd::text(float x, float y, const char* s, PP_Color c, float scale) {
    if (!r_ || !s || !*s) return;
    if (scale < 0.5f) scale = 0.5f;

    Font& f = Font::instance();
    if (f.loaded()) {
        f.drawText(r_, x, y, s, c, pxForScale(scale));
        return;
    }
    /* 回退：没有字体文件（或字体不是 TrueType 轮廓）时仍要有界面，
     * 只是非 ASCII 会被降级成 '?' —— 这是"可读"而不是"可用"，会打 WARN 提醒。 */
    std::string t = sanitize(s);
    if (t.empty()) return;
    setColor(r_, c);
    SDL_SetRenderScale(r_, scale, scale);
    SDL_RenderDebugText(r_, x / scale, y / scale, t.c_str());
    SDL_SetRenderScale(r_, 1.0f, 1.0f);
}

float Osd::textWidth(const char* s, float scale) const {
    if (!s) return 0.0f;
    if (scale < 0.5f) scale = 0.5f;
    const Font& f = Font::instance();
    if (f.loaded()) return f.measure(s, pxForScale(scale));
    /* 回退口径仍按"显示宽度单位"算（全宽记 2），比 strlen 少一点错 */
    return (float)utf8DisplayWidth(s) * (SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE / 2.0f) * scale;
}

float Osd::lineHeight(float scale) const {
    if (scale < 0.5f) scale = 0.5f;
    const Font& f = Font::instance();
    if (f.loaded()) return f.lineHeight(pxForScale(scale));
    return (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * scale;
}

void Osd::fillRect(float x, float y, float w, float h, PP_Color c) {
    if (!r_ || w <= 0 || h <= 0) return;
    setColor(r_, c);
    SDL_FRect fr{x, y, w, h};
    SDL_RenderFillRect(r_, &fr);
}

void Osd::rect(float x, float y, float w, float h, PP_Color c, float thickness) {
    if (!r_) return;
    setColor(r_, c);
    SDL_FRect fr{x, y, w, h};
    if (thickness <= 1.0f) {
        SDL_RenderRect(r_, &fr);
        return;
    }
    SDL_FRect t{x, y, w, thickness};
    SDL_RenderFillRect(r_, &t);
    t = SDL_FRect{x, y + h - thickness, w, thickness};
    SDL_RenderFillRect(r_, &t);
    t = SDL_FRect{x, y, thickness, h};
    SDL_RenderFillRect(r_, &t);
    t = SDL_FRect{x + w - thickness, y, thickness, h};
    SDL_RenderFillRect(r_, &t);
}

void Osd::panel(float x, float y, float w, float h, PP_Color bg, PP_Color border) {
    if (!r_) return;
    SDL_BlendMode old = SDL_BLENDMODE_NONE;
    SDL_GetRenderDrawBlendMode(r_, &old);
    SDL_SetRenderDrawBlendMode(r_, SDL_BLENDMODE_BLEND);
    fillRect(x, y, w, h, bg);
    if ((border >> 24) != 0) rect(x, y, w, h, border, 1.0f);
    SDL_SetRenderDrawBlendMode(r_, old);
}

void Osd::line(float x1, float y1, float x2, float y2, PP_Color c, float thickness) {
    if (!r_) return;
    setColor(r_, c);
    if (thickness <= 1.0f) {
        SDL_RenderLine(r_, x1, y1, x2, y2);
        return;
    }
    /* 粗线：用多段平行线近似，足够用于 OSD。 */
    float dx = x2 - x1, dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.0001f) { SDL_RenderLine(r_, x1, y1, x2, y2); return; }
    float nx = -dy / len, ny = dx / len;
    int half = (int)(thickness * 0.5f);
    for (int i = -half; i <= half; ++i) {
        SDL_RenderLine(r_, x1 + nx * i, y1 + ny * i, x2 + nx * i, y2 + ny * i);
    }
}

void Osd::bars(float x, float y, float w, float h, const float* values,
               int count, PP_Color c) {
    if (!r_ || !values || count <= 0 || w <= 0 || h <= 0) return;
    setColor(r_, c);
    float slot = w / (float)count;
    float bw = slot > 2.0f ? slot - 1.0f : slot;
    for (int i = 0; i < count; ++i) {
        float v = values[i];
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        float bh = h * v;
        if (bh <= 0.0f) continue;
        SDL_FRect fr{x + i * slot, y + (h - bh), bw, bh};
        SDL_RenderFillRect(r_, &fr);
    }
}

bool Osd::size(float* w, float* h) const {
    if (!r_) return false;
    if (w) *w = (float)outW_;
    if (h) *h = (float)outH_;
    return true;
}

bool Osd::videoRect(float* x, float* y, float* w, float* h) const {
    if (!r_ || vw_ <= 0.0f || vh_ <= 0.0f) return false;
    if (x) *x = vx_;
    if (y) *y = vy_;
    if (w) *w = vw_;
    if (h) *h = vh_;
    return true;
}

/* ======================= C ABI 包装 ======================= */

static void c_text(PP_Host*, float x, float y, const char* s, PP_Color c, float sc) {
    Osd::instance().text(x, y, s, c, sc);
}
static float c_textWidth(PP_Host*, const char* s, float sc) {
    return Osd::instance().textWidth(s, sc);
}
static float c_lineHeight(PP_Host*, float sc) {
    return Osd::instance().lineHeight(sc);
}
static void c_panel(PP_Host*, float x, float y, float w, float h, PP_Color bg, PP_Color bd) {
    Osd::instance().panel(x, y, w, h, bg, bd);
}
static void c_fillRect(PP_Host*, float x, float y, float w, float h, PP_Color c) {
    Osd::instance().fillRect(x, y, w, h, c);
}
static void c_rect(PP_Host*, float x, float y, float w, float h, PP_Color c, float t) {
    Osd::instance().rect(x, y, w, h, c, t);
}
static void c_line(PP_Host*, float x1, float y1, float x2, float y2, PP_Color c, float t) {
    Osd::instance().line(x1, y1, x2, y2, c, t);
}
static void c_bars(PP_Host*, float x, float y, float w, float h,
                   const float* v, int n, PP_Color c) {
    Osd::instance().bars(x, y, w, h, v, n, c);
}
static int c_size(PP_Host*, float* w, float* h) {
    return Osd::instance().size(w, h) ? 1 : 0;
}
static int c_videoRect(PP_Host*, float* x, float* y, float* w, float* h) {
    return Osd::instance().videoRect(x, y, w, h) ? 1 : 0;
}

const PP_OsdApi* Osd::cApi() {
    static const PP_OsdApi api = {
        c_text, c_textWidth, c_lineHeight, c_panel,
        c_fillRect, c_rect, c_line, c_bars, c_size, c_videoRect,
    };
    return &api;
}

} // namespace pp
