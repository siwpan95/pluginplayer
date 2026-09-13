/* util.cpp — 日志、字符串、PNG 写出 */
#include "util.h"

#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <climits>
#include <algorithm>
#include <cerrno>

#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>
#else
#  include <unistd.h>
#  include <sys/stat.h>
#  include <dirent.h>
#endif

namespace pp {

LogLevel Log::level_ = L_INFO;
FILE* Log::fp_ = nullptr;

void Log::init(const std::string& file, LogLevel level) {
    level_ = level;
    if (!file.empty()) {
        fp_ = fopen(file.c_str(), "w");
    }
}

void Log::shutdown() {
    if (fp_) { fflush(fp_); fclose(fp_); fp_ = nullptr; }
}

std::string Log::levelName(LogLevel lv) {
    switch (lv) {
        case L_TRACE: return "TRACE";
        case L_DEBUG: return "DEBUG";
        case L_INFO:  return "INFO ";
        case L_WARN:  return "WARN ";
        case L_ERROR: return "ERROR";
    }
    return "?????";
}

/* 日志是「一行一条」。如果正文里带未净化的换行，任何可控输入
 * （例如播放器打开的文件路径含 \n）就能伪造出额外的日志行 —— 即日志注入。
 * 这里把 \r\n 转义成字面量 \\r \\n。 */
static std::string sanitizeLogBody(const char* s) {
    std::string o;
    for (const char* p = s; p && *p; ++p) {
        if (*p == '\n')      o += "\\n";
        else if (*p == '\r') o += "\\r";
        else                 o.push_back(*p);
    }
    return o;
}

void Log::write(LogLevel lv, const char* fmt, ...) {
    if (lv < level_) return;
    char body[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    time_t t = time(nullptr);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    /* 转义后最长 2 倍，缓冲相应放大 */
    std::string safe = sanitizeLogBody(body);
    char line[8400];
    snprintf(line, sizeof(line), "[%02d:%02d:%02d][%s] %s\n",
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, levelName(lv).c_str(), safe.c_str());

    fputs(line, lv >= L_WARN ? stderr : stdout);
    fflush(lv >= L_WARN ? stderr : stdout);
    if (fp_) { fputs(line, fp_); fflush(fp_); }
}

/* ======================= 字符串 ======================= */

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)tolower(c); });
    return s;
}

bool endsWith(const std::string& s, const std::string& suf) {
    if (s.size() < suf.size()) return false;
    std::string a = lower(s.substr(s.size() - suf.size()));
    return a == lower(suf);
}

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == delim) { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

std::string format(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return std::string(buf);
}

std::string fmtTime(double seconds, bool withMs) {
    if (seconds < 0 || seconds != seconds) return withMs ? "--:--.-" : "--:--:--";
    int total = (int)seconds;
    int h = total / 3600, m = (total % 3600) / 60, sec = total % 60;
    if (withMs) {
        int tenth = (int)((seconds - total) * 10.0);
        if (h > 0) return format("%d:%02d:%02d.%d", h, m, sec, tenth);
        return format("%02d:%02d.%d", m, sec, tenth);
    }
    return format("%02d:%02d:%02d", h, m, sec);
}

std::string fmtBytes(uint64_t bytes) {
    const char* u[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)bytes;
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    return format(i == 0 ? "%.0f %s" : "%.1f %s", v, u[i]);
}

std::string pathJoin(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    char last = a[a.size() - 1];
    if (last == '/' || last == '\\') return a + b;
    return a + "/" + b;
}

std::string baseName(const std::string& path) {
    size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? path : path.substr(p + 1);
}

std::string dirName(const std::string& path) {
    size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? std::string() : path.substr(0, p);
}

std::string exeDir() {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0) return ".";
    std::string s(buf, n);
    return dirName(s);
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return ".";
    buf[n] = 0;
    return dirName(buf);
#endif
}

/* ======================= 路径规范化 / 包含判定 ======================= */

