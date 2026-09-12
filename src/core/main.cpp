/* main.cpp — 命令行入口
 *
 * 用法:
 *   pluginplayer.exe [选项] <媒体文件>
 *
 * 选项:
 *   --help, -h              显示帮助
 *   --list-plugins          列出插件加载报告后退出
 *   --write-plugin-manifest 生成 plugins/manifest.sha256（插件完整性清单）后退出
 *   --no-audio              不打开音频设备（静音播放 / 无音频环境下使用）
 *   --hidden                不显示窗口（配合控制通道做无人值守控制）
 *   --allow-network-input   允许打开 http/rtsp/UNC 等网络输入（默认拒绝）
 *   --size WxH              初始窗口尺寸（默认 1280x720；范围 160..8192）
 *   --start SEC             从指定秒数开始播放
 *   --volume V              初始音量 0..1（默认 1.0）
 *   --log-level N           0=TRACE 1=DEBUG 2=INFO 3=WARN 4=ERROR（默认 2）
 *   --selftest              无人值守自检模式
 *   --frames N              自检模式解码帧数（默认 60；范围 1..1000000）
 *   --out DIR               自检产物目录（默认 ./selftest_out）
 *   --write-dir DIR         追加插件可写目录（默认只允许 exeDir）
 *   --rpc-port N            控制通道端口（默认 7911；0 = 不启动）
 *   --rpc-token HEX         指定控制通道令牌（默认每次运行随机生成 256-bit）
 *   --reload <序号>         启动后热重载指定插件（用于验证）
 *
 * 所有数值参数都会做范围与格式校验：非法值一律报错退出（exit 2），
 * 不会带着越界值继续跑（历史上 --size 99999x99999 会因 int32 溢出崩溃）。
 */
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <cerrno>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

#include "app.h"
#include "util.h"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <shellapi.h>   /* CommandLineToArgvW（WIN32_LEAN_AND_MEAN 下不被 windows.h 带入） */
#endif

using namespace pp;

/* 窗口尺寸硬边界与帧数上限：与 app.h 的 kMinWindowSize/kMaxWindowSize 共用同一来源，
 * 避免两处各写一份数字后走偏（历史上 --size 99999x99999 因 int32 溢出崩溃）。 */
static const int kMinWin = kMinWindowSize;
static const int kMaxWin = kMaxWindowSize;
static const int kMaxFrames = 1000000;

static bool parseSize(const char* s, int* w, int* h) {
    if (!s) return false;
    int a = 0, b = 0;
    char tail = 0;
    /* 故意多要一个 %c：若返回 3 说明末尾有多余字符，判为非法 */
    if (sscanf(s, "%dx%d%c", &a, &b, &tail) != 2) return false;
    if (a < kMinWin || a > kMaxWin || b < kMinWin || b > kMaxWin) return false;
    *w = a; *h = b;
    return true;
}

static bool parseIntStrict(const char* s, long* out) {
    if (!s || !*s) return false;
    errno = 0;
    char* end = nullptr;
    long v = strtol(s, &end, 10);
    if (end == s || (end && *end != '\0') || errno == ERANGE) return false;
    *out = v;
    return true;
}

static bool parseDoubleStrict(const char* s, double* out) {
    if (!s || !*s) return false;
    errno = 0;
    char* end = nullptr;
    double v = strtod(s, &end);
    if (end == s || (end && *end != '\0') || errno == ERANGE || !std::isfinite(v))
        return false;
    *out = v;
    return true;
}

