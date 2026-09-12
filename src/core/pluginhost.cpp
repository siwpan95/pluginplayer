/* pluginhost.cpp */
#include "pluginhost.h"
#include "util.h"

#include <cstring>
#include <cstdio>
#include <map>
#include <algorithm>

namespace pp {

/* ---- 插件完整性清单（可选） ----
 * 文件 <pluginDir>/manifest.sha256，每行 "<64 位小写 sha256>  <文件名>"，
 * '#' 开头为注释（兼容 sha256sum 的 "*name" 二进制标记）。
 * 存在即启用：未列出或哈希不符的插件一律拒绝加载。 */

static void loadManifest(const std::string& dir, bool* enabled,
                         std::map<std::string, std::string>* out) {
    out->clear();
    *enabled = false;
    std::string mf = pathJoin(dir, "manifest.sha256");
    /* 用 C stdio 而不是 <fstream>：后者会把 iostream/locale 整套拖进二进制，
     * 实测让 exe 从 ~480KB 涨到 ~1.2MB。 */
    FILE* fp = fopen(mf.c_str(), "rb");
    if (!fp) {
        PP_INFO("未发现 plugins/manifest.sha256 —— 本次不做插件完整性校验");
        return;
    }
    int bad = 0;
    char line[1024];
    while (fgets(line, (int)sizeof(line), fp)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        size_t sp = t.find_first_of(" \t");
        if (sp == std::string::npos) { bad++; continue; }
        std::string h = lower(t.substr(0, sp));
        std::string name = trim(t.substr(sp + 1));
        if (!name.empty() && name[0] == '*') name = name.substr(1);
        name = baseName(name);
        if (h.size() != 64 || name.empty()) { bad++; continue; }
        (*out)[lower(name)] = h;
    }
    fclose(fp);
    *enabled = true;
    if (bad > 0)
        PP_WARN("完整性清单有 %d 行无法解析（已忽略）", bad);
    PP_INFO("插件完整性清单已启用: %s（%d 条）", mf.c_str(), (int)out->size());
    if (out->empty())
        PP_WARN("完整性清单为空 —— 所有插件都会被拒绝加载");
}

/** 清单校验：文件不存在或校验通过返回 true。失败时填 err。 */
static bool verifyPluginIntegrity(const std::string& path, const std::string& file,
                                 bool enabled,
                                 const std::map<std::string, std::string>& manifest,
                                 std::string* err) {
    if (!enabled) return true;
    auto it = manifest.find(lower(file));
    if (it == manifest.end()) {
        if (err) *err = "未在 plugins/manifest.sha256 中列出（完整性校验拒绝加载）";
        return false;
    }
    std::string got;
    if (!sha256FileHex(path, &got)) {
        if (err) *err = "无法计算 SHA-256（拒绝加载）";
        return false;
    }
    if (got != it->second) {
        if (err)
            *err = format("SHA-256 与清单不符（期望 %s…，实测 %s…）",
                          it->second.substr(0, 16).c_str(), got.substr(0, 16).c_str());
        return false;
    }
    return true;
}

int PluginHost::discover(const std::string& dir) {
    slots_.clear();
    nextSlot_ = 1;
    if (!dirExists(dir)) {
        PP_WARN("插件目录不存在: %s", dir.c_str());
        return 0;
    }
    loadManifest(dir, &manifestEnabled_, &manifest_);
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

    /* 完整性清单校验（清单存在时才生效） */
    {
        std::string iverr;
        if (!verifyPluginIntegrity(s.path, s.file, manifestEnabled_, manifest_, &iverr)) {
            s.failed = true;
            s.message = iverr;
            PP_ERROR("[%s] %s", s.file.c_str(), s.message.c_str());
            return false;
        }
    }

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

    /* 告诉宿主这个槽位叫什么 —— on_load 里的 config_get 就要按名字取键了 */
    host->setOwnerName(s.slot, s.name);

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

    /* 先"空载"校验新映像，通过了再卸旧的。
     * 旧实现是先卸再装：一旦新 DLL 损坏/ABI 不符，这个插件就被直接丢掉
     * （且它已注册的命令/服务全被撤销），而调用方只拿到一个 false。 */
    if (!fileExists(s.path)) {
        PP_ERROR("热重载失败：文件不存在 %s", s.path.c_str());
        return false;
    }
    {
        std::string iverr;
        if (!verifyPluginIntegrity(s.path, s.file, manifestEnabled_, manifest_, &iverr)) {
            PP_ERROR("热重载失败：%s —— 保留当前版本", iverr.c_str());
            return false;
        }
    }
    {
        SDL_SharedObject* probe = SDL_LoadObject(s.path.c_str());
        if (!probe) {
            PP_ERROR("热重载失败：新映像无法加载 %s（%s）——保留当前版本",
                     s.path.c_str(), SDL_GetError());
            return false;
        }
        typedef uint32_t (*AbiFn)(void);
        typedef const PP_Plugin* (*QueryFn)(void);
        AbiFn pAbi = (AbiFn)SDL_LoadFunction(probe, "pp_plugin_abi_version");
        QueryFn pQuery = (QueryFn)SDL_LoadFunction(probe, PP_PLUGIN_ENTRY);
        uint32_t abi = pAbi ? pAbi() : 0;
        const PP_Plugin* ent = pQuery ? pQuery() : nullptr;
        bool ok = ent && abi == PP_ABI_VERSION && ent->abi_version == PP_ABI_VERSION;
        SDL_UnloadObject(probe);
        if (!ok) {
            PP_ERROR("热重载失败：新映像校验不通过（文件 abi=%u，入口=%s）——保留当前版本",
                     abi, ent ? "有" : "缺失");
            return false;
        }
    }

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
