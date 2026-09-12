/* test_registry.cpp — 服务注册表 / 命令注册表 / 帧处理链
 *
 * 三者的共同契约是「注册是可撤销的副作用」：每个条目记 owner，插件卸载时
 * 按 owner 一次撤干净。项目自检里的 plugin_hot_reload 断言"重载后数量完全不变"，
 * 那是在测"有没有泄漏"；这里测的是**撤销本身对不对**（撤错对象、
 * 撤多了、撤少了，都不会让数量断言失败，但会让功能静默失效）。
 */
#include "harness.h"
#include "registry.h"

#include <string>
#include <vector>

using namespace pp;

/* ======================= 服务注册表 ======================= */

PP_TEST(registry, service_add_get_remove) {
    ServiceRegistry r;
    int iface = 1234;
    PP_CHECK(r.add("hotkeys", "1.0.0", &iface, 1));
    PP_EQ(r.count(), 1);
    PP_CHECK(r.get("hotkeys") == &iface);
    PP_CHECK(r.get("nope") == nullptr);

    auto names = r.names();
    PP_EQ(names.size(), (size_t)1);
    PP_STR_EQ(names[0], "hotkeys");

    r.remove("hotkeys");
    PP_EQ(r.count(), 0);
    PP_CHECK(r.get("hotkeys") == nullptr);
    r.remove("hotkeys");                                  /* 幂等 */
    PP_EQ(r.count(), 0);
}

PP_TEST(registry, service_rejects_bad_input) {
    ServiceRegistry r;
    int iface = 1;
    PP_CHECK(!r.add("", "1", &iface, 1));                 /* 空名字 */
    PP_CHECK(!r.add("x", "1", nullptr, 1));               /* 空接口 */
    PP_EQ(r.count(), 0);
}

PP_TEST(registry, service_removeAllOf_only_own_owner) {
    ServiceRegistry r;
    int a = 1, b = 2, c = 3;
    r.add("s1", "1", &a, 7);
    r.add("s2", "1", &b, 7);
    r.add("s3", "1", &c, 8);
    PP_EQ(r.count(), 3);

    PP_EQ(r.removeAllOf(7), 2);
    PP_EQ(r.count(), 1);
    PP_CHECK_MSG(r.get("s3") == &c, "别的 owner 的服务被误删了");
    PP_EQ(r.removeAllOf(7), 0);
    PP_EQ(r.removeAllOf(999), 0);
    PP_EQ(r.count(), 1);
}

PP_TEST(registry, service_name_is_unique_last_wins) {
    /* 同名服务再注册会覆盖，但 owner 要跟着换 —— 否则卸载 A 会把 B 的服务删掉 */
    ServiceRegistry r;
    int a = 1, b = 2;
    r.add("dup", "1", &a, 1);
    r.add("dup", "2", &b, 2);
    PP_EQ(r.count(), 1);
    PP_CHECK(r.get("dup") == &b);
    PP_EQ(r.removeAllOf(1), 0);                           /* A 已经不再拥有它 */
    PP_CHECK_MSG(r.get("dup") == &b, "同名覆盖后 owner 没跟着换");
    PP_EQ(r.removeAllOf(2), 1);
    PP_EQ(r.count(), 0);
}

/* ======================= 命令注册表 ======================= */

namespace {
int cmdOk(PP_Host*, int argc, const char** argv, void* user) {
    (void)user;
    return argc;                                          /* 把参数个数当返回值，便于断言 */
}
int cmdFail(PP_Host*, int, const char**, void*) { return 7; }
int cmdNull(PP_Host*, int, const char**, void*) { return 0; }

/** 命令回调里能看到 owner 是否被切成所属插件 —— 用 user 指针记录实际收到的 owner。 */
int cmdRecordOwner(PP_Host*, int, const char**, void* user) {
    int* slot = (int*)user;
    return slot ? *slot : -1;
}
} // namespace

PP_TEST(registry, command_add_lookup_invoke) {
    CommandRegistry r;
    PP_CHECK(r.add("snapshot", "snapshot [dir]", &cmdOk, nullptr, 1));
    PP_EQ(r.count(), 1);
    PP_CHECK(r.has("snapshot"));
    PP_CHECK(!r.has("nope"));

    CommandEntry e{};
    PP_CHECK(r.lookup("snapshot", &e));
    PP_STR_EQ(e.name, "snapshot");
    PP_STR_EQ(e.usage, "snapshot [dir]");
    PP_EQ(e.owner, 1);
    PP_CHECK_MSG(e.fn == &cmdOk, "lookup 返回的回调不对");
    PP_CHECK(!r.lookup("nope", &e));

    /* invoke：未找到返回 -1（而不是 0 —— 0 会被当成"命令成功"） */
    PP_EQ(r.invoke(nullptr, "snapshot", 2, nullptr), 2);
    PP_EQ(r.invoke(nullptr, "nope", 0, nullptr), -1);
}