static void printHelp() {
    printf(
        "\n"
        "PluginPlayer — 插件版播放器 (FFmpeg %s + SDL3 + 自研插件宿主)\n"
        "\n"
        "用法:\n"
        "  pluginplayer.exe [选项] <媒体文件>\n"
        "\n"
        "选项:\n"
        "  -h, --help            显示本帮助\n"
        "  --list-plugins        列出插件加载报告后退出\n"
        "  --write-plugin-manifest  生成 plugins/manifest.sha256 完整性清单后退出\n"
        "  --no-audio            不打开音频设备\n"
        "  --hidden              不显示窗口（配合控制通道做无人值守控制）\n"
        "  --allow-network-input 允许打开 http/rtsp/UNC 等网络输入（默认拒绝）\n"
        "  --size WxH            初始窗口尺寸（默认 1280x720，范围 160..8192）\n"
        "  --start SEC           从指定秒数开始播放\n"
        "  --volume V            初始音量 0..1（默认 1.0）\n"
        "  --log-level N         0=TRACE 1=DEBUG 2=INFO 3=WARN 4=ERROR（默认 2）\n"
        "  --selftest            无人值守自检（产出 JSON 报告与帧 PNG）\n"
        "  --frames N            自检解码帧数（默认 60）\n"
        "  --out DIR             自检产物目录（默认 ./selftest_out）\n"
        "  --write-dir DIR       追加插件可写目录（默认只允许 exeDir，可重复）\n"
        "  --rpc-port N          控制通道端口（默认 7911；0 = 不启动）\n"
        "  --rpc-token HEX       指定控制通道令牌（默认每次运行随机生成）\n"
        "  --reload N            启动后热重载第 N 号插件，然后继续播放\n"
        "\n"
        "安全说明: 控制通道监听 127.0.0.1 且**强制令牌认证**。令牌在每次启动时\n"
        "          随机生成，写入 <exeDir>\\config\\rpc-token.txt。客户端必须先发\n"
        "          {\"method\":\"auth\",\"params\":{\"token\":\"<令牌>\"}} 才能调用任何方法。\n"
        "\n"
        "播放中快捷键: 按 H 查看\n",
        "9.x");
}

