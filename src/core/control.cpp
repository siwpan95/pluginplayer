/* control.cpp — 宿主控制通道实现（Winsock2 + 换行分隔 JSON）
 *
 * 安全加固要点（对应上线前安全审计 F-01 / F-03 / F-05 / F-06 / F-07 / F-12）：
 *   · 强制令牌认证（256-bit，系统 CSPRNG 生成），未认证不接受任何方法
 *   · 单条报文 4 MB 硬上限（消除无上限行缓冲的内存耗尽 DoS）
 *   · 行扫描改为增量扫描（消除每轮从头 find 的 O(n²)）
 *   · 并发连接上限 8；recv/send 超时
 *   · 不再使用 SO_REUSEADDR，改用 Windows 的 SO_EXCLUSIVEADDRUSE
 *     （防止他人强绑同一端口造成控制端口劫持）
 *   · 客户端对象改用 shared_ptr 生命周期，消除 stop() 里
 *     closesocket → detach → delete 与 clientLoop 之间的 use-after-free
 *   · 事件只推给已认证连接（未认证连接不再默认收到全部事件流）
 */
#include "control.h"
#include "app.h"
#include "util.h"

#ifndef _WIN32
#error "PluginPlayer 的控制通道目前只实现了 Windows(Winsock2)"
#endif

/* winsock2.h 必须先于 windows.h */
#include <winsock2.h>
#include <ws2tcpip.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#include <cstring>
#include <cstdio>
#include <random>
#include <chrono>
#include <algorithm>

