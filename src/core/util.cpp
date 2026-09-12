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
    char line[4400];
    snprintf(line, sizeof(line), "[%02d:%02d:%02d][%s] %s\n",
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, levelName(lv).c_str(), body);

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

/* ======================= 极简 JSON ======================= */

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

static bool findKey(const std::string& json, const char* key, size_t& pos) {
    std::string pat = std::string("\"") + key + "\"";
    pos = json.find(pat);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + pat.size());
    if (pos == std::string::npos) return false;
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    return true;
}

std::string jsonGetString(const std::string& json, const char* key, const std::string& def) {
    size_t p;
    if (!findKey(json, key, p)) return def;
    if (p >= json.size() || json[p] != '"') return def;
    p++;
    std::string o;
    while (p < json.size() && json[p] != '"') {
        if (json[p] == '\\' && p + 1 < json.size()) {
            char n = json[p + 1];
            switch (n) {
                case 'n': o.push_back('\n'); break;
                case 'r': o.push_back('\r'); break;
                case 't': o.push_back('\t'); break;
                case '"': o.push_back('"');  break;
                case '\\': o.push_back('\\'); break;
                case '/': o.push_back('/');  break;
                default: o.push_back(n);
            }
            p += 2;
        } else {
            o.push_back(json[p++]);
        }
    }
    return o;
}

double jsonGetNumber(const std::string& json, const char* key, double def) {
    size_t p;
    if (!findKey(json, key, p)) return def;
    char* end = nullptr;
    double v = strtod(json.c_str() + p, &end);
    return end == json.c_str() + p ? def : v;
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

} // namespace pp
