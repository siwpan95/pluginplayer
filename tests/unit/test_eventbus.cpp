/* test_eventbus.cpp — 主题化事件总线（emit / waterfall / serial / bail）
 *
 * 四种分发模式的语义差别很细，而它们在项目里承担的是**不同角色的关键路径**：
 *   · WATERFALL  = 帧处理链：插件能改像素、能丢帧（返回 PP_CHAIN_DROP 必须短路）
 *   · BAIL       = 按键处理：插件吞掉按键后，后面的插件不该再看到
 * 语义错了的后果不是崩溃，而是"某个插件偶尔不生效"这类难查的问题。
 */
#include "harness.h"
#include "eventbus.h"

#include <string>
#include <vector>

using namespace pp;

/* ---- 回调用的记录器（PP_EventFn 是 C 函数指针，user 传 this） ---- */
struct Recorder {
    std::vector<std::string> seen;
    std::vector<std::string> payloads;
    int  ret = 0;                 /* 回调返回值（WATERFALL/SERIAL 用） */
    int  consumed = 0;            /* 是否吞掉（BAIL 用） */
    int  seen_mode = -1;
    int* counter = nullptr;       /* 可选：外部计数 */

    static int fn(PP_EventCtx* ctx) {
        Recorder* self = (Recorder*)ctx->user;
        if (!self) return 0;
        self->seen.push_back(ctx->topic ? ctx->topic : "(null)");
        self->payloads.push_back(ctx->json ? ctx->json
                                           : std::string((const char*)ctx->payload,
                                                         (size_t)ctx->payload_size));
        self->seen_mode = ctx->mode;
        if (self->counter) ++(*self->counter);
        if (self->consumed) ctx->consumed = 1;
        return self->ret;
    }
};

PP_TEST(eventbus, subscribe_and_publish_counts) {
    EventBus bus;
    Recorder a, b;
    int ida = bus.subscribe("t1", &Recorder::fn, &a, 1);
    PP_CHECK(ida > 0);
    int idb = bus.subscribe("t1", &Recorder::fn, &b, 2);
    PP_CHECK(idb > 0);
    PP_CHECK_MSG(ida != idb, "两个订阅必须拿到不同的句柄（否则卸载会撤错对象）");

    PP_EQ(bus.subscriberCount("t1"), 2);
    PP_EQ(bus.subscriberCount("t2"), 0);
    PP_EQ(bus.totalSubscriptions(), 2);

    int n = bus.publish(nullptr, "t1", "{\"x\":1}");
    PP_EQ(n, 2);
    PP_EQ(a.seen.size(), (size_t)1);
    PP_EQ(b.seen.size(), (size_t)1);
    PP_STR_EQ(a.seen[0], "t1");
    PP_STR_EQ(a.payloads[0], "{\"x\":1}");

    /* 别的主题不会串台 */
    PP_EQ(bus.publish(nullptr, "t2", "{}"), 0);
    PP_EQ(bus.publish(nullptr, nullptr, "{}"), 0);    /* 空主题：安全返回 */
}

PP_TEST(eventbus, subscribe_rejects_bad_input) {
    EventBus bus;
    Recorder a;
    PP_EQ(bus.subscribe("", &Recorder::fn, &a, 1), 0);      /* 空主题 */
    PP_EQ(bus.subscribe("t", nullptr, &a, 1), 0);          /* 空回调 */
    PP_EQ(bus.totalSubscriptions(), 0);
}

PP_TEST(eventbus, unsubscribe_by_id) {
    EventBus bus;
    Recorder a, b;
    int ida = bus.subscribe("t", &Recorder::fn, &a, 1);
    bus.subscribe("t", &Recorder::fn, &b, 1);

    bus.unsubscribe(ida);
    PP_EQ(bus.subscriberCount("t"), 1);
    bus.publish(nullptr, "t", "{}");
    PP_EQ(a.seen.size(), (size_t)0);                   /* 被撤的不该再收到 */
    PP_EQ(b.seen.size(), (size_t)1);

    /* 重复撤销幂等；非法句柄安全忽略 */
    bus.unsubscribe(ida);
    bus.unsubscribe(0);
    bus.unsubscribe(-5);
    PP_EQ(bus.subscriberCount("t"), 1);
}

