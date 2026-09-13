/* radio_bridge — private-radio-stations 转播服务桥接插件
 * ---------------------------------------------------------------------------
 * 把 G:\repos\private-radio-stations（Node+Cordis 转播电台）的「转播状态」带进
 * PluginPlayer，让本机监听端变成电台的一部分：
 *
 *   1. 后台 worker 线程轮询 radio 服务的 GET /api/status（winsock 直连，2s 超时）。
 *      **HTTP 绝不进主线程** —— 主循环 tick 回调只消费快照，渲染不被网络卡住。
 *   2. 台名/状态推进标题栏（set_window_subtitle）与 OSD 左下角一行。
 *   3. 状态变化（开始/停止转播、failover 切源）→ publish "radio.station" 事件；
 *      开着 radio.follow 1 时，radio 一开始转播本机自动 open /stream 收听、
 *      一停就自动停 —— 无人值守监听。
 *   4. 命令：radio.status / radio.base / radio.poll / radio.follow / radio.playing。
 *   5. 服务：radio_bridge（status_json / station_name / mode，get_service 取）。
 *
 * 安全与红线：
 *   - 不升 ABI：只读 PP_HostApi 既有字段，服务结构定义在本插件内。
 *   - auto-follow 的 open_media 走内核 SSRF 门禁：宿主要带 --allow-network-input
 *     才能打开 http 流（radio 场景只喂 127.0.0.1，集成侧自己约束）。
 *   - worker 线程不调用任何 host API（SDK 约定 HostApi 面向主线程）。
 */
#include <winsock2.h>
#include <ws2tcpip.h>

#include "player_plugin.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <chrono>

static PP_Host*          g_host = nullptr;
static const PP_HostApi* g_api  = nullptr;

/* ---------------- 配置（radio.base/radio.poll/radio.follow，持久化到 ini） ---------------- */
static std::mutex        g_cfgMu;
static std::string       g_base = "http://127.0.0.1:8000"; /* 与 private-radio-stations 默认 HTTP 端口一致；无尾斜杠；只支持 http://host[:port] */
static std::atomic<int>  g_pollSec{3};
static std::atomic<int>  g_follow{0};

/* ---------------- 快照（worker 写，主线程读） ---------------- */
struct Snapshot {
    bool        ok = false;
    std::string mode;        /* idle / relay */
    std::string name;        /* 当前台名 */
    std::string url;         /* 当前源 */
    std::string error;       /* radio 最近错误 */
    int         hlsSegs = -1;
};
static std::mutex  g_snapMu;
static Snapshot    g_snap;

/* ---------------- worker ---------------- */
static std::thread             g_th;
static std::atomic<bool>       g_run{false};
static std::mutex              g_cvMu;
static std::condition_variable g_cv;
static std::mutex              g_errMu;
static std::string             g_lastErr;      /* 最近一次 HTTP 失败阶段 + WSA 错误码（诊断） */

static void setLastErr(const char* stage) {
    std::lock_guard<std::mutex> lk(g_errMu);
    g_lastErr = stage;
}

/* ---------------- 主线程状态 ---------------- */
static std::string g_lastMode;
static std::string g_lastName;
static std::string g_lastUrl;
static bool        g_followOpened = false;   /* follow 替用户开过 /stream */
static std::string g_windowText;             /* 当前标题栏后缀 */
static int         g_subTick = 0;
static int         g_subOsd  = 0;

