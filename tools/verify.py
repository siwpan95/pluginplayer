#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
verify.py — 仓库机械门禁（mechanical gates）
============================================

【为什么要有它】
这个仓库里有大量"同一件事写在两处"的地方：数值边界既在 `src/core/main.cpp` 又在
`tools/pp_cli.py`；控制通道方法名既在 `src/core/control.cpp` 又在 `docs/ARCHITECTURE.md`；
命令数既由插件注册又写在 README 的实测输出里；`.bat` 必须纯 ASCII + CRLF 这条约定
同时写在 README、`build.py`、`tools/pp_cli.py` 三处。
散文没法约束它们 —— 只有可执行的检查能。所以本脚本把每一条"两处必须一致"的断言
变成一道门禁，**跑一次就知道文档有没有跟实现走散**。

设计参考了 deepseek-harness-master 的 `scripts/verify-*.ts` 一族（那边有 150+ 个门禁脚本，
外加 `pnpm run doc-sync` 把它们串起来）。本仓库规模小得多，所以收成一个文件。

门禁分三类：
  · **一致性**：同一事实出现在两处时必须相等（arg-bounds / rpc-methods / exit-codes / …）
  · **契约**：约定必须被遵守（bat-ascii-crlf / plugin-manifest / release-payload）
  · **新鲜度**：生成物必须是最新的（third-party-notices）

【不做什么】
不跑构建、不联网、不启动播放器、不改任何文件（`--fix` 只能重新生成生成物）。
需要 `dist/` 或运行中实例的检查会在前提缺失时记 SKIP 并说明原因 ——
**跳过不等于通过**，统计口径与 `tests/cli_check.py`、`--selftest` 一致。

用法:
    F:\\Python314\\python.exe tools\\verify.py              # 跑全部门禁
    F:\\Python314\\python.exe tools\\verify.py --list        # 列出门禁
    ... --only bat-ascii-crlf,doc-links                      # 只跑指定门禁
    ... --json                                               # 机器可读
    ... --verbose                                            # 打印每条检查的依据

退出码: 0 全部通过（SKIP 不算失败）；1 有 FAIL；6 环境前提缺失（连仓库都读不到）
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)          # import 同目录模块（PYTHONSAFEPATH 下不会自动加）

DIST = os.path.join(ROOT, "dist")
PLUGIN_DIR = os.path.join(DIST, "plugins")
README = os.path.join(ROOT, "README.md")
DOCS = os.path.join(ROOT, "docs")

# 扫描时排除：第三方依赖、构建产物、参考源码、缓存
EXCLUDE_DIRS = {
    ".git", "node_modules", "vendor", "_extract", "_deps", "player", "build", "dist",
    "__pycache__", "_dist_baseline_backup", "_vendor_backup_20260912", ".vscode", ".vs",
    "logs",
}


# ============================================================================
# 框架
# ============================================================================

GATES = []


def gate(name, desc):
    def deco(fn):
        GATES.append((name, desc, fn))
        return fn
    return deco


class Skip(Exception):
    """前提不满足 —— 记为 SKIP 并说明原因。"""


class Fail(Exception):
    """门禁不通过。"""


def read(path, must=True):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fp:
            return fp.read()
    except OSError:
        if must:
            raise Fail("读不到 %s" % os.path.relpath(path, ROOT))
        return ""


def walk_files(exts=None, root=ROOT):
    """遍历仓库（排除 EXCLUDE_DIRS），产出相对 posix 路径。"""
    for cur, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if d not in EXCLUDE_DIRS]
        for f in files:
            if exts and os.path.splitext(f)[1].lower() not in exts:
                continue
            yield os.path.relpath(os.path.join(cur, f), root).replace("\\", "/")


