#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
fetch_deps.py — 准备 vendor/ 依赖（SDL3 + FFmpeg 共享开发包）

本工程不提交二进制依赖，首次克隆后运行本脚本即可重建 vendor/。
优先使用本机已有的压缩包，其次联网下载。

用法:
    F:/Python314/python.exe tools/fetch_deps.py
    ... --src G:/local/player          # 指定本地资源目录（默认会自动探测）
    ... --vendor ./vendor              # 指定输出目录

需要的材料（任选其一来源）:
    SDL3-devel-3.4.16-mingw.zip        SDL3 头文件 + libSDL3.dll.a + 导入库
    SDL3-3.4.16-win32-x64.zip          SDL3.dll 运行时
    ffmpeg-9.0.1-full_build-shared.7z  FFmpeg 9.x 共享版（含 include/ lib/*.dll.a bin/*.dll）
"""
import os
import sys
import glob
import shutil
import argparse
import subprocess
import urllib.request
import ssl

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# 本机常见位置（用户把资源放在这里时可直接复用，不必联网）
DEFAULT_SRC_DIRS = [
    r"G:\local\player",
    r"G:\local\player\_extract",
    os.path.join(ROOT, "_deps"),
]

DOWNLOADS = {
    # 文件名关键字 -> 下载地址（SDL 官方站点，不走 GitHub）
    "SDL3-devel": "https://www.libsdl.org/release/SDL3-devel-3.4.16-mingw.zip",
    "SDL3-win32-x64": "https://www.libsdl.org/release/SDL3-3.4.16-win32-x64.zip",
    "ffmpeg-shared": "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-full-shared.7z",
}

SEVENZ = [
    r"C:\ProgramData\chocolatey\tools\7z.exe",
    r"F:\FLiNGTrainer\7z\7z.exe",
    "7z", "7za",
]


def find_7z():
    for p in SEVENZ:
        if os.path.isfile(p):
            return p
    for p in SEVENZ:
        if shutil.which(p):
            return shutil.which(p)
    return None


def find_local(src_dirs, keyword):
    for d in src_dirs:
        if not os.path.isdir(d):
            continue
        for pat in ("*.7z", "*.zip"):
            for f in glob.glob(os.path.join(d, pat)):
                if keyword.lower() in os.path.basename(f).lower():
                    return f
    return None


def download(url, out):
    print(f"    下载 {url}")
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req, context=ssl.create_default_context(), timeout=900) as r, \
            open(out, "wb") as f:
        total = int(r.headers.get("Content-Length") or 0)
        got = 0
        while True:
            c = r.read(1 << 20)
            if not c:
                break
            f.write(c)
            got += len(c)
    print(f"    完成 {out} ({got} bytes)")
    return os.path.getsize(out) > 1024


def extract(sevenz, archive, dest, keywords=None):
    os.makedirs(dest, exist_ok=True)
    cmd = [sevenz, "x", archive, f"-o{dest}", "-y"]
    if keywords:
        for k in keywords:
            cmd.append(k)
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return r.returncode == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", action="append", default=[])
    ap.add_argument("--vendor", default=os.path.join(ROOT, "vendor"))
    ap.add_argument("--tmp", default=os.path.join(ROOT, "_deps"))
    args = ap.parse_args()

    src_dirs = args.src + DEFAULT_SRC_DIRS
    sevenz = find_7z()
    if not sevenz:
        print("[ERR] 找不到 7z 解压工具，请安装 7-Zip 或用 --src 指向已解压目录")
        return 1

    cache = os.path.join(args.tmp)
    os.makedirs(cache, exist_ok=True) if not os.path.isdir(cache) and False else None
    if not os.path.isdir(cache):
        os.makedirs(cache, exist_ok=True)

    plan = [
        ("SDL3-devel", "SDL3-devel-3.4.16-mingw.zip", None),
        ("SDL3-win32-x64", "SDL3-3.4.16-win32-x64.zip", None),
        ("ffmpeg-shared", "ffmpeg-9.0.1-full_build-shared.7z", None),
    ]

    archives = {}
    for key, nice, _ in plan:
        local = find_local(src_dirs, key.split("-")[0])
        if local:
            print(f"[1/3] {nice}: 使用本机档案 {local}")
            archives[key] = local
        else:
            dst = os.path.join(cache, nice)
            if os.path.isfile(dst) and os.path.getsize(dst) > 1024:
                print(f"[1/3] {nice}: 使用已下载缓存 {dst}")
                archives[key] = dst
            else:
                print(f"[1/3] {nice}: 本机未找到，尝试下载")
                try:
                    download(DOWNLOADS[key], dst)
                    archives[key] = dst
                except Exception as e:
                    print(f"[ERR] 下载失败: {e}")
                    print("      请手工把该档案放到 _deps/ 或 --src 指定目录后重试")
                    return 1

    vendor = args.vendor
    tmp = os.path.join(cache, "extract")
    os.makedirs(tmp, exist_ok=True)

    print("[2/3] 解压…")
    for key, path in archives.items():
        d = os.path.join(tmp, key)
        if not os.path.isdir(d):
            if not extract(sevenz, path, tmp):
                print(f"[ERR] 解压失败: {path}")
                return 1

    print("[3/3] 组装 vendor/ …")
    sdl_root = None
    for cand in glob.glob(os.path.join(tmp, "SDL3-devel*", "SDL3-*", "x86_64-w64-mingw32")):
        sdl_root = cand
    if not sdl_root:
        for cand in glob.glob(os.path.join(tmp, "SDL3-devel*", "x86_64-w64-mingw32")):
            sdl_root = cand
    if not sdl_root:
        print("[ERR] 找不到 SDL3 的 x86_64-w64-mingw32 目录")
        return 1

    ff_root = None
    for cand in glob.glob(os.path.join(tmp, "ffmpeg-shared", "ffmpeg-*-full_build-shared")):
        ff_root = cand
    if not ff_root:
        print("[ERR] 找不到 FFmpeg full_build-shared 目录")
        return 1

    def reset(p):
        if os.path.isdir(p):
            shutil.rmtree(p)
        os.makedirs(p, exist_ok=True)

    reset(os.path.join(vendor, "sdl3", "include"))
    reset(os.path.join(vendor, "sdl3", "lib"))
    reset(os.path.join(vendor, "sdl3", "bin"))
    shutil.copytree(os.path.join(sdl_root, "include"), os.path.join(vendor, "sdl3", "include"),
                    dirs_exist_ok=True)
    shutil.copy2(os.path.join(sdl_root, "lib", "libSDL3.dll.a"),
                 os.path.join(vendor, "sdl3", "lib"))

    sdl_rt = glob.glob(os.path.join(tmp, "SDL3-win32-x64*", "SDL3.dll"))
    if not sdl_rt:
        sdl_rt = glob.glob(os.path.join(tmp, "SDL3-win32-x64*", "*", "SDL3.dll"))
    if sdl_rt:
        shutil.copy2(sdl_rt[0], os.path.join(vendor, "sdl3", "bin"))
    else:
        print("[WARN] 未找到 SDL3.dll 运行时，请手工放入 vendor/sdl3/bin/")

    for sub in ("include", "lib", "bin"):
        reset(os.path.join(vendor, "ffmpeg", sub))
    shutil.copytree(os.path.join(ff_root, "include"), os.path.join(vendor, "ffmpeg", "include"),
                    dirs_exist_ok=True)
    for f in glob.glob(os.path.join(ff_root, "lib", "*.dll.a")):
        shutil.copy2(f, os.path.join(vendor, "ffmpeg", "lib"))
    for f in glob.glob(os.path.join(ff_root, "bin", "*.dll")) + \
             glob.glob(os.path.join(ff_root, "bin", "*.exe")):
        shutil.copy2(f, os.path.join(vendor, "ffmpeg", "bin"))

    print("[OK] vendor/ 已就绪：")
    for sub in ("sdl3/include", "sdl3/lib", "sdl3/bin", "ffmpeg/include", "ffmpeg/lib", "ffmpeg/bin"):
        p = os.path.join(vendor, *sub.split("/"))
        n = len(os.listdir(p)) if os.path.isdir(p) else 0
        print(f"     vendor/{sub}: {n} 项")
    return 0


if __name__ == "__main__":
    sys.exit(main())