/* ================= 极简 JSON 取值（radio /api/status 专用，best-effort） ================= */
/* 跳过空白；返回下标 */
static size_t skipWs(const std::string& s, size_t i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
    return i;
}
/* 在 s 中从 i 起解析一个 JSON 字符串（含转义处理），写 *out，返回结尾下标（越界返回 npos） */
static size_t parseJsonString(const std::string& s, size_t i, std::string* out) {
    if (i >= s.size() || s[i] != '"') return std::string::npos;
    i++;
    out->clear();
    while (i < s.size()) {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size()) {
            char n = s[i + 1];
            switch (n) {
                case '"':  out->push_back('"');  break;
                case '\\': out->push_back('\\'); break;
                case '/':  out->push_back('/');  break;
                case 'n':  out->push_back('\n'); break;
                case 't':  out->push_back('\t'); break;
                case 'r':  out->push_back('\r'); break;
                case 'u':  out->append("\\u");  break;   /* 罕见：不精确解码，保留转义 */
                default:   out->push_back(n);   break;
            }
            i += 2;
            continue;
        }
        if (c == '"') return i + 1;
        out->push_back(c);
        i++;
    }
    return std::string::npos;
}
/* 顶层键取字符串：找 `"key"` → `:` → 字符串 / null。命中返回 true（null → 空串）。 */
static bool jsonGetStr(const std::string& s, const std::string& key, std::string* out) {
    std::string needle = "\"" + key + "\"";
    size_t i = s.find(needle);
    if (i == std::string::npos) return false;
    i = skipWs(s, i + needle.size());
    if (i >= s.size() || s[i] != ':') return false;
    i = skipWs(s, i + 1);
    if (i >= s.size()) return false;
    if (s[i] == '"') {
        size_t e = parseJsonString(s, i, out);
        return e != std::string::npos;
    }
    if (s.compare(i, 4, "null") == 0) { *out = ""; return true; }
    return false;
}
/* 取 `"objKey"` 的对象体（从 { 到配对 }，含花括号），再在其内取字符串 innerKey。 */
static bool jsonGetObjStr(const std::string& s, const std::string& objKey,
                          const std::string& innerKey, std::string* out) {
    std::string needle = "\"" + objKey + "\"";
    size_t i = s.find(needle);
    if (i == std::string::npos) return false;
    i = skipWs(s, i + needle.size());
    if (i >= s.size() || s[i] != ':') return false;
    i = skipWs(s, i + 1);
    if (i >= s.size() || s[i] != '{') return false;
    int depth = 0;
    size_t end = i;
    for (; end < s.size(); ++end) {
        char c = s[end];
        if (c == '"') { /* 跳过字符串 */
            end++;
            while (end < s.size() && s[end] != '"') {
                if (s[end] == '\\') end++;
                end++;
            }
            continue;
        }
        if (c == '{') depth++;
        else if (c == '}') { depth--; if (depth == 0) break; }
    }
    if (end >= s.size()) return false;
    return jsonGetStr(s.substr(i, end - i + 1), innerKey, out);
}
/* 取 `"objKey"` 对象内的数字键（int）。 */
static bool jsonGetObjNum(const std::string& s, const std::string& objKey,
                          const std::string& innerKey, int* out) {
    std::string needle = "\"" + objKey + "\"";
    size_t i = s.find(needle);
    if (i == std::string::npos) return false;
    i = skipWs(s, i + needle.size());
    if (i >= s.size() || s[i] != ':') return false;
    i = skipWs(s, i + 1);
    if (i >= s.size() || s[i] != '{') return false;
    int depth = 0;
    size_t end = i;
    for (; end < s.size(); ++end) {
        char c = s[end];
        if (c == '"') {
            end++;
            while (end < s.size() && s[end] != '"') {
                if (s[end] == '\\') end++;
                end++;
            }
            continue;
        }
        if (c == '{') depth++;
        else if (c == '}') { depth--; if (depth == 0) break; }
    }
    if (end >= s.size()) return false;
    std::string sub = s.substr(i, end - i + 1);
    size_t k = sub.find("\"" + innerKey + "\"");
    if (k == std::string::npos) return false;
    k = skipWs(sub, k + innerKey.size() + 2);
    if (k >= sub.size() || sub[k] != ':') return false;
    k = skipWs(sub, k + 1);
    if (k >= sub.size()) return false;
    if (sub.compare(k, 4, "null") == 0) return false;
    *out = atoi(sub.c_str() + k);
    return true;
}

/* ================= HTTP GET（worker 线程专用，winsock） ================= */
/* 从 "http://host[:port][/path]" 拆出 hostPort 与剩余 path。非法返回 false。 */
static bool splitBase(const std::string& base, std::string* hostPort, std::string* restPath) {
    if (base.compare(0, 7, "http://") != 0) return false;
    std::string t = base.substr(7);
    size_t slash = t.find('/');
    if (slash == std::string::npos) {
        *hostPort = t;
        *restPath = "";
    } else {
        *hostPort = t.substr(0, slash);
        *restPath = t.substr(slash);
    }
    return !hostPort->empty();
}

