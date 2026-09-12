#pragma once
/* font.h — 真字体渲染（P1-a）
 *
 * 用 stb_truetype 把 TTF 解析成字形图集，OSD/字幕的 text 走这里；
 * 字体缺失时由 Osd 回退到 SDL 调试点阵字（不能让"没字体"变成"没界面"）。
 *
 * 为什么自己管图集而不用 stbtt_PackFontRanges：后者要再引一个 stb_rect_pack.h。
 * 这里用最简单的行式打包（纹理满就整体清空重建），OSD 用不到的冷僻字形
 * 自然被淘汰，逻辑也一眼能看懂。
 *
 * 注意 stb_truetype **只支持 TrueType 'glyf' 轮廓**，不支持 OTF/CFF ——
 * 选字体时必须给 .ttf（本项目随包的是 TrueType 轮廓的站酷快乐体）。 */
#include <SDL3/SDL.h>
#include "player_plugin.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace pp {

class Font {
public:
    static Font& instance();

    /** 加载字体文件（失败返回 false，调用方应记日志并回退）。可重复调用以换字体。 */
    bool load(const std::string& ttfPath);
    bool loaded() const { return loaded_; }
    const std::string& path() const { return path_; }

    /** 绘制一段 UTF-8。px 是**逻辑像素**字号（不是倍数），与 measure 必须一致。 */
    void drawText(SDL_Renderer* r, float x, float y, const char* utf8,
                  PP_Color c, int px);
    /** 文本的像素宽度（按每字形 advance 累加，CJK 自然是全宽）。 */
    float measure(const char* utf8, int px) const;
    float lineHeight(int px) const;

    void shutdown();

private:
    Font() = default;
    ~Font() = default;

    struct Glyph {
        SDL_FRect src;      /* 图集内的源矩形（像素） */
        int       w = 0, h = 0;
        int       xoff = 0, yoff = 0;
        float     advance = 0.0f;
    };
    struct Atlas {
        int           px = 0;
        SDL_Texture*  tex = nullptr;
        std::vector<uint8_t> pix;      /* 图集系统内存副本（RGBA） */
        int           texW = 0, texH = 0;
        int           penX = 0, penY = 0, rowH = 0;
        std::unordered_map<uint32_t, Glyph> glyphs;
        bool          dirty = false;
    };

    Atlas* atlasFor(int px);                 /* 取或建字号档（最多 kMaxAtlases 档，LRU） */
    const Glyph* glyph(Atlas* a, uint32_t cp);
    bool bake(Atlas* a, uint32_t cp, Glyph* out);
    void resetAtlas(Atlas* a);

    bool        loaded_ = false;
    std::string path_;
    std::vector<uint8_t> fontData_;
    void*       info_ = nullptr;             /* stbtt_fontinfo（不暴露结构，避免污染头文件） */
    std::vector<Atlas*> atlases_;
    mutable int clock_ = 0;
    std::unordered_map<int, int> lastUse_;   /* px -> 最近使用序号（LRU） */

    static const int kAtlasSize = 512;
    static const int kMaxAtlases = 4;
};

} // namespace pp
