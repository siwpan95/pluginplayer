#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
fetch_libass.py — 准备 vendor/libass/ 依赖（libass + 完整传递依赖闭包）

PluginPlayer 的 libass 字幕渲染（P1-e）需要 libass，而 libass 在 MinGW 上会拉一长串
依赖（freetype / fribidi / harfbuzz / brotli / bzip2 / fontconfig / glib2 / graphite2 /
iconv / libpng / unibreak / zlib …）。本脚本做**依赖闭包固定点抓取**：

  1. 用 mingw-w64-x86_64.db 解析基础四个包（libass/freetype/fribidi/harfbuzz）的真实文件名。
  2. 用 mingw-w64-x86_64.files 建立「DLL 基名 -> 所属 MSYS2 包」映射。
  3. 从 libass-9.dll 出发，用 pe_deps 反复扫描 PE 导入闭包；
     每轮把缺失的 MinGW DLL 映射到包、下载解包、把 bin/*.dll 并进 vendor/libass/bin；
     直到闭包闭合。**MinGW 运行时三件套（libgcc_s_seh-1 / libstdc++-6 / libwinpthread-1）
     默认也由本脚本从上游抓**（gcc-libs 提供前两个、libwinpthread 提供第三个），
     这样 vendor/libass/bin 在**尚未构建**（还没有 dist/）时也是完整可加载集（build.py 亦如此声明）。
     `--skip-runtime` 可退回旧行为（三件套留给 `tools/fetch_deps.py --libass-only` 从本机
     来源 PP_LIBASS_RUNTIME_SRC → PP_GXX 目录 → F:/mingw64/mingw64/bin → 本仓 dist/ 补齐，
     离线可用、幂等）。

全程纯 Python（zstandard + tarfile），不依赖 7z / tar / pacman。供应链防护同 fetch_deps：
HTTPS + 证书校验、围栏检查（无绝对路径 / 无 ..）、基础四包与运行时包 SHA-256 钉死、
其余包 TOFU（首次记录实测哈希后强制比对）。

用法:
    python tools/fetch_libass.py
    python tools/fetch_libass.py --skip-runtime   # 三件套交由 fetch_deps 补
"""
import os
import sys
import io
import ssl
import hashlib
import argparse
import urllib.request
import tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import pe_deps  # 复用 PE 导入闭包扫描

REPO_DB_URL = "https://repo.msys2.org/mingw/x86_64/mingw64.db.tar.zst"
REPO_FILES_URL = "https://repo.msys2.org/mingw/x86_64/mingw64.files.tar.zst"
PKG_BASE = "https://repo.msys2.org/mingw/x86_64/"

# 基础四包（其余依赖由闭包自动推导）
BASE_PACKAGES = [
    "mingw-w64-x86_64-libass",
    "mingw-w64-x86_64-freetype",
    "mingw-w64-x86_64-fribidi",
    "mingw-w64-x86_64-harfbuzz",
]

# MinGW 运行时三件套：**默认随闭包一起从上游抓**（2026-09-23 调整，原先刻意不抓）。
#   libgcc_s_seh-1.dll  ← mingw-w64-x86_64-gcc-libs
#   libstdc++-6.dll     ← mingw-w64-x86_64-gcc-libs
#   libwinpthread-1.dll ← mingw-w64-x86_64-libwinpthread
# 传 --skip-runtime 则退回旧行为：本脚本不抓，交给 tools/fetch_deps.py --libass-only
# 从本机来源补齐（见 README 快速开始 1b) 与 build.py 的「bin/ 下即完整可加载集」声明）。
RUNTIME_DLLS = {
    "libgcc_s_seh-1.dll", "libwinpthread-1.dll", "libstdc++-6.dll",
}

CTX = ssl.create_default_context()

# 基础四包 + 运行时两包（gcc-libs / libwinpthread）的 SHA-256 钉死。
# 其余传递依赖走 TOFU（sha256_tofu.json，首次记录后强制比对）。
DOWNLOADS_SHA256 = {
    "mingw-w64-x86_64-libass-0.17.5-1-any.pkg.tar.zst":
        "11966a8df12b99454ac552c587f58c4ea81bb040bad1cfdd219bf2f50f567f8d",
    "mingw-w64-x86_64-freetype-2.14.3-1-any.pkg.tar.zst":
        "b3e310f457c90348fd14b8c1085e9c036016d168812a657fec21f496f7e3de99",
    "mingw-w64-x86_64-fribidi-1.0.16-1-any.pkg.tar.zst":
        "82d4f9e431082d2ac2fa7b9eddd73aa0c073bf8ae66b7d137195797ec543dffa",
    "mingw-w64-x86_64-harfbuzz-14.4.0-1-any.pkg.tar.zst":
        "7343f6aae536e8b0604395a26ecb2666b7bab4936163bdec61e38139169afea0",
    # MinGW 运行时（2026-09-23 实测钉死；版本号一变即文件名变，钉死项自动失效并回落到 TOFU）
    "mingw-w64-x86_64-gcc-libs-16.2.0-3-any.pkg.tar.zst":          # -> libgcc_s_seh-1 / libstdc++-6
        "f8e25ea67bb796e7f65550f0dca9fce4cdde8aaa3dadafe4d13c6a8233c8de26",
    "mingw-w64-x86_64-libwinpthread-14.0.0.r375.g9c1abbbf5-1-any.pkg.tar.zst":  # -> libwinpthread-1
        "1d0ed7fda332a5c879de210f3d72c75fa1f3df7cc63dd951e763dec8eb02c1ee",
}


def download(url, out):
    print(f"    下载 {url}")
    part = out + ".part"
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    got = 0
    with urllib.request.urlopen(req, context=CTX, timeout=900) as r, open(part, "wb") as f:
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
    print(f"    完成 {out} ({got} 字节)")
    return got


def decompress_zst(path):
    import zstandard
    with open(path, "rb") as f:
        return zstandard.ZstdDecompressor().stream_reader(f).read()


def parse_db_names(db_bytes):
    names = {}
    with tarfile.open(fileobj=io.BytesIO(db_bytes), mode="r:") as tf:
        for m in tf.getmembers():
            if not m.name.endswith("/desc"):
                continue
            pkg = m.name.split("/")[0]
            info = {}
            cur = None
            for line in tf.extractfile(m).read().decode("utf-8", "replace").splitlines():
                if line.startswith("%") and line.endswith("%"):
                    cur = line[1:-1]
                    continue
                if cur:
                    info.setdefault(cur, line)
                    cur = None
            if "NAME" in info and "FILENAME" in info:
                names[info["NAME"]] = info["FILENAME"]
    return names


def parse_files_map(files_bytes, stem_to_base):
    """files DB：每个包 <versioned-dir>/files 列出其全部文件。

    注意：files DB 的目录名是带版本的（如 mingw-w64-x86_64-brotli-1.2.0-1），
    而 db 的 NAME 字段是不带版本的基名（mingw-w64-x86_64-brotli）。
    用 stem_to_base 把版本化目录名归一到基名，再建 basename(dll)->基名pkg 映射。
    """
    mp = {}
    with tarfile.open(fileobj=io.BytesIO(files_bytes), mode="r:") as tf:
        for m in tf.getmembers():
            if not m.name.endswith("/files"):
                continue
            pkgdir = m.name.split("/")[0]
            base = stem_to_base.get(pkgdir)
            if not base:
                continue
            for line in tf.extractfile(m).read().decode("utf-8", "replace").splitlines():
                line = line.strip().replace("\\", "/")
                if line.lower().endswith(".dll") and "/bin/" in line:
                    mp[os.path.basename(line).lower()] = base
    return mp


def safe_extract_bin(pkg_bytes, dest):
    """解包单个 .pkg.tar.zst 到 dest，仅接受 mingw64/ 前缀，返回解出的 bin/*.dll 列表。"""
    bad = []
    members = []
    with tarfile.open(fileobj=io.BytesIO(pkg_bytes), mode="r:") as tf:
        for m in tf.getmembers():
            n = m.name.replace("\\", "/")
            if n.startswith("/") or "/../" in n or n.startswith(".."):
                bad.append(n)
                continue
            if n.startswith("mingw64/"):
                members.append(m)
    if bad:
        raise RuntimeError(f"包内有 {len(bad)} 个可疑条目，拒绝解包：{bad[:5]}")
    dlls = []
    with tarfile.open(fileobj=io.BytesIO(pkg_bytes), mode="r:") as tf:
        for m in members:
            tf.extract(m, dest, set_attrs=False)
            if m.name.replace("\\", "/").startswith("mingw64/bin/") and m.name.lower().endswith(".dll"):
                dlls.append(os.path.basename(m.name))
    return dlls


def sha256_of(path, chunk=1 << 20):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(chunk)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vendor", default=os.path.join(ROOT, "vendor", "libass"))
    ap.add_argument("--tmp", default=os.path.join(ROOT, "_deps", "libass"))
    ap.add_argument("--skip-runtime", action="store_true",
                    help="不抓 MinGW 运行时三件套（默认抓；旧行为=由 fetch_deps --libass-only 从本机补）")
    args = ap.parse_args()
    os.makedirs(args.tmp, exist_ok=True)
    out = args.vendor

    print("[1/5] 拉取 DB + files 索引…")
    db_path = os.path.join(args.tmp, "mingw64.db.tar.zst")
    if not (os.path.isfile(db_path) and os.path.getsize(db_path) > 1024):
        download(REPO_DB_URL, db_path)
    names = parse_db_names(decompress_zst(db_path))

    # 基名 -> 版本化目录名(文件名 stem) 的反查表，供 files 映射归一到基名
    # 注意 db 的 FILENAME 带架构后缀（如 ...-1-any.pkg.tar.zst），
    # 而 files DB 的目录名不带 -any（...-1）。两者都要登记。
    stem_to_base = {}
    for base, fn in names.items():
        stem = fn[:-len(".pkg.tar.zst")] if fn.endswith(".pkg.tar.zst") else fn
        stem_to_base[stem] = base
        if stem.endswith("-any"):
            stem_to_base[stem[:-4]] = base

    files_path = os.path.join(args.tmp, "mingw64.files.tar.zst")
    if not (os.path.isfile(files_path) and os.path.getsize(files_path) > 1024):
        download(REPO_FILES_URL, files_path)
    files_map = parse_files_map(decompress_zst(files_path), stem_to_base)
    print(f"      DB {len(names)} 包；files 映射 {len(files_map)} 个 bin DLL")

    print("\n[2/5] 抓取基础四包…")
    for pkg in BASE_PACKAGES:
        fn = names.get(pkg)
        if not fn:
            print(f"[ERR] 仓库找不到 {pkg}")
            return 1
        print(f"      {pkg} -> {fn}")
        arch = os.path.join(args.tmp, fn)
        if not (os.path.isfile(arch) and os.path.getsize(arch) > 1024):
            download(PKG_BASE + fn, arch)
        h = sha256_of(arch)
        want = DOWNLOADS_SHA256.get(fn)
        if want is None:
            print(f"      [WARN] {fn} 未钉哈希，实测 {h}")
        elif h != want:
            print(f"      [ERR] {fn} SHA-256 不匹配（期望 {want[:16]} 实测 {h[:16]}）")
            return 1
        else:
            print(f"      [OK] {fn} sha256 通过 ({h[:16]}…)")
        safe_extract_bin(decompress_zst(arch), args.tmp)

    print("\n[3/5] 依赖闭包固定点抓取…")
    fetched = set(BASE_PACKAGES)
    round_n = 0
    # 传递依赖的 SHA-256 TOFU 缓存（首次记录，后续强制比对，防供应链替换）
    tofu_path = os.path.join(args.tmp, "sha256_tofu.json")
    tofu = {}
    if os.path.isfile(tofu_path):
        try:
            with open(tofu_path, "r", encoding="utf-8") as f:
                tofu = dict(__import__("json").load(f))
        except Exception:
            tofu = {}
    tofu_changed = False
    while True:
        round_n += 1
        libass_dll = os.path.join(out, "bin", "libass-9.dll")
        # 先把已解包的 bin/*.dll 同步到 vendor/libass/bin
        _sync_bin(args.tmp, out)
        if not os.path.isfile(libass_dll):
            # 首次还没装 libass-9.dll 到 vendor；从临时解包拷过来
            _sync_bin(args.tmp, out)
        provided, missing = pe_deps.closure(libass_dll, [out_bin(out)])
        need = []
        for d in missing:
            if d in RUNTIME_DLLS and args.skip_runtime:
                print(f"      [跳过运行时库] {d}（--skip-runtime；要 vendor 自包含请跑 python tools/fetch_deps.py --libass-only）")
                continue
            pkg = files_map.get(d)
            if not pkg:
                print(f"      [WARN] 无法把 {d} 映射到 MSYS2 包")
                continue
            if pkg not in fetched:
                need.append(pkg)
        if not need:
            print(f"      [第 {round_n} 轮] 闭包已闭合，无新包需拉")
            break
        print(f"      [第 {round_n} 轮] 需补 {len(need)} 个包: {sorted(need)}")
        for pkg in sorted(set(need)):
            fn = names.get(pkg)
            if not fn:
                print(f"      [ERR] 仓库找不到 {pkg}")
                return 1
            arch = os.path.join(args.tmp, fn)
            if not (os.path.isfile(arch) and os.path.getsize(arch) > 1024):
                download(PKG_BASE + fn, arch)
            h = sha256_of(arch)
            want = DOWNLOADS_SHA256.get(fn)
            if want is not None and h != want:
                print(f"      [ERR] {fn} SHA-256 不匹配（钉死 {want[:16]} 实测 {h[:16]}）")
                return 1
            if fn in tofu:
                if h != tofu[fn]:
                    print(f"      [ERR] {fn} TOFU 失败：首次 {tofu[fn][:16]} 实测 {h[:16]}")
                    return 1
                print(f"      [OK] {fn} TOFU 一致 ({h[:16]}…)")
            else:
                tofu[fn] = h
                tofu_changed = True
                print(f"      [TOFU] {fn} 首次记录 sha256={h[:16]}…")
            safe_extract_bin(decompress_zst(arch), args.tmp)
            fetched.add(pkg)
    if tofu_changed:
        with open(tofu_path, "w", encoding="utf-8") as f:
            __import__("json").dump(tofu, f, indent=2, sort_keys=True)
        print(f"      [TOFU] 已写入 {tofu_path}")

    print("\n[4/5] 最终闭包校验…")
    _sync_bin(args.tmp, out)
    provided, missing = pe_deps.closure(libass_dll, [out_bin(out)])
    if args.skip_runtime:
        skipped_runtime = sorted(m for m in missing if m in RUNTIME_DLLS)
        missing = [m for m in missing if m not in RUNTIME_DLLS]
    else:
        skipped_runtime = []
    if missing:
        print(f"[ERR] 仍有缺失: {missing}")
        return 1
    print(f"[OK] libass-9.dll 依赖闭包完整（需随 dist 发布 {len(provided)} 个 DLL）")
    if skipped_runtime:
        print(f"      [注意] --skip-runtime：{len(skipped_runtime)} 个运行时库仍需由 dist/ 或 "
              f"fetch_deps.py --libass-only 提供: {skipped_runtime}")
    for p in sorted(provided):
        print(f"      {p}")

    print("\n[5/5] vendor/libass/bin 汇总…")
    bin_dir = out_bin(out)
    dlls = sorted(f for f in os.listdir(bin_dir) if f.lower().endswith(".dll"))
    print(f"      bin/ 共 {len(dlls)} 个 DLL：")
    for d in dlls:
        print(f"        {d}")
    return 0


def out_bin(out):
    return os.path.join(out, "bin")


def _sync_bin(tmp, out):
    """把 tmp 下所有包的 mingw64/bin/*.dll 合并进 vendor/libass/bin。"""
    import shutil
    dest = out_bin(out)
    os.makedirs(dest, exist_ok=True)
    for root, _dirs, files in os.walk(os.path.join(tmp, "mingw64", "bin")):
        for fn in files:
            if fn.lower().endswith(".dll"):
                sp = os.path.join(root, fn)
                shutil.copy2(sp, os.path.join(dest, fn))
    # 清掉可能混进来的非运行时文件（如 fribidi.exe）
    for fn in os.listdir(dest):
        if not fn.lower().endswith(".dll"):
            try:
                os.remove(os.path.join(dest, fn))
            except OSError:
                pass


if __name__ == "__main__":
    sys.exit(main())