/* GET path，返回 body（含状态行检查）；超时/失败返回 false。timeoutMs 是整体上限。 */
static bool httpGet(const std::string& hostPort, const std::string& path,
                    std::string* out, int timeoutMs) {
    /* 拆 host:port */
    std::string host = hostPort;
    std::string port = "80";
    size_t colon = hostPort.rfind(':');
    if (colon != std::string::npos) {
        host = hostPort.substr(0, colon);
        port = hostPort.substr(colon + 1);
    }
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* ai = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &ai) != 0 || !ai) {
        if (ai) freeaddrinfo(ai);
        setLastErr("dns");
        return false;
    }
    SOCKET sk = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (sk == INVALID_SOCKET) { freeaddrinfo(ai); setLastErr("socket"); return false; }
    /* 非阻塞 connect + select 限时（Windows 下 SO_SNDTIMEO 不作用于 connect） */
    u_long nb = 1;
    ioctlsocket(sk, FIONBIO, &nb);
    int cr = ::connect(sk, ai->ai_addr, (int)ai->ai_addrlen);
    freeaddrinfo(ai);
    bool ok = false;
    if (cr == SOCKET_ERROR) {
        int e = WSAGetLastError();
        if (e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEINVAL) {
            fd_set wf;
            FD_ZERO(&wf);
            FD_SET(sk, &wf);
            timeval tv;
            tv.tv_sec = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;
            if (::select(0, nullptr, &wf, nullptr, &tv) == 1) {
                int soerr = 0;
                int len = sizeof(soerr);
                getsockopt(sk, SOL_SOCKET, SO_ERROR, (char*)&soerr, &len);
                ok = (soerr == 0);
            }
        }
    } else {
        ok = true;
    }
    if (!ok) {
        ::closesocket(sk);
        setLastErr("connect");
        return false;
    }
    nb = 0;
    ioctlsocket(sk, FIONBIO, &nb);
    DWORD tmo = (DWORD)timeoutMs;
    setsockopt(sk, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    setsockopt(sk, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tmo, sizeof(tmo));

    std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + hostPort +
                      "\r\nUser-Agent: pluginplayer/radio_bridge\r\n"
                      "Accept: application/json\r\nConnection: close\r\n\r\n";
    int sent = ::send(sk, req.c_str(), (int)req.size(), 0);
    if (sent <= 0) { ::closesocket(sk); setLastErr("send"); return false; }

    std::string resp;
    char buf[8192];
    for (;;) {
        int n = ::recv(sk, buf, sizeof(buf), 0);
        if (n > 0) {
            resp.append(buf, (size_t)n);
            if (resp.size() > 512u * 1024u) break;   /* 上限 512KB */
        } else {
            break;
        }
    }
    ::closesocket(sk);

    if (resp.empty()) { setLastErr("no-response"); return false; }
    size_t hdrEnd = resp.find("\r\n\r\n");
    if (hdrEnd == std::string::npos) { setLastErr("no-header-end"); return false; }
    /* 状态行 200？ */
    if (resp.compare(0, 7, "HTTP/1.") != 0) { setLastErr("bad-status-line"); return false; }
    size_t sp = resp.find(' ');
    if (sp == std::string::npos || resp.compare(sp + 1, 3, "200") != 0) {
        setLastErr(("status:" + resp.substr(sp + 1, 3)).c_str());
        return false;
    }
    *out = resp.substr(hdrEnd + 4);
    setLastErr("");
    return true;
}

/* 解析 /api/status 到快照 */
static void parseStatus(const std::string& body, Snapshot* s) {
    s->ok = true;
    jsonGetStr(body, "mode", &s->mode);
    jsonGetStr(body, "error", &s->error);
    jsonGetObjStr(body, "current", "name", &s->name);
    jsonGetObjStr(body, "current", "url", &s->url);
    jsonGetObjNum(body, "hls", "segments", &s->hlsSegs);
}

/* ================= worker 线程 ================= */
static void workerLoop() {
    while (g_run.load()) {
        Snapshot s;
        {
            std::lock_guard<std::mutex> lk(g_cfgMu);
            std::string hp, rest;
            (void)rest;
            if (splitBase(g_base, &hp, &rest)) {
                std::string body;
                if (httpGet(hp, "/api/status", &body, 2000)) {
                    parseStatus(body, &s);
                }
            }
        }
        {
            std::lock_guard<std::mutex> lk(g_snapMu);
            g_snap = s;
        }
        std::unique_lock<std::mutex> lk(g_cvMu);
        g_cv.wait_for(lk, std::chrono::seconds(g_pollSec.load()),
                      [] { return !g_run.load(); });
    }
}

