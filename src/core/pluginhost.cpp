/* pluginhost.cpp */
#include "pluginhost.h"
#include "util.h"

#include <cstring>

namespace pp {

int PluginHost::discover(const std::string& dir) {
    slots_.clear();
    nextSlot_ = 1;
    if (!dirExists(dir)) {
        PP_WARN("插件目录不存在: %s", dir.c_str());
        return 0;
    }
    auto files = listDir(dir, ".dll");
    for (const auto& f : files) {
        PluginSlot s;
        s.slot = nextSlot_++;
        s.path = f;
        s.file = baseName(f);
        slots_.push_back(s);
    }
    PP_INFO("发现 %d 个插件文件于 %s", (int)slots_.size(), dir.c_str());
    return (int)slots_.size();
}

bool PluginHost::loadOne(Host* host, PluginSlot& s) {
    s.message.clear();
    s.failed = false;
    s.loaded = false;
    s.started = false;

    s.handle = SDL_LoadObject(s.path.c_str());
    if (!s.handle) {
        s.failed = true;
        s.message = format("SDL_LoadObject 失败: %s", SDL_GetError());
        PP_ERROR("[%s] %s", s.file.c_str(), s.message.c_str());
        return false;
    }

    /* 先做 ABI 探针（可选符号），拿到版本号再决定是否继续 */
    typedef uint32_t (*AbiFn)(void);
    typedef const PP_Plugin* (*QueryFn)(void);

    AbiFn abiFn = (AbiFn)SDL_LoadFunction(s.handle, "pp_plugin_abi_version");
    QueryFn queryFn = (QueryFn)SDL_LoadFunction(s.handle, PP_PLUGIN_ENTRY);
    if (!queryFn) {
        s.failed = true;
        s.message = format("缺少入口符号 %s（是否忘了 PP_PLUGIN_DECL？）", PP_PLUGIN_ENTRY);
        PP_ERROR("[%s] %s", s.file.c_str(), s.message.c_str());
        SDL_UnloadObject(s.handle);
        s.handle = nullptr;
        return false;
    }
    s.abi = abiFn ? (int)abiFn() : 0;
    if (s.abi != (int)PP_ABI_VERSION) {
        s.failed = true;
        s.message = format("ABI 版本不匹配：插件=%d 宿主=%u", s.abi, PP_ABI_VERSION);
        PP_ERROR("[%s] %s", s.file.c_str(), s.message.c_str());
        SDL_UnloadObject(s.handle);
        s.handle = nullptr;
        return false;
    }

    s.entry = queryFn();
    if (!s.entry) {
        s.failed = true;
        s.message = "pp_plugin_query 返回空指针";
        SDL_UnloadObject(s.handle);
        s.handle = nullptr;
        return false;
    }
    if (s.entry->abi_version != PP_ABI_VERSION) {
        s.failed = true;
        s.message = format("PP_Plugin.abi_version=%u 与宿主的 %u 不一致",
                           s.entry->abi_version, PP_ABI_VERSION);
        SDL_UnloadObject(s.handle);
        s.handle = nullptr;
        return false;
    }

    s.name = s.entry->name ? s.entry->name : s.file;
    s.version = s.entry->version ? s.entry->version : "-";
    s.author = s.entry->author ? s.entry->author : "-";
    s.description = s.entry->description ? s.entry->description : "-";

    if (s.entry->on_load) {
        Host::OwnerScope scope(host, s.slot);
        int r = s.entry->on_load(host->handle(), host->api());
        if (r != 0) {
            s.failed = true;
            s.message = format("on_load 返回 %d", r);
            host->revokeAll(s.slot);
            PP_ERROR("[%s] %s", s.name.c_str(), s.message.c_str());
            s.entry = nullptr;
            SDL_UnloadObject(s.handle);
            s.handle = nullptr;
            return false;
        }
    }

    s.loaded = true;
    s.message = "OK";
    PP_INFO("已加载插件 #%d %s v%s (%s) — %s", s.slot, s.name.c_str(),
            s.version.c_str(), s.file.c_str(), s.description.c_str());
    return true;
}

void PluginHost::unloadOne(Host* host, PluginSlot& s) {
    if (!s.handle) return;
    if (s.entry) {
        if (s.started && s.entry->on_stop) {
            Host::OwnerScope scope(host, s.slot);
            s.entry->on_stop(host->handle());
        }
        int revoked = host->revokeAll(s.slot); /* 撤销该插件的所有注册 */
        if (s.entry->on_unload) {
            Host::OwnerScope scope(host, s.slot);
            s.entry->on_unload(host->handle());
        }
        PP_INFO("已卸载插件 #%d %s（撤销 %d 项注册）", s.slot, s.name.c_str(), revoked);
    }
    s.entry = nullptr;
    s.started = false;
    s.loaded = false;
    SDL_UnloadObject(s.handle);
    s.handle = nullptr;
}

int PluginHost::loadAll(Host* host) {
    int ok = 0;
    for (auto& s : slots_) {
        if (loadOne(host, s)) ok++;
    }
    return ok;
}

void PluginHost::startAll(Host* host) {
    for (auto& s : slots_) {
        if (!s.loaded || !s.entry || !s.entry->on_start) continue;
        Host::OwnerScope scope(host, s.slot);
        int r = s.entry->on_start(host->handle());
        if (r != 0) {
            s.message = format("on_start 返回 %d", r);
            PP_WARN("[%s] %s", s.name.c_str(), s.message.c_str());
            host->revokeAll(s.slot);
            if (s.entry->on_unload) s.entry->on_unload(host->handle());
            s.entry = nullptr;
            SDL_UnloadObject(s.handle);
            s.handle = nullptr;
            s.loaded = false;
            s.failed = true;
        } else {
            s.started = true;
        }
    }
}

void PluginHost::stopAll(Host* host) {
    for (auto& s : slots_) {
        if (!s.loaded || !s.started || !s.entry || !s.entry->on_stop) continue;
        Host::OwnerScope scope(host, s.slot);
        s.entry->on_stop(host->handle());
        s.started = false;
    }
}

void PluginHost::unloadAll(Host* host) {
    for (auto& s : slots_) unloadOne(host, s);
}

bool PluginHost::reloadOne(Host* host, int index) {
    if (index < 0 || index >= (int)slots_.size()) return false;
    PluginSlot& s = slots_[index];
    bool wasStarted = s.started;
    PluginSlot keep = s;              /* 保留槽位与元信息 */
    unloadOne(host, s);
    s = keep;
    s.handle = nullptr;
    s.entry = nullptr;
    s.loaded = false;
    s.started = false;
    if (!loadOne(host, s)) return false;
    if (wasStarted && s.entry && s.entry->on_start) {
        Host::OwnerScope scope(host, s.slot);
        s.entry->on_start(host->handle());
        s.started = true;
    }
    return true;
}

int PluginHost::loadedCount() const {
    int n = 0;
    for (const auto& s : slots_) if (s.loaded) n++;
    return n;
}

int PluginHost::failedCount() const {
    int n = 0;
    for (const auto& s : slots_) if (s.failed) n++;
    return n;
}

std::string PluginHost::report(bool verbose) const {
    std::string out = format("插件目录共 %d 个 DLL：成功 %d，失败 %d\n",
                             (int)slots_.size(), loadedCount(), failedCount());
    out += format("%-4s %-22s %-10s %-8s %s\n", "#", "名称", "版本", "状态", "说明");
    out += "------------------------------------------------------------------------------\n";
    for (const auto& s : slots_) {
        const char* st = s.loaded ? "loaded" : (s.failed ? "FAILED" : "skipped");
        out += format("%-4d %-22s %-10s %-8s %s\n", s.slot,
                      s.name.c_str(), s.version.c_str(), st,
                      s.loaded && !verbose ? s.file.c_str() : s.message.c_str());
        if (verbose && s.loaded) {
            out += format("     file=%s\n     desc=%s\n     author=%s\n",
                          s.file.c_str(), s.description.c_str(), s.author.c_str());
        }
    }
    return out;
}

std::string PluginHost::jsonSummary() const {
    std::string j = "[";
    for (size_t i = 0; i < slots_.size(); ++i) {
        const auto& s = slots_[i];
        if (i) j += ",";
        j += format("{\"slot\":%d,\"name\":\"%s\",\"version\":\"%s\",\"file\":\"%s\","
                    "\"state\":\"%s\",\"message\":\"%s\"}",
                    s.slot, jsonEscape(s.name).c_str(), jsonEscape(s.version).c_str(),
                    jsonEscape(s.file).c_str(),
                    s.loaded ? "loaded" : (s.failed ? "failed" : "skipped"),
                    jsonEscape(s.message).c_str());
    }
    j += "]";
    return j;
}

} // namespace pp