int main(int argc, char** argv) {
    /* UTF-8 参数表：Windows 下用 Unicode 命令行重建（见下），其他平台保持原样。 */
    std::vector<std::string> utf8Args;
    std::vector<char*> utf8Argv;
#ifdef _WIN32
    /* 控制台输出统一按 UTF-8 解读：源码与编译器的执行字符集都是 UTF-8，而 Windows
     * 控制台默认是 GBK(936) —— 双击 .bat 启动时中文会整片乱码（"搜索" 显示成
     * "鎼滅储"，用户实测报过）。stdout 重定向到管道/文件时该调用不影响字节流，
     * 捕获方仍按 UTF-8 解码；无控制台（GUI 方式启动）时它只是无害地返回 FALSE。 */
    SetConsoleOutputCP(CP_UTF8);

    /* 参数编码：main 的 argv 由 CRT 从 ANSI(本机 GBK) 命令行转换而来，中文路径到这里
     * 就已经不是 UTF-8 字节；而 FFmpeg 与文件系统接口都期望 UTF-8 —— 拖入/传入含中文
     * 的媒体会直接 "avformat_open_input 失败 (Invalid argument)"（用户实测：
     * 願廣西平安.mp4）。用 Unicode 命令行重取参数并转成 UTF-8；argv 之后的所有解析
     * 逻辑保持原样。容器声明在 main 作用域内，保证 argv 指向的内存在整个 main 期间有效。 */
    int wideArgc = 0;
    wchar_t** wideArgv = CommandLineToArgvW(GetCommandLineW(), &wideArgc);
    if (wideArgv != nullptr) {
        utf8Args.reserve(static_cast<size_t>(wideArgc));
        for (int i = 0; i < wideArgc; ++i) {
            int need = WideCharToMultiByte(CP_UTF8, 0, wideArgv[i], -1, nullptr, 0, nullptr, nullptr);
            std::string text;
            if (need > 1) {
                text.resize(static_cast<size_t>(need - 1));
                WideCharToMultiByte(CP_UTF8, 0, wideArgv[i], -1, &text[0], need, nullptr, nullptr);
            }
            utf8Args.push_back(std::move(text));
        }
        LocalFree(wideArgv);
        utf8Argv.reserve(utf8Args.size());
        for (std::string& text : utf8Args) utf8Argv.push_back(&text[0]);
        argc = wideArgc;
        argv = utf8Argv.data();
    }
#endif
    std::string media;
    std::string outDir = "selftest_out";
    int  winW = 1280, winH = 720;
    int  frames = 60;
    int  logLevel = L_INFO;
    int  reloadIndex = -1;
    bool noAudio = false;
    bool hidden = false;
    bool allowNetworkInput = false;
    bool selfTest = false;
    bool listPlugins = false;
    bool writePluginManifest = false;
    int  rpcPort = 7911;
    double startSec = -1.0;
    double volume = 1.0;
    std::string rpcTokenArg;
    std::vector<std::string> writeDirs;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                fprintf(stderr, "选项 %s 缺少参数\n", name);
                exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") { printHelp(); return 0; }
        else if (a == "--list-plugins") listPlugins = true;
        else if (a == "--no-audio") noAudio = true;
        else if (a == "--hidden") hidden = true;
        else if (a == "--allow-network-input") allowNetworkInput = true;
        else if (a == "--selftest") selfTest = true;
        else if (a == "--size") {
            const char* v = next("--size");
            if (!parseSize(v, &winW, &winH)) {
                fprintf(stderr,
                        "非法的 --size 值: %s\n"
                        "  需要 WxH 形式，且宽高都在 %d..%d 之间（例如 1280x720）\n",
                        v, kMinWin, kMaxWin);
                return 2;
            }
        }
        else if (a == "--start") {
            const char* v = next("--start");
            if (!parseDoubleStrict(v, &startSec)) {
                fprintf(stderr, "非法的 --start 值: %s（需要数值秒数）\n", v);
                return 2;
            }
        }
        else if (a == "--volume") {
            const char* v = next("--volume");
            if (!parseDoubleStrict(v, &volume) || volume < 0.0 || volume > 1.0) {
                fprintf(stderr, "非法的 --volume 值: %s（需要 0..1）\n", v);
                return 2;
            }
        }
        else if (a == "--log-level") {
            const char* v = next("--log-level");
            long n = 0;
            if (!parseIntStrict(v, &n) || n < 0 || n > 4) {
                fprintf(stderr, "非法的 --log-level 值: %s（需要 0..4）\n", v);
                return 2;
            }
            logLevel = (int)n;
        }
        else if (a == "--frames") {
            const char* v = next("--frames");
            long n = 0;
            if (!parseIntStrict(v, &n) || n < 1 || n > kMaxFrames) {
                fprintf(stderr, "非法的 --frames 值: %s（需要 1..%d）\n", v, kMaxFrames);
                return 2;
            }
            frames = (int)n;
        }
        else if (a == "--out") outDir = next("--out");
        else if (a == "--write-plugin-manifest") writePluginManifest = true;
        else if (a == "--write-dir") writeDirs.push_back(next("--write-dir"));
        else if (a == "--rpc-port") {
            const char* v = next("--rpc-port");
            long n = 0;
            if (!parseIntStrict(v, &n) || n < 0 || n > 65535) {
                fprintf(stderr, "非法的 --rpc-port 值: %s（需要 0..65535）\n", v);
                return 2;
            }
            rpcPort = (int)n;
        }
        else if (a == "--rpc-token") {
            rpcTokenArg = next("--rpc-token");
            if (rpcTokenArg.size() < 16) {
                fprintf(stderr, "--rpc-token 太短（至少 16 个字符，建议 64 位 hex）\n");
                return 2;
            }
        }
        else if (a == "--reload") {
            const char* v = next("--reload");
            long n = 0;
            if (!parseIntStrict(v, &n) || n < 0 || n > 9999) {
                fprintf(stderr, "非法的 --reload 值: %s（需要 0..9999）\n", v);
                return 2;
            }
            reloadIndex = (int)n;
        }
        else if (!a.empty() && a[0] == '-') {
            fprintf(stderr, "未知选项: %s\n", a.c_str());
            printHelp();
            return 2;
        } else {
            media = a;
        }
    }

    /* 日志先落到 exe 同目录的 logs/ 下 */
    std::string base = exeDir();
    makeDirs(pathJoin(base, "logs"));
    Log::init(pathJoin(pathJoin(base, "logs"), "pluginplayer.log"), (LogLevel)logLevel);

