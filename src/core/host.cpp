/* host.cpp — PP_HostApi 的具体实现 */
#include "host.h"
#include "app.h"
#include "osd.h"
#include "util.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace pp {

PP_Host* Host::handle() { return &cHandle_; }

Host* Host::from(PP_Host* h) { return (h && h->impl) ? (Host*)h->impl : nullptr; }

bool Host::init(const std::string& appDir) {
    appDir_    = appDir;
    pluginDir_ = pathJoin(appDir, "plugins");
    assetDir_  = pathJoin(appDir, "assets");
    configPath_ = pathJoin(appDir, "config/pluginplayer.ini");
    logPath_    = pathJoin(appDir, "logs/pluginplayer.log");

    makeDirs(dirName(configPath_));
    makeDirs(dirName(logPath_));
    config_.load(configPath_);

    /* 插件写入白名单：默认只有 exeDir（dist/）。
     * 可用 ini 的 security.write_dirs（分号分隔）追加其它目录。 */
    writeRoots_.clear();
    addWriteRoot(appDir_);
    {
        std::string extra = config_.get("security.write_dirs", "");
        if (!extra.empty()) {
            for (const auto& d : split(extra, ';')) {
                std::string t = trim(d);
                if (!t.empty()) addWriteRoot(t);
            }
        }
    }

    cHandle_.impl = this;
    buildApi();
    return true;
}

void Host::addWriteRoot(const std::string& dir) {
    if (dir.empty()) return;
    std::string n = normalizePath(dir);
    if (n.empty()) return;
    for (const auto& r : writeRoots_) if (lower(r) == lower(n)) return;
    writeRoots_.push_back(n);
    PP_INFO("插件写入白名单 + %s", n.c_str());
}

bool Host::isWriteAllowed(const std::string& path) const {
    if (path.empty()) return false;
    for (const auto& r : writeRoots_) {
        if (pathIsInside(path, r)) return true;
    }
    return false;
}

void Host::shutdown() {
    if (config_.dirty()) config_.save();
}

int Host::revokeAll(int owner) {
    int n = 0;
    n += bus_.unsubscribeAllOf(owner);
    n += services_.removeAllOf(owner);
    n += commands_.removeAllOf(owner);
    n += chain_.removeAllOf(owner);      /* 帧链处理器（含帧订阅对应的那些）已在此摘掉 */
    for (auto& a : frameSubs_) {
        /* 只计数：链条目上一步已经按 owner 摘掉了，这里再 chain_.remove() 是空操作，
         * 旧实现却照样 n++，导致撤销数字虚高（日志里看不到真实撤销数）。 */
        if (a && a->owner == owner) n++;
    }
    frameSubs_.erase(std::remove_if(frameSubs_.begin(), frameSubs_.end(),
                                    [owner](const std::unique_ptr<FrameSubAdapter>& p) {
                                        return p && p->owner == owner;
                                    }),
                     frameSubs_.end());
    dropSubsOf(owner);
    bus_.clearTopicModesOf(owner);       /* 插件声明的自定义主题模式一并撤销 */
    if (app_) n += app_->overlayClear(owner);   /* P1-b：插件卸载自动清掉它的图层 */
    return n;
}

/* ---- 插件配置键：按插件名而不是槽位号 ---- */

std::string Host::ownerName(int owner) const {
    auto it = ownerNames_.find(owner);
    if (it != ownerNames_.end() && !it->second.empty()) return it->second;
    return format("#%d", owner);
}

static std::string sanitizeKeyPart(const std::string& s) {
    std::string o;
    for (char c : lower(s)) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-') o.push_back(c);
        else                  o.push_back('_');
    }
    if (o.empty() || o == "_") o = "anon";
    return o;
}

std::string Host::pluginConfigKey(int owner, const std::string& key) {
    std::string nameKey = format("plugin.%s.%s", sanitizeKeyPart(ownerName(owner)).c_str(),
                                 key.c_str());
    std::string slotKey = format("plugin.%d.%s", owner, key.c_str());
    /* 旧版本按槽位号存。首次访问新键时把旧值迁过来，避免升级后配置"丢失"。 */
    if (!config_.has(nameKey) && config_.has(slotKey)) {
        config_.set(nameKey, config_.get(slotKey, ""));
        PP_INFO("配置迁移: %s -> %s", slotKey.c_str(), nameKey.c_str());
    }
    return nameKey;
}

