/* test_path.cpp — 路径规范化 / 白名单包含判定 / 网络输入拒绝
 *
 * 这三条是**安全策略**的实现，不是普通工具函数：
 *   · pathIsInside 决定"插件能不能往这个路径写文件"
 *   · isNetworkInput 决定"要不要拒绝这个输入"（默认拒绝 SSRF 型输入）
 * 所以这里的用例刻意往攻击面上写：前缀欺骗、大小写、.. 穿越、UNC、盘符误判。
 */
#include "harness.h"
#include "util.h"

#include <algorithm>

/* 用一个真实存在的临时目录当"白名单根"，并且**立刻规范化成绝对路径**：
 * 相对路径在 Windows/Linux 上的规范化行为不同（GetFullPathNameA 对不存在的
 * 路径也照常展开；Linux 的 realpath 会失败并原样返回），用绝对路径当基准
 * 才能让下面这些断言在两个平台上都成立。 */
static std::string makeRoot() {
    std::string d = pp::pathJoin(pp_test::tmpDir(), "wl_root");
    pp::makeDirs(d);
    pp::makeDirs(pp::pathJoin(d, "sub"));
    return pp::normalizePath(d);
}

PP_TEST(path, normalize_is_absolute_and_forward_slashed) {
    std::string root = makeRoot();
    PP_CHECK_MSG(!root.empty(), "normalizePath 不该返回空");
    PP_STR_EQ(root.find('\\'), std::string::npos);   /* 统一成正斜杠 */
    /* 绝对路径的两种形态：POSIX 的 "/a/b" 与 Windows 的 "C:/a/b" */
    PP_CHECK_MSG(root[0] == '/' || (root.size() >= 2 && root[1] == ':'),
                 "规范化后应当是绝对路径（/… 或 X:/…）");
    PP_CHECK_MSG(root.size() < 2 || root[root.size() - 1] != '/', "不应保留尾部斜杠");

    /* 空输入直接返回空 */
    PP_STR_EQ(pp::normalizePath(""), "");
}

PP_TEST(path, pathIsInside_basic) {
    std::string root = makeRoot();
    /* root 自身算"在内"（调用方据此允许写 exeDir 本身） */
    PP_CHECK(pp::pathIsInside(root, root));
    PP_CHECK(pp::pathIsInside(pp::pathJoin(root, "sub"), root));
    PP_CHECK(pp::pathIsInside(pp::pathJoin(root, "a.txt"), root));
    /* 用 \ 分隔也要认出层次 */
    PP_CHECK(pp::pathIsInside(root + "\\sub", root));
}

PP_TEST(path, pathIsInside_rejects_prefix_attack) {
    /* 这是最容易写错的一种：用字符串前缀判断会让 "…/dist_evil" 落进 "…/dist" 里。
     * 实现要求 root 之后紧跟分隔符，所以必须为假。 */
    std::string root = makeRoot();
    std::string evil = root + "_evil";
    pp::makeDirs(evil);                              /* 真的把它建出来，排除"路径不存在"的干扰 */
    PP_CHECK_MSG(!pp::pathIsInside(evil, root),
                 "前缀相同但不同层级的目录被误判成在内 —— 写入白名单会被绕过");
    PP_CHECK(!pp::pathIsInside(root + "x.txt", root));
}

PP_TEST(path, pathIsInside_rejects_traversal) {
    std::string root = makeRoot();
    /* 目录穿越：.. 必须被折叠掉，于是落到 root 之外 */
    PP_CHECK_MSG(!pp::pathIsInside(pp::pathJoin(root, "../escaped.txt"), root),
                 "`..` 穿越没被挡住");
    /* root 的父目录当然不在内 */
    PP_CHECK(!pp::pathIsInside(pp::pathJoin(root, ".."), root));
}

PP_TEST(path, pathIsInside_rejects_prefix_attack_deep) {
    /* 再压一层：兄弟目录名前缀相同，且 root 后面紧跟的字符不是分隔符 */
    std::string root = makeRoot();
    std::string sibling = root + "-sibling";
    pp::makeDirs(sibling);
    PP_CHECK_MSG(!pp::pathIsInside(sibling, root), "同前缀的兄弟目录被误判为在内");
}

PP_TEST(path, pathIsInside_empty_and_case) {
    std::string root = makeRoot();
    PP_CHECK(!pp::pathIsInside(root, ""));           /* 空 root：一律不在内 */
    PP_CHECK(!pp::pathIsInside("", root));
    /* Windows 下大小写不敏感（同一目录写成大写也算在内） */
    std::string upper = root;
    for (auto& c : upper) if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    PP_CHECK_MSG(pp::pathIsInside(upper, root), "大小写不同的同一路径应判为在内（Windows 语义）");
}

