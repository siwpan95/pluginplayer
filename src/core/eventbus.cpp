/* eventbus.cpp */
#include "eventbus.h"
#include "util.h"

#include <algorithm>
#include <map>

namespace pp {

/* 主题 -> 分发模式。未登记的主题默认 EMIT。 */
static int defaultModeOf(const std::string& topic) {
    static const std::map<std::string, int> kModes = {
        {PP_TOPIC_VIDEO_FRAME,   PP_DISPATCH_WATERFALL},
        {PP_TOPIC_UI_KEY,        PP_DISPATCH_BAIL},
        {PP_TOPIC_UI_MOUSE,      PP_DISPATCH_BAIL},
        {PP_TOPIC_AUDIO_SAMPLES, PP_DISPATCH_EMIT},
        {PP_TOPIC_APP_TICK,      PP_DISPATCH_EMIT},
        {PP_TOPIC_OSD_DRAW,      PP_DISPATCH_EMIT},
    };
    auto it = kModes.find(topic);
    return it == kModes.end() ? PP_DISPATCH_EMIT : it->second;
}

int EventBus::topicMode(const char* topic) const {
    if (!topic) return PP_DISPATCH_EMIT;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = modeOverride_.find(topic);
    if (it != modeOverride_.end()) return it->second;
    return defaultModeOf(topic);
}

int EventBus::setTopicMode(const std::string& topic, int mode, int owner) {
    if (topic.empty()) return -1;
    if (mode != PP_DISPATCH_EMIT && mode != PP_DISPATCH_WATERFALL &&
        mode != PP_DISPATCH_SERIAL && mode != PP_DISPATCH_BAIL) return -1;
    std::lock_guard<std::mutex> lk(mu_);
    modeOverride_[topic] = mode;
    modeOverrideOwner_[topic] = owner;
    return 0;
}

int EventBus::clearTopicModesOf(int owner) {
    std::lock_guard<std::mutex> lk(mu_);
    int n = 0;
    for (auto it = modeOverrideOwner_.begin(); it != modeOverrideOwner_.end();) {
        if (it->second == owner) {
            modeOverride_.erase(it->first);
            it = modeOverrideOwner_.erase(it);
            n++;
        } else {
            ++it;
        }
    }
    return n;
}

int EventBus::subscribe(const std::string& topic, PP_EventFn fn, void* user, int owner) {
    if (!fn || topic.empty()) return 0;
    std::lock_guard<std::mutex> lk(mu_);
    Subscription s;
    s.id = nextId_++;
    s.owner = owner;
    s.topic = topic;
    s.fn = fn;
    s.user = user;
    subs_.push_back(s);
    return s.id;
}

void EventBus::unsubscribe(int id) {
    if (id <= 0) return;
    std::lock_guard<std::mutex> lk(mu_);
    subs_.erase(std::remove_if(subs_.begin(), subs_.end(),
                               [id](const Subscription& s) { return s.id == id; }),
                subs_.end());
}

int EventBus::unsubscribeAllOf(int owner) {
    std::lock_guard<std::mutex> lk(mu_);
    size_t before = subs_.size();
    subs_.erase(std::remove_if(subs_.begin(), subs_.end(),
                               [owner](const Subscription& s) { return s.owner == owner; }),
                subs_.end());
    return (int)(before - subs_.size());
}

int EventBus::publish(PP_Host* host, const char* topic, const char* json) {
    if (!topic) return 0;
    std::vector<Subscription> snap;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& s : subs_)
            if (s.topic == topic) snap.push_back(s);
    }
    for (auto& s : snap) {
        PP_EventCtx ctx{};
        ctx.topic = s.topic.c_str();
        ctx.host = host;
        ctx.user = s.user;
        ctx.json = json;
        ctx.payload = nullptr;
        ctx.payload_size = 0;
        ctx.mode = PP_DISPATCH_EMIT;
        s.fn(&ctx);
    }
    return (int)snap.size();
}

int EventBus::dispatch(PP_Host* host, const char* topic, void* payload, int size) {
    if (!topic) return 0;
    int mode = topicMode(topic);      /* 走覆盖表：插件自定义主题也能声明分发模式 */
    std::vector<Subscription> snap;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& s : subs_)
            if (s.topic == topic) snap.push_back(s);
    }
    for (auto& s : snap) {
        PP_EventCtx ctx{};
        ctx.topic = s.topic.c_str();
        ctx.host = host;
        ctx.user = s.user;
        ctx.json = nullptr;
        ctx.payload = payload;
        ctx.payload_size = size;
        ctx.consumed = 0;
        ctx.result = 0;
        ctx.mode = mode;
        int ret = s.fn(&ctx);

        if (mode == PP_DISPATCH_WATERFALL) {
            if (ret == PP_CHAIN_DROP) return PP_CHAIN_DROP;
            if (ret == PP_CHAIN_BAIL) return PP_CHAIN_BAIL;
        } else if (mode == PP_DISPATCH_SERIAL) {
            if (ret != 0) return ret;
        } else if (mode == PP_DISPATCH_BAIL) {
            if (ctx.consumed) return 1;
        }
    }
    return 0;
}

int EventBus::subscriberCount(const std::string& topic) const {
    std::lock_guard<std::mutex> lk(mu_);
    int n = 0;
    for (const auto& s : subs_) if (s.topic == topic) n++;
    return n;
}

int EventBus::totalSubscriptions() const {
    std::lock_guard<std::mutex> lk(mu_);
    return (int)subs_.size();
}

std::vector<std::string> EventBus::topics() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> t;
    for (const auto& s : subs_)
        if (std::find(t.begin(), t.end(), s.topic) == t.end()) t.push_back(s.topic);
    std::sort(t.begin(), t.end());
    return t;
}

} // namespace pp
