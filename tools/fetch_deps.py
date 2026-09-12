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
    ... --no-verify                    # 跳过 SHA-256 校验（不推荐，只在校验误报时用）
    ... --check-update                 # 只查上游有没有更新（HEAD 请求，不下载）
    ... --refresh                      # 忽略本机档案，强制重新下载（升级依赖用）

需要的材料（任选其一来源）:
    SDL3-devel-3.4.16-mingw.zip        SDL3 头文件 + libSDL3.dll.a + 导入库
    SDL3-3.4.16-win32-x64.zip          SDL3.dll 运行时
    ffmpeg-9.0.1-full_build-shared.7z  FFmpeg 9.x 共享版（含 include/ lib/*.dll.a bin/*.dll）

供应链防护（本次加固）
    1. 下载走 HTTPS + 系统证书校验（原有行为）。
    2. 每个档案在**解压前**做 SHA-256 校验：命中 DEPS_SHA256 就比对，不命中则告警并
       打印实测哈希，供你自行钉住。
       注意这里钉的是"本机曾经成功构建过的那份"（TOFU），它能检测**内容变了**，
       但**不能证明来源可信** —— 来源可信性由 HTTPS + 官方站点保证。
    3. 解压**前**先用 `7z l -slt` 列出条目，拒绝绝对路径与含 `..` 的条目
       （Zip Slip / 路径穿越）。解压后再做一次"没跑出目标目录"的围栏检查。
    4. 不再从与项目无关的第三方目录里找 7z（原先候选里有游戏修改器自带的 7z）。
"""
import os
import sys
import glob
import hashlib
import shutil
import argparse
import subprocess
import urllib.request
import ssl

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# 本机常见位置（用户把资源放在这里时可直接复用，不必联网）
DEFAULT_SRC_DIRS = [
    os.path.join(ROOT, "player"),        # 项目自带的参考/依赖压缩包（.gitignore 已排除）
    os.path.join(ROOT, "_extract"),
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

# 已知档案的 SHA-256（按小写文件名匹配）。
#
# 定位说明：这是 TOFU（首次使用即信任）式钉值 —— 取自本机已经成功构建过的那份副本。
#   命中 → 校验内容是否被改动；
#   未命中 → 告警 + 打印实测哈希（不阻断，因为 FFmpeg 那条 URL 是 gyan.dev 的
#            滚动构建，内容会随上游更新而变化，硬钉必然过期）。
DEPS_SHA256 = {
    "sdl3-devel-3.4.16-mingw.zip":
        "9828bb735cf8a007bcf0ac5aa9f01f3fcb54b7ca67c932e775c905c5d5053a60",
    "sdl3-devel-3.4.16-mingw.tar.gz":
        "c7ef65bd72eabac6e5b535411dbd8d5824d0aab24fd62ff8812666b336f18a9c",
    "sdl3-3.4.16-win32-x64.zip":
        "4217944b4e51457af4a59c82d883f8443b3e65964b2acd8943484c492756c4b6",
    "ffmpeg-shared.7z":
        "cb4d5e8db6a3353bffdb2100d3eb4b76733457fa443215e236f57c99f9ffdca4",
}

# 只从"可信赖的常规安装位置"和 PATH 里找 7z。
# 刻意不收录来路不明的第三方目录（例如某些软件自带的 7z）—— 解压器本身就是攻击面。
SEVENZ = [
    r"C:\Program Files\7-Zip\7z.exe",
    r"C:\Program Files (x86)\7-Zip\7z.exe",
    r"C:\ProgramData\chocolatey\tools\7z.exe",
    "7z", "7za",
]


def find_7z():
    for p in SEVENZ:
        if os.path.isfile(p):
            return p
    for p in SEVENZ:
        w = shutil.which(p)
        if w:
            return w
    return None


def find_local(src_dirs, keyword, prefer_name=None):
    """在候选目录里找一个文件名含 keyword 的档案。

    注意：keyword 必须足够具体。早期实现用 `key.split("-")[0]`，
    于是 "SDL3-devel" 与 "SDL3-win32-x64" 都退化成搜 "SDL3"，
    两槽位会命中同一个文件（曾导致取到错误的档案）。
    这里额外支持 prefer_name（优先精确文件名）。
    """
    for d in src_dirs:
        if not os.path.isdir(d):
            continue
        cands = []
        for pat in ("*.7z", "*.zip", "*.tar.gz"):
            for f in glob.glob(os.path.join(d, pat)):
                if keyword.lower() in os.path.basename(f).lower():
                    cands.append(f)
        if not cands:
            continue
        # 同一个目录内优先精确文件名；目录之间按 src_dirs 顺序，先命中者胜
        # （否则缓存目录里的"同名半成品"会盖过真正的本地档案）
        if prefer_name:
            for f in cands:
                if os.path.basename(f).lower() == prefer_name.lower():
                    return f
        return cands[0]
    return None


def sha256_of(path, chunk=1 << 20):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(chunk)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def verify_archive(path, label, no_verify=False):
    """解压前校验 SHA-256。返回 False 表示应当中止。"""
    if no_verify:
        print(f"    [SKIP] {label}: --no-verify，跳过哈希校验")
        return True
    name = os.path.basename(path).lower()
    got = sha256_of(path)
    want = DEPS_SHA256.get(name)
    if want is None:
        print(f"    [WARN] {label}: 未钉住 {name} 的哈希，无法校验来源")
        print(f"           实测 sha256={got}")
        print(f"           确认无误后可把它加进 tools/fetch_deps.py 的 DEPS_SHA256")
        return True
    if got == want:
        print(f"    [OK] {label}: sha256 校验通过 ({got[:16]}…)")
        return True
    print(f"    [ERR] {label}: SHA-256 不匹配，拒绝解压！")
    print(f"          期望 {want}")
    print(f"          实测 {got}")
    print(f"          文件 {path}")
    print(f"          若确认是新版本，请更新 tools/fetch_deps.py 里的 DEPS_SHA256；")
    print(f"          仅想跳过校验可加 --no-verify（不推荐）。")
    return False


def list_entries(sevenz, archive):
    """用 7z l -slt 列出**档案内条目**名（解压前检查用）。失败返回 None。

    注意 `-slt` 的输出结构：先是档案自身的元信息块（第一行 `Path = ` 是**档案路径**
    本身，不是条目），然后一行分隔线（`----------`），之后才是真正的条目。
    所以必须先跳过元信息块 —— 早期实现没跳过，会把绝对路径的档案路径当成
    "可疑条目"从而误报（反向的坑：也可能因此漏掉真条目）。
    """
    r = subprocess.run([sevenz, "l", "-slt", archive],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        return None
    out = []
    in_entries = False
    for line in r.stdout.decode("utf-8", "replace").splitlines():
        if not in_entries:
            if line.startswith("-----"):
                in_entries = True
            continue
        if line.startswith("Path = "):
            p = line[len("Path = "):].strip()
            if p:
                out.append(p)
    if not in_entries:
        return None          # 没看到分隔线说明输出格式变了：宁可失败也不放行
    return out


def unsafe_entries(entries):
    """找出绝对路径或含 .. 的条目（Zip Slip / 路径穿越）。"""
    bad = []
    for e in entries:
        p = e.replace("\\", "/")
        if p.startswith("/"):
            bad.append((e, "绝对路径"))
            continue
        if len(p) > 1 and p[1] == ":":
            bad.append((e, "盘符绝对路径"))
            continue
        if ".." in [seg for seg in p.split("/")]:
            bad.append((e, "含 .. 路径段"))
    return bad


def check_entry_safety(sevenz, archive, label):
    """解压前扫描条目。返回 False 表示应当中止。"""
    entries = list_entries(sevenz, archive)
    if entries is None:
        print(f"    [ERR] {label}: 无法列出档案条目（7z l 失败），拒绝解压")
        return False
    bad = unsafe_entries(entries)
    if bad:
        print(f"    [ERR] {label}: 档案里有 {len(bad)} 个可疑条目，拒绝解压：")
        for e, why in bad[:10]:
            print(f"          {why}: {e}")
        return False
    print(f"    [OK] {label}: {len(entries)} 个条目，路径检查通过")
    return True


def check_containment(root):
    """解压后围栏检查：所有产物的真实路径都必须落在 root 之内。"""
    root_real = os.path.realpath(root)
    bad = []
    for dirpath, dirnames, filenames in os.walk(root):
        for n in list(dirnames) + list(filenames):
            p = os.path.realpath(os.path.join(dirpath, n))
            if p != root_real and not p.startswith(root_real + os.sep):
                bad.append(p)
    return bad


def download(url, out):
    print(f"    下载 {url}")
    # 先写 .part，成功后再改名：否则半途失败会留下"够大但损坏"的缓存，
    # 下次被当成有效缓存复用（size>1024 的检查拦不住）。
    part = out + ".part"
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    got = 0
    with urllib.request.urlopen(req, context=ssl.create_default_context(), timeout=900) as r, \
            open(part, "wb") as f:
        total = int(r.headers.get("Content-Length") or 0)
        while True:
            c = r.read(1 << 20)
            if not c:
                break
            f.write(c)
            got += len(c)
    if total and got != total:
        try:
            os.remove(part)
        except OSError:
            pass
        raise RuntimeError(f"下载不完整：期望 {total} 字节，实际 {got} 字节")
    os.replace(part, out)
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


def check_update(src_dirs, cache):
    """只做 HEAD 请求，比较上游 Content-Length 与本机档案大小。
    用来回答"现在有没有值得重新抓取的更新" —— 不必真的下载 58MB。"""
    print("[check-update] 只发 HEAD 请求，不会下载或改动任何文件\n")
    any_new = False
    for key, nice, kw in (("SDL3-devel", "SDL3-devel-3.4.16-mingw.zip", "sdl3-devel"),
                          ("SDL3-win32-x64", "SDL3-3.4.16-win32-x64.zip", "sdl3-3.4.16-win32-x64"),
                          ("ffmpeg-shared", "ffmpeg-9.0.1-full_build-shared.7z", "ffmpeg")):
        url = DOWNLOADS[key]
        local = find_local(src_dirs, kw, prefer_name=nice)
        if not local:
            c = os.path.join(cache, nice)
            local = c if os.path.isfile(c) else None
        local_size = os.path.getsize(local) if local else None
        try:
            req = urllib.request.Request(url, method="HEAD",
                                         headers={"User-Agent": "Mozilla/5.0"})
            with urllib.request.urlopen(req, context=ssl.create_default_context(),
                                        timeout=30) as r:
                up_size = r.headers.get("Content-Length")
                up_lm = r.headers.get("Last-Modified")
            up_size = int(up_size) if up_size else -1
        except Exception as e:
            print(f"  [ERR] {nice}: 查询失败 {e}")
            continue
        if local_size is None:
            print(f"  [{nice}] 上游 {up_size} 字节 / Last-Modified={up_lm}")
            print(f"          本机未找到对应档案 → 需要下载")
            any_new = True
        elif local_size == up_size:
            print(f"  [无更新] {nice}: 上游与本机同为 {up_size} 字节"
                  f"（Last-Modified={up_lm}）")
            print(f"          本机对照档案: {local}")
        else:
            print(f"  [有更新] {nice}: 本机 {local_size} 字节（{local}）"
                  f" → 上游 {up_size} 字节（Last-Modified={up_lm}）")
            print(f"          抓取: python tools/fetch_deps.py --refresh")
            print(f"          抓完请把新哈希更新到本文件的 DEPS_SHA256")
            any_new = True
    print("\n[check-update] " + ("发现可用更新，见上。" if any_new else
                                 "上游与本地一致：当前没有值得重新抓取的更新。"))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", action="append", default=[])
    ap.add_argument("--vendor", default=os.path.join(ROOT, "vendor"))
    ap.add_argument("--tmp", default=os.path.join(ROOT, "_deps"))
    ap.add_argument("--no-verify", action="store_true",
                    help="跳过 SHA-256 校验与条目检查（不推荐）")
    ap.add_argument("--refresh", action="store_true",
                    help="忽略本机已有档案与缓存，强制重新下载（用于升级依赖版本）")
    ap.add_argument("--check-update", action="store_true",
                    help="只查上游是否已更新（HEAD 请求，不下载）")
    args = ap.parse_args()

    if args.check_update:
        return check_update(args.src + DEFAULT_SRC_DIRS, args.tmp)

    src_dirs = args.src + DEFAULT_SRC_DIRS
    sevenz = find_7z()
    if not sevenz:
        print("[ERR] 找不到 7z 解压工具。请安装 7-Zip（https://www.7-zip.org/）"
              "或用 --src 指向已解压目录")
        return 1
    try:
        ver = subprocess.run([sevenz], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT).stdout
        first = ver.decode("utf-8", "replace").strip().splitlines()[0]
    except Exception:
        first = "(版本未知)"
    print(f"[0/3] 解压工具: {sevenz}  —— {first}")

    cache = args.tmp
    if not os.path.isdir(cache):
        os.makedirs(cache, exist_ok=True)

    plan = [
        # key, 下载保存名, 本地匹配关键字
        ("SDL3-devel", "SDL3-devel-3.4.16-mingw.zip", "sdl3-devel"),
        ("SDL3-win32-x64", "SDL3-3.4.16-win32-x64.zip", "sdl3-3.4.16-win32-x64"),
        ("ffmpeg-shared", "ffmpeg-9.0.1-full_build-shared.7z", "ffmpeg"),
    ]

    archives = {}
    for key, nice, kw in plan:
        local = None if args.refresh else find_local(src_dirs, kw, prefer_name=nice)
        if local:
            print(f"[1/3] {nice}: 使用本机档案 {local}")
            archives[key] = local
        else:
            dst = os.path.join(cache, nice)
            cached_ok = (not args.refresh) and os.path.isfile(dst) and os.path.getsize(dst) > 1024
            if cached_ok:
                print(f"[1/3] {nice}: 使用已下载缓存 {dst}")
                archives[key] = dst
            else:
                if args.refresh:
                    print(f"[1/3] {nice}: --refresh，忽略本机档案与缓存，重新下载")
                else:
                    print(f"[1/3] {nice}: 本机未找到，尝试下载")
                try:
                    download(DOWNLOADS[key], dst)
                    archives[key] = dst
                except Exception as e:
                    print(f"[ERR] 下载失败: {e}")
                    print("      请手工把该档案放到 _deps/ 或 --src 指定目录后重试")
                    return 1

    print("\n[1.5/3] 校验档案（SHA-256 + 条目路径检查）…")
    for key, path in archives.items():
        label = os.path.basename(path)
        if not verify_archive(path, label, args.no_verify):
            return 1
        if not args.no_verify and not check_entry_safety(sevenz, path, label):
            return 1

    vendor = args.vendor
    tmp = os.path.join(cache, "extract")
    os.makedirs(tmp, exist_ok=True)

    print("\n[2/3] 解压…")
    for key, path in archives.items():
        d = os.path.join(tmp, key)
        if not os.path.isdir(d):
            if not extract(sevenz, path, tmp):
                print(f"[ERR] 解压失败: {path}")
                return 1

    escaped = check_containment(tmp)
    if escaped:
        print(f"[ERR] 解压产物有 {len(escaped)} 项跑出了目标目录，已中止：")
        for p in escaped[:10]:
            print(f"      {p}")
        return 1
    print(f"    [OK] 围栏检查通过：所有解压产物都在 {tmp} 之内")

    print("\n[3/3] 组装 vendor/ …")
    sdl_root = None
    for pat in (os.path.join(tmp, "SDL3-devel*", "SDL3-*", "x86_64-w64-mingw32"),
                os.path.join(tmp, "SDL3-devel*", "x86_64-w64-mingw32"),
                os.path.join(tmp, "*", "x86_64-w64-mingw32")):
        hits = glob.glob(pat)
        if hits:
            sdl_root = hits[0]
            break
    if not sdl_root:
        print("[ERR] 找不到 SDL3 的 x86_64-w64-mingw32 目录")
        return 1

    ff_root = None
    for pat in (os.path.join(tmp, "ffmpeg-shared", "ffmpeg-*-full_build-shared"),
                os.path.join(tmp, "ffmpeg-*-full_build-shared"),
                os.path.join(tmp, "*", "ffmpeg-*-full_build-shared")):
        hits = glob.glob(pat)
        if hits:
            ff_root = hits[0]
            break
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

    print("\n[OK] vendor/ 已就绪：")
    for sub in ("sdl3/include", "sdl3/lib", "sdl3/bin",
                "ffmpeg/include", "ffmpeg/lib", "ffmpeg/bin"):
        p = os.path.join(vendor, *sub.split("/"))
        n = len(os.listdir(p)) if os.path.isdir(p) else 0
        print(f"     vendor/{sub}: {n} 项")
    return 0


if __name__ == "__main__":
    sys.exit(main())