static void startWorker() {
    if (g_run.load()) return;
    g_run = true;
    g_th = std::thread(&workerLoop);
}

static void stopWorker() {
    if (!g_run.load()) return;
    g_run = false;
    g_cv.notify_all();
    if (g_th.joinable()) g_th.join();   /* HTTP 最多 2s 超时，join 有界 */
}

/* ================= 事件回调（主线程） ================= */
static std::string jsonEscapeStr(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:   o.push_back(c); break;
        }
    }
    return o;
}

static void updateWindowText(const Snapshot& s) {
    std::string text;
    if (!s.ok) {
        text = "";
    } else if (s.mode == "relay") {
        text = "RADIO: " + (s.name.empty() ? "(未知台名)" : s.name);
        if (!s.error.empty()) text += " [err]";
    } else if (s.mode == "idle") {
        text = "RADIO: 空闲";
    } else {
        text = "RADIO: " + s.mode;
    }
    if (text == g_windowText) return;
    g_windowText = text;
    g_api->set_window_subtitle(g_host, text.empty() ? nullptr : text.c_str());
}

/* follow 的核心：保证"转播中 → 本机在听 /stream"。
 *  - 已打开且是 /stream：若被暂停（idle 时自动停过）则 play() 恢复；
 *  - 没打开 / 打开的是别的媒体：open（失败按 3s 节流自动重试，扛转播重启的瞬时竞态）。 */
static uint64_t g_lastOpenAttemptMs = 0;

static void followEnsureOpen(PP_Host* h) {
    Snapshot s;
    {
        std::lock_guard<std::mutex> lk(g_snapMu);
        s = g_snap;
    }
    if (!s.ok || s.mode != "relay") return;
    std::string streamUrl;
    {
        std::lock_guard<std::mutex> lk(g_cfgMu);
        streamUrl = g_base + "/stream";
    }
    PP_MediaInfo mi{};
    bool have = g_api->get_media_info(h, &mi) == 0 && mi.path[0] != '\0';
    if (have && streamUrl == mi.path) {
        if (!g_followOpened) g_followOpened = true;
        if (g_api->is_paused(h)) {
            g_api->play(h);
            g_api->log(h, PP_LOG_INFO, "radio_bridge: 转播恢复，继续播放 %s（%s）",
                       streamUrl.c_str(), s.name.empty() ? "(未知台名)" : s.name.c_str());
        }
        return;
    }
    uint64_t now = g_api->ticks_ms(h);
    if (now - g_lastOpenAttemptMs < 3000) return;   /* 重试节流 */
    g_lastOpenAttemptMs = now;
    if (g_api->open_media(h, streamUrl.c_str()) == 0) {
        g_followOpened = true;
        g_api->log(h, PP_LOG_INFO, "radio_bridge: 自动打开 %s（%s）",
                   streamUrl.c_str(), s.name.empty() ? "(未知台名)" : s.name.c_str());
    } else {
        g_api->log(h, PP_LOG_WARN,
                   "radio_bridge: 自动打开失败（3s 后自动重试；网络输入需 --allow-network-input）: %s",
                   streamUrl.c_str());
    }
}