namespace pp {

static bool g_wsaReady = false;

/* 定长时间比较，避免用响应时间侧信道逐字节猜令牌 */
static bool tokenEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

RpcServer::~RpcServer() { stop(); }

std::string RpcServer::generateToken() {
    unsigned char buf[32] = {0};
    bool ok = (BCryptGenRandom(nullptr, buf, (ULONG)sizeof(buf),
                               BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0);
    if (!ok) {
        /* 兜底：CSPRNG 不可用时，用 random_device + 高精度时钟混合。
         * 强度下降但仍非固定值，且会打日志提醒。 */
        std::random_device rd;
        auto ticks = (uint64_t)std::chrono::high_resolution_clock::now()
                         .time_since_epoch().count();
        for (size_t i = 0; i < sizeof(buf); ++i) {
            uint64_t mix = (uint64_t)rd() ^ (ticks * 0x9E3779B97F4A7C15ull) ^ (i * 0xBF58476D1CE4E5B9ull);
            buf[i] = (unsigned char)((mix >> ((i % 8) * 8)) & 0xFF);
        }
        PP_WARN("BCryptGenRandom 不可用，令牌改用混合熵源（强度下降）");
    }
    static const char* hex = "0123456789abcdef";
    std::string s;
    s.reserve(sizeof(buf) * 2);
    for (size_t i = 0; i < sizeof(buf); ++i) {
        s.push_back(hex[buf[i] >> 4]);
        s.push_back(hex[buf[i] & 0x0F]);
    }
    return s;
}

void RpcServer::sendLine(uintptr_t sock, const std::string& s) {
    std::string out = s;
    out.push_back('\n');
    size_t off = 0;
    while (off < out.size()) {
        int n = ::send((SOCKET)sock, out.data() + off, (int)(out.size() - off), 0);
        if (n <= 0) return;
        off += (size_t)n;
    }
}

void RpcServer::sendLineTo(const std::shared_ptr<Client>& c, const std::string& s) {
    if (!c) return;
    std::lock_guard<std::mutex> lk(c->mu);
    uintptr_t sk = c->sock.load();
    if (sk == (uintptr_t)-1) return;
    sendLine(sk, s);
}

bool RpcServer::start(Host* host, int port, const std::string& token) {
    if (port <= 0 || port > 65535) {
        PP_INFO("控制通道未启用（端口=%d）", port);
        return false;
    }
    if (!g_wsaReady) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            PP_ERROR("WSAStartup 失败");
            return false;
        }
        g_wsaReady = true;
    }
    host_  = host;
    token_ = token.empty() ? generateToken() : token;

    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        PP_ERROR("socket() 失败: %d", WSAGetLastError());
        return false;
    }
    /* Windows 上正确做法是 SO_EXCLUSIVEADDRUSE：
     * 阻止其它进程强绑同一端口（SO_REUSEADDR 在本平台反而允许抢占）。 */
    BOOL excl = TRUE;
    if (::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&excl,
                     sizeof(excl)) == SOCKET_ERROR) {
        PP_WARN("SO_EXCLUSIVEADDRUSE 设置失败: %d（继续，但端口可能被抢占）",
                WSAGetLastError());
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    /* 只监听本机回环，避免把控制接口暴露到局域网 */
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (::bind(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        int e = WSAGetLastError();
        PP_ERROR("bind(127.0.0.1:%d) 失败: %d（端口已被其它进程占用；控制通道未启动）",
                 port, e);
        ::closesocket(s);
        return false;
    }
    if (::listen(s, 8) == SOCKET_ERROR) {
        PP_ERROR("listen 失败: %d", WSAGetLastError());
        ::closesocket(s);
        return false;
    }
    listenSock_ = (uintptr_t)s;
    port_ = port;
    running_ = true;
    acceptTh_ = std::thread(&RpcServer::acceptLoop, this);
    PP_INFO("控制通道已监听 127.0.0.1:%d（强制令牌认证，并发上限 %d）",
            port, kMaxClients);
    return true;
}

void RpcServer::stop() {
    if (!running_.exchange(false)) return;
    if (listenSock_ != (uintptr_t)-1) {
        ::closesocket((SOCKET)listenSock_);
        listenSock_ = (uintptr_t)-1;
    }
    if (acceptTh_.joinable()) acceptTh_.join();

    /* 放出所有还在等主线程执行的请求，否则 stop() 会卡在 join 上：
     * 主循环此刻已经不会再调用 pumpMainThread 了。 */
    {
        std::lock_guard<std::mutex> lk(qmu_);
        for (auto& r : queue_) {
            r->abandoned = true;
            r->cv.notify_all();
        }
        queue_.clear();
    }

    /* 先摘出列表，再逐个体面关闭并 join。
     * 客户端对象由 shared_ptr 持有，join 之前不会被析构，因此不存在
     * 「主线程 delete 掉、工作线程还在用」的 use-after-free。
     *
     * 注意这里**故意不再对客户端 socket 调 ::shutdown**：实测 Windows 下跨线程
     * shutdown(sk, SD_BOTH) 既不唤醒 pending recv，还会**阻塞调用方**直到 recv
     * 的下一个 SO_RCVTIMEO tick 才返回（quit 延迟实测从 30s 修到 1s 后仍残留
     * ~4s，全在这个调用上）。唤醒机制就是 kIdleTimeoutMs=1000 的周期 tick：
     * recv 醒来看到 running_==false 自行 break，≤1s 内退出；阻塞在 send 的线程
     * 受 SO_SNDTIMEO=10s 兜底。 */
    std::vector<std::shared_ptr<Client>> cs;
    {
        std::lock_guard<std::mutex> lk(mu_);
        cs.swap(clients_);
    }
    for (auto& c : cs) {
        if (c->th.joinable()) c->th.join();
        std::lock_guard<std::mutex> lk(c->mu);
        uintptr_t sk = c->sock.exchange((uintptr_t)-1);
        if (sk != (uintptr_t)-1) ::closesocket((SOCKET)sk);
    }
    if (g_wsaReady) { WSACleanup(); g_wsaReady = false; }
    PP_INFO("控制通道已关闭（累计处理 %lld 条请求，认证失败 %lld 次）",
            (long long)served_.load(), (long long)authFailures_.load());
}

int RpcServer::clientCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    return (int)clients_.size();
}