std::string normalizePath(const std::string& p) {
    if (p.empty()) return "";
#ifdef _WIN32
    char buf[32768];
    DWORD n = GetFullPathNameA(p.c_str(), (DWORD)sizeof(buf), buf, nullptr);
    if (n == 0 || n >= sizeof(buf)) return p;   /* 规范化失败：原样返回（会被白名单拒绝） */
    std::string s(buf, n);
#else
    char buf[PATH_MAX];
    if (!realpath(p.c_str(), buf)) return p;
    std::string s(buf);
#endif
    for (auto& c : s) if (c == '\\') c = '/';
    /* 去尾部斜杠，但保留盘符根 "C:/" 与 POSIX 根 "/" */
    while (s.size() > 1 && s.back() == '/') {
        if (s.size() == 3 && s[1] == ':') break;
        s.pop_back();
    }
    return s;
}

bool pathIsInside(const std::string& child, const std::string& root) {
    std::string c = lower(normalizePath(child));
    std::string r = lower(normalizePath(root));
    if (r.empty() || c.size() < r.size()) return false;
    if (c.compare(0, r.size(), r) != 0) return false;
    if (c.size() == r.size()) return true;
    char sep = c[r.size()];
    return sep == '/' || sep == '\\';
}

bool isNetworkInput(const std::string& s) {
    if (s.empty()) return false;
    /* UNC：\\host\share 或 //host/share */
    if (s.size() >= 2 &&
        ((s[0] == '\\' && s[1] == '\\') || (s[0] == '/' && s[1] == '/'))) {
        return true;
    }
    /* scheme:// */
    size_t i = s.find("://");
    if (i == std::string::npos || i < 2) return false;   /* i<2 排除盘符 "C://" */
    for (size_t k = 0; k < i; ++k) {
        char c = s[k];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
        if (!ok) return false;
    }
    return true;
}

bool fileExists(const std::string& p) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

bool dirExists(const std::string& p) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool makeDirs(const std::string& p) {
    if (p.empty() || dirExists(p)) return true;
    std::string cur;
    for (size_t i = 0; i < p.size(); ++i) {
        char c = p[i];
        cur.push_back(c);
        if (c == '/' || c == '\\' || i + 1 == p.size()) {
            if (cur.size() <= 1) continue;
            if (dirExists(cur)) continue;
#ifdef _WIN32
            _mkdir(cur.c_str());
#else
            mkdir(cur.c_str(), 0755);
#endif
        }
    }
    return dirExists(p);
}

std::vector<std::string> listDir(const std::string& dir, const std::string& extFilter) {
    std::vector<std::string> out;
#ifdef _WIN32
    std::string pat = pathJoin(dir, "*");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string name = fd.cFileName;
        if (!extFilter.empty() && !endsWith(name, extFilter)) continue;
        out.push_back(pathJoin(dir, name));
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        if (!extFilter.empty() && !endsWith(name, extFilter)) continue;
        out.push_back(pathJoin(dir, name));
    }
    closedir(d);
#endif
    std::sort(out.begin(), out.end());
    return out;
}

/* ======================= 文件 SHA-256 ======================= */

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <bcrypt.h>
#endif

bool sha256FileHex(const std::string& path, std::string* outHexLower) {
#ifdef _WIN32
    if (!outHexLower) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return false;
    DWORD objLen = 0, cb = 0;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objLen, sizeof(objLen),
                          &cb, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }
    std::vector<unsigned char> obj(objLen ? objLen : 1);
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(alg, &hash, obj.data(), objLen, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }
    bool ok = false;
    FILE* f = fopen(path.c_str(), "rb");
    if (f) {
        std::vector<unsigned char> buf(1 << 20);
        size_t n;
        ok = true;
        while ((n = fread(buf.data(), 1, buf.size(), f)) > 0) {
            if (BCryptHashData(hash, buf.data(), (ULONG)n, 0) != 0) { ok = false; break; }
        }
        if (ferror(f)) ok = false;
        fclose(f);
    }
    unsigned char digest[32];
    if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) != 0) ok = false;
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return false;
    static const char* hx = "0123456789abcdef";
    std::string s;
    s.reserve(64);
    for (size_t i = 0; i < sizeof(digest); ++i) {
        s.push_back(hx[digest[i] >> 4]);
        s.push_back(hx[digest[i] & 0x0F]);
    }
    *outHexLower = s;
    return true;
