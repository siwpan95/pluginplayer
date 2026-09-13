#pragma once
/* util.h — 日志与通用小工具 */
#include <string>
#include <vector>
#include <utility>
#include <cstdint>
#include <cstdio>

namespace pp {

enum LogLevel { L_TRACE = 0, L_DEBUG = 1, L_INFO = 2, L_WARN = 3, L_ERROR = 4 };

class Log {
public:
    static void init(const std::string& file, LogLevel level = L_INFO);
    static void shutdown();
    static void setLevel(LogLevel lv) { level_ = lv; }
    static LogLevel level() { return level_; }
    static void write(LogLevel lv, const char* fmt, ...);
    static std::string levelName(LogLevel lv);
private:
    static LogLevel level_;
    static FILE* fp_;
};
} // namespace pp

#define PP_TRACE(...) ::pp::Log::write(::pp::L_TRACE, __VA_ARGS__)
#define PP_DEBUG(...) ::pp::Log::write(::pp::L_DEBUG, __VA_ARGS__)
#define PP_INFO(...)  ::pp::Log::write(::pp::L_INFO,  __VA_ARGS__)
#define PP_WARN(...)  ::pp::Log::write(::pp::L_WARN,  __VA_ARGS__)
#define PP_ERROR(...) ::pp::Log::write(::pp::L_ERROR, __VA_ARGS__)

