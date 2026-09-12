#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ci.py — CI 编排：本地与云端共用同一个入口
==========================================

【为什么要统一入口】
本项目的构建链是 Windows 独占的（MinGW + SDL3/FFmpeg 的 Windows 二进制 + Winsock2），
而托管 CI 的免费构建环境基本都是 Linux。与其写一份"在云端跑不起来"的流水线，
不如把所有检查拆成**阶段**，每个阶段自己判断前提是否满足：

    前提满足 → 跑，给出 [PASS]/[FAIL]
    前提不满足 → 记 [SKIP] 并说明为什么（**跳过不算通过**，与 --selftest 口径一致）

这样同一份编排既能在这台机器上跑全量，也能在 Gitee Go 的 Ubuntu 环境上跑可移植子集，
而且"哪个阶段在哪个环境跑不了"是显式可见的，不会被蒙在"CI 绿了"里。

【阶段划分与可移植性】
    阶段        跨平台？  前提
    gates        是      无（只读仓库，<1 秒）
    py-unit      是      Python 标准库
    cpp-unit     是      g++ / MinGW（只编不依赖 SDL/FFmpeg 的四个模块）
    smoke        否      Windows + vendor/（SDL3 + FFmpeg）
    build        否      Windows + vendor/
    selftest     否      Windows + dist/
    cli          否(t可部分) Windows + dist/（控制通道用例还需运行中的实例）
    release      否      Windows + vendor/

【用法】
    python tools/ci.py                      # 本地：能跑的都跑
    python tools/ci.py --profile ci         # 云端：只跑可移植阶段
    python tools/ci.py --only gates,cpp-unit
    python tools/ci.py --list
    python tools/ci.py --json

退出码: 0 全通过（SKIP 不算失败）；1 有 FAIL；2 用法错误
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, ROOT)

DIST = os.path.join(ROOT, "dist")
VENDOR = os.path.join(ROOT, "vendor")
EXE = os.path.join(DIST, "pluginplayer.exe")


def load_build():
    """import 根目录的 build.py —— 复用它的源码清单与编译器路径，避免两处维护。"""
    try:
        import build  # noqa: PLC0415
        return build
    except ImportError as exc:
        raise SystemExit("[ERR] 无法 import build.py: %s" % exc)


BUILD = load_build()


# ============================================================================
# 前提判定
# ============================================================================

def on_windows():
    return os.name == "nt"


def have_vendor():
    return (os.path.isdir(os.path.join(VENDOR, "sdl3", "include")) and
            os.path.isdir(os.path.join(VENDOR, "ffmpeg", "include")) and
            os.path.isdir(os.path.join(VENDOR, "ffmpeg", "lib")))


def have_dist():
    return os.path.isfile(EXE)


def compiler():
    """优先用项目约定的 MinGW g++；非 Windows 上退回 PATH 里的 g++。"""
    if on_windows():
        gxx = os.environ.get("PP_GXX", BUILD.GXX)
        if os.path.isfile(gxx):
            return gxx, True
        return gxx, False
    cxx = os.environ.get("CXX", "g++")
    found = shutil.which(cxx)
    return (found or cxx), bool(found)


def run_cmd(argv, cwd=ROOT, env=None, timeout=None):
    """跑一个子进程，返回 (rc, 合并后的输出)。"""
    p = subprocess.run(argv, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       env=env, timeout=timeout)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def tail(text, n=25):
    lines = [l for l in (text or "").splitlines() if l.strip()]
    return "\n".join(lines[-n:])


# ============================================================================
# 阶段实现
# ============================================================================

def stage_gates():
    rc, out = run_cmd([sys.executable, os.path.join(HERE, "verify.py")])
    keep = [l for l in out.splitlines() if l.startswith(("[PASS]", "[FAIL]", "===="))]
    detail = "\n".join(keep[-20:])
    return rc == 0, detail or tail(out)


def stage_py_unit():
    rc, out = run_cmd([sys.executable, "-X", "utf8", "-m", "unittest", "discover",
                       "-v", "-s", "tests/python", "-t", "tests/python"])
    if rc == 0:
        # 用 unittest 自己的汇总行，不要自己数 "… ok" 的行数：
        # 有输出多行的用例时行计数会偏少（第一版就少报了 18 项）。
        import re as _re
        m = _re.search(r"^Ran (\d+) tests?", out, _re.M)
        n = m.group(1) if m else "?"
        return True, "%s 个 Python 单元测试通过（unittest 汇总行）" % n
    return False, tail(out, 40)