static int onTick(PP_EventCtx* ctx) {
    (void)ctx;
    Snapshot s;
    {
        std::lock_guard<std::mutex> lk(g_snapMu);
        s = g_snap;
    }
    if (!s.ok) return 0;

    updateWindowText(s);

    /* 状态/台名/源任一变化 → 事件 */
    bool changed = (s.mode != g_lastMode) || (s.name != g_lastName) || (s.url != g_lastUrl);
    if (changed) {
        char json[2048];
        snprintf(json, sizeof(json),
                 "{\"mode\":\"%s\",\"name\":\"%s\",\"url\":\"%s\"}",
                 jsonEscapeStr(s.mode).c_str(), jsonEscapeStr(s.name).c_str(),
                 jsonEscapeStr(s.url).c_str());
        g_api->publish(g_host, "radio.station", json);
        g_api->log(g_host, PP_LOG_INFO,
                   "radio_bridge: 状态变化 mode=%s name=%s url=%s",
                   s.mode.c_str(), s.name.c_str(), s.url.c_str());
    }

    /* auto-follow：转播中 → 保证在听 /stream（暂停恢复/失败重试）；
     * 转播停止 → 自动停（只停自己开的） */
    if (g_follow.load() && s.ok) {
        if (s.mode == "relay") {
            followEnsureOpen(g_host);
        } else if (g_lastMode == "relay" && g_followOpened) {
            g_api->stop(g_host);
            g_followOpened = false;
            g_api->log(g_host, PP_LOG_INFO, "radio_bridge: 转播停止，自动停止播放");
        }
    }

    g_lastMode = s.mode;
    g_lastName = s.name;
    g_lastUrl = s.url;
    return 0;
}

static int onOsdDraw(PP_EventCtx* ctx) {
    PP_Host* h = ctx->host;
    Snapshot s;
    {
        std::lock_guard<std::mutex> lk(g_snapMu);
        s = g_snap;
    }
    /* 文案 + 状态色：转播=绿、空闲=灰、连不上=橙（一眼可辨，不依赖读文字） */
    std::string text;
    PP_Color accent;
    if (!s.ok) {
        std::string base;
        {
            std::lock_guard<std::mutex> lk(g_cfgMu);
            base = g_base;
        }
        text = "RADIO: 服务不可达 (" + base + ")";
        accent = PP_RGB(232, 150, 60);
    } else if (s.mode == "relay") {
        text = "RADIO: " + (s.name.empty() ? "(未知台名)" : s.name);
        accent = PP_RGB(90, 210, 120);
    } else if (s.mode == "idle") {
        text = "RADIO: 空闲";
        accent = PP_RGB(150, 150, 150);
    } else {
        text = "RADIO: " + s.mode;
        accent = PP_RGB(150, 150, 150);
    }

    if (!g_api || !g_api->osd || !g_api->osd->size) return 0;
    float w = 0, hh = 0;
    /* 实测 size() 成功返回 1（写入 w/h），但 SDK 注释写反；安全做法是忽略返回值、只校验 w/h */
    g_api->osd->size(h, &w, &hh);
    if (w < 64.0f || hh < 40.0f) return 0;      /* 画布太小就不画，避免出界 */

    /* 面板宽度按文字实测宽度自适应（text_width 在真字体与点阵回退下都返回真实像素） */
    const float scale = 1.0f;
    const float padX = 9.0f, dot = 9.0f, gap = 8.0f;
    float tw = g_api->osd->text_width(h, text.c_str(), scale);
    float boxH = 28.0f;
    float boxW = padX * 2.0f + dot + gap + tw + 6.0f;
    if (boxW > w - 16.0f) boxW = w - 16.0f;    /* 不超出画布 */
    float x = 8.0f;
    float y = hh - boxH - 8.0f;

    g_api->osd->panel(h, x, y, boxW, boxH, PP_RGBA(0, 0, 0, 170), accent);
    g_api->osd->fill_rect(h, x + padX, y + (boxH - dot) * 0.5f, dot, dot, accent);
    g_api->osd->text(h, x + padX + dot + gap,
                     y + (boxH - g_api->osd->line_height(h, scale)) * 0.5f,
                     text.c_str(), accent, scale);
    return 0;
}

/* ================= 命令 ================= */
static int cmdStatus(PP_Host* h, int argc, const char** argv, void* user) {
    (void)argv; (void)user;
    if (argc > 0) { g_api->log(h, PP_LOG_WARN, "用法: radio.status（无参数）"); return -1; }
    Snapshot s;
    {
        std::lock_guard<std::mutex> lk(g_snapMu);
        s = g_snap;
    }
    std::string base;
    {
        std::lock_guard<std::mutex> lk(g_cfgMu);
        base = g_base;
    }
    if (!s.ok) {
        std::string err;
        {
            std::lock_guard<std::mutex> lk(g_errMu);
            err = g_lastErr;
        }
        g_api->log(h, PP_LOG_WARN, "radio_bridge: 服务不可达（base=%s, 最后失败: %s）",
                   base.c_str(), err.empty() ? "尚未尝试" : err.c_str());
        return 1;
    }
    g_api->log(h, PP_LOG_INFO,
               "radio_bridge: base=%s poll=%ds follow=%d | mode=%s name=%s url=%s hls_segs=%d%s%s",
               base.c_str(), g_pollSec.load(), g_follow.load(),
               s.mode.c_str(), s.name.empty() ? "-" : s.name.c_str(),
               s.url.empty() ? "-" : s.url.c_str(), s.hlsSegs,
               s.error.empty() ? "" : " error=", s.error.c_str());
    return 0;
}

