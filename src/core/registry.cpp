/* registry.cpp */
#include "registry.h"
#include "util.h"

#include <algorithm>
#include <cstring>

namespace pp {

/* ======================= ServiceRegistry ======================= */

bool ServiceRegistry::add(const std::string& name, const std::string& version,
                          void* iface, int owner) {
    if (name.empty() || !iface) return false;
    std::lock_guard<std::mutex> lk(mu_);
    ServiceEntry e{owner, name, version, iface};
    bool isNew = map_.find(name) == map_.end();
    map_[name] = e;
    return isNew;
}

void ServiceRegistry::remove(const std::string& name) {
    std::lock_guard<std::mutex> lk(mu_);
    map_.erase(name);
}

void* ServiceRegistry::get(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = map_.find(name);
    return it == map_.end() ? nullptr : it->second.iface;
}

int ServiceRegistry::removeAllOf(int owner) {
    std::lock_guard<std::mutex> lk(mu_);
    int n = 0;
    for (auto it = map_.begin(); it != map_.end();) {
        if (it->second.owner == owner) { it = map_.erase(it); n++; }
        else ++it;
    }
    return n;
}

int ServiceRegistry::list(char* out, int maxNames, int stride) const {
    std::lock_guard<std::mutex> lk(mu_);
    int i = 0;
    for (const auto& kv : map_) {
        if (i >= maxNames) break;
        char* dst = out + (size_t)i * stride;
        std::memset(dst, 0, (size_t)stride);
        std::snprintf(dst, (size_t)stride, "%s", kv.first.c_str());
        i++;
    }
    return i;
}

int ServiceRegistry::count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return (int)map_.size();
}

std::vector<std::string> ServiceRegistry::names() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> v;
    for (const auto& kv : map_) v.push_back(kv.first);
    return v;
}

/* ======================= CommandRegistry ======================= */

bool CommandRegistry::add(const std::string& name, const std::string& usage,
                          PP_CommandFn fn, void* user, int owner) {
    if (name.empty() || !fn) return false;
    std::lock_guard<std::mutex> lk(mu_);
    bool isNew = map_.find(name) == map_.end();
    map_[name] = CommandEntry{owner, name, usage, fn, user};
    return isNew;
}

void CommandRegistry::remove(const std::string& name) {
    std::lock_guard<std::mutex> lk(mu_);
    map_.erase(name);
}

int CommandRegistry::removeAllOf(int owner) {
    std::lock_guard<std::mutex> lk(mu_);
    int n = 0;
    for (auto it = map_.begin(); it != map_.end();) {
        if (it->second.owner == owner) { it = map_.erase(it); n++; }
        else ++it;
    }
    return n;
}

bool CommandRegistry::lookup(const std::string& name, CommandEntry* out) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = map_.find(name);
    if (it == map_.end()) return false;
    if (out) *out = it->second;
    return true;
}

int CommandRegistry::invoke(PP_Host* host, const std::string& name,
                            int argc, const char** argv) const {
    CommandEntry e;
    if (!lookup(name, &e)) return -1;
    return e.fn(host, argc, argv, e.user);
}

int CommandRegistry::list(char* out, int outSize) const {
    std::lock_guard<std::mutex> lk(mu_);
    /* 没有缓冲区就没有"已列出"的命令。少了这个判断，outSize<=0 会走到
     * `s.resize(outSize - 1)` 即 resize((size_t)-1) → 抛 length_error，
     * 而调用方（控制通道 command.list）并不会 expect 这个函数抛异常。
     * tests/unit/test_registry.cpp 有一条用例钉这个边界。 */
    if (!out || outSize <= 0) return 0;
    std::string s;
    std::vector<std::string> keys;
    for (const auto& kv : map_) keys.push_back(kv.first);
    for (const auto& k : keys) {
        const auto& e = map_.at(k);
        s += format("%-18s %-34s <plugin#%d>\n", e.name.c_str(),
                    e.usage.empty() ? "-" : e.usage.c_str(), e.owner);
    }
    if ((int)s.size() >= outSize) s.resize(outSize - 1);
    std::memcpy(out, s.c_str(), s.size());
    out[s.size()] = 0;
    return (int)keys.size();
}

int CommandRegistry::count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return (int)map_.size();
}

std::vector<std::string> CommandRegistry::names() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> v;
    for (const auto& kv : map_) v.push_back(kv.first);
    return v;
}

bool CommandRegistry::has(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mu_);
    return map_.find(name) != map_.end();
}

/* ======================= FrameChain ======================= */

int FrameChain::add(PP_FrameFn fn, void* user, int priority, int owner) {
    if (!fn) return 0;
    std::lock_guard<std::mutex> lk(mu_);
    FrameProcessor p{nextId_++, owner, priority, fn, user};
    items_.push_back(p);
    std::stable_sort(items_.begin(), items_.end(),
                     [](const FrameProcessor& a, const FrameProcessor& b) {
                         return a.priority < b.priority;
                     });
    return p.id;
}

void FrameChain::remove(int id) {
    std::lock_guard<std::mutex> lk(mu_);
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [id](const FrameProcessor& p) { return p.id == id; }),
                 items_.end());
}

int FrameChain::removeAllOf(int owner) {
    std::lock_guard<std::mutex> lk(mu_);
    size_t before = items_.size();
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [owner](const FrameProcessor& p) { return p.owner == owner; }),
                 items_.end());
    return (int)(before - items_.size());
}

int FrameChain::run(PP_Host* host, PP_FrameView* view) const {
    std::vector<FrameProcessor> snap;
    {
        std::lock_guard<std::mutex> lk(mu_);
        snap = items_;
    }
    for (const auto& p : snap) {
        int r = p.fn(host, view, p.user);
        if (r == PP_CHAIN_DROP) return PP_CHAIN_DROP;
        if (r == PP_CHAIN_BAIL) return PP_CHAIN_BAIL;
    }
    return PP_CHAIN_CONTINUE;
}

int FrameChain::count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return (int)items_.size();
}

} // namespace pp