def stage_cpp_unit():
    cc, ok = compiler()
    if not ok:
        return None, "找不到 C++ 编译器（%s）" % cc
    os.makedirs(BUILD.BUILD, exist_ok=True)
    out_exe = os.path.join(BUILD.BUILD, "pp_unit.exe" if on_windows() else "pp_unit")
    incs = ["-Isrc/sdk", "-Isrc/core", "-Itests/unit"]
    # 刻意不依赖 vendor/：单元测试只覆盖不碰 SDL/FFmpeg 的四个模块，
    # 所以"还没下依赖"的机器（以及云端 CI）也该能跑。
    if os.path.isdir(os.path.join(VENDOR, "sdl3", "include")):
        incs.append("-Ivendor/sdl3/include")
    if os.path.isdir(os.path.join(VENDOR, "ffmpeg", "include")):
        incs.append("-Ivendor/ffmpeg/include")

    srcs = [os.path.join(ROOT, s)
            for s in BUILD.UNIT_TEST_SOURCES + BUILD.UNIT_CORE_SOURCES]
    missing = [s for s in srcs if not os.path.isfile(s)]
    if missing:
        return False, "源文件缺失: %s" % missing

    if on_windows():
        link = BUILD.LDFLAGS_UNIT
    else:
        link = []          # Linux 上不需要 bcrypt（sha256 走非 Windows 分支返回 false）
    cmd = [cc, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Wno-unused-parameter",
           "-DNDEBUG"] + incs + srcs + ["-o", out_exe] + link
    rc, out = run_cmd(cmd)
    if rc != 0:
        return False, "编译失败:\n" + tail(out, 30)

    # 运行时把编译器的 bin 加进 PATH（MinGW 运行时 DLL 在那里；实测缺它会 0xC0000135）
    env = dict(os.environ)
    extra = [os.path.dirname(cc), DIST, os.path.join(VENDOR, "sdl3", "bin"),
             os.path.join(VENDOR, "ffmpeg", "bin")]
    parts = [p for p in extra if p and os.path.isdir(p)]
    if parts:
        env["PATH"] = os.pathsep.join(parts + [env.get("PATH", "")])
    rc2, out2 = run_cmd([out_exe], env=env)
    keep = [l for l in out2.splitlines()
            if l.startswith(("[PASS]", "[FAIL]", "====")) or l.strip().startswith("[x]")]
    detail = "\n".join(keep) if rc2 == 0 else "\n".join(keep[-25:] or tail(out2).splitlines()[-25:])
    return rc2 == 0, detail


def stage_smoke():
    rc, out = run_cmd([sys.executable, os.path.join(ROOT, "build.py"), "--smoke"])
    keep = [l for l in out.splitlines() if "[PASS]" in l or "[FAIL]" in l]
    return rc == 0, "\n".join(keep) or tail(out)


def stage_build():
    rc, out = run_cmd([sys.executable, os.path.join(ROOT, "build.py")])
    keep = [l for l in out.splitlines() if l.startswith(("[OK]", "[FAIL]", "[PASS]"))]
    return rc == 0, "\n".join(keep[-12:]) or tail(out)


def stage_selftest():
    rc, out = run_cmd([sys.executable, os.path.join(HERE, "pp_cli.py"), "selftest",
                       "--no-rpc", "--out", "selftest_out_ci"])
    keep = [l for l in out.splitlines()
            if "[PASS]" in l or "[FAIL]" in l or "自检结束" in l]
    return rc == 0, "\n".join(keep[-14:]) or tail(out)


def stage_color():
    """色彩还原回归：真解一帧、读像素，比对期望 RGB。"""
    rc, out = run_cmd([sys.executable, "-X", "utf8",
                       os.path.join(ROOT, "tests", "color_check.py")])
    keep = [l for l in out.splitlines()
            if l.startswith(("[PASS]", "[FAIL]", "[SKIP]", "====")) or l.startswith("        ")]
    return rc == 0, "\n".join(keep[-25:]) or tail(out)


def stage_cli():
    rc, out = run_cmd([sys.executable, "-X", "utf8",
                       os.path.join(ROOT, "tests", "cli_check.py")],
                      env=dict(os.environ, PP_CLI_NO_INSTALL="1"))
    keep = [l for l in out.splitlines()
            if l.startswith(("[PASS]", "[FAIL]", "[SKIP]", "===="))]
    return rc == 0, "\n".join(keep[-25:]) or tail(out)


def stage_release():
    rc, out = run_cmd([sys.executable, os.path.join(ROOT, "build.py"), "--release"])
    keep = [l for l in out.splitlines()
            if l.startswith(("[release]", "[FAIL]", "[PASS]"))]
    return rc == 0, "\n".join(keep) or tail(out)