/* ---- 插件可见订阅句柄：隔离 FrameChain 与 EventBus 的 id 空间 ---- */

int Host::registerSub(SubKind kind, int realId, int owner) {
    if (realId <= 0) return 0;
    int id = nextSubId_++;
    subMap_[id] = SubRef{kind, realId, owner};
    return id;
}

bool Host::takeSub(int id, int* kind, int* realId) {
    auto it = subMap_.find(id);
    if (it == subMap_.end()) return false;
    if (kind)   *kind   = (int)it->second.kind;
    if (realId) *realId = it->second.realId;
    subMap_.erase(it);
    return true;
}

void Host::dropSubsOf(int owner) {
    for (auto it = subMap_.begin(); it != subMap_.end();) {
        if (it->second.owner == owner) it = subMap_.erase(it);
        else ++it;
    }
}

void Host::dropFrameSub(int chainId) {
    chain_.remove(chainId);
    frameSubs_.erase(std::remove_if(frameSubs_.begin(), frameSubs_.end(),
                                    [chainId](const std::unique_ptr<FrameSubAdapter>& p) {
                                        return p && p->chainId == chainId;
                                    }),
                     frameSubs_.end());
}

int Host::runFrameChain(PP_FrameView* view) {
    return chain_.run(handle(), view);
}

int Host::invokeCommand(const std::string& name, int argc, const char** argv) {
    CommandEntry e;
    if (!commands_.lookup(name, &e)) return -1;
    OwnerScope scope(this, e.owner);
    return e.fn(handle(), argc, argv, e.user);
}

int Host::publishTopic(const char* topic, const char* json) {
    if (!topic) return 0;
    int n = bus_.publish(handle(), topic, json);
    if (eventSink_) eventSink_(topic, json ? json : "{}");
    return n;
}

void Host::emitOsdDraw() {
    if (!bus_.subscriberCount(PP_TOPIC_OSD_DRAW)) return;
    bus_.publish(handle(), PP_TOPIC_OSD_DRAW, "{}");
}

void Host::cleanSnapshot() {
    snapFrame_.reset();
}

void Host::setSnapFrame(const std::shared_ptr<VideoFrame>& f, PP_FrameView* out) {
    snapFrame_ = f;
    std::memset(&snapView_, 0, sizeof(snapView_));
    snapView_.data = snapFrame_->rgba.data();
    snapView_.width = snapFrame_->w;
    snapView_.height = snapFrame_->h;
    snapView_.pitch = snapFrame_->pitch;
    snapView_.format = PP_PIXFMT_RGBA8888;
    snapView_.pts = snapFrame_->pts;
    snapView_.index = snapFrame_->index;
    snapView_.serial = snapFrame_->serial;
    *out = snapView_;
}

std::shared_ptr<VideoFrame> Host::takeSnapFrame() {
    auto f = snapFrame_;
    snapFrame_.reset();
    return f;
}

const char* Host::cacheConfig(const std::string& key, const std::string& value) {
    auto it = cfgCache_.find(key);
    if (it == cfgCache_.end()) {
        it = cfgCache_.emplace(key, value).first;
    } else if (it->second != value) {
        it->second = value;
    }
    return it->second.c_str();
}

std::string Host::commandsText() const {
    char* buf = (char*)malloc(16384);
    if (!buf) return "";
    int n = commands_.list(buf, 16384);
    std::string s = format("共 %d 条命令:\n", n);
    s += buf;
    free(buf);
    return s;
}

std::string Host::servicesText() const {
    auto names = services_.names();
    std::string s = format("共 %d 个服务:\n", (int)names.size());
    for (const auto& n : names) s += "  " + n + "\n";
    return s;
}

std::string Host::topicsText() const {
    auto t = bus_.topics();
    std::string s = format("共 %d 个已订阅主题:\n", (int)t.size());
    for (const auto& x : t)
        s += format("  %-18s x%d\n", x.c_str(), bus_.subscriberCount(x));
    return s;
}

/* ======================================================================
 * HostApi 各函数的 C 实现
 * ====================================================================== */

#define HOST(h) ::pp::Host::from(h)

