#pragma once
/* eventbus.h — 主题化事件总线（emit / waterfall / serial / bail）
 *
 * 语义与 Cordis 的事件分发模式对齐（见 docs/PLUGIN_GUIDE.md）：
 *   EMIT      广播，忽略返回值
 *   WATERFALL 环绕中间件：可改写负载、可短路；帧处理链用它
 *   SERIAL    顺序执行，返回首个非零结果
 *   BAIL      顺序执行，遇到第一个 consumed 即停
 *
 * 所有注册项都记录 owner（插件槽位），卸载插件时可一次性撤销 —— 对应
 * Cordis 的「注册是可逆的副作用」。
 */
#include <string>
#include <vector>
#include <map>
#include <mutex>

#include "player_plugin.h"

namespace pp {

struct Subscription {
    int         id;
    int         owner;
    std::string topic;
    PP_EventFn  fn;
    void*       user;
};

class EventBus {
public:
    int  subscribe(const std::string& topic, PP_EventFn fn, void* user, int owner);
    void unsubscribe(int id);
    int  unsubscribeAllOf(int owner);           /* 返回撤销数量 */

    /** EMIT 语义。返回被通知的监听者数量。 */
    int  publish(PP_Host* host, const char* topic, const char* json);

    /** WATERFALL / SERIAL / BAIL 语义。返回链结果。 */
    int  dispatch(PP_Host* host, const char* topic, void* payload, int size);

    int  topicMode(const char* topic) const;
    /** 插件自定义主题的分发模式（覆盖默认模式表）。
     *  不提供这个接口时，插件自定义的主题只能走 EMIT —— 想用 waterfall/BAIL
     *  就无处声明。注册会记 owner，插件卸载时一并清除（可撤销注册）。 */
    int  setTopicMode(const std::string& topic, int mode, int owner);
    int  clearTopicModesOf(int owner);
    int  subscriberCount(const std::string& topic) const;
    int  totalSubscriptions() const;
    std::vector<std::string> topics() const;

private:
    mutable std::mutex         mu_;
    std::vector<Subscription>  subs_;
    int                        nextId_ = 1;
    std::map<std::string, int> modeOverride_;      /* 主题 -> 模式 */
    std::map<std::string, int> modeOverrideOwner_; /* 主题 -> 注册它的插件槽位 */
};

} // namespace pp
