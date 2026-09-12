#pragma once
/* util.h — 日志与通用小工具 */
#include <string>
#include <vector>
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

/* ---- 极简 JSON 取值（够用即可，不引入第三方库） ---- */
std::string jsonGetString(const std::string& json, const char* key, const std::string& def = "");
double jsonGetNumber(const std::string& json, const char* key, double def = 0.0);
std::string jsonEscape(const std::string& s);

/* ---- PNG 写出（内置实现：deflate stored + CRC32 + Adler32，无第三方依赖） ---- */
bool writePngRGBA(const std::string& path, const uint8_t* rgba, int w, int h, int pitch);
bool writePngGray8(const std::string& path, const uint8_t* gray, int w, int h, int pitch);

} // namespace pp
