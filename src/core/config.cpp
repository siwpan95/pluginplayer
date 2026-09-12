/* config.cpp */
#include "config.h"
#include "util.h"

#include <cstdio>
#include <cstdlib>

namespace pp {

bool Config::load(const std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    path_ = path;
    map_.clear();
    dirty_ = false;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        std::string s = trim(line);
        if (s.empty() || s[0] == '#' || s[0] == ';') continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(s.substr(0, eq));
        std::string v = trim(s.substr(eq + 1));
        if (!k.empty()) map_[k] = v;
    }
    fclose(f);
    return true;
}

bool Config::saveAs(const std::string& path) const {
    std::map<std::string, std::string> snap;
    {
        std::lock_guard<std::mutex> lk(mu_);
        snap = map_;
    }
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    fputs("# PluginPlayer 配置（自动生成，可手工编辑）\n", f);
    fputs("# 格式: key=value   '#' 开头为注释\n\n", f);
    for (const auto& kv : snap) {
        fprintf(f, "%s=%s\n", kv.first.c_str(), kv.second.c_str());
    }
    fclose(f);

    /* 原子替换 */
    remove(path.c_str());
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        remove(tmp.c_str());
        return false;
    }
    return true;
}

bool Config::save() const {
    std::string p;
    {
        std::lock_guard<std::mutex> lk(mu_);
        p = path_;
    }
    if (p.empty()) return false;
    return saveAs(p);
}

std::string Config::get(const std::string& key, const std::string& def) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = map_.find(key);
    return it == map_.end() ? def : it->second;
}

double Config::getNumber(const std::string& key, double def) const {
    std::string v = get(key);
    if (v.empty()) return def;
    char* end = nullptr;
    double d = strtod(v.c_str(), &end);
    return end == v.c_str() ? def : d;
}

int Config::getInt(const std::string& key, int def) const {
    return (int)getNumber(key, (double)def);
}

bool Config::getBool(const std::string& key, bool def) const {
    std::string v = lower(trim(get(key)));
    if (v.empty()) return def;
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

void Config::set(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lk(mu_);
    if (map_.find(key) != map_.end() && map_[key] == value) return;
    map_[key] = value;
    dirty_ = true;
}

void Config::setNumber(const std::string& key, double value) {
    set(key, format("%g", value));
}

void Config::setInt(const std::string& key, int value) {
    set(key, format("%d", value));
}

bool Config::has(const std::string& key) const {
    std::lock_guard<std::mutex> lk(mu_);
    return map_.find(key) != map_.end();
}

std::map<std::string, std::string> Config::all() const {
    std::lock_guard<std::mutex> lk(mu_);
    return map_;
}

} // namespace pp
