/* test_config.cpp — 配置持久化（key=value 文本）
 *
 * 这块代码承载的是"插件的私有配置"（plugin.<插件名>.<键>）。它的失败模式很安静：
 * 读写不对称、dirty 标记错、保存不是原子替换 —— 表现都是"用户改了设置，
 * 下次启动又变回去了"，或者断电后配置文件变成半个空文件。
 */
#include "harness.h"
#include "config.h"

#include <string>

using namespace pp;

PP_TEST(config, load_parses_comments_and_whitespace) {
    std::string p = pp_test::writeTmpFile("cfg_a.ini",
        "# 注释行\n"
        "; 另一种注释\n"
        "\n"
        "   plugin.osd_stats.show = 1  \n"
        "plugin.snapshot.count=42\n"
        "key_without_value\n"          /* 没有 = 的行：跳过 */
        "=novalue\n"                   /* 空键：跳过 */
        "  \t  \n");
    Config c;
    PP_CHECK(c.load(p));
    PP_STR_EQ(c.get("plugin.osd_stats.show"), "1");    /* 两边空白被 trim */
    PP_STR_EQ(c.get("plugin.snapshot.count"), "42");
    PP_CHECK(!c.has("key_without_value"));
    PP_CHECK(!c.has(""));
    PP_STR_EQ(c.get("missing", "DEF"), "DEF");
    PP_EQ(c.all().size(), (size_t)2);
    PP_CHECK_MSG(!c.dirty(), "刚 load 完不该是 dirty");
    PP_STR_EQ(c.path(), p);
    pp_test::removeTmp("cfg_a.ini");
}

PP_TEST(config, load_missing_file_is_not_fatal) {
    Config c;
    PP_CHECK(!c.load(pp_test::tmpPath("no_such_cfg_xyz.ini")));
    /* 关键：读不到文件之后，取值必须退回默认值，而不是崩或给垃圾 */
    PP_STR_EQ(c.get("any", "DEF"), "DEF");
    PP_CHECK(!c.has("any"));
    PP_EQ(c.getNumber("any", 3.5), 3.5);
}

PP_TEST(config, duplicate_key_last_wins) {
    std::string p = pp_test::writeTmpFile("cfg_dup.ini", "a=1\na=2\na=3\n");
    Config c;
    PP_CHECK(c.load(p));
    PP_STR_EQ(c.get("a"), "3");
    PP_EQ(c.all().size(), (size_t)1);
    pp_test::removeTmp("cfg_dup.ini");
}

PP_TEST(config, set_get_and_dirty_semantics) {
    Config c;
    PP_CHECK(!c.dirty());
    c.set("k", "v");
    PP_EQ(c.get("k"), "v");
    PP_CHECK_MSG(c.dirty(), "改过值之后应当置 dirty");
    PP_CHECK(c.has("k"));

    /* 写同样的值不该置 dirty —— 依赖这个语义的地方是"惰性迁移"：
     * 迁移代码会先算一遍再写回，值没变时不该把文件标脏并触发保存。 */
    Config c2;
    c2.load(pp_test::writeTmpFile("cfg_dirty.ini", "k=v\n"));
    PP_CHECK(!c2.dirty());
    c2.set("k", "v");
    PP_CHECK_MSG(!c2.dirty(), "写入相同的值不该置 dirty");
    c2.set("k", "v2");
    PP_CHECK(c2.dirty());

    c2.setNumber("n", 0.5);
    PP_STR_EQ(c2.get("n"), "0.5");
    c2.setInt("i", -7);
    PP_STR_EQ(c2.get("i"), "-7");
    /* setNumber 用 %g：整数值不该拖 .000000 的尾巴 */
    c2.setNumber("n2", 1.0);
    PP_STR_EQ(c2.get("n2"), "1");
    pp_test::removeTmp("cfg_dirty.ini");
}

PP_TEST(config, save_and_reload_roundtrip) {
    std::string p = pp_test::tmpPath("cfg_rt.ini");
    {
        Config c;
        PP_CHECK(!c.save());                           /* 没 load 过：没有路径，保存失败 */
        c.load(p);                                     /* 文件不存在 → false，但路径记住了 */
        c.set("plugin.snapshot.count", "12");
        c.setNumber("plugin.video_effects.gain", 1.25);
        c.setInt("plugin.audio_viz.mode", 3);
        PP_CHECK(c.save());
    }
    {
        Config c;
        PP_CHECK(c.load(p));
        PP_STR_EQ(c.get("plugin.snapshot.count"), "12");
        PP_EQ(c.getNumber("plugin.video_effects.gain", -1), 1.25);
        PP_EQ(c.getInt("plugin.audio_viz.mode", -1), 3);
        PP_CHECK_MSG(!c.dirty(), "load 完的文件不该是 dirty");
    }
    /* 保存必须是"临时文件 + 原子替换"：不该留下 .tmp */
    PP_CHECK_MSG(!pp::fileExists(p + ".tmp"), "保存后残留了 .tmp 临时文件");
    PP_CHECK(pp::fileExists(p));
    pp_test::removeTmp("cfg_rt.ini");
}

