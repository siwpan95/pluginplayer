#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
build.py — PluginPlayer 构建脚本
================================

不依赖 cmake / make：直接用本机 MinGW-w64 g++ 编译，产物汇总到 dist/。

用法:
    F:/Python314/python.exe build.py            # 完整构建（内核 + 全部插件 + 打包 dist）
    ... build.py --core                         # 只编译内核
    ... build.py --plugins osd_stats,snapshot   # 只编译指定插件
    ... build.py --clean                        # 清理 build/ 与 dist/
    ... build.py --list                         # 列出可编译的插件

工具链（可用环境变量覆盖）:
    PP_GXX   默认 F:/mingw64/mingw64/bin/g++.exe
"""
import os
import sys
import glob
import hashlib
import shutil
import subprocess
import argparse
import time

ROOT = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(ROOT, "src")
BUILD = os.path.join(ROOT, "build")
DIST = os.path.join(ROOT, "dist")
VENDOR = os.path.join(ROOT, "vendor")

GXX = os.environ.get("PP_GXX", r"F:/mingw64/mingw64/bin/g++.exe")

CORE_SOURCES = [
    "core/main.cpp", "core/app.cpp", "core/host.cpp", "core/player.cpp",
    "core/pluginhost.cpp", "core/osd.cpp", "core/eventbus.cpp",
    "core/registry.cpp", "core/config.cpp", "core/util.cpp", "core/control.cpp",
    "core/font.cpp",           # P1-a 真字体渲染（stb_truetype）
]

# 插件目录名 -> 输出 DLL 名
PLUGINS = ["osd_stats", "video_effects", "audio_viz", "snapshot", "hotkey_ext",
           "subtitles", "track_selector", "audio_device", "video_quality",
           "radio_bridge", "video_tonemap"]

# tests/ 下的冒烟测试：编译后立即运行，退出码 0 视为通过。
# 早期版本这两个文件不被任何脚本引用 —— 等于没有测试入口。
SMOKE_TESTS = ["smoke_link", "smoke_audio"]

# ---- 单元测试（--unit）----
# 只编译**不依赖 SDL / FFmpeg** 的四个模块：util / config / eventbus / registry。
# 它们的价值在于「纯逻辑 + 安全策略」（严格 JSON 解析、写入白名单、撤销语义），
# 而且没有平台依赖 —— 所以同一套源码在 Windows(MinGW) 与 Linux(g++) 上都能跑，
# Gitee Go 的 Ubuntu 构建环境正是靠这一点跑 C++ 单元测试（见 tools/ci.py）。
UNIT_CORE_SOURCES = [
    "src/core/util.cpp",
    "src/core/config.cpp",
    "src/core/eventbus.cpp",
    "src/core/registry.cpp",
]
UNIT_TEST_SOURCES = [
    "tests/unit/main.cpp",
    "tests/unit/test_string.cpp",
    "tests/unit/test_path.cpp",
    "tests/unit/test_json.cpp",
    "tests/unit/test_hash_png.cpp",
    "tests/unit/test_capture.cpp",
    "tests/unit/test_color.cpp",
    "tests/unit/test_eventbus.cpp",
    "tests/unit/test_registry.cpp",
    "tests/unit/test_config.cpp",
    "tests/unit/test_utf8.cpp",  # P1-a UTF-8 解码与全宽判定
    "tests/unit/test_tonemap.cpp",  # P2-3 Phase1: HDR→SDR 色调映射纯数学
]
# 单元测试不需要 SDL3 / FFmpeg 的头文件，但 util.cpp 用了 bcrypt 做 SHA-256
LDFLAGS_UNIT = ["-lbcrypt"] + ["-static-libgcc", "-static-libstdc++",
                               "-Wl,-Bstatic", "-lwinpthread", "-Wl,-Bdynamic"]

CXXFLAGS = [
    "-std=c++17", "-O2", "-Wall", "-Wextra",
    "-Wno-unused-parameter", "-DNDEBUG", "-s",     # -s: 去符号，发布体积小
]

# --debug：便于用 gdb 抓崩溃栈（体积大、不优化）
CXXFLAGS_DEBUG = [
    "-std=c++17", "-O0", "-g", "-Wall", "-Wextra",
    "-Wno-unused-parameter", "-DNDEBUG",
]

INCLUDES = [
    "-Isrc/sdk", "-Isrc/core",
    "-Ivendor/sdl3/include", "-Ivendor/ffmpeg/include",
    "-Ivendor/stb",            # stb_truetype.h（P1-a 真字体）
    "-Ivendor/libass/include", # P1-e libass 字幕渲染（仅 subtitles 插件用到）
]

# 只静态链接运行时库（libgcc / libstdc++ / libwinpthread），
# 而 SDL3 与 FFmpeg 仍然走 .dll.a 导入库动态链接。
# 注意：不能用 `-static`，那样 ld 会去找 libSDL3.a / libav*.a 静态库而报找不到。
STATIC_RUNTIME = [
    "-static-libgcc", "-static-libstdc++",
    "-Wl,-Bstatic", "-lwinpthread", "-Wl,-Bdynamic",
]

LDFLAGS_CORE = [
    "-Lvendor/sdl3/lib", "-Lvendor/ffmpeg/lib",
    "-lSDL3", "-lavformat", "-lavcodec", "-lavutil", "-lswscale", "-lswresample",
    "-lws2_32", "-lbcrypt",
] + STATIC_RUNTIME

LDFLAGS_PLUGIN = ["-shared"] + STATIC_RUNTIME

# 每个插件的额外链接参数（按插件名）。subtitles 需要链接 libass 导入库
# （libass.dll.a），运行时由 Windows 加载器从 dist/ 解析 libass-9.dll 及其闭包。
PLUGIN_EXTRA_LDFLAGS = {
    "subtitles": ["-Lvendor/libass/lib", "-lass"],
    "radio_bridge": ["-lws2_32"],   # winsock 直连 radio 服务 /api/status
}

# 需要 vendor/libass 才能编译的插件（缺依赖时给出明确报错而不是一堆找不到 include 的噪声）
PLUGIN_NEEDS_LIBASS = {"subtitles"}


def run(cmd, cwd=ROOT, quiet=False, env=None):
    if not quiet:
        print("  $ " + " ".join(cmd))
    p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, encoding="utf-8", errors="replace", env=env)
    return p.returncode, p.stdout


def runtime_env():
    """运行产物需要的 PATH：dist/ 与 vendor 下的 DLL 目录，外加**编译器自带的 bin**。
    （build/*.exe 与 dist/ 不在同一目录，不补 PATH 会报 0xC0000135 找不到 DLL。）

    编译器 bin 也要加：`libwinpthread-1.dll` / `libgcc_s_seh-1.dll` / `libstdc++-6.dll`
    就在那里。少了它，单元测试这种"还没打包 dist/ 就要跑"的目标会以
    0xC0000135 直接挂掉 —— 实测踩过。"""
    env = dict(os.environ)
    extra = [DIST, os.path.join(VENDOR, "sdl3", "bin"), os.path.join(VENDOR, "ffmpeg", "bin"),
             os.path.dirname(GXX)]
    parts = [p for p in extra if p and os.path.isdir(p)]
    if parts:
        env["PATH"] = os.pathsep.join(parts + [env.get("PATH", "")])
    return env


def check_toolchain():
    if not os.path.isfile(GXX):
        print(f"[ERR] 找不到编译器: {GXX}")
        print("      可用环境变量 PP_GXX 指定，例如 PP_GXX=<MinGW安装目录>/bin/g++.exe")
        return False
    rc, out = run([GXX, "--version"], quiet=True)
    if rc != 0:
        print(f"[ERR] 编译器不可用: {out}")
        return False
    print("[OK] 编译器: " + out.splitlines()[0])
    for d in ("sdl3/include", "sdl3/lib", "ffmpeg/include", "ffmpeg/lib"):
        if not os.path.isdir(os.path.join(VENDOR, d)):
            print(f"[ERR] 缺少依赖目录 vendor/{d}")
            print("      请先执行 tools/fetch_deps.py 或手工准备 vendor/（见 README）")
            return False
    return True


def build_core(flags=None):
    print("\n== 编译内核 pluginplayer.exe ==")
    os.makedirs(BUILD, exist_ok=True)
    out = os.path.join(BUILD, "pluginplayer.exe")
    srcs = [os.path.join(SRC, s) for s in CORE_SOURCES]
    cmd = [GXX] + (flags or CXXFLAGS) + INCLUDES + srcs + ["-o", out] + LDFLAGS_CORE
    rc, log = run(cmd)
    if rc != 0:
        print("[FAIL] 内核编译失败：\n" + log)
        return False
    size = os.path.getsize(out)
    print(f"[OK] {out}  ({size/1024:.0f} KB)")
    return True


def build_plugin(name, flags=None):
    src = os.path.join(SRC, "plugins", name, "plugin.cpp")
    if not os.path.isfile(src):
        print(f"[SKIP] 插件源码不存在: {src}")
        return False
    if name in PLUGIN_NEEDS_LIBASS and not os.path.isdir(os.path.join(VENDOR, "libass", "include")):
        print(f"[ERR] 插件 {name} 需要 vendor/libass/（先跑 tools/fetch_libass.py）")
        return False
    os.makedirs(BUILD, exist_ok=True)
    out = os.path.join(BUILD, name + ".dll")
    extra = PLUGIN_EXTRA_LDFLAGS.get(name, [])
    cmd = ([GXX] + (flags or CXXFLAGS) + INCLUDES + [src, "-o", out] + LDFLAGS_PLUGIN + extra)
    rc, log = run(cmd)
    if rc != 0:
        print(f"[FAIL] 插件 {name} 编译失败：\n{log}")
        return False
    print(f"[OK] {out}  ({os.path.getsize(out)/1024:.0f} KB)")
    return True


def build_smoke_test(name, flags=None):
    """编译并运行 tests/<name>.cpp；退出码 0 视为通过。"""
    src = os.path.join(ROOT, "tests", name + ".cpp")
    if not os.path.isfile(src):
        print(f"[SKIP] 冒烟测试源码不存在: {src}")
        return False
    os.makedirs(BUILD, exist_ok=True)
    out = os.path.join(BUILD, name + ".exe")
    cmd = [GXX] + (flags or CXXFLAGS) + INCLUDES + [src, "-o", out] + LDFLAGS_CORE
    rc, log = run(cmd)
    if rc != 0:
        print(f"[FAIL] 冒烟测试 {name} 编译失败：\n{log}")
        return False
    print(f"[OK] {out}  ({os.path.getsize(out)/1024:.0f} KB)")
    rc2, out2 = run([out], quiet=True, env=runtime_env())
    tail = "\n".join([l for l in out2.strip().splitlines() if l.strip()][-6:])
    if tail:
        print("  " + tail.replace("\n", "\n  "))
    print(f"[{'PASS' if rc2 == 0 else 'FAIL'}] 冒烟测试 {name} 运行退出码 {rc2}")
    if rc2 == 0xC0000135:
        print("        (0xC0000135 = 找不到 DLL；确认 dist/ 与 vendor/ 下运行时 DLL 已就位)")
    return rc2 == 0


def build_unit_tests(flags=None):
    """编译并运行 C++ 单元测试（tests/unit/）。

    刻意**不检查 vendor/**：单元测试只覆盖不依赖 SDL/FFmpeg 的模块，
    所以"还没下依赖"的机器也该能跑测试 —— 依赖缺失不该挡住测试。
    """
    if not os.path.isfile(GXX):
        print(f"[ERR] 找不到编译器: {GXX}")
        return False
    os.makedirs(BUILD, exist_ok=True)
    out = os.path.join(BUILD, "pp_unit.exe")
    srcs = [os.path.join(ROOT, s) for s in UNIT_TEST_SOURCES + UNIT_CORE_SOURCES]
    missing = [s for s in srcs if not os.path.isfile(s)]
    if missing:
        print("[ERR] 单元测试源文件缺失:\n  " + "\n  ".join(missing))
        return False
    incs = ["-Isrc/sdk", "-Isrc/core", "-Itests/unit"]
    if os.path.isdir(os.path.join(VENDOR, "sdl3", "include")):
        incs.append("-Ivendor/sdl3/include")
    if os.path.isdir(os.path.join(VENDOR, "ffmpeg", "include")):
        incs.append("-Ivendor/ffmpeg/include")
    cmd = [GXX] + (flags or CXXFLAGS) + incs + srcs + ["-o", out] + LDFLAGS_UNIT
    rc, log = run(cmd)
    if rc != 0:
        print("[FAIL] 单元测试编译失败：\n" + log)
        return False
    print(f"[OK] {out}  ({os.path.getsize(out)/1024:.0f} KB)")

    rc2, out2 = run([out], quiet=True, env=runtime_env())
    text = out2 or ""
    for line in text.strip().splitlines():
        # 只转发用例结果与汇总，编译器的噪声不上屏
        if line.startswith(("[PASS]", "[FAIL]", "==== 单元测试")) or line.strip().startswith("[x]"):
            print("  " + line)
    print(f"[{'PASS' if rc2 == 0 else 'FAIL'}] 单元测试运行退出码 {rc2}")
    if rc2 == 0xC0000135:
        print("        (0xC0000135 = 找不到 DLL；确认 %s 存在)" % os.path.dirname(GXX))
    return rc2 == 0


def build_api_test_plugin(flags=None):
    """编译 tests/plugin_apitest/plugin.cpp → build/plugin_apitest.dll。
    这是插件 API 的回归测试插件（不随 dist/ 发布）；跑法见该文件头部注释。"""
    src = os.path.join(ROOT, "tests", "plugin_apitest", "plugin.cpp")
    if not os.path.isfile(src):
        print(f"[SKIP] 测试插件源码不存在: {src}")
        return False
    os.makedirs(BUILD, exist_ok=True)
    out = os.path.join(BUILD, "plugin_apitest.dll")
    cmd = [GXX] + (flags or CXXFLAGS) + INCLUDES + [src, "-o", out] + LDFLAGS_PLUGIN
    rc, log = run(cmd)
    if rc != 0:
        print(f"[FAIL] 测试插件编译失败：\n{log}")
        return False
    print(f"[OK] {out}  ({os.path.getsize(out)/1024:.0f} KB)")
    print("     跑法: 复制到 dist/plugins/ → 启动播放器 → 调用命令 api.check（返回 0 = 通过），"
          "跑完记得删除该 DLL")
    return True


def package():
    print("\n== 打包 dist/ ==")
    os.makedirs(DIST, exist_ok=True)
    os.makedirs(os.path.join(DIST, "plugins"), exist_ok=True)
    os.makedirs(os.path.join(DIST, "config"), exist_ok=True)
    os.makedirs(os.path.join(DIST, "assets"), exist_ok=True)

    def cp(src, dst):
        if os.path.isfile(src):
            shutil.copy2(src, dst)
            return True
        return False

    # 内核
    cp(os.path.join(BUILD, "pluginplayer.exe"), DIST)
    # 运行时 DLL —— 只带真正被链接到的那些。
    # avfilter / avdevice 本播放器没用到，带上会白占 ~110MB；
    # 需要它们的是 ffmpeg.exe 命令行工具，见 vendor/ffmpeg/bin。
    FF_RUNTIME = {"avutil", "avcodec", "avformat", "swscale", "swresample"}
    n = 0
    sdl_bin = os.path.join(VENDOR, "sdl3", "bin")
    if os.path.isdir(sdl_bin):
        for f in os.listdir(sdl_bin):
            if f.lower().endswith(".dll") and cp(os.path.join(sdl_bin, f), DIST):
                n += 1
    ff_bin = os.path.join(VENDOR, "ffmpeg", "bin")
    if os.path.isdir(ff_bin):
        for f in os.listdir(ff_bin):
            stem = f.split("-")[0].lower()
            if f.lower().endswith(".dll") and stem in FF_RUNTIME:
                if cp(os.path.join(ff_bin, f), DIST):
                    n += 1
    # 插件
    p = 0
    for pl in PLUGINS:
        if cp(os.path.join(BUILD, pl + ".dll"), os.path.join(DIST, "plugins")):
            p += 1
    print(f"[OK] 复制运行时 DLL {n} 个，插件 {p} 个")

    # libass 字幕栈（P1-e）：仅 subtitles 插件依赖，但其 DLL 闭包必须随 dist 发，
    # 否则 dist/ 里启动后会以 0xC0000135 找不到 libass-9.dll（或它的传递依赖）。
    # 闭包由 tools/fetch_libass.py 固定点抓取，bin/ 下即完整可加载集。
    libass_bin = os.path.join(VENDOR, "libass", "bin")
    if os.path.isdir(libass_bin):
        m = 0
        for f in sorted(os.listdir(libass_bin)):
            if f.lower().endswith(".dll") and cp(os.path.join(libass_bin, f), DIST):
                m += 1
        if m:
            print(f"[OK] 复制 libass 字幕栈 DLL {m} 个")

    # 随包字体（P1-a 真字体）。assets/ 整体复制 —— 目录里现在是
    # assets/fonts/*.ttf；以后放别的静态资源也不用改这里。
    # 字体是**发布载荷的一部分**：release-payload 门禁会把 dist/ 实际文件数与
    # README 声明比对，随包字体变了要同步改 README。
    assets_src = os.path.join(ROOT, "assets")
    a_n = 0
    if os.path.isdir(assets_src):
        for dirpath, _dirnames, filenames in os.walk(assets_src):
            rel = os.path.relpath(dirpath, assets_src)
            dst_dir = os.path.join(DIST, "assets") if rel == "." else os.path.join(DIST, "assets", rel)
            os.makedirs(dst_dir, exist_ok=True)
            for fn in filenames:
                if fn == ".gitkeep":
                    continue
                if os.path.isfile(os.path.join(dirpath, fn)):
                    shutil.copy2(os.path.join(dirpath, fn), os.path.join(dst_dir, fn))
                    a_n += 1
    if a_n:
        print(f"[OK] 复制 assets {a_n} 个文件（含随包字体）")
    write_plugin_manifest()

    # MinGW 运行时 DLL（libstdc++/libgcc 已静态链接，但 libwinpthread 仍需随行）
    mingw_bin = os.path.dirname(GXX)
    for rt in ("libwinpthread-1.dll", "libgcc_s_seh-1.dll", "libstdc++-6.dll"):
        pth = os.path.join(mingw_bin, rt)
        if os.path.isfile(pth):
            cp(pth, DIST)

    # 默认配置。只有文件不存在时才生成 —— 不覆盖用户已有的设置。
    cfg = os.path.join(DIST, "config", "pluginplayer.ini")
    if not os.path.isfile(cfg):
        with open(cfg, "w", encoding="utf-8") as f:
            f.write(
                "# PluginPlayer 配置（自动生成，可手工编辑）\n"
                "#\n"
                "# 【插件私有配置】由宿主自动写入/读取，键名格式：\n"
                "#     plugin.<插件名>.<键>\n"
                "#   例如 plugin.snapshot.count、plugin.video_effects.mode。\n"
                "#   插件名取 PP_Plugin.name（小写；非 [a-z0-9_-] 的字符替换成下划线）。\n"
                "#\n"
                "#   历史说明：早期版本用的是 plugin.<槽位号>.<键>，而槽位号按 DLL 名字母序\n"
                "#   分配，增删插件会让所有配置整体错位。改成按插件名后，首次访问某个键时\n"
                "#   会自动从旧的槽位键迁移过来（日志里会打印「配置迁移: ...」）。\n"
                "#\n"
                "# 【安全相关】\n"
                "#   security.write_dirs  追加「插件可写目录」，分号分隔\n"
                "#                        默认只允许 exeDir（即 dist/），例：\n"
                "#                        security.write_dirs=D:/captures;E:/shots\n"
                "#\n"
                "# 注意：控制通道令牌不在本文件里，它由播放器每次启动随机生成，\n"
                "#       写在同目录的 rpc-token.txt。\n"
            )
    return True


# 运行期产物：不该随发布物分发，--release 会从 dist/ 里清掉
RUNTIME_ARTIFACTS = [
    "logs", "selftest_out", "test_assets",
]


def write_plugin_manifest():
    """为 dist/plugins 生成 SHA-256 完整性清单。
    宿主在清单**存在时**会强制校验：未列出或哈希不符的插件拒绝加载。
    用 Python 的 hashlib 算，不必启动播放器。"""
    pdir = os.path.join(DIST, "plugins")
    if not os.path.isdir(pdir):
        return 0
    lines = [
        "# PluginPlayer 插件完整性清单",
        "# 格式: <sha256>  <文件名>（与 sha256sum 兼容）",
        "# 由 build.py 自动生成。手动往 plugins/ 里加 DLL 后必须重新生成，",
        "# 否则该 DLL 会被宿主拒绝加载（也可运行时执行 pluginplayer.exe --write-plugin-manifest）。",
        "",
    ]
    n = 0
    for f in sorted(os.listdir(pdir)):
        if not f.lower().endswith(".dll"):
            continue
        h = hashlib.sha256()
        with open(os.path.join(pdir, f), "rb") as fp:
            for chunk in iter(lambda: fp.read(1 << 20), b""):
                h.update(chunk)
        lines.append(f"{h.hexdigest()}  {f}")
        n += 1
    with open(os.path.join(pdir, "manifest.sha256"), "w",
              encoding="utf-8", newline="\n") as fp:
        fp.write("\n".join(lines) + "\n")
    print(f"[OK] 已生成 plugins/manifest.sha256（{n} 个插件）")
    return n


def strip_runtime_artifacts():
    """发布前清理 dist/ 里的运行期产物（日志、自检证据、测试素材、令牌）。"""
    removed = []
    for name in RUNTIME_ARTIFACTS:
        # 前缀匹配：selftest_out / selftest_out_short / selftest_out_audio … 一起清掉
        for p in glob.glob(os.path.join(DIST, name + "*")):
            if os.path.isdir(p):
                shutil.rmtree(p, ignore_errors=True)
                removed.append(os.path.basename(p) + "/")
            elif os.path.isfile(p):
                try:
                    os.remove(p)
                    removed.append(os.path.basename(p))
                except OSError:
                    pass
    tok = os.path.join(DIST, "config", "rpc-token.txt")
    if os.path.isfile(tok):
        try:
            os.remove(tok)
            removed.append("config/rpc-token.txt")
        except OSError:
            pass
    for f in glob.glob(os.path.join(DIST, "**", "*.log"), recursive=True):
        try:
            os.remove(f)
            removed.append(os.path.relpath(f, DIST).replace("\\", "/"))
        except OSError:
            pass
    return removed


def release_summary():
    files = [f for f in glob.glob(os.path.join(DIST, "**", "*"), recursive=True)
             if os.path.isfile(f)]
    total = sum(os.path.getsize(f) for f in files)
    print(f"\n[release] 可发布文件 {len(files)} 个，合计 {total/1048576:.1f} MB")
    print(f"[release] 目录: {DIST}")
    return len(files), total


def make_launcher():
    # 纯 ASCII 内容 + CRLF：cmd.exe 按 GBK 解读 .bat，混入中文会在非中文代码页
    # 的机器上乱码/截断（旧版这里带中文注释，属可移植性问题）。
    #
    # 【踩过的坑】不要写 newline="\r\n"：那会让 open() 把字符串里**已经写好的**
    # \n 再翻译一次，于是 "a\r\nb" 落盘成 "a\r\r\nb"（CR CR LF）—— 实测过，
    # git diff 看着像 CRLF，实际每行多个 CR。正确做法是 newline="" 让内容原样落盘，
    # 行尾由字符串自己给出（tools/pp_cli.py 的 write_text_exact 用的是同一套）。
    # tools/verify.py 的 bat-ascii-crlf 门禁现在会检查"孤立的 CR"，防它再回来。
    # run.bat 是"拖个文件上来就能播"的入口。旧版只有 4 行，三处不设防：
    #  1. `cd /d %~dp0dist` 与 `pluginplayer.exe` 都不带引号。`cd` 恰好容忍裸空格
    #     （cmd 对路径类命令不按空格拆词），但安装路径里出现 `&` / `^` / `(` 时
    #     `cd` 会失败，而脚本**照样继续往下走** —— 于是在错的目录里找 exe。
    #  2. 不查 exe 在不在：`dist/` 被 `build.py --clean` 清掉、或只拷了 exe 没拷
    #     dist 时，用户看到的报错是"不是内部或外部命令"，看不出真正原因。
    #  3. 失败一律不可见：双击启动时窗口一闪就没了。所以先查 exe，出错与子进程
    #     非 0 退出才 `pause`（正常退出不打断，脚本调用不受影响）。
    # 参数仍按调用方原样传（`%*`）：资源管理器已经给含空格的拖入路径加过引号，
    # `%*` 会把那份引号原样带上，不要再"帮忙"补一遍（补不出对的结果）。
    bat = os.path.join(ROOT, "run.bat")
    with open(bat, "w", encoding="utf-8", newline="") as f:
        f.write("@echo off\r\n"
                "rem PluginPlayer launcher: drag a media file onto this file to play it.\r\n"
                "rem GENERATED by build.py (make_launcher) -- edit the generator, not this file.\r\n"
                "setlocal\r\n"
                'set "PP_ROOT=%~dp0"\r\n'
                'set "PP_EXE=%PP_ROOT%dist\\pluginplayer.exe"\r\n'
                'if not exist "%PP_EXE%" (\r\n'
                "  echo [ERR] not found: %PP_EXE%\r\n"
                "  echo       build it first: python build.py\r\n"
                "  pause\r\n"
                "  exit /b 1\r\n"
                ")\r\n"
                "rem Arguments stay as the caller wrote them: Explorer already quotes a\r\n"
                "rem dragged path that contains a space, and %* preserves that quoting.\r\n"
                'cd /d "%PP_ROOT%dist"\r\n'
                '"%PP_EXE%" %*\r\n'
                'set "PP_RC=%ERRORLEVEL%"\r\n'
                'if not "%PP_RC%"=="0" (\r\n'
                "  echo.\r\n"
                "  echo [ERR] pluginplayer.exe exit code %PP_RC%\r\n"
                "  pause\r\n"
                ")\r\n"
                "exit /b %PP_RC%\r\n")
    selftest = os.path.join(ROOT, "selftest.bat")
    with open(selftest, "w", encoding="utf-8", newline="") as f:
        f.write("@echo off\r\n"
                "rem One-click self test: build -> smoke -> make sample -> full chain -> JSON report\r\n"
                "setlocal\r\n"
                "cd /d %~dp0\r\n"
                "if not exist vendor\\ffmpeg\\bin\\ffmpeg.exe (\r\n"
                "  echo [ERR] vendor\\ffmpeg\\bin\\ffmpeg.exe is missing. Run tools\\fetch_deps.py first.\r\n"
                "  pause & exit /b 1\r\n"
                ")\r\n"
                "F:\\Python314\\python.exe build.py || pause\r\n"
                "echo [1/3] smoke tests (tests/smoke_link.cpp + tests/smoke_audio.cpp)\r\n"
                "F:\\Python314\\python.exe build.py --smoke\r\n"
                "if errorlevel 1 echo [WARN] smoke test had failures, see output above\r\n"
                "cd dist\r\n"
                "if not exist test_assets mkdir test_assets\r\n"
                "if not exist test_assets\\sample.mp4 "
                "..\\vendor\\ffmpeg\\bin\\ffmpeg.exe -y -hide_banner -loglevel error "
                "-f lavfi -i testsrc2=size=1280x720:rate=30 "
                "-f lavfi -i sine=frequency=440:sample_rate=48000 "
                "-t 8 -c:v libx264 -preset veryfast -pix_fmt yuv420p -c:a aac -shortest "
                "test_assets\\sample.mp4\r\n"
                "pluginplayer.exe --selftest --frames 90 --out selftest_out test_assets\\sample.mp4\r\n"
                "echo.\r\necho report: dist\\selftest_out\\selftest_report.json\r\n"
                "pause\r\n")
    # start-dsh.bat：**本项目的启动器**（双击打开播放器窗口；媒体文件拖到它上面即播）。
    # 它转发给 run.bat —— 启动逻辑（解释器/引号/产物检查/退出码三条硬约束，见
    # docs/DEVELOPMENT.md §7）只保留一份，不复制第二遍。历史上这里放的是
    # deepseek-harness 的 Web UI 启动器（上游那份在 G:\notpython\deepseek-harness-master\
    # start-dsh.bat），按项目所有者要求改回"启动本项目"这一职责；两个 dsh 小工具
    # （tools/dsh_bind_workspace.py、tools/dsh_open_ui.py）留在 tools/ 里可独立使用。
    dsh = os.path.join(ROOT, "start-dsh.bat")
    with open(dsh, "w", encoding="utf-8", newline="") as f:
        f.write("@echo off\r\n"
                "rem PluginPlayer launcher -- starts THIS project.\r\n"
                "rem Delegates to run.bat so the start logic exists exactly once\r\n"
                "rem (quoting / artifact check / exit code: docs/DEVELOPMENT.md section 7).\r\n"
                "rem Console code page: the player prints UTF-8 (source and compiler exec charset\r\n"
                "rem are both UTF-8) while a fresh console defaults to GBK here, which garbles\r\n"
                "rem Chinese output. The program itself also calls SetConsoleOutputCP; this line\r\n"
                "rem covers the shell around it, so a double-click always reads cleanly.\r\n"
                "rem Double-click to open the player; drop a media file on it to play that file.\r\n"
                "rem GENERATED by build.py (make_launcher) -- edit the generator, not this file.\r\n"
                "setlocal\r\n"
                "chcp 65001 >nul\r\n"
                'if not exist "%~dp0run.bat" (\r\n'
                "  echo [ERR] run.bat is missing next to this launcher.\r\n"
                "  pause\r\n"
                "  exit /b 1\r\n"
                ")\r\n"
                'call "%~dp0run.bat" %*\r\n'
                "exit /b %ERRORLEVEL%\r\n")
    ci = os.path.join(ROOT, "ci.bat")
    with open(ci, "w", encoding="utf-8", newline="") as f:
        f.write("@echo off\r\n"
                "rem PluginPlayer one-click CI: gates + python/cpp unit tests + smoke + selftest + CLI.\r\n"
                "rem Windows-only stages need Windows; on Linux use: python3 tools/ci.py --profile ci\r\n"
                "rem Stages whose prerequisites are missing are SKIPPED with a reason (skip != pass).\r\n"
                "setlocal\r\n"
                "cd /d %~dp0\r\n"
                "set \"PP_PY=%~dp0.venv\\Scripts\\python.exe\"\r\n"
                "if not exist \"%PP_PY%\" set \"PP_PY=\"\r\n"
                "rem Do NOT use `if not defined X where py && set ...`: in cmd the && ends\r\n"
                "rem the if scope (if guards only the first command), so set runs always.\r\n"
                "if not defined PP_PY (\r\n"
                "  where py >nul 2>nul\r\n"
                "  if not errorlevel 1 set \"PP_PY=py -3\"\r\n"
                ")\r\n"
                "if not defined PP_PY (\r\n"
                "  where python >nul 2>nul\r\n"
                "  if not errorlevel 1 set \"PP_PY=python\"\r\n"
                ")\r\n"
                "if not defined PP_PY (\r\n"
                "  echo [ERR] Python not found. Set PP_PYTHON to a python.exe path. 1>&2\r\n"
                "  pause & exit /b 6\r\n"
                ")\r\n"
                "if not exist vendor\\ffmpeg\\lib (\r\n"
                "  echo [WARN] vendor\\ is not ready: Windows stages will be SKIPPED.\r\n"
                "  echo        Prepare it with: %PP_PY% tools\\fetch_deps.py\r\n"
                ")\r\n"
                "%PP_PY% tools\\ci.py %*\r\n"
                "set \"PP_RC=%ERRORLEVEL%\"\r\n"
                "if not \"%PP_RC%\"==\"0\" echo [FAIL] CI had failures, see output above.\r\n"
                "exit /b %PP_RC%\r\n")
    print("[OK] 已生成 run.bat、selftest.bat、start-dsh.bat 与 ci.bat（纯 ASCII，CRLF）")


def main():
    ap = argparse.ArgumentParser(description="PluginPlayer 构建脚本")
    ap.add_argument("--clean", action="store_true")
    ap.add_argument("--core", action="store_true", help="只编译内核")
    ap.add_argument("--smoke", action="store_true",
                    help="编译并运行 tests/ 下的冒烟测试后退出")
    ap.add_argument("--unit", action="store_true",
                    help="编译并运行 C++ 单元测试（tests/unit/，不需要 vendor/）")
    ap.add_argument("--apitest", action="store_true",
                    help="编译插件 API 回归测试插件后退出")
    ap.add_argument("--release", action="store_true",
                    help="干净重建 + 清理 dist/ 里的运行期产物（日志/自检证据/测试素材/令牌）")
    ap.add_argument("--plugins", default="", help="逗号分隔，只编译指定插件")
    ap.add_argument("--list", action="store_true", help="列出插件")
    ap.add_argument("--no-package", action="store_true")
    ap.add_argument("--debug", action="store_true",
                    help="用 -O0 -g 构建（便于 gdb 抓崩溃栈，体积大）")
    args = ap.parse_args()

    # --release 隐含一次干净构建：否则 dist/ 会继承上一次构建的残留
    # （实测：上一轮放进 plugins/ 的测试 DLL 与一堆 selftest_out_* 都留了下来）。
    if args.release:
        args.clean = True

    if args.list:
        for p in PLUGINS:
            print(p)
        return 0

    if args.smoke:
        if not check_toolchain():
            return 1
        okAll = True
        for t in SMOKE_TESTS:
            okAll &= build_smoke_test(t, CXXFLAGS_DEBUG if args.debug else CXXFLAGS)
        print(f"\n{'[PASS]' if okAll else '[FAIL]'} 冒烟测试"
              f"{'全部通过' if okAll else '有失败项'}")
        return 0 if okAll else 1

    if args.unit:
        ok = build_unit_tests(CXXFLAGS_DEBUG if args.debug else CXXFLAGS)
        return 0 if ok else 1

    if args.apitest:
        if not check_toolchain():
            return 1
        ok = build_api_test_plugin(CXXFLAGS_DEBUG if args.debug else CXXFLAGS)
        return 0 if ok else 1

    if args.clean:
        for d in (BUILD, DIST):
            if os.path.isdir(d):
                shutil.rmtree(d)
                print(f"[OK] 已删除 {d}")
        if not args.release:
            return 0
        print("[--release] 已清空 build/ 与 dist/，继续做全新构建")

    if not check_toolchain():
        return 1

    t0 = time.time()
    okAll = True
    flags = CXXFLAGS_DEBUG if args.debug else CXXFLAGS
    if args.debug:
        print("[--debug] 使用 -O0 -g 构建")

    if not args.plugins:
        okAll &= build_core(flags)
        if args.core:
            if not args.no_package:
                package()
            print(f"\n用时 {time.time()-t0:.1f}s")
            return 0 if okAll else 1

    targets = [s.strip() for s in args.plugins.split(",") if s.strip()] or PLUGINS
    for p in targets:
        okAll &= build_plugin(p, flags)

    if not args.no_package:
        package()
        make_launcher()
        if args.release:
            removed = strip_runtime_artifacts()
            if removed:
                print("[release] 已清理运行期产物：" + ", ".join(removed))
            else:
                print("[release] dist/ 中没有运行期产物需要清理")
            release_summary()

    print(f"\n{'[PASS]' if okAll else '[FAIL]'} 构建{'成功' if okAll else '有失败项'}，用时 {time.time()-t0:.1f}s")
    if okAll:
        print(f"产物目录: {DIST}")
    return 0 if okAll else 1


if __name__ == "__main__":
    sys.exit(main())
