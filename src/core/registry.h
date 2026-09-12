#pragma once
/* registry.h — 服务注册表 / 命令注册表 / 帧处理链
 * 三者都是「可撤销的注册」：每个条目记录 owner（插件槽位），卸载时统一撤销。
 * 服务注册表对应 Cordis 的 ctx.<key>；命令注册表对应可被 UI / 外部调用的能力。
 */
#include <string>
#include <vector>
#include <mutex>
#include <map>

#include "player_plugin.h"

namespace pp {

/* --------------------------- 服务注册表 --------------------------- */
struct ServiceEntry {
    int         owner;
    std::string name;
    std::string version;
    void*       iface;
};

class ServiceRegistry {
public:
    bool  add(const std::string& name, const std::string& version, void* iface, int owner);
    void  remove(const std::string& name);
    void* get(const std::string& name) const;
    int   removeAllOf(int owner);
    int   list(char* out, int maxNames, int stride) const;
    int   count() const;
    std::vector<std::string> names() const;
private:
    mutable std::mutex                  mu_;
    std::map<std::string, ServiceEntry> map_;
};

/* --------------------------- 命令注册表 --------------------------- */
struct CommandEntry {
    int          owner;
    std::string  name;
    std::string  usage;
    PP_CommandFn fn;
    void*        user;
};

class CommandRegistry {
public:
    bool add(const std::string& name, const std::string& usage,
             PP_CommandFn fn, void* user, int owner);
    void remove(const std::string& name);
    int  removeAllOf(int owner);
    /** 查出命令条目（含 owner），供宿主在正确的插件上下文里执行它。 */
    bool lookup(const std::string& name, CommandEntry* out) const;
    /** 成功返回回调返回值；未找到返回 -1。
     *  注意：宿主应优先用 Host::invokeCommand()，它会把 owner 切成命令所属插件。 */
    int  invoke(PP_Host* host, const std::string& name,
                int argc, const char** argv) const;
    int  list(char* out, int outSize) const;
    int  count() const;
    std::vector<std::string> names() const;
    bool has(const std::string& name) const;
private:
    mutable std::mutex                  mu_;
    std::map<std::string, CommandEntry> map_;
};

/* --------------------------- 帧处理链 --------------------------- */
struct FrameProcessor {
    int        id;
    int        owner;
    int        priority;
    PP_FrameFn fn;
    void*      user;
};

class FrameChain {
public:
    int  add(PP_FrameFn fn, void* user, int priority, int owner);
    void remove(int id);
    int  removeAllOf(int owner);
    /** 依次调用；返回 PP_CHAIN_CONTINUE / BAIL / DROP。 */
    int  run(PP_Host* host, PP_FrameView* view) const;
    int  count() const;
private:
    mutable std::mutex           mu_;
    std::vector<FrameProcessor>  items_;
    int                          nextId_ = 1;
};

} // namespace pp