PP_TEST(eventbus, unsubscribeAllOf_owner) {
    /* 插件卸载时按 owner 一次性撤销 —— 这是"注册是可逆的副作用"的落地方式。
     * 撤错 owner 会让别的插件的注册凭空消失，所以逐项钉住。 */
    EventBus bus;
    Recorder a, b;
    bus.subscribe("t", &Recorder::fn, &a, 7);
    bus.subscribe("t", &Recorder::fn, &a, 7);
    bus.subscribe("t", &Recorder::fn, &b, 8);

    PP_EQ(bus.unsubscribeAllOf(7), 2);
    PP_EQ(bus.totalSubscriptions(), 1);
    PP_EQ(bus.unsubscribeAllOf(7), 0);                 /* 再来一次：0，不是负数 */
    PP_EQ(bus.totalSubscriptions(), 1);
    PP_EQ(bus.unsubscribeAllOf(8), 1);
    PP_EQ(bus.totalSubscriptions(), 0);
    PP_EQ(bus.unsubscribeAllOf(999), 0);
}

PP_TEST(eventbus, topic_modes_default_table) {
    EventBus bus;
    /* 默认模式表：帧处理链是 WATERFALL，按键/鼠标是 BAIL，其余 EMIT */
    PP_EQ(bus.topicMode(PP_TOPIC_VIDEO_FRAME), PP_DISPATCH_WATERFALL);
    PP_EQ(bus.topicMode(PP_TOPIC_UI_KEY), PP_DISPATCH_BAIL);
    PP_EQ(bus.topicMode(PP_TOPIC_UI_MOUSE), PP_DISPATCH_BAIL);
    PP_EQ(bus.topicMode(PP_TOPIC_AUDIO_SAMPLES), PP_DISPATCH_EMIT);
    PP_EQ(bus.topicMode(PP_TOPIC_APP_TICK), PP_DISPATCH_EMIT);
    PP_EQ(bus.topicMode(PP_TOPIC_OSD_DRAW), PP_DISPATCH_EMIT);
    /* 未登记的主题默认 EMIT；空指针也要安全 */
    PP_EQ(bus.topicMode("my.custom.topic"), PP_DISPATCH_EMIT);
    PP_EQ(bus.topicMode(nullptr), PP_DISPATCH_EMIT);
}

PP_TEST(eventbus, set_topic_mode_override_and_revoke) {
    EventBus bus;
    /* 插件自定义主题想用 waterfall/BAIL，必须能声明；且声明是记 owner 的可逆注册 */
    PP_EQ(bus.setTopicMode("plug.x", PP_DISPATCH_BAIL, 5), 0);
    PP_EQ(bus.topicMode("plug.x"), PP_DISPATCH_BAIL);
    /* 非法 mode / 空主题要拒绝 */
    PP_EQ(bus.setTopicMode("plug.y", 99, 5), -1);
    PP_EQ(bus.setTopicMode("", PP_DISPATCH_EMIT, 5), -1);

    PP_EQ(bus.clearTopicModesOf(5), 1);
    PP_EQ(bus.topicMode("plug.x"), PP_DISPATCH_EMIT);   /* 回到默认 */
    PP_EQ(bus.clearTopicModesOf(5), 0);
    /* 覆盖表不该动到别人 */
    PP_EQ(bus.setTopicMode("a", PP_DISPATCH_SERIAL, 1), 0);
    PP_EQ(bus.setTopicMode("b", PP_DISPATCH_SERIAL, 2), 0);
    PP_EQ(bus.clearTopicModesOf(1), 1);
    PP_EQ(bus.topicMode("b"), PP_DISPATCH_SERIAL);
}

PP_TEST(eventbus, emit_calls_all_and_ignores_return) {
    EventBus bus;
    Recorder a, b;
    a.ret = 123;                                        /* EMIT 下返回值要被忽略 */
    bus.subscribe("e", &Recorder::fn, &a, 1);
    bus.subscribe("e", &Recorder::fn, &b, 2);
    PP_EQ(bus.dispatch(nullptr, "e", nullptr, 0), 0);
    PP_EQ(a.seen.size(), (size_t)1);
    PP_EQ(b.seen.size(), (size_t)1);                    /* 不会被 a 的返回值短路 */
    PP_EQ(a.seen_mode, PP_DISPATCH_EMIT);
}

