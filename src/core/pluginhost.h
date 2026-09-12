#pragma once
/* pluginhost.h — 插件发现 / 加载 / 生命周期 / 卸载 / 热重载
 *
 * 用 SDL3 的动态加载设施（SDL_LoadObject / SDL_LoadFunction），
 * 好处是与窗口/渲染共用同一套 SDL 运行时，不必自己写 LoadLibrary 分支。
 */
#include <string>
#include <vector>
#include <SDL3/SDL.h>

#include "player_plugin.h"
#include "host.h"

namespace pp {

struct PluginSlot {
    int               slot = 0;      /* owner 槽位号，从 1 开始 */
    std::string       file;
    std::string       path;
    SDL_SharedObject* handle = nullptr;
    const PP_Plugin*  entry = nullptr;
    int               abi = 0;
    bool              loaded = false;
    bool              started = false;
    bool              failed = false;
    std::string       message;
    std::string       name = "-";
    std::string       version = "-";
    std::string       author = "-";
    std::string       description = "-";
};

class PluginHost {
public:
    /** 扫描目录下所有 .dll（不加载）。 */
    int discover(const std::string& dir);

    /** 按序加载全部插件：ABI 校验 + on_load。返回成功数。 */
    int loadAll(Host* host);

    /** 全部加载完成后调用 on_start。 */
    void startAll(Host* host);

    /** 停止（on_stop）。 */
    void stopAll(Host* host);

    /** 撤销注册 + on_unload + 释放 DLL。 */
    void unloadAll(Host* host);

    /** 热重载第 index 个插件。 */
    bool reloadOne(Host* host, int index);

    const std::vector<PluginSlot>& slots() const { return slots_; }
    int  loadedCount() const;
    int  failedCount() const;
    std::string report(bool verbose = false) const;
    std::string jsonSummary() const;

private:
    bool loadOne(Host* host, PluginSlot& s);
    void unloadOne(Host* host, PluginSlot& s);

    std::vector<PluginSlot> slots_;
    int                     nextSlot_ = 1;
};

} // namespace pp
