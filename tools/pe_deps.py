#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
pe_deps.py — 纯 Python 解析 PE 导入表，枚举 DLL 传递依赖闭包。

用于核实 libass 这类"会拉一长串 MinGW 依赖"的库，在运行期到底需要哪些
DLL 一起进 dist/。不依赖 objdump / dumpbin / ldd。

用法:
    python tools/pe_deps.py <dll> [额外的搜索目录...]
"""
import os
import sys
import struct

# 常见 Windows 系统 DLL（运行目标自带，不必随 dist 发布）
SYSTEM_DLLS = {
    "kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "msvcrt.dll",
    "msvcp_win.dll", "vcruntime140.dll", "vcruntime140_1.dll", "ucrtbase.dll",
    "api-ms-win-core-*.dll", "api-ms-win-crt-*.dll", "api-ms-win-*.dll",
    "ws2_32.dll", "bcrypt.dll", "ole32.dll", "oleaut32.dll", "shell32.dll",
    "shlwapi.dll", "version.dll", "winmm.dll", "d3d11.dll", "dxgi.dll",
    "dwrite.dll", "imm32.dll", "comctl32.dll", "comdlg32.dll", "crypt32.dll",
    "rpcrt4.dll", "sechost.dll", "setupapi.dll", "gdiplus.dll", "usp10.dll",
    "msimg32.dll", "netapi32.dll", "normaliz.dll", "dwmapi.dll", "uxtheme.dll",
}


def read_pe_imports(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"MZ":
        return []
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b"PE\x00\x00":
        return []
    coff = e_lfanew + 4
    machine, num_sec = struct.unpack_from("<HH", data, coff)
    opt_off = coff + 20
    opt_magic = struct.unpack_from("<H", data, opt_off)[0]
    opt_size = struct.unpack_from("<H", data, coff + 16)[0]
    dd_off = opt_off + (112 if opt_magic == 0x20B else 96)
    imp_rva, imp_size = struct.unpack_from("<II", data, dd_off + 8)
    if imp_rva == 0:
        return []
    sec_off = opt_off + opt_size
    sections = []
    for i in range(num_sec):
        sh = sec_off + i * 40
        vsize, vaddr, rawsize, rawaddr = struct.unpack_from("<IIII", data, sh + 8)
        sections.append((vaddr, rawaddr, rawsize))

    def rva_to_off(rva):
        for vaddr, rawaddr, rawsize in sections:
            if vaddr <= rva < vaddr + rawsize:
                return rawaddr + (rva - vaddr)
        return None

    imports = []
    off = rva_to_off(imp_rva)
    i = 0
    while True:
        desc = off + i * 20
        ilt_rva, _, _, name_rva, _ = struct.unpack_from("<IIIII", data, desc)
        if ilt_rva == 0 and name_rva == 0:
            break
        no = rva_to_off(name_rva)
        if no is None:
            i += 1
            continue
        end = data.index(b"\x00", no)
        imports.append(data[no:end].decode("ascii", "replace"))
        i += 1
        if i > 2000:
            break
    return imports


def is_system(name):
    n = name.lower()
    if n in SYSTEM_DLLS:
        return True
    for pat in SYSTEM_DLLS:
        if "*" in pat and n.startswith(pat.split("*")[0]):
            return True
    return False


def closure(start, search_dirs):
    seen = set()
    missing = set()
    provided = set()  # 在 search_dirs 里找到的（需随 dist 发布）
    stack = [start]
    while stack:
        d = stack.pop()
        base = os.path.basename(d).lower()
        if base in seen:
            continue
        seen.add(base)
        if not os.path.isfile(d):
            if not is_system(base):
                missing.add(base)
            continue
        provided.add(base)
        for imp in read_pe_imports(d):
            il = imp.lower()
            if il in seen:
                continue
            found = None
            for sd in search_dirs:
                p = os.path.join(sd, imp)
                if os.path.isfile(p):
                    found = p
                    break
            stack.append(found if found else imp)
    return provided, missing


def main():
    if len(sys.argv) < 2:
        print("usage: pe_deps.py <dll> [search_dirs...]")
        return 1
    start = sys.argv[1]
    search_dirs = sys.argv[2:] + [os.path.dirname(os.path.abspath(start))]
    provided, missing = closure(start, search_dirs)
    print(f"[扫描] {start}")
    print(f"\n[需随 dist 发布 / 已在搜索目录找到] ({len(provided)}):")
    for p in sorted(provided):
        print(f"   {p}")
    print(f"\n[缺失的 MinGW 依赖] ({len(missing)}):")
    for m in sorted(missing):
        print(f"   {m}")
    if missing:
        print("\n[结论] 上述缺失 DLL 需从 MSYS2 补拉进 vendor/libass/bin，否则运行期加载失败。")
        return 2
    print("\n[结论] 依赖闭包完整，libass 可加载。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