static int cmdBase(PP_Host* h, int argc, const char** argv, void* user) {
    (void)user;
    if (argc < 1) { g_api->log(h, PP_LOG_WARN, "用法: radio.base < http://host:port >"); return -1; }
    std::string hp, rest;
    if (!splitBase(argv[0], &hp, &rest)) {
        g_api->log(h, PP_LOG_WARN, "radio_bridge: 非法 base（要 http://host[:port]）: %s", argv[0]);
        return -1;
    }
    {
        std::lock_guard<std::mutex> lk(g_cfgMu);
        g_base = argv[0];
    }
    g_api->config_set(h, "base", argv[0]);
    g_cv.notify_all();          /* 让 worker 立刻用新 base 抓一次 */
    g_api->log(h, PP_LOG_INFO, "radio_bridge: base = %s", g_base.c_str());
    return 0;
}

static int cmdPoll(PP_Host* h, int argc, const char** argv, void* user) {
    (void)user;
    if (argc < 1) { g_api->log(h, PP_LOG_WARN, "用法: radio.poll < 1..60 秒 >"); return -1; }
    int v = atoi(argv[0]);
    if (v < 1 || v > 60) { g_api->log(h, PP_LOG_WARN, "radio_bridge: 间隔须在 1..60 秒"); return -1; }
    g_pollSec = v;
    g_api->config_set(h, "poll", argv[0]);
    g_cv.notify_all();
    g_api->log(h, PP_LOG_INFO, "radio_bridge: 轮询间隔 = %d s", v);
    return 0;
}

static int cmdFollow(PP_Host* h, int argc, const char** argv, void* user) {
    (void)user;
    if (argc < 1) { g_api->log(h, PP_LOG_WARN, "用法: radio.follow < 0|1 >"); return -1; }
    int v = atoi(argv[0]);
    if (v != 0 && v != 1) { g_api->log(h, PP_LOG_WARN, "radio_bridge: follow 只能 0 或 1"); return -1; }
    g_follow = v;
    if (!v) g_followOpened = false;
    g_api->config_set(h, "follow", argv[0]);
    g_api->log(h, PP_LOG_INFO, "radio_bridge: auto-follow = %s（开转播自动收听）",
               v ? "开" : "关");
    if (v) followEnsureOpen(h);   /* 开启时若已在转播，立即开听/恢复 */
    return 0;
}

static int cmdPlaying(PP_Host* h, int argc, const char** argv, void* user) {
    (void)user;
    if (argc < 1) { g_api->log(h, PP_LOG_WARN, "用法: radio.playing < http(s) 流地址 >"); return -1; }
    int rc = g_api->open_media(h, argv[0]);
    if (rc == 0) {
        g_followOpened = false;   /* 手动开的，follow 停止时不动它 */
        g_api->log(h, PP_LOG_INFO, "radio_bridge: 已打开 %s", argv[0]);
        return 0;
    }
    g_api->log(h, PP_LOG_WARN, "radio_bridge: 打开失败（rc=%d，网络输入需 --allow-network-input）", rc);
    return -1;
}

/* ================= 服务（get_service("radio_bridge")） ================= */
static const char* svcStatusJson(void) {
    Snapshot s;
    {
        std::lock_guard<std::mutex> lk(g_snapMu);
        s = g_snap;
    }
    static char buf[2048];
    snprintf(buf, sizeof(buf),
             "{\"ok\":%d,\"mode\":\"%s\",\"name\":\"%s\",\"url\":\"%s\",\"error\":\"%s\",\"hls_segments\":%d}",
             s.ok ? 1 : 0, jsonEscapeStr(s.mode).c_str(), jsonEscapeStr(s.name).c_str(),
             jsonEscapeStr(s.url).c_str(), jsonEscapeStr(s.error).c_str(), s.hlsSegs);
    return buf;
}
static const char* svcStationName(void) {
    std::lock_guard<std::mutex> lk(g_snapMu);
    return g_snap.name.c_str();   /* 调用方在服务生命周期内使用，插件卸载即失效（与既有服务同口径） */
}
static const char* svcMode(void) {
    std::lock_guard<std::mutex> lk(g_snapMu);
    return g_snap.mode.c_str();
}

