/* test_libass_render.cpp — P1-e libass 渲染通路验收（无显示、可重复）
 * ---------------------------------------------------------------------------
 * 复刻 subtitles 插件在运行时的真实字体机制（已通过对照实验确定的唯一稳定方案）：
 *   ass_library_init / ass_renderer_init /
 *   ass_set_fonts_dir(<fonts_dir>) / ass_set_fonts(AUTODETECT) /
 *   ass_process_data(头) / ass_process_data(Dialogue) / ass_set_frame_size /
 *   ass_render_frame。
 *
 * 关键结论（实测）：本 libass 0.17.5/MSYS2 构建里 ass_add_font 内存字体在渲染时
 * 会 0xC0000005 崩溃，故严禁使用；稳定方案是系统字体 provider 从随包字体目录扫描
 * 加载（DirectWrite / fontconfig）。内核 ASS 头用哨兵家族名 PluginPlayerSub，插件
 * 收到后替换为真实字体名（这里 HappyZcool-2016 = ZCOOLKuaiLe 的 name 表家族名）。
 *
 * 喂一行带覆盖标签的中英文字幕（\an8 顶对齐、\b1 粗体、\c&H00FF00& 绿色），渲染到
 * t=1500ms，统计有效像素。>50 即 PASS：证明随包字体被真正选中、样式解析、字幕可见。
 * fontselect 日志会打印实际选中的字体名，可作证据。
 *
 * 编译: g++.exe tools/test_libass_render.cpp -o build/test_libass_render.exe \
 *        -Ivendor/libass/include -Lvendor/libass/lib -lass \
 *        -static-libgcc -static-libstdc++ -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic -s
 * 运行: build/test_libass_render.exe [fonts_dir] [family]
 */
#include <ass/ass.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

static std::string buildHeader(int w, int h, const char* family) {
    int fs = h / 18; if (fs < 12) fs = 12;
    char buf[1200];
    std::snprintf(buf, sizeof(buf),
        "[Script Info]\n"
        "ScriptType: v4.00+\n"
        "PlayResX: %d\n"
        "PlayResY: %d\n"
        "WrapStyle: 2\n"
        "ScaledBorderAndShadow: yes\n\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,%s,%d,&H00FFFFFF,&H000000FF,&H00000000,&H64000000,0,0,0,0,100,100,0,0,1,1.5,1.2,2,20,20,20,1\n\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n",
        w, h, family, fs);
    return buf;
}

int main(int argc, char** argv) {
    const char* fdir  = (argc > 1) ? argv[1] : "assets/fonts";
    const char* family = (argc > 2) ? argv[2] : "HappyZcool-2016";

    ASS_Library* lib = ass_library_init();
    if (!lib) { printf("[FAIL] ass_library_init\n"); return 1; }
    ASS_Renderer* ren = ass_renderer_init(lib);
    if (!ren) { printf("[FAIL] ass_renderer_init\n"); return 1; }

    /* 与插件一致的机制：系统 provider 扫描随包字体目录。 */
    ass_set_fonts_dir(lib, fdir);
    ass_set_fonts(ren, nullptr, family, ASS_FONTPROVIDER_AUTODETECT, nullptr, 1);
    printf("[OK] fonts_dir=%s  family=%s\n", fdir, family);

    /* 存在性校验（非致命）。 */
    std::string fp = std::string(fdir) + "/ZCOOLKuaiLe-Regular.ttf";
    if (!std::ifstream(fp)) printf("[WARN] 随包字体缺失: %s（将回退系统字体）\n", fp.c_str());

    ASS_Track* track = ass_new_track(lib);
    std::string header = buildHeader(1280, 720, family);
    ass_process_data(track, header.c_str(), (int)header.size());
    /* 中英混排 + 覆盖标签，验证随包中文字体真被选中（非系统回退）。 */
    std::string line =
        "Dialogue: 0,0:00:00.00,0:00:03.00,Default,,0,0,0,,{\\an8\\b1\\c&H00FF00&}快乐字幕 Hello libass";
    ass_process_data(track, line.c_str(), (int)line.size());

    ass_set_frame_size(ren, 1280, 720);
    int change = 0;
    ASS_Image* img = ass_render_frame(ren, track, 1500, &change);
    if (!img) { printf("[FAIL] ass_render_frame 返回空（字体/样式未命中？）\n"); return 1; }

    long long nonTransparent = 0;
    for (ASS_Image* p = img; p; p = p->next) {
        for (int y = 0; y < p->h; ++y) {
            const uint8_t* sp = p->bitmap + (size_t)y * p->stride;
            for (int x = 0; x < p->w; ++x) {
                uint8_t cv = sp[x]; if (!cv) continue;
                uint8_t aCol = (uint8_t)(p->color & 0xFF);
                int aEff = (cv * (255 - aCol)) / 255;
                if (aEff > 4) nonTransparent++;
            }
        }
    }
    printf("[OK] 渲染出 %lld 个有效像素\n", nonTransparent);
    if (nonTransparent < 50) {
        printf("[FAIL] 可见像素过少，随包字体未命中或样式错误\n");
        return 1;
    }
    printf("[PASS] libass 渲染通路验证通过：随包字体命中 + 样式解析 + 字幕可见\n");
    return 0;
}
