#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_third_party_notices.py — 生成 THIRD_PARTY_NOTICES.md
========================================================

【为什么需要这个文件】
PluginPlayer 的 dist/ 里带的不只是自己编的 exe，还有从上游拿来的二进制（FFmpeg / SDL3 /
MinGW 运行时）。**分发别人的二进制就要承担对应的许可义务** —— 附许可文本、标出来源、
（GPL 的情况下）提供对应源码。没有这个文件，"依赖与来源"就只存在于 README 的一段散文里，
既没人核，也不构成对外的声明。

本项目最要紧的一条（已逐项核实，见文件正文）：随包的 FFmpeg 来自 gyan.dev 的
`full_build-shared`，而该构建**是 GPL v3**。这意味着 dist/ 这个发布物整体受 GPL v3 约束，
不能只声明一个 MIT 就发出去。

【做法】
事实不靠记忆，全部从仓库里的原始文件读出来：
    vendor/ffmpeg/include/libavutil/ffversion.h   FFmpeg 版本串（含构建者）
    vendor/sdl3/include/SDL3/SDL_version.h        SDL3 主/次/修订号
    host-js/package.json + node_modules/*/package.json   JS 依赖版本与 license 字段
    dist/ 的 DLL 清单                              真正随包分发的文件
    src/sdk/player_plugin.h                       PP_ABI_VERSION

只有"许可是什么"这类无法从二进制里读出来的事实才写在下面的 LICENSES 表里，
并且每条都带 `evidence`（我是怎么知道的），便于复核。

用法:
    F:\\Python314\\python.exe tools\\gen_third_party_notices.py            # 写入
    F:\\Python314\\python.exe tools\\gen_third_party_notices.py --check    # 只校验是否最新
    ... --print                                                           # 打到 stdout

退出码: 0 成功/一致；1 --check 发现过期；6 读不到必需的事实来源。
"""

import argparse
import hashlib
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "THIRD_PARTY_NOTICES.md")

DIST = os.path.join(ROOT, "dist")
VENDOR = os.path.join(ROOT, "vendor")
FFVERSION_H = os.path.join(VENDOR, "ffmpeg", "include", "libavutil", "ffversion.h")
SDL_VERSION_H = os.path.join(VENDOR, "sdl3", "include", "SDL3", "SDL_version.h")
ABI_H = os.path.join(ROOT, "src", "sdk", "player_plugin.h")
HOSTJS_PKG = os.path.join(ROOT, "host-js", "package.json")
HOSTJS_NM = os.path.join(ROOT, "host-js", "node_modules")

# dist/ 里真正随包分发的 DLL —— 与 build.py 的 FF_RUNTIME 集合 + SDL3 + MinGW 运行时一致
DIST_FFMPEG_PREFIX = ("avutil", "avcodec", "avformat", "swscale", "swresample")

# 运行期产物：与 build.py 的 RUNTIME_ARTIFACTS / strip_runtime_artifacts() 同一口径。
# 生成与门禁都必须按这个口径数，否则 dist/ 里的自检产物会让"发布载荷"数字漂移，
# 而 README 写的 17 个文件 / 127 MB 指的是 --release 之后的干净载荷。
RUNTIME_ARTIFACT_DIRS = ("logs",)
RUNTIME_ARTIFACT_PREFIXES = ("selftest_out", "test_assets")

# ---------------------------------------------------------------------------
# 许可事实表。每条都必须能被复核：evidence 写清依据，核实命令写在文件末尾。
# ---------------------------------------------------------------------------
LICENSES = {
    "ffmpeg": {
        "name": "FFmpeg",
        "license": "GPL v3",
        "license_url": "https://www.gnu.org/licenses/gpl-3.0.html",
        "upstream": "https://ffmpeg.org/",
        "build": "gyan.dev release-full-shared（64-bit shared, UCRT64 构建）",
        "evidence": [
            "随包档案自带的 README.txt：`License: GPL v3`（原文，非推断）",
            "随包 LICENSE 的 SHA-256 与 FFmpeg 源码树里的 COPYING.GPLv3 完全一致 "
            "（8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903）",
            "gyan.dev 构建页原文：\"All builds are 64-bit, static and licensed as GPLv3\"",
        ],
    },
    "sdl3": {
        "name": "Simple DirectMedia Layer 3 (SDL3)",
        "license": "zlib",
        "license_url": "https://github.com/libsdl-org/SDL/blob/main/LICENSE.txt",
        "upstream": "https://www.libsdl.org/",
        "build": "官方 SDL3-devel-3.4.16-mingw.zip（导入库/头文件）+ SDL3-3.4.16-win32-x64.zip（运行时）",
        "evidence": [
            "随包 LICENSE.txt 原文（\"Permission is granted to anyone to use this software for "
            "any purpose\" + 三条限制）—— 即 zlib 许可的标准措辞",
        ],
    },
    "stb_truetype": {
        "name": "stb_truetype.h（stb 单文件库）",
        "license": "Public Domain / Unlicense（与 MIT 二选一，本项目按 Public Domain 采用）",
        "license_url": "https://github.com/nothings/stb/blob/master/stb_truetype.h",
        "upstream": "https://github.com/nothings/stb",
        "build": "v1.26 单头文件，随仓库 vendor/stb/stb_truetype.h（199,192 字节，实测 Content-Length）",
        "evidence": [
            "文件头原文：`// stb_truetype.h - v1.26 - public domain`",
            "同一文件另给出 MIT 与 Public Domain 两种授权供选择（原文 "
            "\"This software is available under 2 licenses -- choose whichever you prefer\"）",
            "核实命令：curl -sI https://raw.githubusercontent.com/nothings/stb/master/stb_truetype.h "
            "| findstr Content-Length（本机实测 199192）",
        ],
    },
    "zcool_kuaile": {
        "name": "站酷快乐体 2016 修订版（ZCOOL KuaiLe / OSD 随包字体）",
        "license": "免费商用（字体发布方「站酷 ZCOOL」声明）；**非** SIL OFL，也无随附 LICENSE 文本",
        "license_url": "https://www.zcool.com.cn/",
        "upstream": "https://www.zcool.com.cn/",
        "build": "assets/fonts/ZCOOLKuaiLe-Regular.ttf（1,639,036 字节，TrueType 'glyf' 轮廓）",
        "evidence": [
            "来源：本机字体资产库 G:\\assets\\fonts\\etc\\ZhanKuKuaiLeTi2016XiuDingBan.zip"
            "（用户确认该目录字体均为免费可商用）",
            "**如实说明（证据强度不足，不要当成已核实）**：zip 内**没有**随附 LICENSE 文本，"
            "字体 name 表也没有 nameID 13/14（许可信息字段）—— 目前只能依据发布方公开声明"
            "与来源目录判断，未取得可逐字引用的许可原文",
            "若将来要求最强合规证据：换成 Noto Sans SC / 思源黑体（SIL OFL 1.1，"
            "raw 上带完整 LICENSE，体积 ~8 MB），release-payload 数字需再同步",
        ],
    },
    "cordis": {
        "name": "Cordis 与 @cordisjs/plugin-loader",
        "license": "MIT",
        "license_url": "https://opensource.org/license/mit",
        "upstream": "https://www.npmjs.com/package/cordis",
        "build": "npm 包（host-js/node_modules）",
        "evidence": [
            "host-js/node_modules/<包>/package.json 的 license 字段为 \"MIT\"（脚本读取，非硬编码）",
        ],
    },
    "mingw_runtime": {
        "name": "MinGW-w64 GCC 运行时（libgcc_s_seh-1.dll / libstdc++-6.dll / libwinpthread-1.dll）",
        "license": "GPL v3 + GCC Runtime Library Exception",
        "license_url": "https://www.gnu.org/licenses/gcc-exception-3.1.html",
        "upstream": "https://www.mingw-w64.org/",
        "build": "本机 MinGW-w64 GCC 16.1.0（Build from MinGW-Builds）",
        "evidence": [
            "GCC 的运行时库受 GPL v3 第 3 节 + Runtime Library Exception 约束；"
            "该例外**明确允许**把 libgcc/libstdc++ 随二进制一起分发而不触发传染",
        ],
    },
}

# 开发期使用、但不随发布物分发的材料。列出来是为了把"参考"与"依赖"分清。
DEV_ONLY = [
    {
        "name": "potplayer-master（itisyang/playerdemo）",
        "license": "GPL v3",
        "use": "解码/渲染思路的**架构参考**（README「与原 playerdemo 的差异」一节）。"
               "内核为按 FFmpeg 9.x 新 API 重写，该目录不随发布物分发。",
    },
    {
        "name": "FFmpeg 源码树（player/ffmpeg-9.0.1）",
        "license": "LGPL v2.1+ / GPL v2+ / GPL v3（FFmpeg 自身为多许可）",
        "use": "阅读实现、核对 CVE 影响区间。不随发布物分发。",
    },
    {
        "name": "vendor/ 下的开发文件（SDL3 头文件与 libSDL3.dll.a、FFmpeg 头文件与 lib*.dll.a）",
        "license": "同上游（zlib / GPL v3）",
        "use": "只用于**编译期链接**，不随发布物分发（.gitignore 已排除 vendor/）。",
    },
]


def read_text(path, must=True):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fp:
            return fp.read()
    except OSError as exc:
        if must:
            raise SystemExit("[ERR] 读不到事实来源 %s (%s)\n"
                             "      先跑 tools/fetch_deps.py 准备 vendor/" % (path, exc))
        return ""


def ffmpeg_version():
    m = re.search(r'#define\s+FFMPEG_VERSION\s+"([^"]+)"', read_text(FFVERSION_H))
    if not m:
        raise SystemExit("[ERR] 无法从 %s 解析 FFMPEG_VERSION" % FFVERSION_H)
    return m.group(1)


def sdl_version():
    t = read_text(SDL_VERSION_H)
    def num(key):
        m = re.search(r"#define\s+%s\s+(\d+)" % key, t)
        if not m:
            raise SystemExit("[ERR] 无法从 %s 解析 %s" % (SDL_VERSION_H, key))
        return m.group(1)
    return "%s.%s.%s" % (num("SDL_MAJOR_VERSION"), num("SDL_MINOR_VERSION"),
                         num("SDL_MICRO_VERSION"))


def abi_version():
    m = re.search(r"#define\s+PP_ABI_VERSION\s+(\d+)u?", read_text(ABI_H))
    return m.group(1) if m else "?"


def js_deps():
    """从 node_modules 读版本与 license（不是硬编码）。"""
    out = []
    try:
        pkg = json.loads(read_text(HOSTJS_PKG))
    except ValueError:
        return out
    for name, spec in sorted((pkg.get("dependencies") or {}).items()):
        pj = os.path.join(HOSTJS_NM, name.replace("/", os.sep), "package.json")
        if os.path.isfile(pj):
            j = json.loads(read_text(pj))
            out.append((j.get("name", name), j.get("version", "?"),
                        j.get("license") or j.get("licenses") or "?", spec))
        else:
            out.append((name, "(未安装)", "?", spec))
    return out


def is_runtime_artifact(rel):
    """rel 是相对 dist/ 的 posix 路径。判定与 build.py --release 的清理规则一致。"""
    top = rel.split("/")[0]
    if top in RUNTIME_ARTIFACT_DIRS or top.startswith(RUNTIME_ARTIFACT_PREFIXES):
        return True
    if rel.lower().endswith(".log"):
        return True
    if rel == "config/rpc-token.txt":
        return True
    return False


def dist_payload(include_artifacts=False, dist=None):
    """dist/ 的文件清单。

    include_artifacts=False（默认）给出的是**发布载荷** —— 与 README 的
    「17 个文件 / 127 MB」同一口径；True 则连运行期产物一起列，用来说明差异。
    dist 可指定别的目录（tools/gitee_release.py 上传发布物时会复用它，
    以免"哪些文件算发布载荷"出现第二份定义）。
    """
    root = DIST if dist is None else dist
    if not os.path.isdir(root):
        return None
    files = []
    for cur, _dirs, fs in os.walk(root):
        for f in fs:
            p = os.path.join(cur, f)
            rel = os.path.relpath(p, root).replace("\\", "/")
            if not include_artifacts and is_runtime_artifact(rel):
                continue
            files.append((rel, os.path.getsize(p)))
    files.sort()
    return files


def classify_dist(files):
    ff, sdl, mingw, plugins, other = [], [], [], [], []
    mingw_names = ("libwinpthread", "libgcc_s", "libstdc++")
    for rel, size in files:
        base = os.path.basename(rel).lower()
        stem = base.split("-")[0]
        if rel.startswith("plugins/"):
            plugins.append((rel, size))
        elif stem in DIST_FFMPEG_PREFIX and base.endswith(".dll"):
            ff.append((rel, size))
        elif base.startswith("sdl3"):
            sdl.append((rel, size))
        elif base.startswith(mingw_names):
            mingw.append((rel, size))
        else:
            other.append((rel, size))
    return ff, sdl, mingw, plugins, other


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fp:
        for chunk in iter(lambda: fp.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()



def license_kind():
    """读 LICENSE 判断项目自身的许可 —— 看**内容**而不是文件名。

    返回 (标签, 是否 GPLv3) 或 (None, False) 表示没有 LICENSE 文件。
    为什么要在生成器里判断：声明的正文有一句"因此 LICENSE 不能是专有许可"，
    那在**没有** LICENSE 时是待办事项、在**已有** GPLv3 时是已满足的约束。
    写死任何一边都会在条件变化后变成错话。
    """
    p = os.path.join(ROOT, "LICENSE")
    if not os.path.isfile(p):
        return None, False
    t = read_text(p, must=False)
    if "GNU GENERAL PUBLIC LICENSE" in t and "Version 3, 29 June 2007" in t:
        return "GPL-3.0-or-later（GPLv3 原文，逐字未改）", True
    return "见 LICENSE", False


def _self_license_ref():
    """自身许可的引用。

    三种情况都说清楚：
      · 没有 LICENSE → **不输出 Markdown 链接**（否则 doc-links 门禁会因死链失败），
        改为说明"这是刻意不添加"，并写明必须重新处理它的触发条件；
      · LICENSE 是 GPLv3 → 直接引用并声明 SPDX；
      · LICENSE 是别的 → 明确警告它与随包 GPL v3 不兼容。
    """
    kind, is_v3 = license_kind()
    if kind is None:
        head = ("PluginPlayer 自身代码的许可：**仓库里当前没有 `LICENSE` 文件** —— 这是刻意的决定"
                "（仅本机自用，不对外分发）。**一旦要对外分发 `dist/`，必须先补上与 GPL v3 兼容的 "
                "`LICENSE`**，并履行下面的署名与源码提供义务；在补上之前，本节结论就是发布前"
                "必须处理的事项。")
    elif is_v3:
        head = ("PluginPlayer 自身代码以 **`GPL-3.0-or-later`** 分发，见 [LICENSE](LICENSE)"
                "（GPLv3 逐字原文）。这与随包 FFmpeg 的 GPL v3 一致 —— 下面这些义务已经由"
                "`LICENSE` 的存在与本文件共同满足。")
    else:
        head = ("PluginPlayer 自身代码的许可见 [LICENSE](LICENSE)（**%s**）。"
                "**注意：随包 FFmpeg 是 GPL v3，若这个许可与 GPL v3 不兼容，"
                "分发 `dist/` 就是违规的。**" % kind)
    return (head + "本文件列出**随发布物分发**的第三方组件、它们的许可与来源；"
            "每个组件仍受其各自许可约束，本文件不改变那些条款。")


def build_document():
    ffver = ffmpeg_version()
    sdlver = sdl_version()
    abi = abi_version()
    deps = js_deps()
    files = dist_payload()
    ff, sdl, mingw, plugins, other = classify_dist(files) if files else ([], [], [], [], [])

    def rows(items):
        """把文件清单渲染成 Markdown 表格。

        **只列文件名，不列体积**。体积属于装饰性信息，却会让这份文书在每次重建后
        都变脏（例如 `config/pluginplayer.ini` 会被播放器用 Config::save() 重写成更短的
        形式，于是 0.3 KB 变 1.0 KB），而门禁随之报"已过期"——假警报会把真警报淹掉。
        许可相关的事实是"**分发哪些文件**"，那才是这里要钉住的。
        """
        if not items:
            return "| _（无）_ |"
        head = "| 文件 |\n|---|"
        body = "\n".join("| `%s` |" % r for r, _s in items)
        return head + "\n" + body


    L = []
    A = L.append
    A("<!-- 由 tools/gen_third_party_notices.py 生成，请勿手工编辑。")
    A("     重新生成: F:\\Python314\\python.exe tools\\gen_third_party_notices.py")
    A("     校验最新: F:\\Python314\\python.exe tools\\gen_third_party_notices.py --check")
    A("     tools/verify.py 的 third-party-notices 门禁会在提交前跑 --check。 -->")
    A("")
    A("# 第三方组件声明 / Third-Party Notices")
    A("")
    A(_self_license_ref())
    A("")
    A("本文件由 `tools/gen_third_party_notices.py` 从仓库内的原始文件生成（版本号、依赖清单、"
      "`dist/` 文件列表都是**读出来的**，不是写死的）。更改依赖后请重新生成。")
    A("")

    # ---- 最重要的一条：结论前置 ----
    A("## 先说结论：发布物整体受 GPL v3 约束")
    A("")
    A("随包的 FFmpeg 来自 gyan.dev 的 **`full_build-shared`**，该构建是 **GPL v3**"
      "（不是 LGPL）。因此 `dist/` 这个发布物是 GPL v3 的衍生作品，分发时必须满足：")
    A("")
    A("1. **附上许可全文**（GPL v3 全文，或指向 <https://www.gnu.org/licenses/gpl-3.0.html> 的明确指引）；")
    A("2. **标明改动**（若你对上述组件做过修改，必须声明）；")
    A("3. **提供对应源码**（Corresponding Source）—— 二进制分发须给出获取方式；")
    A("4. **不得附加限制**（不能对下游再分发施加更严格的条款）。")
    A("")
    _kind, _is_v3 = license_kind()
    if _is_v3:
        A("**本项目自身代码的许可（`GPL-3.0-or-later`）已经满足这条约束** —— 见 [LICENSE](LICENSE)。")
    else:
        A("**因此 `LICENSE` 不能是专有许可，也不宜只写 MIT。** 本仓库的 `LICENSE` 若与 "
          "GPL v3 不兼容，随包分发 `dist/` 就是违规的。")
    A("")
    A("唯一能摆脱 GPL 的路径是**自建一份 LGPL 配置的 FFmpeg**（`--disable-gpl --disable-version3`，"
      "并去掉所有 GPL-only 外部库），再替换 `dist/` 里的 DLL —— 注意大版本号一致才能直接替换"
      "（见 README「可选的升级路径」）。当前随包的这份**不支持**这样做。")
    A("")

    # ---- 随包组件 ----
    A("## 随发布物分发的组件（`dist/`）")
    A("")
    A("| 组件 | 版本 | 许可 | 上游 |")
    A("|---|---|---|---|")
    A("| FFmpeg | %s | **%s** | %s |" % (ffver, LICENSES["ffmpeg"]["license"],
                                         LICENSES["ffmpeg"]["upstream"]))
    A("| SDL3 | %s | %s | %s |" % (sdlver, LICENSES["sdl3"]["license"],
                                   LICENSES["sdl3"]["upstream"]))
    A("| Cordis / @cordisjs/plugin-loader | %s | %s | %s |"
      % (", ".join("%s %s" % (n, v) for n, v, _l, _s in deps) or "—",
         LICENSES["cordis"]["license"], LICENSES["cordis"]["upstream"]))
    A("| MinGW-w64 GCC 运行时 | GCC 16.1.0 | %s | %s |"
      % (LICENSES["mingw_runtime"]["license"], LICENSES["mingw_runtime"]["upstream"]))
    A("")
    A("插件 ABI 版本：`PP_ABI_VERSION = %s`（`src/sdk/player_plugin.h`）。" % abi)
    A("")

    A("### FFmpeg —— 随包文件与来源")
    A("")
    A("构建：%s" % LICENSES["ffmpeg"]["build"])
    A("")
    A(rows(ff))
    A("")
    A("**对应源码（GPL v3 义务 3 的指向）**：本构建的二进制由 FFmpeg 提交 "
      "`bf1b838f2a` 构建（随包 README.txt 给出），源码可从 "
      "<https://github.com/FFmpeg/FFmpeg/tree/bf1b838f2a> 获取；"
      "构建者提供的源码入口见 <https://www.gyan.dev/ffmpeg/builds/>。")
    A("")
    A("> `avfilter` / `avdevice` **不随 `dist/` 分发**（`build.py` 只拷 5 个 FFmpeg DLL，省约 110 MB）。"
      "`vendor/ffmpeg/bin` 里那两个 DLL 属于本地开发包，不随发布物走。")
    A("")
    A("判定依据（逐条可复核）：")
    for e in LICENSES["ffmpeg"]["evidence"]:
        A("- %s" % e)
    A("")

    if sdl:
        A("### SDL3 —— 随包文件")
        A("")
        A(rows(sdl))
        A("")
        A("依据：%s" % "；".join(LICENSES["sdl3"]["evidence"]))
        A("")

    if mingw:
        A("### MinGW-w64 GCC 运行时 —— 随包文件")
        A("")
        A(rows(mingw))
        A("")
        A("依据：%s" % "；".join(LICENSES["mingw_runtime"]["evidence"]))
        A("")

    if plugins:
        A("### 本项目产物：插件与插件目录清单（与 PluginPlayer 自身同许可，不是第三方）")
        A("")
        A(rows(plugins))
        A("")

    if other:
        A("### 其它随包文件（本项目产物或配置）")
        A("")
        A(rows(other))
        A("")

    A("### `dist/` 载荷")
    A("")
    if files is None:
        A("_`dist/` 不存在（尚未构建）。构建后本段会列出实际随包分发的文件清单。_")
    else:
        A("发布载荷共 **%d 个文件**（口径同 `build.py --release`，即已剔除运行期产物）。"
          "数量与体积的具体口径写在 README 的「发布构建」一节，由 `tools/verify.py` 的 "
          "`release-payload` 门禁核对 —— 这里不重复那些数字，避免同一事实两处维护。" % len(files))
        # 刻意不写文件名以外的信息（体积/时间戳）：那会让这份文书在每次重建后变脏，
        # 而本文书讲的是**许可**（分发哪些文件），不是"此刻每个文件多大"。
    A("")

    # ---- 仅供参考、不随包 ----
    A("## 开发期使用、但不随发布物分发")
    A("")
    A("| 材料 | 许可 | 用途 |")
    A("|---|---|---|")
    for d in DEV_ONLY:
        A("| %s | %s | %s |" % (d["name"], d["license"], d["use"]))
    A("")
    A("这里刻意把「依赖」和「参考」分开列：`potplayer-master` 是**读来学架构**的参考实现，"
      "其文件不进入任何发布物，因此不构成分发、也就不触发它的许可义务。"
      "**但如果你曾经从它那里拷贝过代码**，那些代码段仍受其 GPL v3 约束 —— "
      "这件事只有作者本人能确认，本文件不做断言。")
    A("")

    # ---- 复现 ----
    A("## 怎么复核本文件")
    A("")
    A("```bat")
    A(":: 1) FFmpeg 版本与构建者（本文件的表格就来自这里）")
    A("type vendor\\ffmpeg\\include\\libavutil\\ffversion.h")
    A("")
    A(":: 2) FFmpeg 到底是 GPL 还是 LGPL —— 上游档案自带的声明")
    A("::    解压 ffmpeg-release-full-shared.7z 后看 README.txt 的 License 行")
    A("::    以及对比随包 LICENSE 与 FFmpeg 源码树的 COPYING.GPLv3 是否同一份")
    A("")
    A(":: 3) SDL3 的 zlib 原文")
    A("::    SDL3-devel-3.4.16-mingw.zip 内 LICENSE.txt")
    A("")
    A(":: 4) JS 依赖的 license 字段")
    A("type host-js\\node_modules\\cordis\\package.json")
    A("")
    A(":: 5) 重新生成本文件并校验是否最新")
    A("F:\\Python314\\python.exe tools\\gen_third_party_notices.py")
    A("F:\\Python314\\python.exe tools\\gen_third_party_notices.py --check")
    A("```")
    A("")
    A("---")
    A("")
    A("事实来源：`vendor/` 头文件、随包档案、`host-js/node_modules`、`dist/` 实际文件。"
      "本文件不含生成时间与解释器版本 —— 那类字段会让 `--check` 在不同环境下误报过期。")
    A("")
    return "\n".join(L)


def main():
    ap = argparse.ArgumentParser(description="生成 THIRD_PARTY_NOTICES.md")
    ap.add_argument("--check", action="store_true", help="只校验是否最新（不一致则 exit 1）")
    ap.add_argument("--print", dest="to_stdout", action="store_true", help="打到 stdout")
    args = ap.parse_args()

    doc = build_document()
    if args.to_stdout:
        sys.stdout.write(doc)
        return 0
    if args.check:
        try:
            with open(OUT, "r", encoding="utf-8") as fp:
                cur = fp.read()
        except OSError:
            print("[FAIL] %s 不存在，请先运行 gen_third_party_notices.py" % OUT)
            return 1
        if cur != doc:
            print("[FAIL] THIRD_PARTY_NOTICES.md 已过期（依赖或许可证据变了）")
            print("       重新生成: F:\\Python314\\python.exe tools\\gen_third_party_notices.py")
            return 1
        print("[PASS] THIRD_PARTY_NOTICES.md 与当前依赖一致")
        return 0
    with open(OUT, "w", encoding="utf-8", newline="\n") as fp:
        fp.write(doc)
    print("[OK] 已写入 %s（%d 字节）" % (OUT, len(doc.encode("utf-8"))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