void RpcServer::acceptLoop() {
    while (running_.load()) {
        sockaddr_in cli{};
        int clen = sizeof(cli);
        SOCKET c = ::accept((SOCKET)listenSock_, (sockaddr*)&cli, &clen);
        if (c == INVALID_SOCKET) {
            if (!running_.load()) break;
            continue;
        }
        /* 并发上限：超限直接拒绝，避免线程/资源耗尽 */
        if (clientCount() >= kMaxClients) {
            PP_WARN("控制通道：并发连接已达上限 %d，拒绝 %s", kMaxClients,
                    inet_ntoa(cli.sin_addr));
            sendLine((uintptr_t)c,
                     "{\"ok\":false,\"error\":\"并发连接已达上限，请稍后重试\"}");
            ::closesocket(c);
            continue;
        }
        /* 超时设置：recv 空闲超时（不因此断连，仅用于周期性检查退出），send 防止对端不读阻塞 */
        DWORD rcvTo = kIdleTimeoutMs, sndTo = kSendTimeoutMs;
        ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rcvTo, sizeof(rcvTo));
        ::setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sndTo, sizeof(sndTo));

        auto cl = std::make_shared<Client>();
        cl->sock = (uintptr_t)c;
        {
            std::lock_guard<std::mutex> lk(mu_);
            clients_.push_back(cl);
        }
        cl->th = std::thread(&RpcServer::clientLoop, this, cl);
        PP_INFO("控制通道：客户端接入 %s（未认证）", inet_ntoa(cli.sin_addr));
    }
}

void RpcServer::clientLoop(std::shared_ptr<Client> c) {
    std::string buf;
    size_t scanFrom = 0;
    char tmp[4096];

    for (;;) {
        int n = ::recv((SOCKET)c->sock.load(), tmp, sizeof(tmp), 0);
        if (n > 0) {
            buf.append(tmp, (size_t)n);

            /* 增量扫描换行：只从上次扫描位置继续，避免 O(n²) */
            size_t pos;
            while ((pos = buf.find('\n', scanFrom)) != std::string::npos) {
                std::string line = trim(buf.substr(0, pos));
                buf.erase(0, pos + 1);
                scanFrom = 0;
                if (line.empty()) continue;
                served_++;
                /* 交主线程执行：见 pumpMainThread() 的说明。
                 * 这样 handleRequest 里对 Player/Host 状态的全部读写都发生在
                 * 主循环线程，不再与主循环并发。 */
                std::string resp;
                if (!dispatchToMainThread(line, c, &resp)) {
                    sendLineTo(c, "{\"ok\":false,\"error\":\"主线程繁忙或正在退出，请求未执行\"}");
                    continue;
                }
                if (!resp.empty()) sendLineTo(c, resp);
                if (c->closing.load()) goto done;
            }
            scanFrom = buf.size();

            /* 单条报文硬上限：防止无上限行缓冲把内存吃干（实测可涨到 500MB+） */
            if (buf.size() > kMaxLineBytes) {
                PP_WARN("控制通道：客户端单条报文超过 %zu 字节上限，断开",
                        kMaxLineBytes);
                sendLineTo(c, format("{\"ok\":false,\"error\":\"单条报文超过 %zu 字节上限\"}",
                                     kMaxLineBytes));
                break;
            }
        } else if (n == 0) {
            break;                          /* 对端正常关闭 */
        } else {
            int e = WSAGetLastError();
            if (e == WSAETIMEDOUT) {         /* 空闲：不是错误，继续等待 */
                if (!running_.load()) break;
                continue;
            }
            break;
        }
        if (c->closing.load()) break;
    }

done:
    /* 先关 socket 再摘出列表：这样 stop() 一旦看不到本客户端，
     * 就说明它已经不再做任何 WSA 调用，避免了 WSACleanup 之后的窗口期。
     * （摘除顺序不能反：broadcastEvent 已按 sock==-1 跳过已关闭的客户端。） */
    {
        std::lock_guard<std::mutex> lk(c->mu);
        uintptr_t sk = c->sock.exchange((uintptr_t)-1);
        if (sk != (uintptr_t)-1) ::closesocket((SOCKET)sk);
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        clients_.erase(std::remove(clients_.begin(), clients_.end(), c), clients_.end());
    }
}

/* ------------------------- 主线程执行队列 ------------------------- */

bool RpcServer::dispatchToMainThread(const std::string& line,
                                     const std::shared_ptr<Client>& c,
                                     std::string* out) {
    auto req = std::make_shared<Pending>();
    req->line = line;
    req->client = c;
    {
        std::lock_guard<std::mutex> lk(qmu_);
        if (!running_.load()) return false;
        queue_.push_back(req);
    }
    std::unique_lock<std::mutex> lk(req->mu);
    bool signaled = req->cv.wait_for(
        lk, std::chrono::seconds(kRequestTimeoutSec),
        [&] { return req->done || req->abandoned.load(); });
    if (!signaled) {
        /* 主线程 10 秒都没轮到（例如正卡在一次解码/打开媒体上）：
         * 放弃这条请求，别把工作线程和客户端一起拖死。 */
        req->abandoned = true;
        return false;
    }
    if (req->abandoned.load() && !req->done) return false;
    *out = req->response;
    return true;
}