#ifdef _WIN32
    /* 加固 DLL 搜索路径：把「当前工作目录」从默认搜索顺序里去掉，
     * 否则在 CWD 放一个同名 DLL 就可能被优先加载（DLL 劫持）。
     * 必须在加载任何插件之前调用；进程启动时由导入表加载的
     * SDL3 / FFmpeg DLL 不受影响。
     * 用 GetProcAddress 动态取：老系统（无 KB2533623）没有这个函数，跳过并告警。 */
    {
        HMODULE k32 = ::GetModuleHandleA("kernel32.dll");
        typedef BOOL (WINAPI *SetDefaultDllDirsFn)(DWORD);
        /* 经 void* 中转：直接函数指针互转会触发 -Wcast-function-type */
        SetDefaultDllDirsFn fn = k32
            ? reinterpret_cast<SetDefaultDllDirsFn>(
                  reinterpret_cast<void*>(::GetProcAddress(k32, "SetDefaultDllDirectories")))
            : nullptr;
        if (!fn) {
            PP_WARN("本机不支持 SetDefaultDllDirectories，DLL 搜索路径未加固（建议升级系统）");
        } else if (!fn(0x00001000u /* LOAD_LIBRARY_SEARCH_DEFAULT_DIRS */)) {
            PP_WARN("SetDefaultDllDirectories 失败（err=%lu），DLL 搜索路径未加固",
                    (unsigned long)::GetLastError());
        } else {
            PP_INFO("DLL 搜索路径已加固：CWD 不再参与 DLL 解析");
        }
    }
