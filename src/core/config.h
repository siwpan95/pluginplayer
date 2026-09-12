#pragma once
/* config.h — 极简持久化配置（key=value 文本），支持默认值、注释与原子保存 */
#include <string>
#include <map>
#include <mutex>

namespace pp {

class Config {
public:
    bool load(const std::string& path);
    bool save() const;                       /* 保存到 load 时的路径 */
    bool saveAs(const std::string& path) const;

    std::string get(const std::string& key, const std::string& def = "") const;
    double      getNumber(const std::string& key, double def) const;
    int         getInt(const std::string& key, int def) const;
    bool        getBool(const std::string& key, bool def) const;
    void        set(const std::string& key, const std::string& value);
    void        setNumber(const std::string& key, double value);
    void        setInt(const std::string& key, int value);
    bool        has(const std::string& key) const;
    std::map<std::string, std::string> all() const;
    const std::string& path() const { return path_; }
    bool dirty() const { return dirty_; }

private:
    mutable std::mutex                 mu_;
    std::string                        path_;
    std::map<std::string, std::string> map_;
    bool                               dirty_ = false;
};

} // namespace pp