def git_tracked(patterns=()):
    """git ls-files；失败返回 None（例如没有 git）。patterns 为空则全部文件。"""
    try:
        p = subprocess.run(["git", "ls-files", "-z"] + list(patterns), cwd=ROOT,
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    except OSError:
        return None
    if p.returncode != 0:
        return None
    return [x for x in p.stdout.decode("utf-8", "replace").split("\0") if x]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fp:
        for chunk in iter(lambda: fp.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def load_cli():
    """import tools/pp_cli.py。它只依赖标准库，import 不会跑 main()。"""
    try:
        import pp_cli
        return pp_cli
    except ImportError as exc:
        raise Fail("无法 import tools/pp_cli.py: %s" % exc)


def newline_count(raw):
    return raw.count(b"\n") - raw.count(b"\r\n")


# ============================================================================
# 契约类
# ============================================================================

@gate("bat-ascii-crlf", "每个 .bat/.cmd 都是纯 ASCII + 规范 CRLF（无裸 LF、无孤立 CR）")
def g_bat_ascii_crlf(verbose):
    # 取「已跟踪」与「在盘」的并集：这样新加但还没 commit 的 .cmd 也会被检查，
    # 而不是等到提交之后才发现行尾不对。
    tracked = git_tracked(["*.bat", "*.cmd"]) or []
    ondisk = list(walk_files((".bat", ".cmd")))
    files = sorted(set(tracked) | set(ondisk))
    if not files:
        raise Skip("仓库里没有 .bat/.cmd")
    bad = []
    for rel in files:
        p = os.path.join(ROOT, rel)
        if not os.path.isfile(p):
            continue
        raw = open(p, "rb").read()
        non_ascii = sorted({b for b in raw if b > 127})
        lone_lf = newline_count(raw)
        # 孤立 CR：任何不是 \r\n 组成部分的 CR。这条专门防 "\r\r\n" ——
        # 那种文件只看 "裸 LF 数" 是看不出来的（\n 全都配了 \r），
        # 但它不是规范 CRLF。build.py 曾经因为 newline="\r\n" 生成过这种文件。
        stray_cr = len(re.findall(rb"\r(?!\n)", raw))
        if non_ascii:
            bad.append("%s 含非 ASCII 字节 %s" % (rel, non_ascii[:6]))
        if lone_lf:
            bad.append("%s 有 %d 个裸 LF（行尾必须全 CRLF）" % (rel, lone_lf))
        if stray_cr:
            crcrlf = raw.count(b"\r\r\n")
            bad.append("%s 有 %d 个孤立 CR%s" % (
                rel, stray_cr,
                "（其中 %d 个是 CR CR LF —— 典型成因是写文件时 newline=\"\\r\\n\" "
                "又把已经写好的 \\r\\n 翻译了一遍）" % crcrlf if crcrlf else ""))
    if bad:
        raise Fail("；".join(bad))
    return "%d 个文件：0 非 ASCII 字节、0 裸 LF、0 孤立 CR（%s）" % (
        len(files), ", ".join(sorted(os.path.basename(f) for f in files)))


@gate("plugin-manifest", "dist/plugins/manifest.sha256 与在盘 DLL 逐一相符，且无未登记项")
def g_plugin_manifest(verbose):
    mf = os.path.join(PLUGIN_DIR, "manifest.sha256")
    if not os.path.isdir(PLUGIN_DIR):
        raise Skip("dist/plugins 不存在（先 build.py）")
    if not os.path.isfile(mf):
        raise Skip("manifest.sha256 不存在（宿主不校验；发布前应生成）")
    try:
        import pp_cli
        entries = pp_cli.read_manifest()
    except Exception as exc:                       # noqa: BLE001
        raise Fail("解析清单失败: %s" % exc)
    if not entries:
        raise Fail("清单解析为空")
    dlls = sorted(f for f in os.listdir(PLUGIN_DIR) if f.lower().endswith(".dll"))
    bad, unlisted = [], []
    for f in dlls:
        if f not in entries:
            unlisted.append(f)
        elif sha256(os.path.join(PLUGIN_DIR, f)) != entries[f]:
            bad.append(f)
    missing = [f for f in entries if f not in dlls]
    if bad or unlisted or missing:
        raise Fail("哈希不符 %s；未登记 %s；清单有但文件缺失 %s" % (bad, unlisted, missing))
    return "%d 个 DLL 哈希与清单逐一相符" % len(dlls)


@gate("plugin-command-count", "插件注册的命令数与 README / tests / docs 里声明的数字一致")
def g_plugin_command_count(verbose):
    pat = re.compile(r'register_command\(\s*host\s*,\s*"([^"]+)"')
    names = []
    for rel in walk_files((".cpp",)):
        if not rel.startswith("src/plugins/"):
            continue
        names += pat.findall(read(os.path.join(ROOT, rel)))
    if not names:
        raise Fail("src/plugins 下没有找到 register_command 调用")
    dup = sorted({n for n in names if names.count(n) > 1})
    if dup:
        raise Fail("命令名重复注册: %s" % dup)
    n = len(names)
    # 三处声明必须都等于 n
    src_cli = read(os.path.join(ROOT, "tests", "cli_check.py"))
    m = re.search(r"int\(m\.group\(1\)\)\s*==\s*(\d+)", src_cli)
    if not m:
        raise Fail("tests/cli_check.py 里找不到命令数断言（期望 `int(m.group(1)) == N`）")
    if int(m.group(1)) != n:
        raise Fail("tests/cli_check.py 断言 %s，实际注册 %d" % (m.group(1), n))
    for rel, pat_doc in (("README.md", r"`commands`（断言恰好 (\d+) 个）"),
                         ("docs/CLI.md", r"`commands`（(\d+) 个）")):
        mm = re.search(pat_doc, read(os.path.join(ROOT, rel)))
        if not mm:
            raise Fail("%s 里找不到命令数声明（期望匹配 %s）" % (rel, pat_doc))
        if int(mm.group(1)) != n:
            raise Fail("%s 声明 %s 个，实际注册 %d" % (rel, mm.group(1), n))
    return "注册 %d 个命令，README/tests/cli_check.py/docs/CLI.md 三处声明一致" % n


@gate("native-plugin-set", "build.py 的插件表 = src/plugins/ 目录 = dist/plugins/ 里的 DLL")
def g_native_plugin_set(verbose):
    src = read(os.path.join(ROOT, "build.py"))
    m = re.search(r"^PLUGINS\s*=\s*\[([^\]]*)\]", src, re.M)
    if not m:
        raise Fail("build.py 里找不到 PLUGINS 列表")
    listed = sorted(re.findall(r'"([^"]+)"', m.group(1)))
    if not listed:
        raise Fail("PLUGINS 列表为空")
    on_disk = sorted(d for d in os.listdir(os.path.join(ROOT, "src", "plugins"))
                     if os.path.isdir(os.path.join(ROOT, "src", "plugins", d)))
    if listed != on_disk:
        raise Fail("build.py PLUGINS %s != src/plugins/ %s" % (listed, on_disk))
    detail = "%d 个：%s" % (len(listed), ", ".join(listed))
    if os.path.isdir(PLUGIN_DIR):
        dlls = sorted(f[:-4] for f in os.listdir(PLUGIN_DIR) if f.lower().endswith(".dll"))
        if dlls != listed:
            raise Fail("dist/plugins 里的 DLL %s != PLUGINS %s" % (dlls, listed))
        detail += "；dist/plugins 一致"
    else:
        detail += "；dist/ 未构建（未交叉核对）"
    # README 的"5 个原生插件"声明
    n_claim = re.search(r"(\d+) 个原生插件", read(README))
    if n_claim and int(n_claim.group(1)) != len(listed):
        raise Fail("README 声明 %s 个原生插件，实际 %d" % (n_claim.group(1), len(listed)))
    return detail


@gate("js-plugin-count", "README 声明的 JS 插件数与 host-js/plugins/*.mjs 数量一致")
def g_js_plugin_count(verbose):
    pdir = os.path.join(ROOT, "host-js", "plugins")
    if not os.path.isdir(pdir):
        raise Fail("host-js/plugins 不存在")
    mjs = sorted(f for f in os.listdir(pdir) if f.endswith(".mjs"))
    if not mjs:
        raise Fail("host-js/plugins 下没有 .mjs")
    txt = read(README)
    claims = [int(x) for x in re.findall(r"(\d+) 个 JS 插件", txt)]
    if not claims:
        raise Fail("README 里找不到「N 个 JS 插件」声明")
    for c in claims:
        if c != len(mjs):
            raise Fail("README 声明 %d 个 JS 插件，实际 %d（%s）" % (c, len(mjs), ", ".join(mjs)))
    # 插件表里的行数也要对得上（README 表格写的是去掉扩展名的插件名）
    listed = sorted(re.findall(r"^\|\s*`(\d\d-[a-z-]+)`", txt, re.M))
    if listed and listed != [os.path.splitext(f)[0] for f in mjs]:
        raise Fail("README 的 JS 插件表 %s != 实际 %s"
                   % (listed, [os.path.splitext(f)[0] for f in mjs]))
    return "%d 个（%s），README 声明与实际一致" % (len(mjs), ", ".join(mjs))


@gate("release-payload", "dist/ 的发布载荷数量/体积与 README 记录的口径一致")
def g_release_payload(verbose):
    if not os.path.isdir(DIST):
        raise Skip("dist/ 不存在（先 build.py --release）")
    try:
        import gen_third_party_notices as notices
    except ImportError as exc:
        raise Fail("无法 import tools/gen_third_party_notices.py: %s" % exc)
    files = notices.dist_payload() or []
    total = sum(s for _r, s in files)
    txt = read(README)
    m = re.search(r"实测产物\s*(\d+)\s*个文件\s*/\s*(\d+)\s*MB", txt)
    if not m:
        raise Fail("README 里找不到「实测产物 N 个文件 / M MB」")
    n_claim, mb_claim = int(m.group(1)), int(m.group(2))
    mb = total / 1048576.0
    if len(files) != n_claim:
        raise Fail("README 声明 %d 个文件，实际 %d" % (n_claim, len(files)))
    if abs(mb - mb_claim) > 3.0:
        raise Fail("README 声明 %d MB，实际 %.1f MB（容差 ±3）" % (mb_claim, mb))
    return "%d 个文件 / %.1f MB，与 README 的 %d / %d MB 一致" % (
        len(files), mb, n_claim, mb_claim)


# ============================================================================
# 可移植性（CI 前提）
# ============================================================================

# 这四个模块 + 单元测试被设计成**不依赖 Windows**，所以能在 Linux 上编译运行 ——
# Gitee Go 的 Ubuntu 构建环境就是靠这一点跑 C++ 单元测试的（托管 CI 没有 Windows
# 构建环境）。本机装不了 Linux，所以这个前提没法靠"跑一遍"证明，只能靠**静态检查**
# 保证：这些文件里的 Windows API 只能出现在 `#ifdef _WIN32` 分支里。
PORTABLE_MODULE_FILES = [
    "src/core/util.cpp", "src/core/util.h",
    "src/core/config.cpp", "src/core/config.h",
    "src/core/eventbus.cpp", "src/core/eventbus.h",
    "src/core/registry.cpp", "src/core/registry.h",
]

WINAPI_TOKENS = [
    "windows.h", "direct.h", "bcrypt", "BCrypt", "GetFullPathNameA", "GetModuleFileNameA",
    "_mkdir", "FindFirstFileA", "FindNextFileA", "FindClose", "WIN32_FIND_DATA",
    "MAX_PATH", "HANDLE", "DWORD", "HINSTANCE", "GetLastError", "BOOL",
]


def strip_comments(text):
    """去掉 C/C++ 注释，保留字符串字面量。

    为什么门禁需要它：注释里提到 `GetFullPathNameA`、`bcrypt` 这类名字是**正常且有用**的
    （本项目大量注释在解释"为什么这么写"），但按裸词搜索会把它们当成真实调用而误报。
    反过来，去掉注释也会漏掉"注释里藏了一个 API 名"的情况 —— 那不是问题。
    """
    out = []
    i, n = 0, len(text)
    state = None          # None / 'line' / 'block' / 'str' / 'chr'
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state is None:
            if c == "/" and nxt == "/":
                state = "line"; i += 2; continue
            if c == "/" and nxt == "*":
                state = "block"; i += 2; continue
            if c == '"':
                state = "str"; out.append(c); i += 1; continue
            if c == "'":
                state = "chr"; out.append(c); i += 1; continue
            out.append(c); i += 1; continue
        if state == "line":
            if c == "\n":
                state = None; out.append(c)
            i += 1; continue
        if state == "block":
            if c == "*" and nxt == "/":
                state = None; i += 2
                out.append(" ")          # 用空格占位，避免把两个 token 粘在一起
                continue
            if c == "\n":
                out.append(c)
            i += 1; continue
        # 字符串/字符字面量
        if c == "\\":
            out.append(c)
            if i + 1 < n:
                out.append(text[i + 1])
            i += 2; continue
        if (state == "str" and c == '"') or (state == "chr" and c == "'"):
            state = None
        out.append(c); i += 1
    return "".join(out)


def non_windows_text(text):
    """剥掉 `#ifdef _WIN32 ... #else ... #endif` 里的 Windows 分支，
    返回"在非 Windows 上会参与编译"的那部分文本（注释已去掉）。

    关键点：必须跟踪**所有**预处理条件块，而不只是 `_WIN32` 的。
    util.cpp 里有这种嵌套：

        #ifdef _WIN32
        #  ifndef WIN32_LEAN_AND_MEAN
        #    define WIN32_LEAN_AND_MEAN
        #  endif            <-- 这个 #endif 属于内层 #ifndef
        #  include <windows.h>
        #  include <bcrypt.h>
        #endif              <-- 这个才属于 #ifdef _WIN32

    只看 `_WIN32` 的话，内层的 `#endif` 会把外层提前弹掉，
    于是 `windows.h` / `bcrypt.h` 被误判成"在条件编译之外"。第一版就是这么错的。
    """
    out = []
    stack = []                       # 每层: {"in_win": bool, "seen_else": bool}
    for line in strip_comments(text).splitlines():
        s = line.strip()
        if re.match(r"#\s*if", s):
            is_win = bool(re.match(r"#\s*ifdef\s+_WIN32\b", s) or
                          re.match(r"#\s*if\s+defined\s*\(\s*_WIN32\s*\)", s))
            # `#ifndef _WIN32` 的**首分支**是"非 Windows"，所以 in_win 初值为 False
            stack.append({"in_win": is_win, "seen_else": False})
            continue
        if re.match(r"#\s*(else|elif)\b", s):
            if stack and not stack[-1]["seen_else"]:
                stack[-1]["in_win"] = not stack[-1]["in_win"]
                stack[-1]["seen_else"] = True
            continue
        if re.match(r"#\s*endif\b", s):
            if stack:
                stack.pop()
            continue
        if any(f["in_win"] for f in stack):
            continue                 # 当前位于"仅 Windows 编译"的分支 → 跳过
        out.append(line)
    return "\n".join(out)


@gate("portable-modules", "可移植模块与单元测试里的 WinAPI 只出现在 #ifdef _WIN32 分支内")
def g_portable_modules(verbose):
    targets = PORTABLE_MODULE_FILES + [f for f in walk_files((".cpp", ".h"))
                                       if f.startswith("tests/unit/")]
    missing = [f for f in targets if not os.path.isfile(os.path.join(ROOT, f))]
    if missing:
        raise Fail("文件不存在: %s" % missing)
    bad = []
    for rel in targets:
        text = read(os.path.join(ROOT, rel))
        outside = non_windows_text(text)
        for tok in WINAPI_TOKENS:
            if re.search(r"\b%s" % re.escape(tok), outside):
                bad.append("%s 在 #ifdef _WIN32 之外用了 %s" % (rel, tok))
    if bad:
        raise Fail("；".join(sorted(set(bad))[:6]) +
                   "  —— 这些文件要在 Linux CI 上编译，Windows API 必须包在条件编译里")
    return "%d 个文件的 Windows API 全部限定在条件编译分支内（Linux CI 的前提）" % len(targets)


# 凭据类：键名 + 分隔符 + 32 位以上 hex。刻意要求"键名在前"，
# 这样普通的长哈希（sha256 是 64 位 hex，仓库里到处都有）**不会**被误报。
CREDENTIAL_PATTERNS = [
    re.compile(r"(?:access_token|api_?key|auth_?token|password|passwd|secret|token)"
               r"\s*[=:]\s*[\"']?([0-9a-fA-F]{32,})", re.I),
    re.compile(r"gitee\.com[^\s\"']*[?&]access_token=[0-9a-fA-F]{16,}", re.I),
    re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----"),
]

SCAN_EXTS = {".py", ".md", ".txt", ".json", ".yml", ".yaml", ".bat", ".cmd", ".ini",
             ".mjs", ".js", ".c", ".cpp", ".h", ".hpp", ".ps1", ".sh", ".env", ""}


@gate("no-credentials", "被跟踪的文件里没有硬编码的令牌/私钥（公开仓库的底线）")
def g_no_credentials(verbose):
    """一条 32 位 hex 的密钥一旦进过公开仓库，就等于泄露 —— 即使后来删掉，
    它仍然留在历史里，只能吊销重发。所以推之前必须机械地拦一道。"""
    files = git_tracked()
    if files is None:
        files = list(walk_files())
    bad, scanned = [], 0
    for rel in files:
        if os.path.splitext(rel)[1].lower() not in SCAN_EXTS:
            continue
        p = os.path.join(ROOT, rel)
        if not os.path.isfile(p) or os.path.getsize(p) > 4 * 1024 * 1024:
            continue
        scanned += 1
        try:
            text = open(p, "r", encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        for pat in CREDENTIAL_PATTERNS:
            m = pat.search(text)
            if m:
                frag = m.group(0)[:24]
                bad.append("%s: %s..." % (rel, frag))
    if bad:
        raise Fail("疑似硬编码凭据（%s）—— 公开仓库里等同于泄露，必须改环境变量/凭据文件"
                   % "；".join(sorted(set(bad))[:5]))
    return "%d 个文本文件已扫描，未发现硬编码凭据" % scanned


@gate("ci-entrypoints", "CI 流水线 / 本地钩子 / ci.bat 里引用的 tools、tests 路径都存在")
def g_ci_entrypoints(verbose):
    """改个工具名、或者把 tools/xxx.py 挪个位置，CI 与钩子会**静默失效**——
    流水线仍然"绿"（如果它压根没跑到那一步）或红得莫名其妙。这条门禁把引用钉住。"""
    targets = [f for f in walk_files((".yml", ".yaml")) if f.startswith(".workflow/")]
    targets += [f for f in walk_files((".bat",)) if os.path.basename(f).startswith("ci")]
    if os.path.isfile(os.path.join(ROOT, ".githooks", "pre-commit")):
        targets.append(".githooks/pre-commit")
    if not targets:
        raise Skip("没有 .workflow/*.yml、ci*.bat 或 .githooks/pre-commit")

    pat = re.compile(r"\b((?:tools|tests)[/\\][A-Za-z0-9_./\\-]+\.(?:py|bat|exe))")
    missing, checked = [], 0
    for rel in sorted(set(targets)):
        text = read(os.path.join(ROOT, rel))
        for m in pat.finditer(text):
            p = m.group(1).replace("\\", "/")
            checked += 1
            if not os.path.isfile(os.path.join(ROOT, p)):
                missing.append("%s -> %s" % (rel, p))
    if missing:
        raise Fail("引用了不存在的文件: %s" % "；".join(sorted(set(missing))[:6]))
    return "%d 个引用全部存在（%s）" % (checked, ", ".join(sorted(set(targets))))


# ============================================================================
# 一致性类（同一事实写在两处）
# ============================================================================

def cpp_bound(text, pattern, label):
    m = re.search(pattern, text)
    if not m:
        raise Fail("在 C++ 源码里找不到 %s（模式 %s）" % (label, pattern))
    return m.group(1)


@gate("arg-bounds-align", "C++ 侧数值边界与 tools/pp_cli.py 的具名常量一一相等")
def g_arg_bounds(verbose):
    app_h = read(os.path.join(ROOT, "src", "core", "app.h"))
    main_cpp = read(os.path.join(ROOT, "src", "core", "main.cpp"))
    ctrl_h = read(os.path.join(ROOT, "src", "core", "control.h"))
    cli = load_cli()

    # (说明, C++ 值, CLI 值)
    pairs = [
        ("窗口最小边", cpp_bound(app_h, r"kMinWindowSize\s*=\s*(\d+)", "kMinWindowSize"),
         cli.MIN_WIN),
        ("窗口最大边", cpp_bound(app_h, r"kMaxWindowSize\s*=\s*(\d+)", "kMaxWindowSize"),
         cli.MAX_WIN),
        ("自检帧数上限", cpp_bound(main_cpp, r"kMaxFrames\s*=\s*(\d+)", "kMaxFrames"),
         cli.MAX_FRAMES),
        # main.cpp 里是 `volume < 0.0 || volume > 1.0`
        ("音量下界", cpp_bound(main_cpp, r"volume\s*<\s*([\d.]+)", "volume 下界"), cli.MIN_VOLUME),
        ("音量上界", cpp_bound(main_cpp, r"volume\s*>\s*([\d.]+)", "volume 上界"), cli.MAX_VOLUME),
        ("日志等级下界", cpp_bound(main_cpp, r"log-level 值: %s（需要 (\d+)\.\.4）", "log-level 下界"),
         cli.MIN_LOG_LEVEL),
        ("日志等级上界", cpp_bound(main_cpp, r"log-level 值: %s（需要 \d+\.\.(\d+)）", "log-level 上界"),
         cli.MAX_LOG_LEVEL),
        ("热重载下界", cpp_bound(main_cpp, r"reload 值: %s（需要 (\d+)\.\.9999）", "reload 下界"),
         cli.MIN_RELOAD),
        ("热重载上界", cpp_bound(main_cpp, r"reload 值: %s（需要 \d+\.\.(\d+)）", "reload 上界"),
         cli.MAX_RELOAD),
        ("端口上界", cpp_bound(main_cpp, r"rpc-port 值: %s（需要 0\.\.(\d+)）", "rpc-port 上界"),
         cli.MAX_PORT),
        ("令牌最短长度", cpp_bound(main_cpp, r"rpc-token\s*太短（至少 (\d+) 个字符", "token 长度"),
         cli.MIN_TOKEN_LEN),
        ("单条报文上限", cpp_bound(ctrl_h, r"kMaxLineBytes\s*=\s*(\d+)u?\s*\*\s*1024u?\s*\*\s*1024u?",
                                 "kMaxLineBytes"), cli.SERVER_MAX_LINE_BYTES // (1024 * 1024)),
        ("并发连接上限", cpp_bound(ctrl_h, r"kMaxClients\s*=\s*(\d+)", "kMaxClients"),
         cli.SERVER_MAX_CLIENTS),
    ]
    # 单条报文那对要按 MB 比（C++ 是 4*1024*1024，CLI 是字节数）
    mismatch = []
    checked = []
    for label, cval, pyval in pairs:
        if label == "单条报文上限":
            c_bytes = int(cval) * 1024 * 1024
            if c_bytes != cli.SERVER_MAX_LINE_BYTES:
                mismatch.append("%s: C++ %d 字节 vs CLI %d 字节"
                                % (label, c_bytes, cli.SERVER_MAX_LINE_BYTES))
        else:
            try:
                cnum = float(cval)
            except ValueError:
                raise Fail("%s 的 C++ 值 %r 不是数字" % (label, cval))
            if abs(cnum - float(pyval)) > 1e-9:
                mismatch.append("%s: C++ %s vs CLI %s" % (label, cval, pyval))
        checked.append(label)
    if mismatch:
        raise Fail("；".join(mismatch))
    return "%d 项边界与 C++ 源码一致（%s）" % (len(checked), "、".join(checked[:4]) + " …")


def expand_method_notation(text):
    """把 `player.info|stats|play` 这种压缩写法展开成完整方法名集合。"""
    out = set()
    for item in re.split(r"[、,，]", text):
        item = item.strip().strip("`").strip()
        if not item or item.startswith("（"):
            continue
        if "|" in item:
            segs = [s.strip() for s in item.split("|") if s.strip()]
            head = segs[0]
            prefix = head.rsplit(".", 1)[0] + "." if "." in head else ""
            for s in segs:
                out.add(s if "." in s else prefix + s)
        else:
            out.add(item)
    return out


@gate("rpc-methods-align", "control.cpp 的方法集 = ARCHITECTURE.md 的方法一览 = CLI 用到的方法")
def g_rpc_methods(verbose):
    ctrl = read(os.path.join(ROOT, "src", "core", "control.cpp"))
    code = set(re.findall(r'method\s*==\s*"([^"]+)"', ctrl))
    if not code:
        raise Fail("control.cpp 里找不到 method == \"...\" 形式的比较")

    arch = read(os.path.join(DOCS, "ARCHITECTURE.md"))
    m = re.search(r"方法一览：(.*?)。", arch, re.S)
    if not m:
        raise Fail("docs/ARCHITECTURE.md 里找不到「方法一览」")
    # 文档里这条列表会折行，而且中间可能插括号补充（例如 topic.subscribe（"*" 订阅全部））；
    # 先把括号补充去掉，再按顿号拆 —— 否则折行与括号会让解析结果少一半。
    doc_line = re.sub(r"（[^）]*）", "", m.group(1))
    doc = expand_method_notation(doc_line)

    if code != doc:
        only_code = sorted(code - doc)
        only_doc = sorted(doc - code)
        raise Fail("实现与文档不一致：只在代码里有 %s；只在文档里有 %s" % (only_code, only_doc))

    # CLI 里写死的方法名必须是实现真有的（防手滑改成不存在的方法）
    cli = load_cli()
    src = read(os.path.join(HERE, "pp_cli.py"))
    used = set(re.findall(r'"(player\.[a-z_.]+|app\.quit|command\.[a-z_.]+|service\.list'
                          r'|plugin\.list|topic\.subscribe|ping|auth)"', src))
    unknown = sorted(used - code)
    if unknown:
        raise Fail("tools/pp_cli.py 引用了实现里不存在的方法: %s" % unknown)
    return "实现 %d 个方法，与 ARCHITECTURE.md 及 CLI 引用一致" % len(code)


@gate("exit-codes-align", "docs/CLI.md 的退出码表与 tools/pp_cli.py 的 E_* 常量一致")
def g_exit_codes(verbose):
    cli = load_cli()
    consts = {name: val for name, val in vars(cli).items()
              if name.startswith("E_") and isinstance(val, int)}
    if not consts:
        raise Fail("tools/pp_cli.py 里找不到 E_* 常量")
    codes_code = {v for v in consts.values()}

    doc = read(os.path.join(DOCS, "CLI.md"))
    m = re.search(r"## 5\. 退出码(.*?)(?:\n## |\Z)", doc, re.S)
    if not m:
        raise Fail("docs/CLI.md 里找不到「## 5. 退出码」章节")
    codes_doc = {int(x) for x in re.findall(r"^\|\s*(\d+)\s*\|", m.group(1), re.M)}
    if not codes_doc:
        raise Fail("退出码表里没解析到任何数字（表格格式变了？）")
    if codes_code != codes_doc:
        raise Fail("CLI 常量 %s != 文档表格 %s" % (sorted(codes_code), sorted(codes_doc)))

    # 表格里每个码都要有含义说明，不能只列数字
    rows = re.findall(r"^\|\s*(\d+)\s*\|\s*([^|]+)\|", m.group(1), re.M)
    thin = [c for c, meaning in rows if len(meaning.strip()) < 2]
    if thin:
        raise Fail("退出码 %s 在文档里没有含义说明" % thin)
    return "%d 个退出码（%s）与 CLI 常量一一对应，且都有含义说明" % (
        len(codes_code), "、".join(str(c) for c in sorted(codes_code)))


@gate("cli-commands-align", "README 与 docs/CLI.md 的 CLI 命令表 = pp_cli.command_table()")
def g_cli_commands(verbose):
    cli = load_cli()
    table = set(cli.command_table())
    if not table:
        raise Fail("command_table() 为空")

    def section(text, header):
        m = re.search(header + r"(.*?)(?:\n## |\Z)", text, re.S)
        if not m:
            raise Fail("找不到章节 %r" % header)
        return m.group(1)

    bad = []

    # README：表头是「| 分组 | 命令 |」，命令名在**第二个**单元格里
    sec = section(read(README), r"## 命令行接口（CLI）")
    rows = re.findall(r"^\|\s*(?:本地编排|控制通道)\s*\|(.*)\|\s*$", sec, re.M)
    if not rows:
        bad.append("README: CLI 章节没解析到「本地编排 / 控制通道」命令行")
    rm = {c for row in rows for c in re.findall(r"`([a-z][a-z0-9-]*)`", row)}
    if rm != table:
        bad.append("README: 表里少 %s；表里多 %s" % (sorted(table - rm), sorted(rm - table)))

    # docs/CLI.md：表头是「| 命令 | 作用 |」，命令名在**第一个**单元格里。
    # 两个坑：① 一个单元格里可能排多条命令（`play` / `pause` / `toggle` / `stop`）；
    # ② 命令常带参数（`open <媒体> [选项]`、`volume [0..1|+N|-N]`），
    # 所以取"反引号后紧跟的那个词"，且要求它后面不是 `.`/单词字符（排除 `player.info`）。
    sec = section(read(os.path.join(DOCS, "CLI.md")), r"## 2\. 命令总览")
    first_cells = re.findall(r"^\|([^|]*)\|", sec, re.M)
    dm = {c for cell in first_cells
          for c in re.findall(r"`([a-z][a-z0-9-]*)(?![\w.])", cell)}
    if not dm:
        bad.append("docs/CLI.md: 命令总览没解析到命令表")
    elif dm != table:
        bad.append("docs/CLI.md: 表里少 %s；表里多 %s" % (sorted(table - dm), sorted(dm - table)))

    if bad:
        raise Fail("；".join(bad))
    return "%d 个命令，README 与 docs/CLI.md 的表与实现一致" % len(table)


def dep_row(text, name):
    """取 README「依赖与来源」表里某一行（不含表头与分隔行）。

    必须定位到**那一行**再比版本号 —— 只判断"版本串在 README 里出现过"是假绿：
    README 里同一版本号会在 CVE 表、变更记录等处重复出现，改错了依赖表也照样过。
    （这条正是 --selfcheck 抓出来的。）
    """
    m = re.search(r"^\|\s*%s\s*\|([^|]*)\|" % re.escape(name), text, re.M)
    return m.group(1) if m else None


@gate("versions-align", "README 依赖表里的版本 = vendor 头文件 / node_modules 里的真实版本")
def g_versions(verbose):
    # vendor/ 不存在时**必须 SKIP 而不是 FAIL**：托管 CI（Linux）不会有 vendor/，
    # 那里跑门禁的目的是抓文档漂移，不该因为"没下依赖"而报红。
    ff_h = os.path.join(ROOT, "vendor", "ffmpeg", "include", "libavutil", "ffversion.h")
    sdl_h = os.path.join(ROOT, "vendor", "sdl3", "include", "SDL3", "SDL_version.h")
    if not os.path.isfile(ff_h) or not os.path.isfile(sdl_h):
        raise Skip("vendor/ 未准备（先 tools/fetch_deps.py）—— 版本号要从 vendor 头文件读")

    readme = read(README)
    out = []
    # FFmpeg：ffversion.h 的版本串里必须含 README 依赖表里写的那个版本
    ffv = re.search(r'#define\s+FFMPEG_VERSION\s+"([^"]+)"', read(ff_h))
    if not ffv:
        raise Fail("无法从 ffversion.h 解析 FFMPEG_VERSION")
    ffver = re.search(r"(\d+\.\d+\.\d+)", ffv.group(1))
    if not ffver:
        raise Fail("无法从 FFMPEG_VERSION %r 解析版本号" % ffv.group(1))
    row = dep_row(readme, "FFmpeg")
    if row is None:
        raise Fail("README 的依赖表里没有 FFmpeg 行（表结构变了？）")
    if ffver.group(1) not in row:
        raise Fail("vendor 里的 FFmpeg 是 %s，但 README 依赖表那一行写的是 %r"
                   % (ffver.group(1), row.strip()[:80]))
    out.append("FFmpeg %s" % ffver.group(1))

    # SDL3：SDL_version.h ↔ README 依赖表
    sv = read(sdl_h)
    def num(k):
        m = re.search(r"#define\s+%s\s+(\d+)" % k, sv)
        if not m:
            raise Fail("SDL_version.h 里找不到 %s" % k)
        return m.group(1)
    sver = "%s.%s.%s" % (num("SDL_MAJOR_VERSION"), num("SDL_MINOR_VERSION"),
                         num("SDL_MICRO_VERSION"))
    row = dep_row(readme, "SDL3")
    if row is None:
        raise Fail("README 的依赖表里没有 SDL3 行（表结构变了？）")
    if sver not in row:
        raise Fail("vendor 里的 SDL3 是 %s，但 README 依赖表那一行写的是 %r"
                   % (sver, row.strip()[:80]))
    out.append("SDL3 %s" % sver)

    # ABI：player_plugin.h ↔ ARCHITECTURE.md 的 abi 字段
    abi = re.search(r"#define\s+PP_ABI_VERSION\s+(\d+)u?",
                    read(os.path.join(ROOT, "src", "sdk", "player_plugin.h")))
    if not abi:
        raise Fail("找不到 PP_ABI_VERSION")
    arch = read(os.path.join(DOCS, "ARCHITECTURE.md"))
    if '"abi":%s' % abi.group(1) not in arch and '"abi": %s' % abi.group(1) not in arch:
        raise Fail("ARCHITECTURE.md 里没有 abi:%s（PP_ABI_VERSION=%s）" % (abi.group(1), abi.group(1)))
    out.append("PP_ABI_VERSION %s" % abi.group(1))

    # Cordis：node_modules 的实际版本必须出现在 README 的依赖表里
    pkg = os.path.join(ROOT, "host-js", "node_modules", "cordis", "package.json")
    if os.path.isfile(pkg):
        v = json.loads(read(pkg)).get("version")
        row = dep_row(readme, "Cordis")
        if row is None:
            raise Fail("README 的依赖表里没有 Cordis 行（表结构变了？）")
        if v and v not in row:
            raise Fail("node_modules 里的 cordis 是 %s，但 README 依赖表那一行写的是 %r"
                       % (v, row.strip()[:80]))
        out.append("Cordis %s" % v)
    else:
        out.append("Cordis（node_modules 未安装，未核对）")
    return "；".join(out) + " —— 与 README 依赖表 / ARCHITECTURE.md 声明一致"


# ============================================================================
# 新鲜度类
# ============================================================================

@gate("third-party-notices", "THIRD_PARTY_NOTICES.md 与当前依赖/许可证据一致")
def g_notices(verbose):
    try:
        import gen_third_party_notices as notices
    except ImportError as exc:
        raise Fail("无法 import tools/gen_third_party_notices.py: %s" % exc)
    # 生成器需要 vendor 头文件读版本；缺失时 SKIP（Linux CI 上不会有 vendor/）。
    if not os.path.isfile(notices.FFVERSION_H) or not os.path.isfile(notices.SDL_VERSION_H):
        raise Skip("vendor/ 未准备 —— 声明里的版本号要从 vendor 头文件读")
    out = notices.OUT
    if not os.path.isfile(out):
        raise Fail("THIRD_PARTY_NOTICES.md 不存在，请先运行 tools/gen_third_party_notices.py")
    cur = read(out)
    try:
        want = notices.build_document()
    except SystemExit as exc:
        # 生成器用 SystemExit 报"事实来源读不到"；在门禁里那是 SKIP 不是崩溃
        raise Skip("生成器缺少事实来源: %s" % exc)
    if cur != want:
        raise Fail("THIRD_PARTY_NOTICES.md 已过期（依赖或许可证据变了）——"
                   "运行 tools/gen_third_party_notices.py 重新生成")
    # 结论段的要点必须在，否则"生成了但没结论"也能过
    for kw in ("GPL v3", "Corresponding Source", "LICENSE"):
        if kw not in cur:
            raise Fail("声明里缺少关键结论 %r" % kw)
    return "与当前依赖一致，且含 GPL v3 义务说明"


# ============================================================================
# 文档类
# ============================================================================

@gate("doc-links", "所有 Markdown 里的相对链接都能解析到真实文件")
def g_doc_links(verbose):
    bad, total = [], 0
    for rel in walk_files((".md",)):
        p = os.path.join(ROOT, rel)
        base = os.path.dirname(p)
        for label, target in re.findall(r"\[([^\]]*)\]\(([^)\s]+)\)", read(p)):
            if re.match(r"^[a-zA-Z][a-zA-Z0-9+.-]*:", target) or target.startswith("#"):
                continue          # 外链、锚点、mailto 等
            path_part = target.split("#", 1)[0]
            if not path_part:
                continue
            total += 1
            if not os.path.exists(os.path.normpath(os.path.join(base, path_part))):
                bad.append("%s -> %s" % (rel, target))
    if bad:
        raise Fail("；".join(bad[:8]) + (" …共 %d 处" % len(bad) if len(bad) > 8 else ""))
    return "%d 条相对链接全部可解析" % total


@gate("docs-index", "docs/ 下每份文档都被 docs/README.md 收录")
def g_docs_index(verbose):
    index = os.path.join(DOCS, "README.md")
    if not os.path.isfile(index):
        raise Fail("docs/README.md 不存在（文档索引）")
    listed = set(re.findall(r"\]\(([^)#]+\.md)\)", read(index)))
    listed |= {os.path.basename(x) for x in listed}
    on_disk = sorted(f for f in os.listdir(DOCS) if f.endswith(".md") and f != "README.md")
    missing = [f for f in on_disk if f not in listed]
    if missing:
        raise Fail("以下文档没有被 docs/README.md 收录: %s" % missing)
    return "%d 份文档全部收录" % len(on_disk)


def _tree_tokens(line):
    """从目录树的某一行取"像路径"的片段。

    处理四种写法：
      · 单个名字            src/
      · 一行两个名字         config.cpp/h  util.cpp/h      配置持久化 / 日志
      · 带注释的名字         player.cpp/h    播放内核
      · 合并缩写            app.cpp/h  ->  app.cpp + app.h

    分隔符的判断依据是 README 的对齐习惯：**名字之间是 1~2 个空格**，
    名字与行尾说明之间是**3 个以上空格**（说明被对齐到同一列）。
    所以先用 `\\s{3,}` 切掉说明，再用空白拆分名字 —— 用 `\\s{2,}` 会把
    `config.cpp/h  util.cpp/h` 里的第二个名字当成说明而漏掉。
    """
    s = re.sub(r"^[\s│├└─]+", "", line).strip()
    if not s:
        return []
    s = re.split(r"\s{3,}", s)[0]          # 去掉行尾说明
    out = []
    for tok in s.split():
        # 名字后面可能紧跟括号补充（例如 `AGENTS.md（CLAUDE.md 指向它）`），先切掉；
        # 切完仍带括号的残余（如 `指向它）`）直接丢弃 —— 路径里不会有括号。
        tok = re.split(r"[（(]", tok)[0].strip("★").strip()
        if not tok or re.search(r"[（()）]", tok):
            continue
        m = re.fullmatch(r"([A-Za-z0-9_.\-]+)\.([a-z0-9]+)/([a-z0-9]+)", tok)
        if m:                               # app.cpp/h 这种合并写法
            out += ["%s.%s" % (m.group(1), m.group(2)), "%s.%s" % (m.group(1), m.group(3))]
            continue
        out.append(tok)
    return out


@gate("readme-tree", "README 目录结构里列出的文件/目录都真实存在")
def g_readme_tree(verbose):
    text = read(README)
    m = re.search(r"## 目录结构\s*```(.*?)```", text, re.S)
    if not m:
        raise Fail("README 里找不到「## 目录结构」代码块")
    block = m.group(1)

    # basename 索引：文件与目录都收（README 的树是按缩进表示层级的，
    # 完整路径重建太脆，这里只保证"名字还在"——足以拦住改名与删除）
    index = set()
    for cur, dirs, files in os.walk(ROOT):
        dirs[:] = [d for d in dirs if d not in EXCLUDE_DIRS]
        index.update(dirs)
        index.update(files)
    root_name = os.path.basename(ROOT)

    missing, checked = [], 0
    for line in block.splitlines():
        for tok in _tree_tokens(line):
            name = tok.rstrip("/")
            base = os.path.basename(name)
            if not base or base == root_name:
                continue                       # 树根（pluginplayer/）本身不算
            if "." not in base and "/" not in name:
                continue                       # 纯目录名（如 src）单独出现时不查
            checked += 1
            if base not in index:
                missing.append(name)
    if missing:
        raise Fail("README 目录结构里列了但仓库中找不到: %s" % sorted(set(missing)))
    return "目录结构里 %d 个条目全部存在（按 basename 核对）" % checked


# ============================================================================
# 自检：证明门禁"拦得住"
# ============================================================================
#
# 一条永远为绿的门禁等于没有 —— 这个项目自己的 README 就批评过这种写法
# （"早期版本只断言 commands > 0，收集了泄漏证据却从不断言，等于这项检查永远为绿"）。
# 所以每条门禁都要能回答："我把它要防的那件事做出来，它会不会红？"
#
# 做法：把仓库复制到临时目录（排除 dist/vendor 等重目录，另补两个必需的 vendor 头文件），
# 对每条门禁施加一次**针对性的破坏**，然后断言该门禁在沙箱里 FAIL、且破坏前是 PASS。
#
# 参考 deepseek-harness-master 的 AGENTS.md：把可机械检查的不变量接到一条会实际执行的
# 顶层门禁上，并为每条改动的验收路径证明它能拒绝一个非法输入。

SANDBOX_EXTRA = [
    "vendor/ffmpeg/include/libavutil/ffversion.h",
    "vendor/sdl3/include/SDL3/SDL_version.h",
    # 这两份 package.json 决定 JS 依赖的版本与许可，第三方声明与 versions 门禁都要读它们。
    # node_modules 整体在沙箱里被排除（体积），所以单独补这两个小文件。
    "host-js/node_modules/cordis/package.json",
    "host-js/node_modules/@cordisjs/plugin-loader/package.json",
]

# 沙箱里不拷的目录（体积大或与门禁无关）
SANDBOX_SKIP = EXCLUDE_DIRS | {"dist", "logs"}


def _subst(rel, old, new, count=1):
    def apply(root):
        p = os.path.join(root, rel)
        s = open(p, "r", encoding="utf-8", errors="replace").read()
        if old not in s:
            raise RuntimeError("沙箱破坏失败：%s 里找不到 %r" % (rel, old))
        open(p, "w", encoding="utf-8", newline="\n").write(s.replace(old, new, count))
    return apply


def _subst_bytes(rel, old, new, count=1):
    """按**字节**替换。文本模式读会把 \\r\\n 折成 \\n，改行尾的用例必须走这里。"""
    def apply(root):
        p = os.path.join(root, rel)
        data = open(p, "rb").read()
        if old not in data:
            raise RuntimeError("沙箱破坏失败：%s 里找不到 %r" % (rel, old))
        open(p, "wb").write(data.replace(old, new, count))
    return apply


def _insert_after(rel, anchor, text):
    def apply(root):
        p = os.path.join(root, rel)
        s = open(p, "r", encoding="utf-8", errors="replace").read()
        i = s.find(anchor)
        if i < 0:
            raise RuntimeError("沙箱破坏失败：%s 里找不到锚点 %r" % (rel, anchor))
        j = s.index("\n", i) + 1
        open(p, "w", encoding="utf-8", newline="\n").write(s[:j] + text + s[j:])
    return apply


def _drop_line(rel, needle):
    def apply(root):
        p = os.path.join(root, rel)
        lines = open(p, "r", encoding="utf-8", errors="replace").read().splitlines(True)
        kept = [l for l in lines if needle not in l]
        if len(kept) == len(lines):
            raise RuntimeError("沙箱破坏失败：%s 里没有含 %r 的行" % (rel, needle))
        open(p, "w", encoding="utf-8", newline="\n").write("".join(kept))
    return apply


def _append(rel, text):
    def apply(root):
        p = os.path.join(root, rel)
        with open(p, "a", encoding="utf-8", newline="\n") as fp:
            fp.write(text)
    return apply


def _addfile(rel, data):
    def apply(root):
        p = os.path.join(root, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as fp:
            fp.write(data)
    return apply


def _rename(rel, newname):
    def apply(root):
        p = os.path.join(root, rel)
        if not os.path.exists(p):
            raise RuntimeError("沙箱破坏失败：%s 不存在" % rel)
        os.rename(p, os.path.join(os.path.dirname(p), newname))
    return apply


def _pick(root, pattern):
    """在沙箱里按 glob 找一个存在的文件（用于 dist/ 里的用例）。"""
    import glob
    hits = sorted(glob.glob(os.path.join(root, pattern)))
    if not hits:
        raise RuntimeError("沙箱破坏失败：找不到匹配 %s 的文件" % pattern)
    return hits[0]


def _corrupt(pattern):
    """改掉文件第一个字节 —— 用来验证"真的重算了哈希"而不是只看文件在不在。"""
    def apply(root):
        p = _pick(root, pattern)
        data = bytearray(open(p, "rb").read())
        if not data:
            raise RuntimeError("沙箱破坏失败：%s 是空文件" % p)
        data[0] ^= 0xFF
        open(p, "wb").write(bytes(data))
    return apply


def _delete(pattern):
    def apply(root):
        os.remove(_pick(root, pattern))
    return apply


def selfcheck_cases():
    return [
        ("bat-ascii-crlf", "给一个 .bat 塞进非 ASCII 字节（cmd.exe 会乱码）",
         _addfile("bad_launcher.bat", b"@echo off\r\nrem \xd6\xd0\xce\xc4\r\n")),
        ("bat-ascii-crlf", "把 .bat 的行尾折成裸 LF",
         _subst_bytes("run.bat", b"\r\n", b"\n", count=99)),
        ("plugin-command-count", "把 tests/cli_check.py 的命令数断言改错",
         _subst("tests/cli_check.py", "== 20", "== 14")),
        ("native-plugin-set", "把 build.py 的插件表里删掉一个",
         _subst("build.py", '"osd_stats", ', "")),
        ("js-plugin-count", "把 README 的 JS 插件数改错",
         _subst("README.md", "3 个 JS 插件", "4 个 JS 插件")),
        ("arg-bounds-align", "把 CLI 的窗口最小边改成 161（与 app.h 不符）",
         _subst("tools/pp_cli.py", "MIN_WIN, MAX_WIN = 160, 8192",
                "MIN_WIN, MAX_WIN = 161, 8192")),
        ("rpc-methods-align", "从 ARCHITECTURE.md 的方法一览里删掉 app.quit",
         _subst("docs/ARCHITECTURE.md", "、`app.quit`。", "。")),
        ("exit-codes-align", "从 CLI.md 的退出码表里删掉 130 那一行",
         _drop_line("docs/CLI.md", "| 130 | 被 Ctrl-C 中断")),
        # 必须插在「## 2. 命令总览」的表里 —— 追加到文件末尾门禁看不到，
        # 那样这个用例就成了假绿（第一版就是这么写的，被自检自己抓到了）。
        ("cli-commands-align", "往 CLI.md 的命令表里塞一个不存在的命令",
         _insert_after("docs/CLI.md", "| `version` | 版本与环境摘要 |",
                       "| `nonexistent-cmd` | 演示用 |\n")),
        ("versions-align", "把 README 依赖表里的 FFmpeg 版本号改错",
         _subst("README.md", "| FFmpeg | 9.0.1", "| FFmpeg | 9.0.2")),
        ("third-party-notices", "手工改动生成物（应被判过期）",
         _append("THIRD_PARTY_NOTICES.md", "\n手工加的一行\n")),
        ("doc-links", "在 README 里加一个指向不存在文件的链接",
         _append("README.md", "\n[不存在](NO_SUCH_FILE_XYZ.md)\n")),
        ("docs-index", "加一份没被 docs/README.md 收录的文档",
         _addfile("docs/orphan.md", b"# Orphan\n")),
        # 必须插进**已有**的那棵目录树里；另起一个 ``` 块会被 re.search 忽略。
        ("readme-tree", "在 README 的目录结构里加一个不存在的文件",
         _subst("README.md", "│  │  ├─ player.cpp/h",
                "│  │  ├─ nonexistent_thing.cpp\n│  │  ├─ player.cpp/h")),
        ("plugin-manifest", "把已登记插件改 1 个字节（必须靠重算哈希发现）",
         _corrupt("dist/plugins/*.dll")),
        ("release-payload", "从 dist/ 里删掉一个文件（载荷数量就不对了）",
         _delete("dist/pluginplayer.exe")),
        ("portable-modules", "在可移植模块的空行处塞一句 Windows API 调用",
         _insert_after("src/core/config.cpp", '#include "util.h"',
                       'void pp_probe(void) { DWORD x = 0; (void)x; }\n')),
        ("ci-entrypoints", "把流水线里引用的工具名改成不存在的一个",
         _subst(".workflow/pluginplayer-ci.yml", "tools/ci.py", "tools/ci_RENAMED.py")),
        ("no-credentials", "往一个源文件里塞一个硬编码令牌",
         _append("tools/pp_cli.py", "\nGITEE_TOKEN = \"deadbeefdeadbeefdeadbeefdeadbeef\"\n")),
    ]


def _placeholder_dist(dst):
    """在沙箱里造一份 dist/ 的占位副本：**文件名与大小**与真实产物一致。

    为什么要这么绕：release-payload / third-party-notices / plugin-manifest 三条门禁
    只有在 dist/ 存在时才有意义，可真拷 127 MB 太慢。于是用 seek+write 造稀疏文件
    （瞬间完成），内容无所谓 —— 但两处必须跟着修正，否则沙箱基线会误红：

      1. `plugins/manifest.sha256` 要按占位 DLL 重算哈希；
      2. 重算后的清单**大小**要与真实清单一致（第三方声明里列出了它的大小），
         所以写完后补齐到原始字节数。
    """
    src_dist = os.path.join(ROOT, "dist")
    if not os.path.isdir(src_dist):
        return
    real = []
    manifest_size = 0
    for cur, _dirs, fs in os.walk(src_dist):
        for f in fs:
            p = os.path.join(cur, f)
            rel = os.path.relpath(p, src_dist)
            if os.path.basename(rel) == "manifest.sha256":
                manifest_size = os.path.getsize(p)
                continue
            real.append((rel, os.path.getsize(p)))
    for rel, size in real:
        target = os.path.join(dst, "dist", rel)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(target, "wb") as fp:
            if size:
                fp.seek(size - 1)
                fp.write(b"\0")

    pdir = os.path.join(dst, "dist", "plugins")
    if os.path.isdir(pdir):
        lines = ["# sandbox placeholder manifest", ""]
        for f in sorted(os.listdir(pdir)):
            if f.lower().endswith(".dll"):
                lines.append("%s  %s" % (sha256(os.path.join(pdir, f)), f))
        blob = ("\n".join(lines) + "\n").encode("utf-8")
        if manifest_size and len(blob) < manifest_size:
            blob += b"\n" + b" " * (manifest_size - len(blob) - 1)
        with open(os.path.join(pdir, "manifest.sha256"), "wb") as fp:
            fp.write(blob)


def _sandbox_copy(dst):
    for cur, dirs, files in os.walk(ROOT):
        dirs[:] = [d for d in dirs if d not in SANDBOX_SKIP]
        rel = os.path.relpath(cur, ROOT)
        target = dst if rel == "." else os.path.join(dst, rel)
        os.makedirs(target, exist_ok=True)
        for f in files:
            src = os.path.join(cur, f)
            try:
                import shutil
                shutil.copy2(src, os.path.join(target, f))
            except OSError:
                pass
    # 补上门禁必需、但被 SANDBOX_SKIP 排除的少数文件
    import shutil
    for rel in SANDBOX_EXTRA:
        src = os.path.join(ROOT, rel)
        if os.path.isfile(src):
            d = os.path.join(dst, os.path.dirname(rel))
            os.makedirs(d, exist_ok=True)
            shutil.copy2(src, os.path.join(dst, rel))
    _placeholder_dist(dst)


def _run_gate_in(root, gate_name):
    """在 root 里跑单条门禁，返回 (status, detail)。"""
    p = subprocess.run([sys.executable, "-X", "utf8", os.path.join(root, "tools", "verify.py"),
                        "--only", gate_name, "--json"],
                       cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        doc = json.loads(p.stdout.decode("utf-8", "replace"))
    except ValueError:
        return "ERROR", p.stdout.decode("utf-8", "replace")[:200]
    if not doc.get("gates"):
        return "ERROR", "没有返回门禁结果"
    return doc["gates"][0]["status"], doc["gates"][0]["detail"]


def run_selfcheck():
    import shutil
    import tempfile
    print("=" * 78)
    print("verify.py --selfcheck：证明每条门禁都能拦住它要防的事")
    print("=" * 78)

    with tempfile.TemporaryDirectory(prefix="pp_verify_") as tmp:
        pristine = os.path.join(tmp, "pristine")
        os.makedirs(pristine)
        _sandbox_copy(pristine)

        results = []
        for gate_name, why, apply in selfcheck_cases():
            base_status, base_detail = _run_gate_in(pristine, gate_name)
            if base_status not in ("PASS", "SKIP"):
                results.append((gate_name, "FAIL",
                                "沙箱基线就不是 PASS/SKIP（%s: %s）—— 沙箱没有正确复现仓库"
                                % (base_status, base_detail[:150])))
                print("[FAIL] %-22s 沙箱基线 %s: %s" % (gate_name, base_status, base_detail[:90]))
                continue
            if base_status == "SKIP":
                results.append((gate_name, "SKIP", "沙箱里前提不满足，无法验证: %s" % base_detail[:80]))
                print("[SKIP] %-22s %s" % (gate_name, base_detail[:80]))
                continue
            work = os.path.join(tmp, "work_%d" % len(results))
            shutil.copytree(pristine, work)
            try:
                apply(work)
            except RuntimeError as exc:
                results.append((gate_name, "FAIL", str(exc)))
                print("[FAIL] %-22s %s" % (gate_name, exc))
                continue
            st, detail = _run_gate_in(work, gate_name)
            ok = st == "FAIL"
            results.append((gate_name, "PASS" if ok else "FAIL",
                            ("破坏了「%s」→ 门禁如期报红" % why) if ok
                            else ("破坏了「%s」但门禁仍是 %s: %s" % (why, st, detail[:150]))))
            print("[%s] %-22s %s" % ("PASS" if ok else "FAIL", gate_name,
                                     ("拦住了：%s" % why) if ok else ("没拦住！状态 %s" % st)))

        npass = sum(1 for _g, s, _d in results if s == "PASS")
        nfail = sum(1 for _g, s, _d in results if s == "FAIL")
        nskip = sum(1 for _g, s, _d in results if s == "SKIP")
        print()
        print("==== 门禁自检: %d/%d 通过，跳过 %d 项，失败 %d 项 ====" %
              (npass, npass + nfail, nskip, nfail))
        if nfail:
            print("（有门禁拦不住它声称要防的事 —— 那它就等于装饰品，必须修）")
        return 1 if nfail else 0


# ============================================================================
# 运行
# ============================================================================

def main():
    ap = argparse.ArgumentParser(description="PluginPlayer 仓库机械门禁")
    ap.add_argument("--list", action="store_true", help="列出门禁后退出")
    ap.add_argument("--only", default="", help="逗号分隔，只跑指定门禁")
    ap.add_argument("--json", action="store_true", help="机器可读输出")
    ap.add_argument("--verbose", action="store_true", help="打印每条检查的依据")
    ap.add_argument("--fix", action="store_true",
                    help="把可自动修复的生成物重新生成（目前只有 THIRD_PARTY_NOTICES.md）")
    ap.add_argument("--selfcheck", action="store_true",
                    help="对每条门禁做一次故意破坏，断言它必须失败（证明门禁拦得住）")
    args = ap.parse_args()

    if args.selfcheck:
        return run_selfcheck()

    if args.list:
        for name, desc, _fn in GATES:
            print("%-24s %s" % (name, desc))
        return 0

    only = {s.strip() for s in args.only.split(",") if s.strip()}
    selected = [(n, d, f) for n, d, f in GATES if not only or n in only]
    unknown = only - {n for n, _d, _f in GATES}
    if unknown:
        print("[ERR] 未知门禁: %s（用 --list 查看）" % ", ".join(sorted(unknown)), file=sys.stderr)
        return 2

    results = []
    for name, desc, fn in selected:
        try:
            detail = fn(args.verbose)
            status, detail = "PASS", detail or ""
        except Skip as exc:
            status, detail = "SKIP", str(exc)
        except Fail as exc:
            status, detail = "FAIL", str(exc)
        except Exception as exc:                      # noqa: BLE001 - 门禁自身出错也算失败
            status = "FAIL"
            detail = "门禁异常 %s: %s" % (type(exc).__name__, exc)
        results.append({"gate": name, "status": status, "detail": detail, "desc": desc})

    npass = sum(1 for r in results if r["status"] == "PASS")
    nfail = sum(1 for r in results if r["status"] == "FAIL")
    nskip = sum(1 for r in results if r["status"] == "SKIP")

    if args.json:
        print(json.dumps({"ok": nfail == 0, "pass": npass, "fail": nfail, "skip": nskip,
                          "gates": results}, ensure_ascii=False, indent=2))
    else:
        width = max(len(r["gate"]) for r in results)
        for r in results:
            print("[%s] %-*s %s" % (r["status"], width, r["gate"], r["detail"]))
            if args.verbose and r["status"] != "SKIP":
                print("         (%s)" % r["desc"])
        print()
        print("==== 机械门禁: %d/%d 通过，跳过 %d 项，失败 %d 项 ====" %
              (npass, npass + nfail, nskip, nfail))
        if nskip:
            print("（跳过项需要先 build.py 或 tools/fetch_deps.py 准备前置条件，不计入通过数）")

    if args.fix and nfail:
        import gen_third_party_notices as notices
        notices.main_quiet = True
        with open(notices.OUT, "w", encoding="utf-8", newline="\n") as fp:
            fp.write(notices.build_document())
        print("\n[--fix] 已重新生成 THIRD_PARTY_NOTICES.md，请重跑门禁确认")

    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main())