/* ---- 日志 ---- */
static void api_log(PP_Host* h, int level, const char* fmt, ...) {
    char body[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    LogLevel lv = (LogLevel)std::max(0, std::min(4, level));
    (void)h;
    Log::write(lv, "[plugin] %s", body);
}

/* ---- 播放控制 ---- */
static int   api_play(PP_Host* h) { Player* p = HOST(h)->player(); if (p) { p->setPaused(false); } return 0; }
static int   api_pause(PP_Host* h) { Player* p = HOST(h)->player(); if (p) { p->setPaused(true); } return 0; }
static int   api_toggle_pause(PP_Host* h) { Player* p = HOST(h)->player(); if (p) p->togglePause(); return 0; }
static int   api_stop(PP_Host* h) {
    /* 「停止」应当真的停下来并回到起点。旧实现只做 pause ——
     * 名字与行为不符：调 stop 之后位置还停在原处。 */
    Host* H = HOST(h);
    Player* p = H->player();
    if (!p) return -1;
    p->setPaused(true);
    p->seekAbsolute(0.0);
    H->bus().publish(h, PP_TOPIC_PLAY_STOPPED, "{}");
    return 0;
}
static int   api_seek_abs(PP_Host* h, double s) { Player* p = HOST(h)->player(); return (p && p->seekAbsolute(s)) ? 0 : -1; }
static int   api_seek_rel(PP_Host* h, double d) { Player* p = HOST(h)->player(); return (p && p->seekRelative(d)) ? 0 : -1; }
/* 音量/静音/倍速必须**同时**作用到 SDL 音频流，否则插件调用后「状态变了但听不出来」。
 * 走 App::applyAudioParams() 统一下发，避免与控制通道、快捷键三条路径各写一份。 */
static int   api_set_volume(PP_Host* h, float v) {
    Host* H = HOST(h);
    Player* p = H->player();
    if (!p) return -1;
    p->setVolume(v);
    if (H->app()) H->app()->applyAudioParams();
    return 0;
}
static float api_get_volume(PP_Host* h) { Player* p = HOST(h)->player(); return p ? p->volume() : 0.0f; }
static int   api_set_muted(PP_Host* h, int m) {
    Host* H = HOST(h);
    Player* p = H->player();
    if (!p) return -1;
    p->setMuted(m != 0);
    if (H->app()) H->app()->applyAudioParams();
    return 0;
}
static int   api_is_muted(PP_Host* h) { Player* p = HOST(h)->player(); return (p && p->muted()) ? 1 : 0; }
static int   api_set_speed(PP_Host* h, double s) {
    Host* H = HOST(h);
    if (!H->player() || !H->player()->setSpeed(s)) return -1;
    if (H->app()) H->app()->applyAudioParams();
    H->bus().publish(h, "player.speed_changed",
                     format("{\"speed\":%g}", H->player()->speed()).c_str());
    return 0;
}
static int   api_is_paused(PP_Host* h) { Player* p = HOST(h)->player(); return (p && p->paused()) ? 1 : 0; }
static double api_get_position(PP_Host* h) { Player* p = HOST(h)->player(); return p ? p->position() : 0.0; }
static double api_get_duration(PP_Host* h) { Player* p = HOST(h)->player(); return p ? p->duration() : 0.0; }
static int   api_open_media(PP_Host* h, const char* path) {
    Host* H = HOST(h);
    if (!H->app() || !path) return -1;
    return H->app()->openMedia(path) ? 0 : -1;
}
static int   api_get_media_info(PP_Host* h, PP_MediaInfo* out) {
    Host* H = HOST(h);
    if (!H->player() || !out) return -1;
    H->player()->fillMediaInfo(out);
    return 0;
}
static int   api_get_stats(PP_Host* h, PP_PlaybackStats* out) {
    Host* H = HOST(h);
    if (!H->player() || !out) return -1;
    H->player()->fillStats(out);
    return 0;
}

/* ---- 事件 ---- */
static int frameSubThunk(PP_Host* h, PP_FrameView* view, void* user);

static int api_subscribe(PP_Host* h, const char* topic, PP_EventFn fn, void* user) {
    Host* H = HOST(h);
    if (!topic || !fn) return 0;
    if (strcmp(topic, PP_TOPIC_VIDEO_FRAME) == 0) {
        auto a = std::unique_ptr<Host::FrameSubAdapter>(new Host::FrameSubAdapter());
        a->owner = H->owner();
        a->fn = fn;
        a->user = user;
        a->chainId = H->chain().add(&frameSubThunk, a.get(), 100, H->owner());
        int cid = a->chainId;
        H->frameSubs().push_back(std::move(a));
        /* 返回 Host 分配的插件可见 id，而不是帧链内部 id */
        return H->registerSub(Host::SubKind::FrameChain, cid, H->owner());
    }
    int eid = H->bus().subscribe(topic, fn, user, H->owner());
    return H->registerSub(Host::SubKind::EventBus, eid, H->owner());
}

static void api_unsubscribe(PP_Host* h, int id) {
    Host* H = HOST(h);
    int kind = -1, realId = -1;
    if (!H->takeSub(id, &kind, &realId)) {
        /* 不是本宿主分配的 id：不猜、不乱撤（早期版本会误撤到另一类订阅） */
        PP_WARN("unsubscribe: 未知订阅句柄 #%d（已忽略）", id);
        return;
    }
    if (kind == (int)Host::SubKind::FrameChain) H->dropFrameSub(realId);
    else                                          H->bus().unsubscribe(realId);
}

static int api_publish(PP_Host* h, const char* topic, const char* json) {
    return HOST(h)->publishTopic(topic, json);
}

static int api_dispatch(PP_Host* h, const char* topic, void* payload, int size) {
    return HOST(h)->bus().dispatch(h, topic, payload, size);
}

static int api_topic_mode(PP_Host* h, const char* topic) {
    return HOST(h)->bus().topicMode(topic);
}

/** 声明插件自定义主题的分发模式（emit/waterfall/serial/bail）。
 *  注册归当前插件所有，卸载时随 revokeAll 一起清除。 */
static int api_set_topic_mode(PP_Host* h, const char* topic, int mode) {
    if (!topic) return -1;
    Host* H = HOST(h);
    return H->bus().setTopicMode(topic, mode, H->owner());
}

static int frameSubThunk(PP_Host* h, PP_FrameView* view, void* user) {
    Host::FrameSubAdapter* a = (Host::FrameSubAdapter*)user;
    if (!a) return PP_CHAIN_CONTINUE;
    PP_EventCtx ctx{};
    ctx.topic = PP_TOPIC_VIDEO_FRAME;
    ctx.host = h;
    ctx.user = a->user;
    ctx.payload = view;
    ctx.payload_size = (int)sizeof(PP_FrameView);
    ctx.mode = PP_DISPATCH_WATERFALL;
    int r = a->fn(&ctx);
    if (r == PP_CHAIN_DROP || r == PP_CHAIN_BAIL) return r;
    return PP_CHAIN_CONTINUE;
}

/* ---- 服务 ---- */
static int api_register_service(PP_Host* h, const char* name, const char* ver, void* iface) {
    Host* H = HOST(h);
    bool ok = H->services().add(name ? name : "", ver ? ver : "0", iface, H->owner());
    if (ok) PP_INFO("插件 #%d 注册服务: %s v%s", H->owner(), name, ver ? ver : "0");
    return ok ? 0 : -1;
}
static void api_unregister_service(PP_Host* h, const char* name) {
    if (name) HOST(h)->services().remove(name);
}
static void* api_get_service(PP_Host* h, const char* name) {
    return name ? HOST(h)->services().get(name) : nullptr;
}
static int api_list_services(PP_Host* h, char* out, int maxNames, int stride) {
    if (!out || maxNames <= 0 || stride <= 0) return 0;
    return HOST(h)->services().list(out, maxNames, stride);
}

/* ---- 命令 ---- */
static int api_register_command(PP_Host* h, const char* name, const char* usage,
                                PP_CommandFn fn, void* user) {
    Host* H = HOST(h);
    bool ok = H->commands().add(name ? name : "", usage ? usage : "", fn, user, H->owner());
    if (ok) PP_INFO("插件 #%d 注册命令: %s", H->owner(), name);
    return ok ? 0 : -1;
}
static void api_unregister_command(PP_Host* h, const char* name) {
    if (name) HOST(h)->commands().remove(name);
}
static int api_invoke_command(PP_Host* h, const char* name, int argc, const char** argv) {
    if (!name) return -1;
    return HOST(h)->invokeCommand(name, argc, argv);
}
static int api_list_commands(PP_Host* h, char* out, int outSize) {
    if (!out || outSize <= 0) return 0;
    return HOST(h)->commands().list(out, outSize);
}

/* ---- 帧访问 ---- */
static int api_acquire_frame(PP_Host* h, PP_FrameView* out) {
    Host* H = HOST(h);
    if (!out || !H->player()) return -1;
    auto c = H->player()->copyCurrentFrame();
    if (!c) return -1;
    H->setSnapFrame(c, out);
    return 0;
}
static void api_release_frame(PP_Host* h, PP_FrameView* v) {
    Host* H = HOST(h);
    (void)v;
    auto f = H->takeSnapFrame();
    if (f && H->player()) H->player()->writeBackCurrentFrame(f);
}
static int api_set_frame_processor(PP_Host* h, PP_FrameFn fn, void* user, int priority) {
    Host* H = HOST(h);
    if (!fn) return 0;
    return H->chain().add(fn, user, priority, H->owner());
}
static void api_remove_frame_processor(PP_Host* h, int id) {
    HOST(h)->chain().remove(id);
}

/* ---- OSD ---- */
static const PP_OsdApi* api_osd() { return Osd::cApi(); }

/* ---- P1-b 通用叠加层 ---- */
static int api_overlay_set(PP_Host* h, int id, const uint8_t* rgba, int w, int hgt,
                           float x, float y, float dw, float dh, float alpha) {
    App* A = HOST(h)->app();
    if (!A || !rgba || w <= 0 || hgt <= 0) return 0;
    return A->overlaySet(HOST(h)->owner(), id, rgba, w, hgt, x, y, dw, dh, alpha);
}
static int api_overlay_move(PP_Host* h, int id, float x, float y, float dw, float dh, float alpha) {
    App* A = HOST(h)->app();
    if (!A) return 0;
    return A->overlayMove(HOST(h)->owner(), id, x, y, dw, dh, alpha);
}
static int api_overlay_visible(PP_Host* h, int id, int visible) {
    App* A = HOST(h)->app();
    if (!A) return 0;
    return A->overlayVisible(HOST(h)->owner(), id, visible != 0);
}
static int api_overlay_remove(PP_Host* h, int id) {
    App* A = HOST(h)->app();
    if (!A) return 0;
    return A->overlayRemove(HOST(h)->owner(), id);
}
static int api_overlay_clear(PP_Host* h) {
    App* A = HOST(h)->app();
    if (!A) return 0;
    return A->overlayClear(HOST(h)->owner());
}
static const PP_OverlayApi* api_overlay() {
    static const PP_OverlayApi api = {
        &api_overlay_set, &api_overlay_move, &api_overlay_visible,
        &api_overlay_remove, &api_overlay_clear,
    };
    return &api;
}

/* ---- 配置 ---- */
static const char* api_config_get(PP_Host* h, const char* key, const char* def) {
    Host* H = HOST(h);
    if (!key) return def;
    std::string k = H->pluginConfigKey(H->owner(), key);
    std::string v = H->config().get(k, def ? def : "");
    return H->cacheConfig(k, v);
}
static int api_config_set(PP_Host* h, const char* key, const char* value) {
    Host* H = HOST(h);
    if (!key) return -1;
    H->config().set(H->pluginConfigKey(H->owner(), key), value ? value : "");
    return 0;
}

/* ---- 杂项 ---- */
static const char* api_app_dir(PP_Host* h) { return HOST(h)->appDir().c_str(); }
static const char* api_plugin_dir(PP_Host* h) { return HOST(h)->pluginDir().c_str(); }
static const char* api_asset_dir(PP_Host* h) { return HOST(h)->assetDir().c_str(); }
static uint64_t api_ticks_ms(PP_Host* h) { (void)h; return SDL_GetTicks(); }
static void api_request_quit(PP_Host* h) { if (HOST(h)->app()) HOST(h)->app()->requestQuit(); }
static void api_set_subtitle(PP_Host* h, const char* text) {
    HOST(h)->setSubtitle(text ? text : "");
}

/* 单张图的最大边长与最大像素数：既防越界（w*h*pitch 溢出），也防插件用
 * 巨大尺寸把内存吃干。 */
static const int    kMaxImageDim    = 16384;
static const int64_t kMaxImagePixels = 16384LL * 16384LL;

static int api_save_png(PP_Host* h, const char* path, const uint8_t* rgba,
                        int w, int hgt, int pitch) {
    Host* H = HOST(h);
    if (!H || !path || !rgba) return -1;
    /* 尺寸与行距校验：pitch 不足会让逐行读取越过缓冲末尾 */
    if (w <= 0 || hgt <= 0) return -1;
    if (w > kMaxImageDim || hgt > kMaxImageDim) {
        PP_WARN("save_png 拒绝：尺寸 %dx%d 超过 %d 上限", w, hgt, kMaxImageDim);
        return -1;
    }
    if ((int64_t)w * (int64_t)hgt > kMaxImagePixels) {
        PP_WARN("save_png 拒绝：像素数 %lld 超过上限", (long long)((int64_t)w * hgt));
        return -1;
    }
    if (pitch < w * 4) {
        PP_WARN("save_png 拒绝：pitch %d 小于一行所需 %d", pitch, w * 4);
        return -1;
    }
    /* 写入白名单：插件（进而被 RPC 触发的插件命令）不得写到白名单之外 */
    if (!H->isWriteAllowed(path)) {
        PP_WARN("save_png 拒绝：目标不在允许的写入目录内: %s", path);
        return -1;
    }
    return writePngRGBA(path, rgba, w, hgt, pitch) ? 0 : -1;
}

static int api_write_text_file(PP_Host* h, const char* path, const char* utf8) {
    Host* H = HOST(h);
    if (!H || !path) return -1;
    if (!H->isWriteAllowed(path)) {
        PP_WARN("write_text_file 拒绝：目标不在允许的写入目录内: %s", path);
        return -1;
    }
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    const char* s = utf8 ? utf8 : "";
    size_t n = strlen(s);
    size_t wr = fwrite(s, 1, n, f);
    fclose(f);
    return wr == n ? 0 : -1;
}

/* ====================================================================== */

void Host::buildApi() {
    std::memset(&api_, 0, sizeof(api_));
    api_.abi_version = PP_ABI_VERSION;
    api_.struct_size = (uint32_t)sizeof(PP_HostApi);

    api_.log = &api_log;

    api_.play = &api_play;
    api_.pause = &api_pause;
    api_.toggle_pause = &api_toggle_pause;
    api_.stop = &api_stop;
    api_.seek_absolute = &api_seek_abs;
    api_.seek_relative = &api_seek_rel;
    api_.set_volume = &api_set_volume;
    api_.get_volume = &api_get_volume;
    api_.set_muted = &api_set_muted;
    api_.is_muted = &api_is_muted;
    api_.set_speed = &api_set_speed;
    api_.is_paused = &api_is_paused;
    api_.get_position = &api_get_position;
    api_.get_duration = &api_get_duration;
    api_.open_media = &api_open_media;
    api_.get_media_info = &api_get_media_info;
    api_.get_stats = &api_get_stats;

    api_.subscribe = &api_subscribe;
    api_.unsubscribe = &api_unsubscribe;
    api_.publish = &api_publish;
    api_.dispatch = &api_dispatch;
    api_.topic_mode = &api_topic_mode;
    api_.set_topic_mode = &api_set_topic_mode;

    api_.register_service = &api_register_service;
    api_.unregister_service = &api_unregister_service;
    api_.get_service = &api_get_service;
    api_.list_services = &api_list_services;

    api_.register_command = &api_register_command;
    api_.unregister_command = &api_unregister_command;
    api_.invoke_command = &api_invoke_command;
    api_.list_commands = &api_list_commands;

    api_.acquire_frame = &api_acquire_frame;
    api_.release_frame = &api_release_frame;
    api_.set_frame_processor = &api_set_frame_processor;
    api_.remove_frame_processor = &api_remove_frame_processor;

    api_.osd = api_osd();
    api_.overlay = api_overlay();       /* P1-b 通用叠加层 */

    api_.config_get = &api_config_get;
    api_.config_set = &api_config_set;

    api_.app_dir = &api_app_dir;
    api_.plugin_dir = &api_plugin_dir;
    api_.asset_dir = &api_asset_dir;
    api_.ticks_ms = &api_ticks_ms;
    api_.request_quit = &api_request_quit;
    api_.set_window_subtitle = &api_set_subtitle;
    api_.save_png = &api_save_png;
    api_.write_text_file = &api_write_text_file;
}

} // namespace pp
