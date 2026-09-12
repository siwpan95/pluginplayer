/* font.cpp — 真字体渲染（P1-a），stb_truetype 实现 */
#include "font.h"
#include "util.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>

namespace pp {

Font& Font::instance() {
    static Font inst;
    return inst;
}

void Font::shutdown() {
    for (Atlas* a : atlases_) {
        if (a->tex) SDL_DestroyTexture(a->tex);
        delete a;
    }
    atlases_.clear();
    lastUse_.clear();
    fontData_.clear();
    delete (stbtt_fontinfo*)info_;
    info_ = nullptr;
    loaded_ = false;
    path_.clear();
}

bool Font::load(const std::string& ttfPath) {
    shutdown();
    if (ttfPath.empty()) return false;

    FILE* f = nullptr;
#ifdef _WIN32
    fopen_s(&f, ttfPath.c_str(), "rb");
#else
    f = fopen(ttfPath.c_str(), "rb");
#endif
    if (!f) {
        PP_WARN("字体打不开: %s", ttfPath.c_str());
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); PP_WARN("字体文件为空: %s", ttfPath.c_str()); return false; }
    fontData_.resize((size_t)sz);
    size_t got = fread(fontData_.data(), 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { fontData_.clear(); PP_WARN("字体读取不完整: %s", ttfPath.c_str()); return false; }

    /* TTC 字体集合要取第 0 个字体的偏移 —— 本项目的字体是单字体 TTF，但留着通用。 */
    int off = stbtt_GetFontOffsetForIndex(fontData_.data(), 0);
    stbtt_fontinfo* fi = new stbtt_fontinfo();
    if (!stbtt_InitFont(fi, fontData_.data(), off)) {
        delete fi;
        fontData_.clear();
        PP_WARN("字体解析失败（可能不是 TrueType 'glyf' 轮廓，OTF/CFF 不支持）: %s",
                ttfPath.c_str());
        return false;
    }
    info_ = fi;
    loaded_ = true;
    path_ = ttfPath;
    PP_INFO("字体已加载: %s (%ld 字节)", ttfPath.c_str(), sz);
    return true;
}

void Font::resetAtlas(Atlas* a) {
    a->glyphs.clear();
    a->penX = 1; a->penY = 1; a->rowH = 0;
    memset(a->pix.data(), 0, a->pix.size());
    a->dirty = true;
}

Font::Atlas* Font::atlasFor(int px) {
    if (px < 6) px = 6;
    if (px > 96) px = 96;
    for (Atlas* a : atlases_) {
        if (a->px == px) { lastUse_[px] = ++clock_; return a; }
    }
    /* 档数上限：超了淘汰最久未用的（OSD 同时用的字号就那么两三种） */
    if ((int)atlases_.size() >= kMaxAtlases) {
        int victimPx = -1, oldest = INT_MAX;
        for (const auto& kv : lastUse_) {
            if (kv.second < oldest) { oldest = kv.second; victimPx = kv.first; }
        }
        for (size_t i = 0; i < atlases_.size(); i++) {
            if (atlases_[i]->px == victimPx) {
                if (atlases_[i]->tex) SDL_DestroyTexture(atlases_[i]->tex);
                delete atlases_[i];
                atlases_.erase(atlases_.begin() + i);
                lastUse_.erase(victimPx);
                break;
            }
        }
    }
    Atlas* a = new Atlas();
    a->px = px;
    a->texW = kAtlasSize;
    a->texH = kAtlasSize;
    a->pix.assign((size_t)a->texW * a->texH * 4, 0);
    a->penX = 1; a->penY = 1; a->rowH = 0;
    atlases_.push_back(a);
    lastUse_[px] = ++clock_;
    return a;
}

/* 把一个码点烤进图集。返回 false 表示图集放不下（调用方负责清空重来一次）。 */
bool Font::bake(Atlas* a, uint32_t cp, Glyph* out) {
    stbtt_fontinfo* fi = (stbtt_fontinfo*)info_;
    float scale = stbtt_ScaleForPixelHeight(fi, (float)a->px);
    int w = 0, h = 0, xoff = 0, yoff = 0;
    unsigned char* bmp = stbtt_GetCodepointBitmap(fi, scale, scale, (int)cp,
                                                  &w, &h, &xoff, &yoff);
    if (!bmp) {
        /* 没有字形（空白/未收录）：给一个零尺寸占位，advance 仍要正确 */
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(fi, (int)cp, &adv, &lsb);
        out->src = SDL_FRect{0, 0, 0, 0};
        out->w = out->h = 0;
        out->xoff = out->yoff = 0;
        out->advance = (float)adv * scale;
        return true;
    }
    if (w <= 0 || h <= 0) { stbtt_FreeBitmap(bmp, nullptr); return false; }

    /* 行式打包：放不下就换行；换不了行就返回 false（调用方清空重烤） */
    if (a->penX + w + 1 > a->texW) { a->penX = 1; a->penY += a->rowH + 1; a->rowH = 0; }
    if (a->penY + h + 1 > a->texH) { stbtt_FreeBitmap(bmp, nullptr); return false; }

    uint8_t* dst = a->pix.data();
    for (int y = 0; y < h; y++) {
        uint8_t* row = dst + ((a->penY + y) * a->texW + a->penX) * 4;
        const unsigned char* src = bmp + y * w;
        for (int x = 0; x < w; x++) {
            row[x * 4 + 0] = 255;
            row[x * 4 + 1] = 255;
            row[x * 4 + 2] = 255;
            row[x * 4 + 3] = src[x];     /* 灰度覆盖度当 alpha */
        }
    }
    stbtt_FreeBitmap(bmp, nullptr);

    out->src = SDL_FRect{(float)a->penX, (float)a->penY, (float)w, (float)h};
    out->w = w; out->h = h;
    out->xoff = xoff; out->yoff = yoff;
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(fi, (int)cp, &adv, &lsb);
    out->advance = (float)adv * scale;

    a->penX += w + 1;
    if (h + 1 > a->rowH) a->rowH = h + 1;
    a->dirty = true;
    return true;
}

const Font::Glyph* Font::glyph(Atlas* a, uint32_t cp) {
    auto it = a->glyphs.find(cp);
    if (it != a->glyphs.end()) return &it->second;
    Glyph g;
    if (!bake(a, cp, &g)) {
        /* 图集满了：整体清空后重烤一次（冷字形被淘汰，热字形马上会再烤回来） */
        resetAtlas(a);
        if (!bake(a, cp, &g)) return nullptr;
    }
    auto res = a->glyphs.emplace(cp, g);
    return &res.first->second;
}

float Font::lineHeight(int px) const {
    if (!loaded_) return (float)px * 1.25f;
    stbtt_fontinfo* fi = (stbtt_fontinfo*)info_;
    float scale = stbtt_ScaleForPixelHeight(fi, (float)px);
    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(fi, &asc, &desc, &gap);
    return (float)(asc - desc + gap) * scale;
}

float Font::measure(const char* utf8, int px) const {
    if (!utf8 || !*utf8 || !loaded_) return 0.0f;
    Font* self = const_cast<Font*>(this);
    Atlas* a = self->atlasFor(px);
    size_t len = strlen(utf8);
    float x = 0.0f;
    size_t i = 0;
    while (i < len) {
        uint32_t cp = 0;
        int n = utf8Decode(utf8 + i, len - i, &cp);
        if (n <= 0) break;
        i += (size_t)n;
        const Glyph* g = self->glyph(a, cp);
        if (g) x += g->advance;
    }
    return x;
}

void Font::drawText(SDL_Renderer* r, float x, float y, const char* utf8,
                    PP_Color c, int px) {
    if (!r || !utf8 || !*utf8 || !loaded_) return;
    Atlas* a = atlasFor(px);

    /* 先把这一串里会用到的字形都准备好（图集可能因此重排），再逐字画。
     * 顺序很重要：边画边烤会触发图集清空，把已经算好的 src 矩形作废。 */
    size_t len = strlen(utf8);
    std::vector<uint32_t> cps;
    cps.reserve(len);
    size_t i = 0;
    while (i < len) {
        uint32_t cp = 0;
        int n = utf8Decode(utf8 + i, len - i, &cp);
        if (n <= 0) break;
        i += (size_t)n;
        cps.push_back(cp);
    }
    for (uint32_t cp : cps) glyph(a, cp);

    if (a->dirty) {
        if (!a->tex) {
            a->tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32,
                                       SDL_TEXTUREACCESS_STREAMING,
                                       a->texW, a->texH);
            if (!a->tex) { PP_WARN("字形图集纹理创建失败: %s", SDL_GetError()); return; }
            SDL_SetTextureBlendMode(a->tex, SDL_BLENDMODE_BLEND);
        }
        if (!SDL_UpdateTexture(a->tex, nullptr, a->pix.data(), a->texW * 4)) {
            PP_WARN("字形图集上传失败: %s", SDL_GetError());
            return;
        }
        a->dirty = false;
    }

    Uint8 ca = (Uint8)((c >> 24) & 0xFF);
    SDL_SetTextureColorMod(a->tex, (Uint8)(c & 0xFF), (Uint8)((c >> 8) & 0xFF),
                           (Uint8)((c >> 16) & 0xFF));
    SDL_SetTextureAlphaMod(a->tex, ca);

    /* 基线：y 传进来是"这一行顶部"，字形按 ascent 对齐到基线 */
    stbtt_fontinfo* fi = (stbtt_fontinfo*)info_;
    float scale = stbtt_ScaleForPixelHeight(fi, (float)a->px);
    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(fi, &asc, &desc, &gap);
    float baseY = y + (float)asc * scale;

    float penX = x;
    for (uint32_t cp : cps) {
        const Glyph* g = glyph(a, cp);
        if (!g) continue;
        if (g->w > 0 && g->h > 0) {
            SDL_FRect dst{penX + (float)g->xoff,
                          baseY + (float)g->yoff,
                          (float)g->w, (float)g->h};
            SDL_RenderTexture(r, a->tex, &g->src, &dst);
        }
        penX += g->advance;
    }
    SDL_SetTextureAlphaMod(a->tex, 255);
}

} // namespace pp