void RpcServer::pumpMainThread() {
    for (;;) {
        std::shared_ptr<Pending> req;
        {
            std::lock_guard<std::mutex> lk(qmu_);
            if (queue_.empty()) return;
            req = queue_.front();
            queue_.pop_front();
        }
        if (!req || req->abandoned.load()) continue;
        std::string resp = handleRequest(req->line, req->client);
        {
            std::lock_guard<std::mutex> lk(req->mu);
            req->response = resp;
            req->done = true;
        }
        req->cv.notify_all();
    }
}

void RpcServer::broadcastEvent(const std::string& topic, const std::string& json) {
    if (!running_.load()) return;
    std::vector<std::shared_ptr<Client>> targets;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& c : clients_) {
            if (!c->authed.load()) continue;      /* 未认证连接不接收任何事件 */
            std::lock_guard<std::mutex> cl(c->mu);
            if (c->sock.load() == (uintptr_t)-1) continue;
            if (c->topics.empty() ||
                std::find(c->topics.begin(), c->topics.end(), topic) != c->topics.end()) {
                targets.push_back(c);
            }
        }
    }
    if (targets.empty()) return;
    std::string msg = format("{\"event\":\"%s\",\"data\":%s}",
                             jsonEscape(topic).c_str(),
                             json.empty() ? "{}" : json.c_str());
    for (auto& c : targets) sendLineTo(c, msg);
}

/* ------------------------- 请求处理 ------------------------- */