PP_TEST(registry, command_rejects_bad_input) {
    CommandRegistry r;
    PP_CHECK(!r.add("", "u", &cmdOk, nullptr, 1));
    PP_CHECK(!r.add("x", "u", nullptr, nullptr, 1));
    PP_EQ(r.count(), 0);
    CommandEntry e{};
    PP_CHECK(!r.lookup("x", nullptr));                    /* out 为空要安全 */
}

PP_TEST(registry, command_removeAllOf_and_owner_reported) {
    CommandRegistry r;
    r.add("a", "u", &cmdRecordOwner, (void*)&r, 3);
    r.add("b", "u", &cmdFail, nullptr, 3);
    r.add("c", "u", &cmdNull, nullptr, 4);
    PP_EQ(r.count(), 3);

    PP_EQ(r.removeAllOf(3), 2);
    PP_EQ(r.count(), 1);
    PP_CHECK(r.has("c"));
    PP_CHECK(!r.has("a"));

    /* 撤掉的命令不能再被调用 */
    PP_EQ(r.invoke(nullptr, "a", 0, nullptr), -1);
    PP_EQ(r.invoke(nullptr, "c", 0, nullptr), 0);
    PP_EQ(r.removeAllOf(3), 0);
}

PP_TEST(registry, command_names_are_sorted) {
    CommandRegistry r;
    r.add("z", "u", &cmdNull, nullptr, 1);
    r.add("a", "u", &cmdNull, nullptr, 1);
    r.add("m", "u", &cmdNull, nullptr, 1);
    auto n = r.names();
    PP_EQ(n.size(), (size_t)3);
    PP_STR_EQ(n[0], "a"); PP_STR_EQ(n[1], "m"); PP_STR_EQ(n[2], "z");
}

PP_TEST(registry, command_list_writes_text) {
    CommandRegistry r;
    r.add("snapshot", "snapshot [dir]  保存当前帧为 PNG", &cmdNull, nullptr, 1);
    char buf[512] = {0};
    int n = r.list(buf, (int)sizeof(buf));
    PP_EQ(n, 1);
    std::string s(buf);
    PP_CHECK_MSG(s.find("snapshot") != std::string::npos, "list 输出里没有命令名");
    PP_CHECK_MSG(s.find("保存当前帧") != std::string::npos, "list 输出里没有用法说明");

    /* 缓冲区不够时必须截断而不是越界写 */
    char tiny[8];
    std::memset(tiny, 'X', sizeof(tiny));
    r.list(tiny, (int)sizeof(tiny));
    PP_CHECK_MSG(tiny[sizeof(tiny) - 1] == 0,
                 "小缓冲区上 list 应当截断并以 \\0 收尾，而不是原地不动");
    /* 零容量/空指针：不能抛异常，返回"已列出 0 条" */
    char zero[1] = {'Z'};
    PP_EQ(r.list(zero, 0), 0);
    PP_EQ(zero[0], 'Z');                                  /* 零容量：一个字节都不该写 */
    PP_EQ(r.list(nullptr, 64), 0);
}

/* ======================= 帧处理链 ======================= */

namespace {
struct FrameRec {
    std::vector<int>* order = nullptr;
    int  id = 0;
    int  ret = 0;                                        /* PP_CHAIN_CONTINUE/BAIL/DROP */
    int* calls = nullptr;
};
int frameFn(PP_Host*, PP_FrameView* view, void* user) {
    FrameRec* r = (FrameRec*)user;
    if (!r) return 0;
    if (r->order) r->order->push_back(r->id);
    if (r->calls) ++(*r->calls);
    if (view) view->index = r->id;                       /* 记录谁最后一个动过 view */
    return r->ret;
}
} // namespace