typedef struct RadioBridgeService {
    const char* (*status_json)(void);
    const char* (*station_name)(void);
    const char* (*mode)(void);
} RadioBridgeService;

static RadioBridgeService g_service = { &svcStatusJson, &svcStationName, &svcMode };

/* ================= 生命周期 ================= */
static int onLoad(PP_Host* host, const PP_HostApi* api) {
    if (api->abi_version != PP_ABI_VERSION || api->struct_size < sizeof(PP_HostApi)) return -1;
    g_host = host;
    g_api = api;

    /* winsock：插件自己管理引用计数（控制通道已先行 cleanup，卸载顺序在其后，安全） */
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return -1;
    }

    const char* b = api->config_get(host, "base", "");
    const char* p = api->config_get(host, "poll", "");
    const char* f = api->config_get(host, "follow", "");
    if (b && *b) { std::string hp, rest; if (splitBase(b, &hp, &rest)) g_base = b; }
    if (p && *p) { int v = atoi(p); if (v >= 1 && v <= 60) g_pollSec = v; }
    if (f && *f) { int v = atoi(f); if (v == 0 || v == 1) g_follow = v; }

    api->register_service(host, "radio_bridge", "1.0.0", &g_service);
    api->register_command(host, "radio.status",
                          "radio.status                      转播状态（轮询自 radio 服务）",
                          &cmdStatus, nullptr);
    api->register_command(host, "radio.base",
                          "radio.base < http://host:port >   设置 radio 服务地址",
                          &cmdBase, nullptr);
    api->register_command(host, "radio.poll",
                          "radio.poll < 1..60 >              轮询间隔（秒）",
                          &cmdPoll, nullptr);
    api->register_command(host, "radio.follow",
                          "radio.follow < 0|1 >             开转播自动收听 / 停转播自动停",
                          &cmdFollow, nullptr);
    api->register_command(host, "radio.playing",
                          "radio.playing < url >             手动播放指定流",
                          &cmdPlaying, nullptr);

    startWorker();
    return 0;
}

static int onStart(PP_Host* host) {
    g_subTick = g_api->subscribe(host, PP_TOPIC_APP_TICK, &onTick, nullptr);
    g_subOsd  = g_api->subscribe(host, PP_TOPIC_OSD_DRAW, &onOsdDraw, nullptr);
    g_api->log(host, PP_LOG_INFO,
               "radio_bridge 启动（base=%s, 轮询 %ds, follow=%d, 订阅 tick=%d osd=%d）",
               g_base.c_str(), g_pollSec.load(), g_follow.load(), g_subTick, g_subOsd);
    return 0;
}

static void onStop(PP_Host* host) {
    if (g_subTick) { g_api->unsubscribe(host, g_subTick); g_subTick = 0; }
    if (g_subOsd)  { g_api->unsubscribe(host, g_subOsd);  g_subOsd = 0; }
    if (!g_windowText.empty()) {
        g_api->set_window_subtitle(host, nullptr);
        g_windowText.clear();
    }
    g_api->log(host, PP_LOG_INFO, "radio_bridge 停止");
}

static void onUnload(PP_Host* host) {
    stopWorker();                       /* join 有界：HTTP 2s 超时 + cv 立即唤醒 */
    WSACleanup();
    g_api->log(host, PP_LOG_INFO, "radio_bridge 卸载");
}

static PP_Plugin g_plugin = {
    PP_ABI_VERSION,
    "radio_bridge",
    "1.0.0",
    "PluginPlayer 内置",
    "桥接 private-radio-stations 转播服务：状态轮询/标题栏台名/OSD/事件/自动跟随收听",
    0,
    &onLoad, &onStart, &onStop, &onUnload,
};

PP_PLUGIN_DECL(&g_plugin)