namespace pp {

/* ---- 字符串 ---- */
std::string trim(const std::string& s);
std::string lower(std::string s);
bool endsWith(const std::string& s, const std::string& suf);
std::vector<std::string> split(const std::string& s, char delim);
std::string format(const char* fmt, ...);          /* printf 风格，返回 std::string */
std::string fmtTime(double seconds, bool withMs = false); /* 01:23:45 / 01:23.4 */
std::string fmtBytes(uint64_t bytes);
std::string pathJoin(const std::string& a, const std::string& b);
std::string baseName(const std::string& path);
std::string dirName(const std::string& path);
std::string exeDir();                              /* 当前可执行文件所在目录 */
bool fileExists(const std::string& p);
bool dirExists(const std::string& p);
bool makeDirs(const std::string& p);
std::vector<std::string> listDir(const std::string& dir, const std::string& extFilter);

/* ---- 路径规范化与包含判定（用于「插件只许往白名单目录写」的安全策略） ----
 * normalizePath: 绝对化 + 折叠 "." / ".." + 统一为正斜杠 + 去尾部斜杠。
 *                注意：不解析 Windows 重解析点（junction/symlink）。本程序
 *                的白名单根都是 exeDir 与本机用户显式声明的目录，插件无法
 *                在其中的路径上制造重解析点，因此这一层足够。
 * pathIsInside : child 规范化后是否位于 root 之内（Windows 下大小写不敏感）。
 *                root 自身算「在内」。 */
std::string normalizePath(const std::string& p);
bool pathIsInside(const std::string& child, const std::string& root);

/* ---- 网络输入判定（用于默认拒绝 SSRF 型输入） ----
 * 识别两类「会出网」的输入：
 *   1) scheme://  —— http/https/ftp/rtsp/rtmp/smb/file/udp/tcp …（要求 scheme ≥2 字符，
 *      这样不会把 Windows 盘符 "C://x" 误判成协议）
 *   2) UNC 路径   —— \\host\share 或 //host/share（会触发 SMB 认证，可被用于 NTLM 中继）
 * 注意：普通本地路径（C:\a\b.mp4、/tmp/a.mp4、相对路径）都不会命中。 */
bool isNetworkInput(const std::string& s);

/* ---- 严格 JSON 取值 ----
 * 早期实现用 `find("\"key\"")` 做子串搜索，有三类问题：
 *   1. 键名出现在**字符串值里**也会命中（{"path":"a\"id\"b"} 会误取 id）
 *   2. 不校验合法性，非法输入被静默当默认值，容易出现"参数走私"
 *   3. 重复键无语义（先出现的胜出）
 * 现在改为真正的词法扫描：只认**顶层**键、正确处理转义、拒绝重复键与超深嵌套。 */
bool jsonParseObject(const std::string& json,
                     std::vector<std::pair<std::string, std::string>>* out,
                     std::string* err);
std::string jsonGetString(const std::string& json, const char* key, const std::string& def = "");
double jsonGetNumber(const std::string& json, const char* key, double def = 0.0);
/** 取顶层键的字符串数组（元素必须都是字符串）；缺键返回 false 且 out 清空。 */
bool jsonGetStringArray(const std::string& json, const char* key,
                        std::vector<std::string>* out);
std::string jsonEscape(const std::string& s);

/* ---- 文件哈希（用于插件完整性清单） ----
 * 用系统 CSPRNG 同一套 bcrypt 的 SHA-256；不可用时返回 false。 */
bool sha256FileHex(const std::string& path, std::string* outHexLower);

/* ---- PNG 写出（内置实现：deflate stored + CRC32 + Adler32，无第三方依赖） ---- */
bool writePngRGBA(const std::string& path, const uint8_t* rgba, int w, int h, int pitch);
bool writePngGray8(const std::string& path, const uint8_t* gray, int w, int h, int pitch);

/* ---- YUV → RGB 的矩阵与电平范围选择 ----
 *
 * 为什么需要它：`sws_getContext()` 一次性建好上下文、**不接受任何色彩信息**，
 * 于是 swscale 只能按自己的默认值猜矩阵。真实文件的色彩矩阵是**写在码流里**的
 * （`AVFrame.colorspace` / `AVFrame.color_range`），忽略它就等于用错的矩阵去还原：
 * 典型症状是肤色/红色偏色、整体发闷。SD 尺寸的素材尤其容易踩到
 * （分辨率启发式会猜 BT.601，而素材可能明确标着 BT.709）。
 *
 * 入参直接用 FFmpeg 的 `AVColorSpace` / `AVColorRange` 取值（本模块刻意不 include
 * FFmpeg 头，以保持可移植、可单测）：
 *
 *   AVColorSpace : RGB=0  BT709=1  UNSPECIFIED=2  FCC=4  BT470BG=5
 *                  SMPTE170M=6  SMPTE240M=7  YCGCO=8  BT2020_NCL=9  BT2020_CL=10
 *   AVColorRange : UNSPECIFIED=0  MPEG(有限范围 16-235)=1  JPEG(全范围 0-255)=2
 *
 * 未标注（UNSPECIFIED）时用 **FFmpeg 自己 vf_scale 的 `auto` 规则**：
 * 高度 > 576 行按 BT.709，否则按 BT.601 —— 而不是无脑用 SWS_CS_DEFAULT(601)。
 * 范围未标注时取**有限范围**（与 swscale 自己的默认一致，也是绝大多数素材的情况）。 */
enum YuvMatrix {
    YUV_MATRIX_BT709     = 1,   /* 取值与 libswscale 的 SWS_CS_ITU709 一致 */
    YUV_MATRIX_FCC       = 4,   /* == SWS_CS_FCC */
    YUV_MATRIX_BT601     = 5,   /* == SWS_CS_ITU601 / SWS_CS_SMPTE170M */
    YUV_MATRIX_SMPTE240M = 7,   /* == SWS_CS_SMPTE240M */
    YUV_MATRIX_BT2020    = 9,   /* == SWS_CS_BT2020 */
};

struct YuvColorChoice {
    /** enum YuvMatrix。取值与 SWS_CS_* 相同，可直接喂 sws_getCoefficients()。 */
    int matrix;
    /** 0 = 有限范围(16-235)，1 = 全范围(0-255)。对应 sws_setColorspaceDetails 的 srcRange。 */
    int fullRange;
    /** enum AVColorPrimaries（FFmpeg）。默认 UNSPECIFIED=2。P2-L1：参与 HDR 判据。 */
    int primaries = 2;
    /** enum AVColorTransferCharacteristic（FFmpeg）。默认 UNSPECIFIED=2。
     *  P2-L1：BT.2020 原色 + PQ(16)/HLG(18) 传输 → HDR。 */
    int trc = 2;
};

YuvColorChoice chooseYuvColor(int avColorSpace, int avColorRange, int width, int height);

/** P2-L1：该色彩选择是否构成 HDR。
 *  判据：BT.2020 原色(AVCOL_PRI_BT2020=9) + PQ(SMPTE2084=16) 或 HLG(ARIB_STD_B67=18)。
 *  注意：SD 标称 BT.2020 原色但走 G22/BT.709 传输（trc=1/2）仍是 SDR。 */
static inline bool isHdrColorChoice(const YuvColorChoice& c) {
    return c.primaries == 9 /* AVCOL_PRI_BT2020 */
        && (c.trc == 16      /* AVCOL_TRC_SMPTE2084 (PQ) */
            || c.trc == 18   /* AVCOL_TRC_ARIB_STD_B67 (HLG) */);
}

/* ---- 同一套色彩选择到 SDL_Colorspace 的映射 ----
 * 走 NV12 纹理时 YUV→RGB 由 GPU 做，矩阵/范围必须**在建纹理时**告诉 SDL：
 * SDL 3.4.16 没有 SDL_SetTextureColorSpace，只有创建属性
 * SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER（NV12 纹理的**默认值是
 * SDL_COLORSPACE_JPEG**，即全范围 BT.601 —— 不显式设置就是错色）。
 *
 * P2-L1 扩展：原色 + 传输函数决定"是不是 HDR"。BT.2020 原色 + PQ 传输 →
 * SDL_COLORSPACE_HDR10（RGB_FULL_G2084_NONE_P2020，即 HDR10/PQ）；BT.2020 + HLG
 * 则 SDL 没有专门的枚举，退回 BT.2020（矩阵正确，但传输函数按 G22 处理 —— 已知限制）。
 * SDR 仍按矩阵 + 范围映射（行为与 P2-L1 前完全一致）。
 *
 * 取值直接抄 SDL_pixels.h（本模块不 include SDL 头，以保持可移植、可单测）：
 *   BT601_LIMITED=0x211018c6  BT601_FULL=0x221018c6
 *   BT709_LIMITED=0x21100421  BT709_FULL=0x22100421
 *   BT2020_LIMITED=0x21102609 BT2020_FULL=0x22102609
 *   SRGB=0x120005a0           SRGB_LINEAR=0x12000500  HDR10=0x12002600
 * SMPTE240M 与 FCC 在 SDL 里没有对应值 —— 退回 BT.601，并用 *fellBack 告知调用方
 * 打日志，避免"悄悄用错矩阵"。 */
enum SdlColorSpaceId {
    PP_SDL_CS_BT601_LIMITED  = 0x211018c6,
    PP_SDL_CS_BT601_FULL     = 0x221018c6,
    PP_SDL_CS_BT709_LIMITED  = 0x21100421,
    PP_SDL_CS_BT709_FULL     = 0x22100421,
    PP_SDL_CS_BT2020_LIMITED = 0x21102609,
    PP_SDL_CS_BT2020_FULL    = 0x22102609,
    PP_SDL_CS_SRGB           = 0x120005a0u,  /* 默认 SDR 输出（=SDL_COLORSPACE_SRGB）*/
    PP_SDL_CS_SRGB_LINEAR    = 0x12000500u, /* HDR 输出（d3d11/d3d12/metal 支持，值可超 1.0）*/
    PP_SDL_CS_HDR10          = 0x12002600u, /* RGB_FULL_G2084_NONE_P2020（BT.2020 + PQ）*/
};

int sdlColorspaceFor(const YuvColorChoice& c, int* fellBack);

/* ---- 抓屏取证：矩形内"恰好等于指定颜色"的像素占比 ----
 * 参数非法（空指针 / 尺寸非正 / pitch < w*4 / 矩形容不下任何像素）时返回 **-1.0**，
 * 调用方据此区分"占比为 0"与"压根没量到"；否则返回 0..1。
 * rgba 按 R,G,B,A 字节序（与 writePngRGBA 一致），矩形会被裁剪到图像范围内。
 *
 * 为什么需要它：自检要判断抓屏图里**到底有没有画面**。窗口清屏色是一个
 * **特定**的 RGB 值，而视频纹理若没被绘制，画面矩形内就会整片是清屏色
 * （本机实测 86.6%，其余是 OSD）；真实视频内容哪怕是一整帧 YUV 有限范围的黑场，
 * 转成 RGBA 也是 (0,0,0) 附近，不会"恰好"等于清屏色。
 * 于是"该占比异常高"就是"画面没画上去"的可靠特征 —— 而"图里是黑的"
 * 本身说明不了任何问题（黑场是合法内容）。 */
double rgbaColorRatioInRect(const uint8_t* rgba, int pitch, int w, int h,
                            int x, int y, int rw, int rh,
                            uint8_t r, uint8_t g, uint8_t b);

/* ---- UTF-8 解码与东亚全宽判定（P1-a 真字体 OSD 的纯逻辑层）----
 *
 * 放在 util 里而不是 font 里，原因和色彩选择函数一样：**可移植、能在 Linux CI 上单测**。
 * font.cpp 依赖 SDL 与 stb_truetype，进不了 portable 子集；而"一个 UTF-8 序列解出什么
 * 码点""这个码点是不是全宽"是纯查表逻辑，错了就是整屏错位，必须有单测兜底。
 *
 * 为什么需要全宽判定：文本宽度要按字形 advance 累加，而 CJK 标点/假名/全角字母在
 * 半宽字体里也是双宽。只按"码点 > 0x2E80 就算双宽"会漏掉全角 ASCII（U+FF01..FF60）。 */

/** 解出一个 UTF-8 序列。非法序列按 U+FFFD 处理，并返回该序列实际消耗的字节数
 *  （保证调用方能继续往后走，不会死循环）。返回 0 表示已到串尾。
 *  out 可为 nullptr（只想知道长度）。 */
int utf8Decode(const char* s, size_t len, uint32_t* out);

/** 该码点是否应占"双宽"（东亚宽 W / 全宽 F）。用于文本宽度与换行的粗排。 */
bool isWideCodepoint(uint32_t cp);

/** UTF-8 串的"显示宽度单位数"：全宽记 2，其余记 1。用于不依赖字体的宽度估算。 */
int utf8DisplayWidth(const char* s);

} // namespace pp
