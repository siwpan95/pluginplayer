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
]

# 插件目录名 -> 输出 DLL 名
PLUGINS = ["osd_stats", "video_effects", "audio_viz", "snapshot", "hotkey_ext"]

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


def run(cmd, cwd=ROOT, quiet=False):
    if not quiet:
        print("  $ " + " ".join(cmd))
    p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, encoding="utf-8", errors="replace")
    return p.returncode, p.stdout


def check_toolchain():
    if not os.path.isfile(GXX):
        print(f"[ERR] 找不到编译器: {GXX}")
        print("      可用环境变量 PP_GXX 指定，例如 PP_GXX=D:/mingw/bin/g++.exe")
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
    os.makedirs(BUILD, exist_ok=True)
    out = os.path.join(BUILD, name + ".dll")
    cmd = ([GXX] + (flags or CXXFLAGS) + INCLUDES + [src, "-o", out] + LDFLAGS_PLUGIN)
    rc, log = run(cmd)
    if rc != 0:
        print(f"[FAIL] 插件 {name} 编译失败：\n{log}")
        return False
    print(f"[OK] {out}  ({os.path.getsize(out)/1024:.0f} KB)")
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

    # MinGW 运行时 DLL（libstdc++/libgcc 已静态链接，但 libwinpthread 仍需随行）
    mingw_bin = os.path.dirname(GXX)
    for rt in ("libwinpthread-1.dll", "libgcc_s_seh-1.dll", "libstdc++-6.dll"):
        pth = os.path.join(mingw_bin, rt)
        if os.path.isfile(pth):
            cp(pth, DIST)

    # 默认配置
    cfg = os.path.join(DIST, "config", "pluginplayer.ini")
    if not os.path.isfile(cfg):
        with open(cfg, "w", encoding="utf-8") as f:
            f.write("# PluginPlayer 配置（自动生成，可手工编辑）\n"
                    "# 插件私有配置统一使用 plugin.<槽位号>.<键> 前缀\n\n"
                    "plugin.1.count=0\n"
                    "plugin.1.stamp=0\n"
                    "plugin.3.mode=1\n"
                    "plugin.5.enabled=1\n")
    return True


def make_launcher():
    bat = os.path.join(ROOT, "run.bat")
    with open(bat, "w", encoding="gbk", errors="replace") as f:
        f.write("@echo off\r\n"
                "rem PluginPlayer 启动器：把媒体文件拖到本文件上即可播放\r\n"
                "setlocal\r\n"
                "cd /d %~dp0dist\r\n"
                "pluginplayer.exe %*\r\n")
    selftest = os.path.join(ROOT, "selftest.bat")
    with open(selftest, "w", encoding="gbk", errors="replace") as f:
        f.write("@echo off\r\n"
                "rem 一键自检：构建 -> 生成测试视频 -> 跑通全链路 -> 输出 JSON 报告\r\n"
                "setlocal\r\n"
                "cd /d %~dp0\r\n"
                "if not exist vendor\\ffmpeg\\bin\\ffmpeg.exe (\r\n"
                "  echo [ERR] 缺少 vendor\\ffmpeg\\bin\\ffmpeg.exe，请先执行 tools\\fetch_deps.py\r\n"
                "  pause & exit /b 1\r\n"
                ")\r\n"
                "F:\\Python314\\python.exe build.py || pause\r\n"
                "cd dist\r\n"
                "if not exist test_assets mkdir test_assets\r\n"
                "if not exist test_assets\\sample.mp4 "
                "..\\vendor\\ffmpeg\\bin\\ffmpeg.exe -y -hide_banner -loglevel error "
                "-f lavfi -i testsrc2=size=1280x720:rate=30 "
                "-f lavfi -i sine=frequency=440:sample_rate=48000 "
                "-t 8 -c:v libx264 -preset veryfast -pix_fmt yuv420p -c:a aac -shortest "
                "test_assets\\sample.mp4\r\n"
                "pluginplayer.exe --selftest --frames 90 --out selftest_out test_assets\\sample.mp4\r\n"
                "echo.\r\necho 报告: dist\\selftest_out\\selftest_report.json\r\n"
                "pause\r\n")
    print("[OK] 已生成 run.bat 与 selftest.bat")


def main():
    ap = argparse.ArgumentParser(description="PluginPlayer 构建脚本")
    ap.add_argument("--clean", action="store_true")
    ap.add_argument("--core", action="store_true", help="只编译内核")
    ap.add_argument("--plugins", default="", help="逗号分隔，只编译指定插件")
    ap.add_argument("--list", action="store_true", help="列出插件")
    ap.add_argument("--no-package", action="store_true")
    ap.add_argument("--debug", action="store_true",
                    help="用 -O0 -g 构建（便于 gdb 抓崩溃栈，体积大）")
    args = ap.parse_args()

    if args.list:
        for p in PLUGINS:
            print(p)
        return 0

    if args.clean:
        for d in (BUILD, DIST):
            if os.path.isdir(d):
                shutil.rmtree(d)
                print(f"[OK] 已删除 {d}")
        return 0

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

    print(f"\n{'[PASS]' if okAll else '[FAIL]'} 构建{'成功' if okAll else '有失败项'}，用时 {time.time()-t0:.1f}s")
    if okAll:
        print(f"产物目录: {DIST}")
    return 0 if okAll else 1


if __name__ == "__main__":
    sys.exit(main())