PP_TEST(registry, frameChain_runs_in_priority_order) {
    /* 优先级决定处理顺序，而顺序对图像处理是有意义的（先灰度再画框 ≠ 先画框再灰度）。
     * 同优先级按注册先后。 */
    FrameChain c;
    std::vector<int> order;
    FrameRec r1{&order, 1, 0, nullptr};
    FrameRec r2{&order, 2, 0, nullptr};
    FrameRec r3{&order, 3, 0, nullptr};

    c.add(&frameFn, &r2, 50, 1);
    c.add(&frameFn, &r1, 10, 1);                          /* 更小的优先级应当先跑 */
    c.add(&frameFn, &r3, 90, 1);
    PP_EQ(c.count(), 3);

    PP_FrameView v{};
    v.index = 0;
    PP_EQ(c.run(nullptr, &v), PP_CHAIN_CONTINUE);
    PP_EQ(order.size(), (size_t)3);
    PP_EQ(order[0], 1);
    PP_EQ(order[1], 2);
    PP_EQ(order[2], 3);
    PP_EQ(v.index, (int64_t)3);                           /* 最后一个（优先级最大）*/
}

PP_TEST(registry, frameChain_drop_and_bail_short_circuit) {
    FrameChain c;
    std::vector<int> order;
    FrameRec a{&order, 1, PP_CHAIN_DROP, nullptr};
    FrameRec b{&order, 2, 0, nullptr};
    c.add(&frameFn, &a, 10, 1);
    c.add(&frameFn, &b, 20, 1);

    PP_FrameView v{};
    PP_EQ(c.run(nullptr, &v), PP_CHAIN_DROP);
    PP_CHECK_MSG(order.size() == 1, "DROP 应中止链，后面的处理器不该被调用");

    FrameChain c2;
    std::vector<int> order2;
    FrameRec d{&order2, 1, PP_CHAIN_BAIL, nullptr};
    FrameRec e{&order2, 2, 0, nullptr};
    c2.add(&frameFn, &d, 10, 1);
    c2.add(&frameFn, &e, 20, 1);
    PP_FrameView v2{};
    PP_EQ(c2.run(nullptr, &v2), PP_CHAIN_BAIL);
    PP_CHECK_MSG(order2.size() == 1, "BAIL 应中止链");
}

PP_TEST(registry, frameChain_remove_and_revoke) {
    FrameChain c;
    int callsA = 0, callsB = 0, callsC = 0;
    FrameRec a{nullptr, 1, 0, &callsA};
    FrameRec b{nullptr, 2, 0, &callsB};
    FrameRec cc{nullptr, 3, 0, &callsC};
    int idA = c.add(&frameFn, &a, 10, 1);
    c.add(&frameFn, &b, 20, 1);
    c.add(&frameFn, &cc, 30, 2);
    PP_CHECK(idA > 0);

    int idB = c.add(&frameFn, &b, 40, 1);                 /* 再注册一份 b */
    PP_CHECK(idB != idA);

    PP_FrameView v{};
    c.run(nullptr, &v);
    PP_CHECK(callsB >= 1);

    /* 按 id 撤：只撤指定的那一个 */
    c.remove(idA);
    PP_EQ(c.count(), 3);
    callsA = callsB = callsC = 0;
    c.run(nullptr, &v);
    PP_CHECK_MSG(callsA == 0, "按 id 撤销后不该再被调用");
    PP_CHECK(callsC == 1);

    /* 按 owner 撤：owner 1 名下有两条（idB 与前面那次注册的 b），owner 2 的 cc 不受影响。
     * 这里刻意让同一个回调注册两次，用来确认"按 owner 撤"是按**注册项**撤，
     * 而不是按回调去重。 */
    PP_EQ(c.removeAllOf(1), 2);
    PP_EQ(c.count(), 1);
    callsA = callsB = callsC = 0;
    c.run(nullptr, &v);
    PP_CHECK_MSG(callsB == 0, "owner 撤销不彻底");
    PP_CHECK_MSG(callsC == 1, "撤销 owner 1 时误删了 owner 2 的处理器");

    c.remove(999999);                                     /* 不存在的 id：安全 */
    c.remove(-1);
    PP_EQ(c.count(), 1);
    PP_EQ(c.removeAllOf(2), 1);
    PP_EQ(c.count(), 0);
}

PP_TEST(registry, frameChain_rejects_bad_input) {
    FrameChain c;
    PP_EQ(c.add(nullptr, nullptr, 0, 1), 0);              /* 空回调 */
    PP_EQ(c.count(), 0);
    /* run 在空链上返回 CONTINUE，不崩 */
    PP_FrameView v{};
    PP_EQ(c.run(nullptr, &v), PP_CHAIN_CONTINUE);
}