# name, 说明, 可移植, 前提检查(返回 None 或"缺失原因"), 函数
STAGES = [
    ("gates", "机械门禁（tools/verify.py）", True, None, stage_gates),
    ("py-unit", "Python 单元测试（unittest）", True, None, stage_py_unit),
    ("cpp-unit", "C++ 单元测试（tests/unit/）", True, None, stage_cpp_unit),
    ("smoke", "工具链冒烟（SDL3 + FFmpeg 能否链接解码）", False,
     lambda: None if (on_windows() and have_vendor()) else
     ("需要 Windows + vendor/" + ("" if on_windows() else "（当前不是 Windows）")), stage_smoke),
    ("build", "构建内核与插件（build.py）", False,
     lambda: None if (on_windows() and have_vendor()) else "需要 Windows + vendor/", stage_build),
    ("selftest", "全链路自检（--selftest）", False,
     lambda: None if (on_windows() and have_dist()) else "需要 Windows + 已构建的 dist/", stage_selftest),
    ("color", "色彩还原回归（tests/color_check.py）", False,
     lambda: None if (on_windows() and have_vendor() and have_dist()) else
     "需要 Windows + vendor/（随包 ffmpeg）+ 已构建的 dist/", stage_color),
    ("cli", "CLI 回归（tests/cli_check.py）", False,
     lambda: None if (on_windows() and have_dist()) else "需要 Windows + 已构建的 dist/", stage_cli),
    ("release", "发布构建（--release）", False,
     lambda: None if (on_windows() and have_vendor()) else "需要 Windows + vendor/", stage_release),
]

# --profile ci 只跑这些（托管 CI 是 Linux，Windows 独占阶段跑不了）
CI_PORTABLE = {"gates", "py-unit", "cpp-unit"}


# ============================================================================
# 运行
# ============================================================================

def main():
    ap = argparse.ArgumentParser(description="PluginPlayer CI 编排")
    ap.add_argument("--profile", default="local", choices=("local", "ci"),
                    help="local=能跑的都跑；ci=只跑可移植阶段（托管 CI 用）")
    ap.add_argument("--only", default="", help="逗号分隔，只跑指定阶段")
    ap.add_argument("--list", action="store_true", help="列出阶段")
    ap.add_argument("--json", action="store_true", help="机器可读")
    args = ap.parse_args()

    if args.list:
        for name, desc, portable, _need, _fn in STAGES:
            print("%-10s %-4s %s" % (name, "跨平台" if portable else "Windows", desc))
        return 0

    only = {s.strip() for s in args.only.split(",") if s.strip()}
    unknown = only - {s[0] for s in STAGES}
    if unknown:
        print("[ERR] 未知阶段: %s（用 --list 查看）" % ", ".join(sorted(unknown)), file=sys.stderr)
        return 2

    selected = []
    for name, desc, portable, need, fn in STAGES:
        if only:
            if name in only:
                selected.append((name, desc, need, fn))
        elif args.profile == "ci":
            if name in CI_PORTABLE:
                selected.append((name, desc, need, fn))
        else:
            selected.append((name, desc, need, fn))

    results = []
    t0 = time.time()
    for name, desc, need, fn in selected:
        reason = need() if need else None
        if reason:
            results.append({"stage": name, "status": "SKIP", "detail": reason, "desc": desc})
            if not args.json:
                print("[SKIP] %-10s %s" % (name, reason))
            continue
        st = time.time()
        try:
            ok, detail = fn()
        except Exception as exc:                      # noqa: BLE001 - 阶段异常也算失败
            ok, detail = False, "阶段异常 %s: %s" % (type(exc).__name__, exc)
        if ok is None:
            status = "SKIP"
        else:
            status = "PASS" if ok else "FAIL"
        results.append({"stage": name, "status": status, "detail": detail, "desc": desc,
                        "seconds": round(time.time() - st, 1)})
        if not args.json:
            print("[%s] %-10s (%.1fs) %s" % (status, name, time.time() - st, desc))
            for line in (detail or "").splitlines():
                print("           " + line)

    npass = sum(1 for r in results if r["status"] == "PASS")
    nfail = sum(1 for r in results if r["status"] == "FAIL")
    nskip = sum(1 for r in results if r["status"] == "SKIP")

    if args.json:
        print(json.dumps({"ok": nfail == 0, "pass": npass, "fail": nfail, "skip": nskip,
                          "profile": args.profile, "stages": results,
                          "seconds": round(time.time() - t0, 1)},
                         ensure_ascii=False, indent=2))
    else:
        print()
        print("==== CI: %d/%d 阶段通过，跳过 %d 项，失败 %d 项，用时 %.1fs ====" %
              (npass, npass + nfail, nskip, nfail, time.time() - t0))
        if nskip:
            print("（跳过项的前提不满足：本机不是 Windows，或还没跑 fetch_deps.py / 构建）")
    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main())