#else
    (void)path;
    if (outHexLower) outHexLower->clear();
    return false;   /* 非 Windows 平台暂未提供：调用方应据此跳过校验并告警 */
#endif
}

/* ======================= JSON 转义 ======================= */

std::string jsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) o += format("\\u%04x", (unsigned char)c);
                else o.push_back(c);
        }
    }
    return o;
}

/* ======================= 严格 JSON 取值 =======================
 * 不用第三方库，但要做成真正的词法扫描（见 util.h 的说明）。
 * 深度上限、重复键、非法字面量一律判非法。
 * ============================================================== */

static const int kMaxJsonDepth = 32;

static bool jIsWs(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static bool jIsDigit(char c) { return c >= '0' && c <= '9'; }

static void jSkipWs(const std::string& s, size_t& i) {
    while (i < s.size() && jIsWs(s[i])) ++i;
}

/** 解析一个 JSON 字符串（i 指向起始的 '"'）；out 可传 nullptr 表示只跳过。 */
static bool jParseString(const std::string& s, size_t& i, std::string* out) {
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    std::string o;
    while (i < s.size()) {
        char c = s[i];
        if (c == '"') { ++i; if (out) *out = o; return true; }
        if ((unsigned char)c < 0x20) return false;      /* 未转义的控制字符：非法 */
        if (c != '\\') { o.push_back(c); ++i; continue; }
        if (i + 1 >= s.size()) return false;
        char e = s[i + 1];
        switch (e) {
            case '"':  o.push_back('"');  i += 2; break;
            case '\\': o.push_back('\\'); i += 2; break;
            case '/':  o.push_back('/');  i += 2; break;
            case 'b':  o.push_back('\b'); i += 2; break;
            case 'f':  o.push_back('\f'); i += 2; break;
            case 'n':  o.push_back('\n'); i += 2; break;
            case 'r':  o.push_back('\r'); i += 2; break;
            case 't':  o.push_back('\t'); i += 2; break;
            case 'u': {
                if (i + 5 >= s.size()) return false;
                unsigned cp = 0;
                for (int k = 0; k < 4; ++k) {
                    char h = s[i + 2 + k];
                    unsigned d;
                    if (h >= '0' && h <= '9')      d = (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') d = (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') d = (unsigned)(h - 'A' + 10);
                    else return false;
                    cp = (cp << 4) | d;
                }
                /* 只处理 BMP；代理项按 UTF-8 原样输出（够本项目的控制通道用） */
                if (cp < 0x80) {
                    o.push_back((char)cp);
                } else if (cp < 0x800) {
                    o.push_back((char)(0xC0 | (cp >> 6)));
                    o.push_back((char)(0x80 | (cp & 0x3F)));
                } else {
                    o.push_back((char)(0xE0 | (cp >> 12)));
                    o.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                    o.push_back((char)(0x80 | (cp & 0x3F)));
                }
                i += 6;
                break;
            }
            default: return false;
        }
    }
    return false;   /* 未闭合 */
}

/** 跳过任意一个合法 JSON 值（不递归下降进对象内部，只保证整体合法）。 */
static bool jSkipValue(const std::string& s, size_t& i, int depth) {
    if (depth > kMaxJsonDepth) return false;
    jSkipWs(s, i);
    if (i >= s.size()) return false;
    char c = s[i];
    if (c == '"') return jParseString(s, i, nullptr);
    if (c == '{' || c == '[') {
        char close = (c == '{') ? '}' : ']';
        ++i;
        jSkipWs(s, i);
        if (i < s.size() && s[i] == close) { ++i; return true; }
        for (;;) {
            if (c == '{') {
                std::string k;
                if (!jParseString(s, i, &k)) return false;
                jSkipWs(s, i);
                if (i >= s.size() || s[i] != ':') return false;
                ++i;
            }
            if (!jSkipValue(s, i, depth + 1)) return false;
            jSkipWs(s, i);
            if (i >= s.size()) return false;
            if (s[i] == ',') { ++i; jSkipWs(s, i); continue; }
            if (s[i] == close) { ++i; return true; }
            return false;
        }
    }
    if (c == 't') { if (s.compare(i, 4, "true")  == 0) { i += 4; return true; } return false; }
    if (c == 'f') { if (s.compare(i, 5, "false") == 0) { i += 5; return true; } return false; }
    if (c == 'n') { if (s.compare(i, 4, "null")  == 0) { i += 4; return true; } return false; }
    /* 数字：严格按 JSON number 文法 */
    size_t st = i;
    if (i < s.size() && s[i] == '-') ++i;
    if (i >= s.size() || !jIsDigit(s[i])) return false;
    if (s[i] == '0') { ++i; }
    else { while (i < s.size() && jIsDigit(s[i])) ++i; }
    if (i < s.size() && s[i] == '.') {
        ++i;
        if (i >= s.size() || !jIsDigit(s[i])) return false;
        while (i < s.size() && jIsDigit(s[i])) ++i;
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
        if (i >= s.size() || !jIsDigit(s[i])) return false;
        while (i < s.size() && jIsDigit(s[i])) ++i;
    }
    return i > st;
}

bool jsonParseObject(const std::string& json,
                     std::vector<std::pair<std::string, std::string>>* out,
                     std::string* err) {
    if (out) out->clear();
    size_t i = 0;
    jSkipWs(json, i);
    if (i >= json.size() || json[i] != '{') {
        if (err) *err = "不是 JSON 对象";
        return false;
    }
    ++i;
    jSkipWs(json, i);
    if (i < json.size() && json[i] == '}') {
        ++i;
        jSkipWs(json, i);
        if (i != json.size()) { if (err) *err = "对象之后有多余内容"; return false; }
        return true;
    }
    for (;;) {
        jSkipWs(json, i);
        std::string key;
        if (!jParseString(json, i, &key)) {
            if (err) *err = "键不是合法字符串";
            return false;
        }
        jSkipWs(json, i);
        if (i >= json.size() || json[i] != ':') {
            if (err) *err = "键 \"" + key + "\" 后缺少 ':'";
            return false;
        }
        ++i;
        jSkipWs(json, i);
        size_t vstart = i;
        size_t vt = i;
        if (!jSkipValue(json, vt, 0)) {
            if (err) *err = "键 \"" + key + "\" 的值非法";
            return false;
        }
        if (out) {
            for (const auto& kv : *out) {
                if (kv.first == key) {
                    if (err) *err = "重复键: " + key;
                    return false;
                }
            }
            out->push_back({key, json.substr(vstart, vt - vstart)});
        }
        i = vt;
        jSkipWs(json, i);
        if (i >= json.size()) { if (err) *err = "对象未闭合"; return false; }
        if (json[i] == ',') { ++i; continue; }
        if (json[i] == '}') { ++i; break; }
        if (err) *err = "对象中出现意外字符";
        return false;
    }
    jSkipWs(json, i);
    if (i != json.size()) { if (err) *err = "对象之后有多余内容"; return false; }
    return true;
}

std::string jsonGetString(const std::string& json, const char* key, const std::string& def) {
    if (!key) return def;
    std::vector<std::pair<std::string, std::string>> kvs;
    if (!jsonParseObject(json, &kvs, nullptr)) return def;
    for (const auto& kv : kvs) {
        if (kv.first != key) continue;
        if (kv.second.empty() || kv.second[0] != '"') return def;   /* 类型不符 */
        size_t i = 0;
        std::string v;
        if (!jParseString(kv.second, i, &v)) return def;
        return v;
    }
    return def;
}

double jsonGetNumber(const std::string& json, const char* key, double def) {
    if (!key) return def;
    std::vector<std::pair<std::string, std::string>> kvs;
    if (!jsonParseObject(json, &kvs, nullptr)) return def;
    for (const auto& kv : kvs) {
        if (kv.first != key) continue;
        if (kv.second.empty() || kv.second[0] == '"') return def;   /* 字符串不算数字 */
        const char* b = kv.second.c_str();
        char* end = nullptr;
        double v = strtod(b, &end);
        if (end == b) return def;
        while (end && *end && jIsWs(*end)) ++end;                    /* 允许尾随空白 */
        if (end && *end != '\0') return def;                         /* 还有残留 → 非法 */
        return v;
    }
    return def;
}

bool jsonGetStringArray(const std::string& json, const char* key,
                        std::vector<std::string>* out) {
    if (out) out->clear();
    if (!key) return false;
    std::vector<std::pair<std::string, std::string>> kvs;
    if (!jsonParseObject(json, &kvs, nullptr)) return false;
    const std::string* raw = nullptr;
    for (const auto& kv : kvs) if (kv.first == key) { raw = &kv.second; break; }
    if (!raw || raw->size() < 2 || (*raw)[0] != '[') return false;
    /* 逐元素解析；元素必须都是字符串 */
    size_t i = 0;
    jSkipWs(*raw, i);
    if (i >= raw->size() || (*raw)[i] != '[') return false;
    ++i;
    jSkipWs(*raw, i);
    if (i < raw->size() && (*raw)[i] == ']') return true;   /* 空数组：合法 */
    for (;;) {
        jSkipWs(*raw, i);
        std::string v;
        if (!jParseString(*raw, i, &v)) return false;
        if (out) out->push_back(v);
        jSkipWs(*raw, i);
        if (i >= raw->size()) return false;
        if ((*raw)[i] == ',') { ++i; continue; }
        if ((*raw)[i] == ']') { ++i; return true; }
        return false;
    }
}

/* ======================= PNG 写出 =======================
 * 不依赖 zlib：使用 deflate 的 "stored"（无压缩）块 + 手写 CRC32/Adler32。
 * 体积偏大但完全自包含，插件与内核都可直接用。
 * ====================================================== */

static uint32_t crcTable[256];
static bool crcReady = false;

static void initCrc() {
    if (crcReady) return;
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crcTable[n] = c;
    }
    crcReady = true;
}