PP_TEST(eventbus, waterfall_short_circuits_on_drop_and_bail) {
    EventBus bus;
    Recorder a, b;
    bus.subscribe(PP_TOPIC_VIDEO_FRAME, &Recorder::fn, &a, 1);
    bus.subscribe(PP_TOPIC_VIDEO_FRAME, &Recorder::fn, &b, 2);

    a.ret = PP_CHAIN_DROP;
    PP_EQ(bus.dispatch(nullptr, PP_TOPIC_VIDEO_FRAME, nullptr, 0), PP_CHAIN_DROP);
    PP_CHECK_MSG(b.seen.empty(), "WATERFALL 收到 DROP 必须短路：后面的插件不该被调用");
    PP_EQ(a.seen_mode, PP_DISPATCH_WATERFALL);

    /* BAIL 同理：链被中止 */
    Recorder c, d;
    EventBus bus2;
    bus2.subscribe(PP_TOPIC_VIDEO_FRAME, &Recorder::fn, &c, 1);
    bus2.subscribe(PP_TOPIC_VIDEO_FRAME, &Recorder::fn, &d, 2);
    c.ret = PP_CHAIN_BAIL;
    PP_EQ(bus2.dispatch(nullptr, PP_TOPIC_VIDEO_FRAME, nullptr, 0), PP_CHAIN_BAIL);
    PP_CHECK_MSG(d.seen.empty(), "WATERFALL 收到 BAIL 必须短路");

    /* CONTINUE：全链跑完，返回 0 */
    Recorder e, f;
    EventBus bus3;
    bus3.subscribe(PP_TOPIC_VIDEO_FRAME, &Recorder::fn, &e, 1);
    bus3.subscribe(PP_TOPIC_VIDEO_FRAME, &Recorder::fn, &f, 2);
    PP_EQ(bus3.dispatch(nullptr, PP_TOPIC_VIDEO_FRAME, nullptr, 0), 0);
    PP_EQ(e.seen.size(), (size_t)1);
    PP_EQ(f.seen.size(), (size_t)1);
}

PP_TEST(eventbus, bail_consumed_stops_chain) {
    EventBus bus;
    Recorder a, b, c;
    a.consumed = 1;                                     /* 第一个订阅者吞掉按键 */
    bus.subscribe(PP_TOPIC_UI_KEY, &Recorder::fn, &a, 1);
    bus.subscribe(PP_TOPIC_UI_KEY, &Recorder::fn, &b, 2);
    bus.subscribe(PP_TOPIC_UI_KEY, &Recorder::fn, &c, 3);

    PP_EQ(bus.dispatch(nullptr, PP_TOPIC_UI_KEY, nullptr, 0), 1);   /* 1 = 被消费 */
    PP_EQ(a.seen.size(), (size_t)1);
    PP_CHECK_MSG(b.seen.empty(), "吞掉按键后，后续插件不该再收到");
    PP_CHECK_MSG(c.seen.empty(), "同上");
    PP_EQ(a.seen_mode, PP_DISPATCH_BAIL);

    /* 没人吞：全链跑完，返回 0 */
    Recorder d, e;
    EventBus bus2;
    bus2.subscribe(PP_TOPIC_UI_KEY, &Recorder::fn, &d, 1);
    bus2.subscribe(PP_TOPIC_UI_KEY, &Recorder::fn, &e, 2);
    PP_EQ(bus2.dispatch(nullptr, PP_TOPIC_UI_KEY, nullptr, 0), 0);
    PP_EQ(d.seen.size(), (size_t)1);
    PP_EQ(e.seen.size(), (size_t)1);
}

PP_TEST(eventbus, mouse_bail_consumed_stops_chain) {
    /* P1-b 契约：鼠标事件先给插件（BAIL）。插件吞掉后，内核的"内置响应"
     * （点画面跳转）必须不再触发 —— 否则同一个点击既被插件处理又被内核跳转，
     * 表现就是"点了按钮却跳进度"。这条与按键的 BAIL 是同一套机制，但鼠标是
     * 新加的主题，单独钉一次防止回归。 */
    EventBus bus;
    Recorder a, b, c;
    a.consumed = 1;
    bus.subscribe(PP_TOPIC_UI_MOUSE, &Recorder::fn, &a, 1);
    bus.subscribe(PP_TOPIC_UI_MOUSE, &Recorder::fn, &b, 2);
    bus.subscribe(PP_TOPIC_UI_MOUSE, &Recorder::fn, &c, 3);

    PP_MouseEvent me{};
    me.type = PP_MOUSE_DOWN; me.button = 1; me.x = 100; me.y = 50;
    PP_EQ(bus.dispatch(nullptr, PP_TOPIC_UI_MOUSE, &me, (int)sizeof(me)), 1);
    PP_EQ(a.seen.size(), (size_t)1);
    PP_CHECK_MSG(b.seen.empty(), "鼠标被插件消费后，后续插件不该再收到");
    PP_CHECK_MSG(c.seen.empty(), "同上");
    PP_EQ(a.seen_mode, PP_DISPATCH_BAIL);

    /* 没人吞：全链跑完返回 0 */
    Recorder d, e;
    EventBus bus2;
    bus2.subscribe(PP_TOPIC_UI_MOUSE, &Recorder::fn, &d, 1);
    bus2.subscribe(PP_TOPIC_UI_MOUSE, &Recorder::fn, &e, 2);
    PP_EQ(bus2.dispatch(nullptr, PP_TOPIC_UI_MOUSE, &me, (int)sizeof(me)), 0);
    PP_EQ(d.seen.size(), (size_t)1);
    PP_EQ(e.seen.size(), (size_t)1);
}