#endif

    App app;
    if (!app.init(winW, winH, hidden || listPlugins || selfTest || writePluginManifest, !noAudio)) {
        fprintf(stderr, "初始化失败\n");
        return 3;
    }
    app.setAllowNetworkInput(allowNetworkInput);

    /* 插件写入白名单：默认只允许 exeDir（见 Host::addWriteRoot）。
     * 需要让插件往别的目录写（例如截图存到视频目录）时用 --write-dir 追加。 */
    for (const auto& d : writeDirs) app.host().addWriteRoot(d);

    /* 插件：发现 -> 加载 -> 启动 */
    int found = app.plugins().discover(app.host().pluginDir());

    if (writePluginManifest) {
        /* 生成插件完整性清单。注意：必须在 loadAll 之前做 ——
         * 清单一旦存在，未列出的插件就会被拒绝加载。 */
        std::string dir = app.host().pluginDir();
        std::string mf  = pathJoin(dir, "manifest.sha256");
        std::string out = "# PluginPlayer 插件完整性清单\n"
                          "# 格式: <sha256>  <文件名>（与 sha256sum 兼容）\n"
                          "# 生成: pluginplayer.exe --write-plugin-manifest\n"
                          "# 提交/发布前请重新生成；新增插件后若不同步更新，该插件会被拒绝加载。\n";
        int n = 0, bad = 0;
        for (const auto& s : app.plugins().slots()) {
            std::string h;
            if (!sha256FileHex(s.path, &h)) { bad++; continue; }
            out += h + "  " + s.file + "\n";
            n++;
        }
        FILE* f = fopen(mf.c_str(), "wb");
        if (!f) {
            fprintf(stderr, "无法写入 %s\n", mf.c_str());
            app.shutdown();
            return 5;
        }
        fwrite(out.data(), 1, out.size(), f);
        fclose(f);
        printf("%s\n已写入 %s（%d 个插件，失败 %d）\n", out.c_str(), mf.c_str(), n, bad);
        printf("提示：提交前把这个文件一起纳入版本控制，插件被改动就会被发现。\n");
        app.shutdown();
        return bad == 0 ? 0 : 5;
    }

    int okCnt = app.plugins().loadAll(&app.host());
    app.plugins().startAll(&app.host());

    printf("插件目录: %s\n", app.host().pluginDir().c_str());
    printf("%s\n", app.plugins().report(false).c_str());

    if (listPlugins) {
        app.shutdown();
        if (found <= 0) {
            fprintf(stderr, "未发现任何插件\n");
            return 4;
        }
        if (okCnt != found) {
            fprintf(stderr, "插件加载不完整：成功 %d / 发现 %d\n", okCnt, found);
            return 4;
        }
        return 0;
    }

    if (reloadIndex > 0) {
        PP_INFO("热重载插件槽位 #%d ...", reloadIndex);
        bool ok = app.plugins().reloadOne(&app.host(), reloadIndex - 1);
        PP_INFO("热重载结果: %s", ok ? "成功" : "失败");
        printf("%s\n", app.plugins().report(false).c_str());
    }

    app.host().bus().publish(app.host().handle(), PP_TOPIC_APP_READY, "{}");

    /* 控制通道：进程外插件（host-js / Cordis、外部脚本）从这里驱动播放器。
     * 强制令牌认证：令牌每次启动随机生成，落入 <exeDir>/config/rpc-token.txt，
     * 客户端必须先 auth 才能调用任何方法（详见 control.h 的安全模型）。
     *
     * 注意：只有控制通道**真正监听成功**才写令牌文件。否则第二个实例在端口被
     * 占用时会用新令牌覆盖掉正在运行实例的令牌文件，把它的所有客户端一起搞坏。 */
    std::string rpcToken = rpcTokenArg.empty() ? RpcServer::generateToken() : rpcTokenArg;
    if (app.startRpc(rpcPort, rpcToken)) {
        std::string tokDir  = pathJoin(base, "config");
        std::string tokPath = pathJoin(tokDir, "rpc-token.txt");
        makeDirs(tokDir);
        FILE* tf = fopen(tokPath.c_str(), "wb");
        if (tf) {
            fwrite(rpcToken.data(), 1, rpcToken.size(), tf);
            fclose(tf);
            PP_INFO("控制通道令牌已写入: %s", tokPath.c_str());
        } else {
            PP_WARN("无法写入令牌文件: %s（客户端可用 --rpc-token 直接传入）", tokPath.c_str());
        }
        printf("控制通道: 127.0.0.1:%d   令牌文件: %s\n", rpcPort, tokPath.c_str());
    } else if (rpcPort > 0) {
        fprintf(stderr,
                "控制通道启动失败（端口 %d 不可用），播放器继续运行。\n"
                "  提示：可能已有另一个实例在监听该端口；本实例未写入令牌文件，\n"
                "        以免影响正在运行实例的客户端。\n", rpcPort);
    }

    int rc = 0;
    if (selfTest) {
        if (media.empty()) {
            fprintf(stderr, "自检模式必须给定媒体文件\n");
            app.shutdown();
            return 2;
        }
        rc = app.selfTest(media, frames, outDir);
    } else {
        if (!media.empty()) {
            if (!app.openMedia(media)) {
                fprintf(stderr, "打开媒体失败，按 H 查看帮助；程序仍将保持运行以便排查。\n");
            } else {
                app.player().setVolume((float)volume);
                if (startSec > 0) app.player().seekAbsolute(startSec);
            }
        }
        app.run();
    }

    app.shutdown();
    Log::shutdown();
    return rc;
}
