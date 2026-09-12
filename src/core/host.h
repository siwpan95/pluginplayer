#pragma once
/* host.h — 插件宿主：把内核能力以 PP_HostApi 的形式暴露给插件
 *
 * 这里是「宿主侧」的全部状态：事件总线、服务注册表、命令注册表、帧处理链、配置。
 * 所有注册都带 owner（插件槽位号），插件卸载时可一次性撤销 —— 对应 Cordis 的
 * 「每个注册都应有对应的 disposer」。
 */
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>

#include "player_plugin.h"
#include "eventbus.h"
#include "registry.h"
#include "config.h"
#include "player.h"

/* SDK 中的不透明句柄 PP_Host 在宿主侧的真实定义（必须与插件看到的布局一致）。
 * 注意：必须定义在全局作用域，且与 SDK 的 `typedef struct PP_Host PP_Host;` 同名同型。 */
struct PP_Host {
    void* impl;
};

namespace pp {

class App;

class Host {
public:
    bool init(const std::string& appDir);
    void shutdown();

    void setApp(App* a) { app_ = a; }
    App* app() const { return app_; }
    void setPlayer(Player* p) { player_ = p; }
    Player* player() const { return player_; }

    EventBus&        bus() { return bus_; }
    ServiceRegistry& services() { return services_; }
    CommandRegistry& commands() { return commands_; }
    FrameChain&      chain() { return chain_; }
    Config&          config() { return config_; }

    /** 转为插件可见的不透明句柄。 */
    PP_Host* handle();
    static Host* from(PP_Host* h);

    /** 当前正在执行的插件槽位；所有注册都归到它名下。 */
    void setOwner(int owner) { owner_ = owner; }
    int  owner() const { return owner_; }

    /* --- 插件名（用于配置键） ---
     * 槽位号是按 DLL 字母序分配的，增删插件会让 plugin.<槽位>.<键> 整体错位。
     * 因此配置键改用插件名：plugin.<插件名>.<键>（插件名来自 PP_Plugin.name，
     * 非 [a-z0-9_-] 字符替换为下划线）。首次访问新键时会自动把旧槽位键的值迁过来。 */
    void setOwnerName(int owner, const std::string& name) { ownerNames_[owner] = name; }
    std::string ownerName(int owner) const;
    /** 把 "键" 解析成实际使用的配置键（含旧键迁移）。 */
    std::string pluginConfigKey(int owner, const std::string& key);

    /** 调用插件回调时自动切换 owner 的守卫。 */
    struct OwnerScope {
        Host* h;
        int   prev;
        OwnerScope(Host* host, int owner) : h(host), prev(host->owner()) { h->setOwner(owner); }
        ~OwnerScope() { h->setOwner(prev); }
    };

    /** 撤销某个插件的全部注册，返回撤销条目数。 */
    int revokeAll(int owner);

    /* --- 插件可见的订阅句柄 ---
     * FrameChain 与 EventBus 各有自己的 nextId_，**都从 1 开始**。
     * 若 subscribe() 直接把内部 id 返回给插件，两类订阅极易撞号，
     * 而早期版本的 unsubscribe() 会先扫帧链再退事件总线 → 撤错对象。
     * 这里由 Host 统一分配插件可见 id，并记录归属，彻底隔离两个命名空间。 */
    enum class SubKind { EventBus = 0, FrameChain = 1 };
    int  registerSub(SubKind kind, int realId, int owner);
    bool takeSub(int id, int* kind, int* realId);
    void dropSubsOf(int owner);

    /* --- 插件写入白名单（安全策略） ---
     * save_png / write_text_file 只允许写进这些目录，默认只有 exeDir()。
     * 可通过 ini 的 security.write_dirs（分号分隔）或命令行 --write-dir 追加。
     * 目的：一个已被 RPC 触发的插件命令不再能把文件写到任意路径。 */
    void addWriteRoot(const std::string& dir);
    const std::vector<std::string>& writeRoots() const { return writeRoots_; }
    bool isWriteAllowed(const std::string& path) const;

    /* --- 由 App 驱动 --- */
    int  runFrameChain(PP_FrameView* view);   /* WATERFALL：video.frame */
    void emitOsdDraw();                       /* EMIT：osd.draw */
    void cleanSnapshot();

    /** 执行插件命令：会把 owner 切成该命令所属插件，
     *  这样命令里的 config_get/set、register_* 都归属于正确的插件。 */
    int  invokeCommand(const std::string& name, int argc, const char** argv);
    bool hasCommand(const std::string& name) const { return commands_.has(name); }

    /** 发布事件并同步转发给进程外监听者（控制通道 / host-js）。 */
    int  publishTopic(const char* topic, const char* json);
    using EventSink = std::function<void(const std::string&, const std::string&)>;
    void setEventSink(EventSink s) { eventSink_ = std::move(s); }

    /* --- 内部产物 --- */
    std::string commandsText() const;
    std::string servicesText() const;
    std::string topicsText() const;
    const std::string& appDir() const { return appDir_; }
    const std::string& pluginDir() const { return pluginDir_; }
    const std::string& assetDir() const { return assetDir_; }

    void setSubtitle(const std::string& s) { subtitle_ = s; subtitleDirty_ = true; }
    bool takeSubtitleDirty() { bool d = subtitleDirty_; subtitleDirty_ = false; return d; }
    const std::string& subtitle() const { return subtitle_; }

    const PP_HostApi* api() { return &api_; }

    /* --- subscribe(PP_TOPIC_VIDEO_FRAME) 的适配器：把事件回调接进帧处理链 --- */
    struct FrameSubAdapter {
        int        owner;
        PP_EventFn fn;
        void*      user;
        int        chainId;
    };
    std::vector<std::unique_ptr<FrameSubAdapter>>& frameSubs() { return frameSubs_; }
    void dropFrameSub(int chainId);

    /* --- 快照会话（acquire_frame / release_frame） --- */
    void setSnapFrame(const std::shared_ptr<VideoFrame>& f, PP_FrameView* out);
    std::shared_ptr<VideoFrame> takeSnapFrame();

    /** config_get 需要返回稳定指针，这里缓存（std::map 节点地址稳定）。 */
    const char* cacheConfig(const std::string& key, const std::string& value);

private:
    void buildApi();

    App*    app_ = nullptr;
    Player* player_ = nullptr;
    int     owner_ = 0;
    std::map<int, std::string> ownerNames_;   /* 槽位 -> 插件名 */

    EventBus        bus_;
    ServiceRegistry services_;
    CommandRegistry commands_;
    FrameChain      chain_;
    Config          config_;

    std::string appDir_, pluginDir_, assetDir_, configPath_, logPath_;
    std::string subtitle_;
    bool        subtitleDirty_ = false;
    EventSink   eventSink_;

    /* 给 config_get 返回稳定指针用的缓存（std::map 节点地址稳定） */
    std::map<std::string, std::string> cfgCache_;

    /* acquire_frame / release_frame 的会话状态 */
    std::shared_ptr<VideoFrame> snapFrame_;
    PP_FrameView                snapView_{};

    std::vector<std::unique_ptr<FrameSubAdapter>> frameSubs_;

    /* 插件写入白名单（见 addWriteRoot 注释） */
    std::vector<std::string> writeRoots_;

    /* 插件可见订阅 id → 真实归属（见 SubKind 注释） */
    struct SubRef { SubKind kind; int realId; int owner; };
    std::map<int, SubRef> subMap_;
    int                   nextSubId_ = 1;

    PP_HostApi  api_{};
    PP_Host     cHandle_{};
};

} // namespace pp