PP_TEST(path, isNetworkInput_rejects_unc) {
    /* UNC 会触发 SMB 认证，可被用于 NTLM 中继 —— 必须默认拒绝 */
    PP_CHECK(pp::isNetworkInput("\\\\server\\share\\a.mp4"));
    PP_CHECK(pp::isNetworkInput("//server/share/a.mp4"));
    PP_CHECK(pp::isNetworkInput("\\\\127.0.0.1\\c$\\x"));
    /* 单个反斜杠开头不是 UNC */
    PP_CHECK(!pp::isNetworkInput("\\a.mp4"));
    PP_CHECK(!pp::isNetworkInput("/a.mp4"));
}

PP_TEST(path, isNetworkInput_rejects_schemes) {
    PP_CHECK(pp::isNetworkInput("http://example.com/a.mp4"));
    PP_CHECK(pp::isNetworkInput("https://example.com/a.mp4"));
    PP_CHECK(pp::isNetworkInput("rtsp://h/s"));
    PP_CHECK(pp::isNetworkInput("rtmp://h/s"));
    PP_CHECK(pp::isNetworkInput("ftp://h/f"));
    PP_CHECK(pp::isNetworkInput("file:///c:/a.mp4"));
    PP_CHECK(pp::isNetworkInput("smb://h/s"));
    PP_CHECK(pp::isNetworkInput("udp://h:1"));
    PP_CHECK(pp::isNetworkInput("HTTP://EXAMPLE.COM/A"));   /* 大写也要认 */
}

PP_TEST(path, isNetworkInput_accepts_local) {
    /* 本地路径一个都不能误判 —— 误判的后果是正常文件打不开 */
    PP_CHECK(!pp::isNetworkInput("C:\\videos\\a.mp4"));
    PP_CHECK(!pp::isNetworkInput("C:/videos/a.mp4"));
    PP_CHECK(!pp::isNetworkInput("/tmp/a.mp4"));
    PP_CHECK(!pp::isNetworkInput("a.mp4"));
    PP_CHECK(!pp::isNetworkInput("./a.mp4"));
    PP_CHECK(!pp::isNetworkInput("..\\a.mp4"));
    PP_CHECK(!pp::isNetworkInput(""));
    /* 盘符 "C://x" 不能被当成协议（scheme 至少 2 字符） */
    PP_CHECK_MSG(!pp::isNetworkInput("C://x/a.mp4"), "盘符被误判成 URL scheme");
    PP_CHECK(!pp::isNetworkInput("D://"));
    /* 文件名里含 :// 但不是开头，也不算协议 */
    PP_CHECK(!pp::isNetworkInput("my http://note.txt"));
    /* scheme 位置含非法字符 → 不是协议 */
    PP_CHECK(!pp::isNetworkInput("a b://x"));
}

PP_TEST(path, exists_and_mkdirs) {
    std::string d = pp::pathJoin(pp_test::tmpDir(), "mk/a/b/c");
    PP_CHECK(pp::makeDirs(d));
    PP_CHECK(pp::dirExists(d));
    PP_CHECK(pp::makeDirs(d));                       /* 幂等 */

    std::string f = pp_test::writeTmpFile("exists_probe.bin", "hello");
    PP_CHECK(pp::fileExists(f));
    PP_CHECK(!pp::dirExists(f));
    PP_CHECK(!pp::fileExists(d));                    /* 目录不算文件 */
    PP_CHECK(!pp::fileExists(pp::pathJoin(pp_test::tmpDir(), "no_such_file_xyz.bin")));
    PP_CHECK(!pp::dirExists(pp::pathJoin(pp_test::tmpDir(), "no_such_dir_xyz")));
    pp_test::removeTmp("exists_probe.bin");
}

PP_TEST(path, listDir_with_ext_filter) {
    pp::makeDirs(pp_test::tmpDir());
    pp_test::writeTmpFile("ld_a.dll", "x");
    pp_test::writeTmpFile("ld_b.DLL", "x");          /* 大写也要被 .dll 过滤器收进来 */
    pp_test::writeTmpFile("ld_c.txt", "x");

    auto all = pp::listDir(pp_test::tmpDir(), "");
    int dlls = 0, txts = 0;
    for (const auto& p : all) {
        if (pp::endsWith(p, ".dll")) ++dlls;
        if (pp::endsWith(p, ".txt")) ++txts;
    }
    PP_CHECK_MSG(dlls >= 2, "大小写不同的 .dll 都应该命中过滤器");
    PP_CHECK(txts >= 1);

    /* 结果必须有序（调用方依赖稳定顺序来决定插件槽位） */
    auto sorted = all;
    std::sort(sorted.begin(), sorted.end());
    PP_CHECK_MSG(all == sorted, "listDir 的结果应当是排好序的");

    pp_test::removeTmp("ld_a.dll");
    pp_test::removeTmp("ld_b.DLL");
    pp_test::removeTmp("ld_c.txt");
}