PP_TEST(config, save_writes_sorted_and_is_readable_by_hand) {
    std::string p = pp_test::tmpPath("cfg_sorted.ini");
    Config c;
    c.load(p);
    c.set("z", "1");
    c.set("a", "2");
    c.set("m", "3");
    PP_CHECK(c.save());
    std::string body;
    {
        FILE* f = std::fopen(p.c_str(), "rb");
        PP_CHECK(f != nullptr);
        if (f) {
            char buf[4096];
            size_t n = std::fread(buf, 1, sizeof(buf), f);
            body.assign(buf, n);
            std::fclose(f);
        }
    }
    size_t pa = body.find("a=2"), pm = body.find("m=3"), pz = body.find("z=1");
    PP_CHECK(pa != std::string::npos && pm != std::string::npos && pz != std::string::npos);
    PP_CHECK_MSG(pa < pm && pm < pz, "保存出来的键应当是排好序的（便于人工编辑与 diff）");
    PP_CHECK_MSG(body.find('#') != std::string::npos, "保存的文件应当带说明头注释");
    pp_test::removeTmp("cfg_sorted.ini");
}

PP_TEST(config, number_and_bool_parsing) {
    Config c;
    c.load(pp_test::writeTmpFile("cfg_types.ini",
        "num=2.5\n"
        "int=-3\n"
        "bad=abc\n"
        "partial=12abc\n"
        "t1=1\nt2=true\nt3=YES\nt4=On\nt5=TRUE\n"
        "f1=0\nf2=false\nf3=no\nf4=off\nf5=\n"));

    PP_EQ(c.getNumber("num", -1), 2.5);
    PP_EQ(c.getInt("int", 0), -3);
    /* 完全不是数字 → 默认值 */
    PP_EQ(c.getNumber("bad", -1), -1);
    /* "12abc"：strtod 会取到 12（不校验尾部）。这里把实际行为写下来，
     * 提醒调用方：配置值是**人手编辑**的，别指望它严格。 */
    PP_EQ(c.getNumber("partial", -1), 12);

    PP_CHECK(c.getBool("t1", false));
    PP_CHECK(c.getBool("t2", false));
    PP_CHECK(c.getBool("t3", false));
    PP_CHECK(c.getBool("t4", false));
    PP_CHECK(c.getBool("t5", false));
    PP_CHECK(!c.getBool("f1", true));
    PP_CHECK(!c.getBool("f2", true));
    PP_CHECK(!c.getBool("f3", true));
    PP_CHECK(!c.getBool("f4", true));
    /* 空值 → 默认值（而不是 false） */
    PP_CHECK(c.getBool("f5", true));
    PP_CHECK(!c.getBool("f5", false));
    /* 缺键 → 默认值 */
    PP_CHECK(c.getBool("nope", true));
    PP_CHECK(!c.getBool("nope", false));
    pp_test::removeTmp("cfg_types.ini");
}

PP_TEST(config, all_returns_copy) {
    Config c;
    c.load(pp_test::writeTmpFile("cfg_copy.ini", "a=1\nb=2\n"));
    auto m = c.all();
    PP_EQ(m.size(), (size_t)2);
    m["a"] = "MUTATED";
    m["c"] = "NEW";
    PP_STR_EQ(c.get("a"), "1");                        /* 外部改动不该影响内部 */
    PP_CHECK(!c.has("c"));
    pp_test::removeTmp("cfg_copy.ini");
}

PP_TEST(config, long_line_is_truncated_not_crashing) {
    /* load 用 fgets + 2048 字节行缓冲：超长值会被截断（并可能在后续行首留下残片）。
     * 这里只钉住"不崩、且不会把截断的残余错当成新键"。 */
    std::string big(3000, 'x');
    std::string p = pp_test::writeTmpFile("cfg_long.ini", "k=" + big + "\nk2=v\n");
    Config c;
    PP_CHECK(c.load(p));
    PP_CHECK(c.has("k"));
    PP_CHECK_MSG(c.get("k").size() <= 2048, "单行长度受行缓冲限制");
    PP_STR_EQ(c.get("k2"), "v");                        /* 后续正常的键仍然要读到 */
    pp_test::removeTmp("cfg_long.ini");
}