static uint32_t crc32buf(const uint8_t* buf, size_t len, uint32_t crc = 0xFFFFFFFFu) {
    initCrc();
    for (size_t i = 0; i < len; i++) crc = crcTable[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

static void putU32(std::string& s, uint32_t v) {
    s.push_back((char)((v >> 24) & 0xFF));
    s.push_back((char)((v >> 16) & 0xFF));
    s.push_back((char)((v >> 8) & 0xFF));
    s.push_back((char)(v & 0xFF));
}

static void putChunk(std::string& out, const char* type, const std::string& data) {
    putU32(out, (uint32_t)data.size());
    std::string t(type, 4);
    out += t;
    out += data;
    uint32_t crc = crc32buf((const uint8_t*)t.data(), 4);
    crc = crc32buf((const uint8_t*)data.data(), data.size(), crc);
    putU32(out, crc ^ 0xFFFFFFFFu);
}

static std::string zlibStore(const std::string& raw) {
    std::string z;
    z.push_back((char)0x78); /* CMF: deflate, 32K window */
    z.push_back((char)0x01); /* FLG: 无字典, 最快 */
    size_t off = 0;
    while (off < raw.size()) {
        size_t n = raw.size() - off;
        if (n > 65535) n = 65535;
        bool final = (off + n >= raw.size());
        z.push_back((char)(final ? 1 : 0));
        z.push_back((char)(n & 0xFF));
        z.push_back((char)((n >> 8) & 0xFF));
        uint16_t nlen = (uint16_t)(~n & 0xFFFF);
        z.push_back((char)(nlen & 0xFF));
        z.push_back((char)((nlen >> 8) & 0xFF));
        z.append(raw, off, n);
        off += n;
    }
    if (raw.empty()) {
        z.push_back(1);
        z.push_back(0); z.push_back(0);
        z.push_back(0xFF); z.push_back(0xFF);
    }
    uint32_t a = 1, b = 0;
    for (unsigned char c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    putU32(z, (b << 16) | a);
    return z;
}

static bool writePngImpl(const std::string& path, const uint8_t* pix, int w, int h,
                         int pitch, int channels) {
    if (!pix || w <= 0 || h <= 0) return false;
    /* pitch 必须容得下一行像素。少了这个检查，调用方给错行距时
     * `pix + y*pitch` 会读到相邻行之外（缓冲区小的时候就是越界读），
     * 而且产物还是一张"看着正常"的错图 —— 属于"配错了要大声失败"那一类。
     * （tests/unit/test_hash_png.cpp 有一条用例专门钉这个。） */
    if (pitch < w * channels) return false;
    std::string raw;
    raw.reserve((size_t)h * (1 + (size_t)w * channels));
    for (int y = 0; y < h; y++) {
        raw.push_back('\0'); /* filter: none */
        raw.append((const char*)(pix + (size_t)y * pitch), (size_t)w * channels);
    }

    std::string out;
    const char sig[8] = {(char)0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    out.append(sig, 8);

    std::string ihdr;
    putU32(ihdr, (uint32_t)w);
    putU32(ihdr, (uint32_t)h);
    ihdr.push_back(8);                              /* bit depth */
    ihdr.push_back((char)(channels == 4 ? 6 : 0));  /* color type: RGBA / Gray */
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    putChunk(out, "IHDR", ihdr);
    putChunk(out, "IDAT", zlibStore(raw));
    putChunk(out, "IEND", std::string());

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t wr = fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return wr == out.size();
}

bool writePngRGBA(const std::string& path, const uint8_t* rgba, int w, int h, int pitch) {
    return writePngImpl(path, rgba, w, h, pitch, 4);
}

bool writePngGray8(const std::string& path, const uint8_t* gray, int w, int h, int pitch) {
    return writePngImpl(path, gray, w, h, pitch, 1);
}

/* ======================= 抓屏取证 ======================= */

double rgbaColorRatioInRect(const uint8_t* rgba, int pitch, int w, int h,
                            int x, int y, int rw, int rh,
                            uint8_t r, uint8_t g, uint8_t b) {
    /* pitch 配错要大声失败，不能猜：按错的 pitch 走会读到相邻行之外，
     * 统计出一个"看着合理"的假数字（与 writePngImpl 的检查同一口径）。 */
    if (!rgba || w <= 0 || h <= 0 || pitch < w * 4) return -1.0;
    if (rw <= 0 || rh <= 0) return -1.0;

    /* 裁到图像内；完全落在外面就等于没量到 */
    if (x < 0) { rw += x; x = 0; }
    if (y < 0) { rh += y; y = 0; }
    if (x >= w || y >= h) return -1.0;
    if (x + rw > w) rw = w - x;
    if (y + rh > h) rh = h - y;
    if (rw <= 0 || rh <= 0) return -1.0;

    size_t hit = 0;
    for (int yy = 0; yy < rh; ++yy) {
        const uint8_t* row = rgba + (size_t)(y + yy) * (size_t)pitch + (size_t)x * 4;
        for (int xx = 0; xx < rw; ++xx, row += 4) {
            if (row[0] == r && row[1] == g && row[2] == b) ++hit;
        }
    }
    return (double)hit / ((double)rw * (double)rh);
}

/* ======================= YUV→RGB 色彩选择 ======================= */

YuvColorChoice chooseYuvColor(int avColorSpace, int avColorRange, int width, int height) {
    (void)width;
    YuvColorChoice r{};

    switch (avColorSpace) {
        case 1:  r.matrix = YUV_MATRIX_BT709;     break;   /* AVCOL_SPC_BT709 */
        case 5:  r.matrix = YUV_MATRIX_BT601;     break;   /* AVCOL_SPC_BT470BG（PAL/SECAM） */
        case 6:  r.matrix = YUV_MATRIX_BT601;     break;   /* AVCOL_SPC_SMPTE170M（NTSC） */
        case 7:  r.matrix = YUV_MATRIX_SMPTE240M; break;
        case 4:  r.matrix = YUV_MATRIX_FCC;       break;
        case 9:  r.matrix = YUV_MATRIX_BT2020;    break;   /* AVCOL_SPC_BT2020_NCL */
        case 10: r.matrix = YUV_MATRIX_BT2020;    break;   /* AVCOL_SPC_BT2020_CL */
        default:
            /* UNSPECIFIED / RGB / YCGCO / 未知：按分辨率走 FFmpeg 的 auto 规则。
             * 用 > 576 而不是 >= 576：576p(SD) 属于 SD，720p 起才算 HD。 */
            r.matrix = (height > 576) ? YUV_MATRIX_BT709 : YUV_MATRIX_BT601;
            break;
    }

    switch (avColorRange) {
        case 2:  r.fullRange = 1; break;   /* AVCOL_RANGE_JPEG：全范围 0-255 */
        case 1:  r.fullRange = 0; break;   /* AVCOL_RANGE_MPEG：有限范围 16-235 */
        default: r.fullRange = 0; break;   /* 未标注 → 有限范围（swscale 的默认口径） */
    }
    return r;
}

int sdlColorspaceFor(const YuvColorChoice& c, int* fellBack) {
    if (fellBack) *fellBack = 0;
    /* HDR：BT.2020 原色 + PQ 传输 → HDR10（PQ 是 SDL 唯一有枚举的 HDR 传输函数）。
     * 这一支优先于矩阵映射：HDR 的矩阵一定是 BT.2020，但逆之不成立。 */
    if (c.primaries == 9 /* AVCOL_PRI_BT2020 */ && c.trc == 16 /* AVCOL_TRC_SMPTE2084 */)
        return PP_SDL_CS_HDR10;
    /* HLG：BT.2020 原色 + HLG 传输。SDL 3.4.16 没有 HLG 枚举，只能用 BT.2020
     * （矩阵正确；传输函数按 G22，HLG 会轻微偏色 —— 已知限制，待 SDL 补枚举）。 */
    if (c.primaries == 9 /* AVCOL_PRI_BT2020 */ && c.trc == 18 /* AVCOL_TRC_ARIB_STD_B67 */) {
        const int full = c.fullRange ? 1 : 0;
        return full ? PP_SDL_CS_BT2020_FULL : PP_SDL_CS_BT2020_LIMITED;
    }
    /* SDR：矩阵 + 范围映射（P2-L1 前的行为，完全不变）。 */
    const int full = c.fullRange ? 1 : 0;
    switch (c.matrix) {
        case YUV_MATRIX_BT709:
            return full ? PP_SDL_CS_BT709_FULL : PP_SDL_CS_BT709_LIMITED;
        case YUV_MATRIX_BT2020:
            return full ? PP_SDL_CS_BT2020_FULL : PP_SDL_CS_BT2020_LIMITED;
        case YUV_MATRIX_BT601:
            return full ? PP_SDL_CS_BT601_FULL : PP_SDL_CS_BT601_LIMITED;
        default:
            /* FCC / SMPTE240M：SDL 没有对应枚举，退回 BT.601 并让调用方告警。 */
            if (fellBack) *fellBack = 1;
            return full ? PP_SDL_CS_BT601_FULL : PP_SDL_CS_BT601_LIMITED;
    }
}

/* ---------------- UTF-8 纯逻辑（P1-a） ---------------- */

int utf8Decode(const char* s, size_t len, uint32_t* out) {
    if (!s || len == 0) return 0;
    const unsigned char* p = (const unsigned char*)s;
    unsigned char c = p[0];
    uint32_t cp = 0xFFFD;          /* 非法序列一律替换字符 */
    int need = 0;

    if (c < 0x80)        { cp = c;        need = 0; }
    else if (c >= 0xC2 && c <= 0xDF) { cp = c & 0x1Fu; need = 1; }
    else if (c >= 0xE0 && c <= 0xEF) { cp = c & 0x0Fu; need = 2; }
    else if (c >= 0xF0 && c <= 0xF4) { cp = c & 0x07u; need = 3; }
    else {
        /* 0x80..0xC1（多余续字节/超长编码）与 0xF5..0xFF：吃 1 字节，给替换字符 */
        if (out) *out = cp;
        return 1;
    }
    if ((size_t)need + 1 > len) {
        /* 截断的序列：必须给替换字符。
         * 注意 cp 此时已被写成首字节的有效位（如 0xE6 → 0x06），
         * 直接返回它等于"把半个码点当成一个码点"，会让后面的宽度与查字形全错。 */
        if (out) *out = 0xFFFD;
        return 1;
    }
    for (int i = 1; i <= need; i++) {
        unsigned char cc = p[i];
        if ((cc & 0xC0) != 0x80) {  /* 续字节不合规 */
            if (out) *out = 0xFFFD;
            return i;               /* 只吃掉已确认的前缀 */
        }
        cp = (cp << 6) | (cc & 0x3Fu);
    }
    /* 超长编码与代理区（U+D800..DFFF）都判非法 —— 否则会被当成有效码点送进字体 */
    if ((need == 1 && cp < 0x80) ||
        (need == 2 && cp < 0x800) ||
        (need == 3 && cp < 0x10000) ||
        (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
        cp = 0xFFFD;
    }
    if (out) *out = cp;
    return need + 1;
}

bool isWideCodepoint(uint32_t cp) {
    /* 只判"东亚宽 W / 全宽 F"里本项目真正会遇到的区间，够用且不误伤：
     *  - CJK 部首/汉字/兼容/扩展 A（其余扩展区罕见，也按宽处理无妨）
     *  - 平假名/片假名、韩文音节、全角 ASCII 与全角标点
     *  - 个别区间（如 U+3000 表意空格、U+2010 附近）单独列出 */
    if (cp >= 0x1100 && cp <= 0x115F) return true;   /* 韩文字母 */
    if (cp >= 0x2E80 && cp <= 0xA4CF) return true;   /* CJK 部首…彝文 */
    if (cp >= 0xAC00 && cp <= 0xD7A3) return true;   /* 韩文音节 */
    if (cp >= 0xF900 && cp <= 0xFAFF) return true;   /* CJK 兼容表意 */
    if (cp >= 0xFE30 && cp <= 0xFE4F) return true;   /* CJK 兼容形式 */
    if (cp >= 0xFF01 && cp <= 0xFF60) return true;   /* 全角 ASCII/标点 */
    if (cp >= 0xFFE0 && cp <= 0xFFE6) return true;   /* 全角符号 */
    if (cp >= 0x20000 && cp <= 0x3FFFD) return true; /* CJK 扩展 B.. */
    return false;
}

int utf8DisplayWidth(const char* s) {
    if (!s) return 0;
    size_t len = strlen(s);
    int w = 0;
    size_t i = 0;
    while (i < len) {
        uint32_t cp = 0;
        int n = utf8Decode(s + i, len - i, &cp);
        if (n <= 0) break;
        i += (size_t)n;
        w += isWideCodepoint(cp) ? 2 : 1;
    }
    return w;
}

} // namespace pp