PP_TEST(eventbus, serial_returns_first_nonzero) {
    EventBus bus;
    Recorder a, b, c;
    bus.subscribe("s", &Recorder::fn, &a, 1);
    bus.subscribe("s", &Recorder::fn, &b, 2);
    bus.subscribe("s", &Recorder::fn, &c, 3);
    PP_EQ(bus.setTopicMode("s", PP_DISPATCH_SERIAL, 0), 0);

    b.ret = 42;                                         /* 第二个返回非零 → 立即返回 */
    PP_EQ(bus.dispatch(nullptr, "s", nullptr, 0), 42);
    PP_EQ(a.seen.size(), (size_t)1);
    PP_EQ(b.seen.size(), (size_t)1);
    PP_CHECK_MSG(c.seen.empty(), "SERIAL 遇非零应立即返回，不该继续");

    /* 全零 → 返回 0，且都跑过 */
    Recorder d, e;
    EventBus bus2;
    bus2.subscribe("s", &Recorder::fn, &d, 1);
    bus2.subscribe("s", &Recorder::fn, &e, 2);
    bus2.setTopicMode("s", PP_DISPATCH_SERIAL, 0);
    PP_EQ(bus2.dispatch(nullptr, "s", nullptr, 0), 0);
    PP_EQ(d.seen.size(), (size_t)1);
    PP_EQ(e.seen.size(), (size_t)1);
}

PP_TEST(eventbus, payload_is_passed_through) {
    EventBus bus;
    Recorder a;
    bus.subscribe(PP_TOPIC_VIDEO_FRAME, &Recorder::fn, &a, 1);
    std::string payload = "PIXELS";
    PP_EQ(bus.dispatch(nullptr, PP_TOPIC_VIDEO_FRAME, (void*)payload.data(),
                       (int)payload.size()), 0);
    PP_EQ(a.payloads.size(), (size_t)1);
    PP_STR_EQ(a.payloads[0], "PIXELS");
}

PP_TEST(eventbus, subscriber_can_unsubscribe_during_dispatch) {
    /* 分发用的是快照，所以回调里撤销自己/别人不会让迭代器失效或崩溃。
     * 插件在 video.frame 里决定"我干完这次就下线"是合理用法。 */
    struct SelfUnsub {
        EventBus* bus;
        int myid;
        int calls = 0;
        static int fn(PP_EventCtx* ctx) {
            SelfUnsub* s = (SelfUnsub*)ctx->user;
            s->calls++;
            s->bus->unsubscribe(s->myid);
            return 0;
        }
    };
    EventBus bus;
    SelfUnsub s{&bus, 0, 0};
    s.myid = bus.subscribe("t", &SelfUnsub::fn, &s, 1);
    Recorder other;
    bus.subscribe("t", &Recorder::fn, &other, 2);

    PP_EQ(bus.dispatch(nullptr, "t", nullptr, 0), 0);   /* 不崩 */
    PP_EQ(s.calls, 1);
    PP_EQ(other.seen.size(), (size_t)1);
    PP_EQ(bus.subscriberCount("t"), 1);                 /* 自己已被撤掉 */
    PP_EQ(bus.dispatch(nullptr, "t", nullptr, 0), 0);
    PP_EQ(s.calls, 1);                                  /* 第二次不再被调用 */
}

PP_TEST(eventbus, topics_lists_unique_sorted) {
    EventBus bus;
    Recorder a;
    bus.subscribe("z", &Recorder::fn, &a, 1);
    bus.subscribe("a", &Recorder::fn, &a, 1);
    bus.subscribe("a", &Recorder::fn, &a, 1);
    auto t = bus.topics();
    PP_EQ(t.size(), (size_t)2);
    PP_STR_EQ(t[0], "a");
    PP_STR_EQ(t[1], "z");
}