std::string RpcServer::handleRequest(const std::string& line, const std::shared_ptr<Client>& c) {
    /* 严格解析顶层请求。非法 JSON 直接拒绝。
     * 旧实现用子串搜索找 "params" 并用花括号计数切分 —— 字符串里出现
     * 大括号就会切错，键名出现在字符串值里也会误命中。 */
    std::vector<std::pair<std::string, std::string>> top;
    std::string jerr;
    if (!jsonParseObject(line, &top, &jerr)) {
        if (jerr.size() > 120) jerr.resize(120);      /* 不回显超长输入 */
        return format("{\"id\":0,\"ok\":false,\"error\":\"非法 JSON 请求: %s\"}",
                      jsonEscape(jerr).c_str());
    }
    int    id = (int)jsonGetNumber(line, "id", 0);
    std::string method = jsonGetString(line, "method", "");
    std::string params = "{}";
    for (const auto& kv : top) {
        if (kv.first == "params") { params = kv.second; break; }   /* 原始切片，按需再解析 */
    }

    App* app = host_ ? host_->app() : nullptr;
    Player* pl = host_ ? host_->player() : nullptr;

    auto ok = [&](const std::string& resultJson) {
        return format("{\"id\":%d,\"ok\":true,\"result\":%s}", id, resultJson.c_str());
    };
    auto err = [&](const std::string& msg) {
        return format("{\"id\":%d,\"ok\":false,\"error\":\"%s\"}", id, jsonEscape(msg).c_str());
    };

    /* ---- 认证握手：必须是第一条消息 ---- */
    if (method == "auth") {
        if (c->authed.load())
            return ok("{\"authenticated\":true,\"already\":true}");
        std::string t = jsonGetString(params, "token", "");
        if (t.empty()) t = jsonGetString(line, "token", "");
        if (t.empty() || !tokenEquals(t, token_)) {
            authFailures_++;
            PP_WARN("控制通道：令牌认证失败，断开连接");
            c->closing = true;
            return err("认证失败");
        }
        c->authed = true;
        PP_INFO("控制通道：客户端认证通过");
        return ok(format("{\"authenticated\":true,\"abi\":%u}", PP_ABI_VERSION));
    }

    /* 未认证：一律拒绝。控制通道可执行任意插件命令（写文件 / 退出进程），
     * 因此在认证前不暴露任何能力，连 ping 也不放行。 */
    if (!c->authed.load()) {
        authFailures_++;
        c->closing = true;
        return err("未认证：请先发送 {\"method\":\"auth\",\"params\":{\"token\":\"...\"}}");
    }

    if (method == "ping") {
        return ok(format("{\"app\":\"pluginplayer\",\"abi\":%u,\"pong\":true}", PP_ABI_VERSION));
    }

    if (method == "player.info") {
        if (!pl) return err("player 未就绪");
        PP_MediaInfo mi{};
        pl->fillMediaInfo(&mi);
        return ok(format(
            "{\"path\":\"%s\",\"format\":\"%s\",\"video_codec\":\"%s\",\"audio_codec\":\"%s\","
            "\"width\":%d,\"height\":%d,\"fps\":%.4f,\"duration\":%.4f,\"bitrate\":%lld,"
            "\"sample_rate\":%d,\"channels\":%d,\"has_video\":%d,\"has_audio\":%d,"
            "\"hwdec\":\"%s\"}",
            jsonEscape(mi.path).c_str(), jsonEscape(mi.format).c_str(),
            jsonEscape(mi.video_codec).c_str(), jsonEscape(mi.audio_codec).c_str(),
            mi.width, mi.height, mi.fps, mi.duration, (long long)mi.bitrate,
            mi.audio_sample_rate, mi.audio_channels, mi.has_video, mi.has_audio,
            pl->hwdecName()));
    }

    if (method == "player.stats") {
        if (!pl) return err("player 未就绪");
        PP_PlaybackStats st{};
        pl->fillStats(&st);
        return ok(format(
            "{\"position\":%.3f,\"duration\":%.3f,\"master_clock\":%.3f,\"video_clock\":%.3f,"
            "\"audio_clock\":%.3f,\"speed\":%.3f,\"paused\":%d,\"eof\":%d,"
            "\"decoded\":%lld,\"displayed\":%lld,\"dropped\":%d,\"fps\":%.2f,"
            "\"vqueue\":%lld,\"abuf\":%lld,\"hwdec\":\"%s\"}",
            st.position, st.duration, st.master_clock, st.video_clock, st.audio_clock,
            st.speed, st.paused, st.eof,
            (long long)st.decoded_frames, (long long)st.displayed_frames, st.dropped_frames,
            st.current_fps, (long long)st.video_queue_depth, (long long)st.audio_buffer_bytes,
            pl->hwdecName()));
    }

    if (method == "player.play" || method == "player.pause" ||
        method == "player.toggle" || method == "player.stop") {
        if (!pl) return err("player 未就绪");
        if (method == "player.play")        pl->setPaused(false);
        else if (method == "player.pause")  pl->setPaused(true);
        else if (method == "player.toggle") pl->togglePause();
        else {                              /* player.stop：停下来并回到起点 */
            pl->setPaused(true);
            pl->seekAbsolute(0.0);
        }
        return ok(format("{\"paused\":%d,\"position\":%.3f}", pl->paused() ? 1 : 0,
                         pl->position()));
    }

    if (method == "player.seek") {
        if (!pl) return err("player 未就绪");
        double sec = jsonGetNumber(params, "seconds", 1e18);
        double rel = jsonGetNumber(params, "delta", 0);
        if (sec > 1e17) pl->seekRelative(rel);
        else            pl->seekAbsolute(sec);
        return ok(format("{\"position\":%.3f}", pl->position()));
    }

    if (method == "player.volume") {
        if (!pl) return err("player 未就绪");
        double v = jsonGetNumber(params, "value", jsonGetNumber(params, "delta", -999));
        /* 简化：delta 由调用方先查 stats 计算，这里只接受绝对值 */
        if (v <= -998 || v > 1.0) {
            if (v <= -998) return ok(format("{\"volume\":%.3f}", (double)pl->volume()));
        }
        pl->setVolume((float)v);
        /* 统一走 App::applyAudioParams()：内部用钳位后的值，且静音时增益恒为 0 */
        if (app) app->applyAudioParams();
        return ok(format("{\"volume\":%.3f}", (double)pl->volume()));
    }

    if (method == "player.mute") {
        if (!pl) return err("player 未就绪");
        double v = jsonGetNumber(params, "value", -1);
        if (v >= 0) pl->setMuted(v != 0);
        else pl->setMuted(!pl->muted());
        if (app) app->applyAudioParams();
        return ok(format("{\"muted\":%d}", pl->muted() ? 1 : 0));
    }

    if (method == "player.speed") {
        if (!pl) return err("player 未就绪");
        double v = jsonGetNumber(params, "value", 1.0);
        pl->setSpeed(v);
        if (app) app->applyAudioParams();
        return ok(format("{\"speed\":%.3f}", pl->speed()));
    }

    if (method == "player.open") {
        std::string path = jsonGetString(params, "path", "");
        if (path.empty() || !app) return err("缺少 path 参数");
        bool r = app->openMedia(path);
        return r ? ok("{\"opened\":true}") : err("打开失败: " + app->lastError());
    }

    /* P1-d：多轨 */
    if (method == "player.tracks") {
        if (!pl) return err("player 未就绪");
        /* listTracksJson() 已是 {video:[...],audio:[...],subtitle:[...]} 对象串。 */
        return ok(pl->listTracksJson());
    }

    if (method == "player.select_track") {
        if (!app) return err("app 未就绪");
        int type  = (int)jsonGetNumber(params, "type", -1);
        int index = (int)jsonGetNumber(params, "index", -1);
        if (type < 0 || index < 0) return err("缺少 type/index 参数");
        bool r = app->selectTrack(type, index);
        return r ? ok(format("{\"selected\":{\"type\":%d,\"index\":%d}}", type, index))
                 : err("切换失败（非法轨或媒体未打开）");
    }

    /* P1-d：音频设备 */
    if (method == "audio.devices") {
        if (!app) return err("app 未就绪");
        /* listAudioDevicesJson() 已是 JSON 数组串。 */
        return ok(app->listAudioDevicesJson());
    }

    if (method == "audio.set_device") {
        if (!app) return err("app 未就绪");
        double did = jsonGetNumber(params, "id", -1.0);
        if (did < 0) return err("缺少 id 参数");
        bool r = app->setAudioDevice((SDL_AudioDeviceID)(int)did);
        return r ? ok(format("{\"device\":{\"id\":%u}}", (unsigned)(int)did))
                 : err("切换音频设备失败");
    }

    if (method == "command.invoke") {
        std::string name = jsonGetString(params, "name", "");
        if (name.empty() || !host_) return err("缺少 name 参数");
        /* args 必须是「字符串数组」；用严格解析，元素不是字符串就判非法
         * （旧实现是手工找引号配对，遇到转义引号会错乱） */
        std::vector<std::string> args;
        if (params.find("\"args\"") != std::string::npos) {
            if (!jsonGetStringArray(params, "args", &args))
                return err("args 必须是字符串数组");
        }
        std::vector<const char*> argv;
        for (auto& s : args) argv.push_back(s.c_str());
        int rc = host_->invokeCommand(name, (int)argv.size(), argv.empty() ? nullptr : argv.data());
        if (rc < 0 && !host_->hasCommand(name)) return err("命令不存在: " + name);
        return ok(format("{\"command\":\"%s\",\"return\":%d}", jsonEscape(name).c_str(), rc));
    }

    if (method == "command.list") {
        if (!host_) return err("宿主未就绪");
        char buf[16384];
        int n = host_->commands().list(buf, sizeof(buf));
        return ok(format("{\"count\":%d,\"text\":\"%s\"}", n, jsonEscape(buf).c_str()));
    }

    if (method == "service.list") {
        if (!host_) return err("宿主未就绪");
        std::string j = "[";
        auto names = host_->services().names();
        for (size_t i = 0; i < names.size(); ++i)
            j += format("%s\"%s\"", i ? "," : "", jsonEscape(names[i]).c_str());
        j += "]";
        return ok(format("{\"services\":%s}", j.c_str()));
    }

    if (method == "plugin.list") {
        if (!app) return err("app 未就绪");
        return ok(format("{\"plugins\":%s}", app->plugins().jsonSummary().c_str()));
    }

    if (method == "topic.subscribe") {
        std::string t = jsonGetString(params, "topic", "");
        if (t.empty()) return err("缺少 topic");
        {
            std::lock_guard<std::mutex> lk(c->mu);
            if (t == "*") c->topics.clear();
            else c->topics.push_back(t);
        }
        return ok(format("{\"subscribed\":\"%s\"}", jsonEscape(t).c_str()));
    }

    if (method == "app.quit") {
        if (app) app->requestQuit();
        return ok("{\"quitting\":true}");
    }

    return err("未知方法: " + method);
}

} // namespace pp
