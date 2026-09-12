#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
pp_cli.py — PluginPlayer 命令行接口（cmd / PowerShell / bash 通用）
=================================================================

把 G:\\local\\pluginplayer 变成一个"能在命令提示符里直接使唤"的工具：

    pluginplayer open 影片.mp4 --hidden --rpc-port 7911
    pluginplayer status --json
    pluginplayer seek +30
    pluginplayer cmd effect.set gray
    pluginplayer events --seconds 5
    pluginplayer doctor

设计参考了 deepseek-harness-master/apps/cli（`dsh` 启动器）的几条原则，
并在本项目里落地：

  1) **语法归属清晰**：启动器只解析自己认识的 flag，`--` 之后的一切原样透传
     给被启动的进程（`open -- --reload 2`）。第一个不认识的 token 立即报错
     （exit 2），不会"猜"。
  2) **模式分离**：本地编排（open/selftest/build/doctor/install-cli）与控制通道
     （status/play/seek/cmd/events/rpc）是两组命令，前者直接继承子进程 stdio，
     后者走 JSON-RPC。
  3) **机器可读**：每个命令都支持 `--json`，脚本可解析；人类模式输出对齐过。
  4) **失败要说清楚**：分级退出码（见下），错误信息带"怎么修"。

零第三方依赖：只用 Python 标准库。

退出码约定
----------
    0   成功
    1   一般失败（子进程非零退出等）
    2   用法错误（未知命令/选项、参数非法、缺参数）
    3   控制通道连不上（没启动播放器 / --rpc-port 0 / 端口不对）
    4   控制通道认证失败（令牌不对/过期；每次启动都会换新令牌）
    5   远端返回错误（应答 ok:false）
    6   环境或依赖缺失（编译器、vendor、dist/ 未构建等）
    130 被 Ctrl-C 中断
转发类命令（open/selftest/build/plugins/manifest/host-js）原样返回子进程退出码。

用法速查
--------
    F:\\Python314\\python.exe tools\\pp_cli.py doctor
    F:\\Python314\\python.exe tools\\pp_cli.py install-cli     # 之后可直接用 pluginplayer
"""

import hashlib
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time
import unicodedata

# ============================================================================
# 0. 常量与路径
# ============================================================================

CLI_VERSION = "1.0.0"

# 仓库根：优先 PP_ROOT，其次本文件所在目录的上一级。
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.environ.get("PP_ROOT", "").strip() or os.path.dirname(HERE))

DIST = os.path.join(ROOT, "dist")
EXE = os.path.join(DIST, "pluginplayer.exe")
PLUGIN_DIR = os.path.join(DIST, "plugins")
DEFAULT_TOKEN_FILE = os.path.join(DIST, "config", "rpc-token.txt")
BUILD_PY = os.path.join(ROOT, "build.py")
FETCH_DEPS = os.path.join(ROOT, "tools", "fetch_deps.py")
HOST_JS = os.path.join(ROOT, "host-js", "src", "index.mjs")
FFMPEG = os.path.join(ROOT, "vendor", "ffmpeg", "bin", "ffmpeg.exe")
VENDOR = os.path.join(ROOT, "vendor")

DEFAULT_PORT = 7911
DEFAULT_SELFTEST_FRAMES = 90      # 与 selftest.bat 保持一致，README 里 10/10 那次就是这个值
DEFAULT_SAMPLE = os.path.join(DIST, "test_assets", "sample.mp4")

# dist/ 必须随包的运行时 DLL（与 build.py 的打包逻辑、README 的 17 个文件一致）
RUNTIME_DLLS = [
    "SDL3.dll", "avutil-61.dll", "avcodec-63.dll", "avformat-63.dll",
    "swscale-10.dll", "swresample-7.dll",
    "libwinpthread-1.dll", "libgcc_s_seh-1.dll", "libstdc++-6.dll",
]

# 与 src/core/main.cpp、src/core/app.h 完全一致的数值边界。
# 故意写成具名常量而不是散在调用点的字面量：tools/verify.py 的
# arg-bounds-align 门禁会把这些名字与 C++ 侧一一比对，改名/改值都会当场被拦。
MIN_WIN, MAX_WIN = 160, 8192           # app.h: kMinWindowSize / kMaxWindowSize
MIN_FRAMES, MAX_FRAMES = 1, 1000000    # main.cpp: --frames
MIN_VOLUME, MAX_VOLUME = 0.0, 1.0      # main.cpp: --volume
MIN_LOG_LEVEL, MAX_LOG_LEVEL = 0, 4    # main.cpp: --log-level
MIN_RELOAD, MAX_RELOAD = 0, 9999       # main.cpp: --reload
MIN_TOKEN_LEN = 16                     # main.cpp: --rpc-token 至少 16 字符
# 只属于 CLI 的边界（C++ 侧没有对应项，见 docs/CLI.md「已知边界」）
MIN_START, MAX_START = 0.0, 1e9        # --start
MIN_SPEED, MAX_SPEED = 0.01, 100.0     # speed
MAX_PORT = 65535                       # main.cpp: --rpc-port
SERVER_MAX_CLIENTS = 8                 # src/core/control.h
SERVER_MAX_LINE_BYTES = 4 * 1024 * 1024

CMD_NAME = "pluginplayer"
SHIM_MARKER = "PP_CLI_SHIM"

# 退出码
E_OK, E_FAIL, E_USAGE, E_CONN, E_AUTH, E_REMOTE, E_ENV, E_INTR = 0, 1, 2, 3, 4, 5, 6, 130


# ============================================================================
# 1. 标准输出
# ============================================================================

def setup_stdio():
    """Windows 控制台是 GBK（cp936），重定向到文件/管道时才该用 UTF-8。

    cmd.exe 按当前代码页解读字节：若强行把 TTY 上的 stdout 改成 UTF-8，
    中文会直接乱码；反之重定向到文件时又必须用 UTF-8 才可读。
    所以这里按 isatty() 分流（只加 errors='replace' 兜底）。
    """
    for name in ("stdout", "stderr"):
        s = getattr(sys, name, None)
        if s is None:
            continue
        try:
            if s.isatty():
                s.reconfigure(errors="replace")
            else:
                s.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError, OSError):
            pass


def out(msg=""):
    try:
        print(msg)
    except (UnicodeEncodeError, OSError):
        try:
            print(msg.encode("ascii", "replace").decode("ascii"))
        except OSError:
            pass


def err(msg):
    try:
        print(msg, file=sys.stderr)
    except (UnicodeEncodeError, OSError):
        pass


class CliError(Exception):
    """带退出码的受控失败。"""

    def __init__(self, message, code=E_FAIL, hint=""):
        super().__init__(message)
        self.message = message
        self.code = code
        self.hint = hint


class UsageError(CliError):
    def __init__(self, message, hint=""):
        super().__init__(message, E_USAGE, hint)


def jdump(obj):
    return json.dumps(obj, ensure_ascii=False, indent=2, sort_keys=False)


def disp_width(s):
    """终端显示宽度：中日韩全角字符占 2 列。用它对齐表格，
    否则中文列宽会比 len() 算出来的多一倍，看起来就是错位的。"""
    w = 0
    for ch in s:
        w += 2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1
    return w


def pad(s, width):
    return s + " " * max(0, width - disp_width(s))


# ---- 终端输出（人类模式）。全部走 ASCII 标记，避免非 GBK 字符在 cmd 里变问号 ----

class Log:
    def __init__(self):
        self.quiet = False
        self.json = False
        self.verbose = False

    def info(self, msg):
        if not self.quiet and not self.json:
            out(msg)

    def ok(self, msg):
        if not self.quiet and not self.json:
            out("[OK] " + msg)

    def warn(self, msg):
        if not self.quiet and not self.json:
            out("[WARN] " + msg)

    def fail(self, msg):
        if not self.json:
            err("[ERR] " + msg)

    def debug(self, msg):
        if self.verbose and not self.json:
            err("[dbg] " + msg)

    def result(self, obj):
        """--json 模式下的结果输出（单份对象；events 用 JSONL，不走这里）。"""
        if self.json:
            out(jdump(obj))


LOG = Log()


# ============================================================================
# 2. 参数工具
# ============================================================================

COMMON_FLAGS_LONG = ("--port", "--token", "--token-file", "--timeout", "--json",
                     "--quiet", "--verbose")
COMMON_FLAGS = COMMON_FLAGS_LONG


def expand_equals(argv):
    """把 `--k=v` 规范成 `--k v`，后面的解析就只需要处理一种形式。"""
    out_argv = []
    for tok in argv:
        if tok.startswith("--") and "=" in tok and not tok.startswith("---"):
            k, v = tok.split("=", 1)
            if k != "--":
                out_argv += [k, v]
                continue
        out_argv.append(tok)
    return out_argv


_UNSET = object()


def take(args, i, flag, usage):
    """取一个需要值的选项。缺值一律 exit 2（绝不猜）。"""
    if i + 1 >= len(args):
        raise UsageError("%s 缺少参数" % flag, usage)
    return args[i + 1]


def parse_number(text, flag, lo, hi, kind="int", usage=""):
    """严格数值校验：整串必须是合法数字，且落在 [lo, hi] 内。

    只认 **ASCII 数字**：Python 的 `\\d` 与 `float()` 默认接受 Unicode 数字
    （全角「１２３」也能解析成 123），而播放器侧的 `sscanf` 是 ASCII 的 ——
    若在这里放行，CLI 说"参数合法"但 exe 会 exit 2，两侧判定不一致。
    （tests/python/test_pp_cli.py 有一条用例钉这个。）
    """
    t = text.strip()
    if not all(ord(c) < 128 for c in t):
        raise UsageError("非法的 %s 值: %s（只接受 ASCII 数字）" % (flag, text), usage)
    try:
        if kind == "int":
            if not re.fullmatch(r"[+-]?[0-9]+", t):
                raise ValueError
            v = int(t)
        else:
            v = float(t)
            if v != v or v in (float("inf"), float("-inf")):
                raise ValueError
    except (ValueError, OverflowError):
        raise UsageError("非法的 %s 值: %s" % (flag, text), usage)
    if v < lo or v > hi:
        raise UsageError("非法的 %s 值: %s（需要 %s..%s）" % (flag, text, lo, hi), usage)
    return v


def parse_wxh(text, usage=""):
    m = re.fullmatch(r"\s*([0-9]+)\s*[xX]\s*([0-9]+)\s*", text or "")
    if not m:
        raise UsageError("非法的 --size 值: %s（需要 WxH 形式，例如 1280x720）" % text, usage)
    w, h = int(m.group(1)), int(m.group(2))
    if not (MIN_WIN <= w <= MAX_WIN) or not (MIN_WIN <= h <= MAX_WIN):
        raise UsageError("非法的 --size 值: %s（宽高都在 %d..%d 之间）" % (text, MIN_WIN, MAX_WIN),
                         usage)
    return w, h


class Opts:
    """全局选项（`--port/--token/--json/...`）。每个命令都可解析这一组。"""

    def __init__(self):
        self.port = DEFAULT_PORT
        self.token = ""
        self.token_file = ""
        self.timeout = 10.0
        self.json = False
        self.quiet = False
        self.verbose = False

    @property
    def token_path(self):
        return self.token_file or DEFAULT_TOKEN_FILE

    def resolve_token(self):
        """令牌来源顺序（与 tools/rpc_client.py 一致，便于排查）：
        1) --token  2) PP_RPC_TOKEN  3) --token-file/默认令牌文件"""
        if self.token.strip():
            return self.token.strip()
        env = os.environ.get("PP_RPC_TOKEN", "").strip()
        if env:
            return env
        path = self.token_path
        try:
            with open(path, "r", encoding="utf-8") as fp:
                t = fp.read().strip()
        except OSError:
            raise CliError(
                "找不到控制通道令牌文件: %s" % path, E_AUTH,
                "先用 `%s open <媒体> --rpc-port %d` 启动播放器（启动时生成该文件），\n"
                "       或用 --token <hex> / 环境变量 PP_RPC_TOKEN 指定。" % (CMD_NAME, self.port))
        if not t:
            raise CliError("令牌文件为空: %s" % path, E_AUTH)
        return t

    def apply_stream_flags(self):
        LOG.json = self.json
        LOG.quiet = self.quiet
        LOG.verbose = self.verbose


def parse_common(args, i, o, usage):
    """就地消费全局选项；返回下一个未消费的下标。"""
    while i < len(args):
        a = args[i]
        if a in ("--port", "-p"):
            o.port = parse_number(take(args, i, a, usage), a, 0, MAX_PORT, usage=usage)
            i += 2
        elif a == "--token":
            o.token = take(args, i, a, usage)
            i += 2
        elif a == "--token-file":
            o.token_file = take(args, i, a, usage)
            i += 2
        elif a == "--timeout":
            o.timeout = parse_number(take(args, i, a, usage), a, 0.1, 3600.0, kind="float",
                                     usage=usage)
            i += 2
        elif a == "--json":
            o.json = True
            i += 1
        elif a in ("--quiet", "-q"):
            o.quiet = True
            i += 1
        elif a in ("--verbose", "-v"):
            o.verbose = True
            i += 1
        else:
            return i
    return i


def strip_common(args, opts, usage):
    """把命令后面的全局选项也摘出来，这样两种写法等价：

        pluginplayer --json status
        pluginplayer status --json

    `--` 之后一律不再解析（原样留给命令自己处理），因此需要"字面上就是这个
    选项"的场合（例如插件命令真要收一个 --json 参数）写 `-- --json` 即可。
    """
    kept, i = [], 0
    while i < len(args):
        a = args[i]
        if a == "--":
            kept += args[i:]
            break
        if a in COMMON_FLAGS_LONG or a in ("-p", "-q", "-v"):
            i = parse_common(args, i, opts, usage)
            continue
        kept.append(a)
        i += 1
    return kept


# ============================================================================
# 3. 控制通道客户端
# ============================================================================

class RpcClosed(Exception):
    pass


class RpcClient:
    """换行分隔 JSON over TCP，强制令牌认证（协议见 src/core/control.h）。

    服务端规则（决定了客户端必须先 auth）：
      · 第一条消息必须是 {"id":1,"method":"auth","params":{"token":"..."}}
      · 认证前任何方法都返回错误，认证失败立即断开
      · 事件只推给已认证连接
    """

    def __init__(self, opts, on_event=None):
        self.opts = opts
        self.on_event = on_event
        self.next_id = 1
        self.buf = b""
        try:
            self.sock = socket.create_connection(("127.0.0.1", opts.port), timeout=5.0)
        except OSError as exc:
            raise CliError(
                "连不上控制通道 127.0.0.1:%d (%s)" % (opts.port, exc), E_CONN,
                "确认播放器正在运行，且启动时带了 --rpc-port %d（--rpc-port 0 = 不启动）。\n"
                "       启动示例: %s open <媒体> --rpc-port %d" % (opts.port, CMD_NAME, opts.port))
        self.sock.settimeout(opts.timeout)
        self._auth(opts.resolve_token())

    # ---- 底层收发 ----

    def _read_line(self, timeout=None):
        """读一行；超时返回 None，连接关闭抛 RpcClosed。"""
        if timeout is not None:
            self.sock.settimeout(timeout)
        while b"\n" not in self.buf:
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                return None
            except OSError as exc:
                raise RpcClosed(str(exc))
            if not chunk:
                raise RpcClosed("连接已被远端关闭")
            self.buf += chunk
            if len(self.buf) > SERVER_MAX_LINE_BYTES + 4096:
                raise RpcClosed("单条报文超过服务端上限")
        i = self.buf.index(b"\n")
        line, self.buf = self.buf[:i], self.buf[i + 1:]
        return line.decode("utf-8", "replace").strip()

    def _send(self, obj):
        self.sock.sendall((json.dumps(obj, ensure_ascii=False) + "\n").encode("utf-8"))

    def _auth(self, token):
        resp = self.call("auth", {"token": token}, allow_unauthenticated=True)
        if not resp.get("ok"):
            raise CliError(
                "控制通道认证失败: %s" % resp.get("error", "?"), E_AUTH,
                "令牌每次启动都会重新生成，读当前值: %s" % self.opts.token_path)
        self.abi = (resp.get("result") or {}).get("abi")

    # ---- 调用 ----

    def call(self, method, params=None, allow_unauthenticated=False, timeout=None):
        rid = self.next_id
        self.next_id += 1
        self._send({"id": rid, "method": method, "params": params or {}})
        deadline = time.time() + (timeout or self.opts.timeout)
        while True:
            remain = deadline - time.time()
            if remain <= 0:
                raise CliError("等待 %s 应答超时（%.1fs）" % (method, timeout or self.opts.timeout),
                               E_FAIL)
            line = self._read_line(remain)
            if line is None:
                continue
            if not line:
                continue
            try:
                obj = json.loads(line)
            except ValueError:
                LOG.debug("忽略无法解析的报文: %r" % line[:200])
                continue
            if isinstance(obj, dict) and "event" in obj:
                self._dispatch_event(obj)
                continue
            if isinstance(obj, dict) and obj.get("id") == rid:
                return obj
            LOG.debug("忽略非本请求的应答: id=%r" % (obj.get("id") if isinstance(obj, dict) else "?"))

    def call_ok(self, method, params=None, timeout=None):
        """调用并要求 ok:true；否则抛 E_REMOTE。"""
        resp = self.call(method, params, timeout=timeout)
        if not resp.get("ok"):
            raise CliError("远端返回错误 (%s): %s" % (method, resp.get("error", "?")), E_REMOTE)
        return resp.get("result")

    def _dispatch_event(self, obj):
        if self.on_event:
            self.on_event(obj.get("event"), obj.get("data"))

    def stream_events(self, topics, seconds, count, raw=False):
        for t in topics:
            self.call_ok("topic.subscribe", {"topic": t})
        LOG.info("已订阅: %s（%.1fs 后停止，Ctrl-C 提前结束）" % (", ".join(topics), seconds))
        deadline = time.time() + seconds
        got = 0
        while True:
            remain = deadline - time.time()
            if remain <= 0:
                break
            line = self._read_line(remain)
            if line is None:
                continue
            try:
                obj = json.loads(line)
            except ValueError:
                continue
            if not isinstance(obj, dict) or "event" not in obj:
                continue
            topic, data = obj.get("event"), obj.get("data")
            got += 1
            if raw or LOG.json:
                out(json.dumps({"event": topic, "data": data}, ensure_ascii=False))
            else:
                body = json.dumps(data, ensure_ascii=False)
                out("[event] %-18s %s" % (topic, body[:220]))
            if count and got >= count:
                break
        return got

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def with_rpc(opts, fn, on_event=None):
    """连上控制通道跑一段逻辑，保证关闭。"""
    c = RpcClient(opts, on_event=on_event)
    try:
        return fn(c)
    finally:
        c.close()


# ============================================================================
# 4. 子进程工具
# ============================================================================

def run_inherit(argv, cwd=None, env=None):
    """继承父进程 stdio 跑子进程 —— 不做 PIPE 捕获。

    刻意不捕获：子进程输出的是 UTF-8 中文，一旦经 Python 解码再用 console
    代码页重编码，反而会引入乱码；直接继承还顺带拿到实时输出。
    """
    LOG.debug("exec: " + " ".join(argv))
    try:
        return subprocess.call(argv, cwd=cwd or ROOT, env=env)
    except OSError as exc:
        raise CliError("无法执行 %s (%s)" % (argv[0], exc), E_ENV)


def run_capture(argv, cwd=None, timeout=60):
    try:
        p = subprocess.run(argv, cwd=cwd or ROOT, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return None, str(exc)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def player_env():
    """让 dist/*.exe 能解析到同目录与 vendor 下的 DLL。"""
    env = dict(os.environ)
    parts = [p for p in (DIST, os.path.join(VENDOR, "sdl3", "bin"),
                         os.path.join(VENDOR, "ffmpeg", "bin")) if os.path.isdir(p)]
    if parts:
        env["PATH"] = os.pathsep.join(parts + [env.get("PATH", "")])
    return env


def require_dist():
    if not os.path.isfile(EXE):
        raise CliError("找不到 %s" % EXE, E_ENV,
                       "先构建: %s build（或直接跑 %s）" % (CMD_NAME, FETCH_DEPS))


def require_media(path):
    if not os.path.isfile(path):
        raise CliError("媒体文件不存在: %s" % path, E_USAGE)
    return os.path.abspath(path)


# ============================================================================
# 5. 命令：本地编排
# ============================================================================

USAGE_OPEN = ("用法: %s open <媒体> [选项]\n"
              "  选项: --hidden --size WxH --start 秒 --volume 0..1 --no-audio\n"
              "        --log-level 0..4 --write-dir 目录(可重复) --allow-network-input\n"
              "        --reload N --no-rpc -- <额外参数原样传给 pluginplayer.exe>" % CMD_NAME)


def cmd_open(opts, argv):
    """启动播放器播放媒体。`--` 之后的参数原样透传给 pluginplayer.exe。"""
    args = expand_equals(argv)
    exe_args, passthrough = [], []
    media, no_rpc, write_dirs = None, False, []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--":
            passthrough = args[i + 1:]
            break
        if a in ("--hidden", "--no-audio", "--allow-network-input"):
            exe_args.append(a)
            i += 1
        elif a == "--no-rpc":
            no_rpc = True
            i += 1
        elif a == "--size":
            parse_wxh(take(args, i, a, USAGE_OPEN), USAGE_OPEN)
            exe_args += ["--size", args[i + 1]]
            i += 2
        elif a == "--start":
            parse_number(take(args, i, a, USAGE_OPEN), a, MIN_START, MAX_START, kind="float",
                         usage=USAGE_OPEN)
            exe_args += ["--start", args[i + 1]]
            i += 2
        elif a == "--volume":
            parse_number(take(args, i, a, USAGE_OPEN), a, MIN_VOLUME, MAX_VOLUME, kind="float",
                         usage=USAGE_OPEN)
            exe_args += ["--volume", args[i + 1]]
            i += 2
        elif a == "--log-level":
            parse_number(take(args, i, a, USAGE_OPEN), a, MIN_LOG_LEVEL, MAX_LOG_LEVEL,
                         usage=USAGE_OPEN)
            exe_args += ["--log-level", args[i + 1]]
            i += 2
        elif a == "--reload":
            parse_number(take(args, i, a, USAGE_OPEN), a, MIN_RELOAD, MAX_RELOAD, usage=USAGE_OPEN)
            exe_args += ["--reload", args[i + 1]]
            i += 2
        elif a == "--write-dir":
            write_dirs.append(take(args, i, a, USAGE_OPEN))
            i += 2
        elif a in ("--rpc-port", "--rpc-token"):
            raise UsageError(
                "未知选项: %s" % a,
                "open 用全局选项 --port/--token，CLI 会自己翻译成 %s 传给播放器：\n"
                "        %s open <媒体> --port 7911 --token <hex>" % (a, CMD_NAME))
        elif a.startswith("-"):
            raise UsageError("未知选项: %s" % a, USAGE_OPEN)
        else:
            if media is not None:
                raise UsageError("只能给一个媒体文件，多给了: %s" % a, USAGE_OPEN)
            media = a
            i += 1
    if media is None:
        raise UsageError("open 需要媒体文件路径", USAGE_OPEN)

    require_dist()
    require_media(media)
    exe_args += ["--rpc-port", "0" if no_rpc else str(opts.port)]
    if opts.token.strip():
        # 与 src/core/main.cpp 的 --rpc-token 校验对齐：太短的令牌播放器会 exit 2，
        # 与其让它在启动后才报错，不如在这里先挡（参见 tools/verify.py 的边界一致性门禁）。
        if len(opts.token.strip()) < MIN_TOKEN_LEN:
            raise UsageError("非法的 --token: 太短（至少 %d 个字符，建议 64 位 hex）" % MIN_TOKEN_LEN,
                             USAGE_OPEN)
        exe_args += ["--rpc-token", opts.token.strip()]
    for d in write_dirs:
        exe_args += ["--write-dir", d]
    cmd = [EXE] + exe_args + [media] + passthrough
    if not opts.json:
        LOG.info("启动: " + " ".join(cmd))
    return run_inherit(cmd, env=player_env())


USAGE_SELFTEST = ("用法: %s selftest [媒体] [选项]\n"
                  "  选项: --frames N --out 目录 --no-rpc\n"
                  "  不给媒体时：用 dist/test_assets/sample.mp4；不存在就用\n"
                  "  vendor/ffmpeg 现场生成一个 8 秒 1280x720 的样例。" % CMD_NAME)


def ensure_sample():
    """自检需要媒体。没有就按 selftest.bat 的同一条命令生成一个。"""
    if os.path.isfile(DEFAULT_SAMPLE):
        return DEFAULT_SAMPLE
    if not os.path.isfile(FFMPEG):
        raise CliError("既没有 %s，也找不到 %s" % (DEFAULT_SAMPLE, FFMPEG), E_ENV,
                       "先跑 %s 准备依赖" % FETCH_DEPS)
    os.makedirs(os.path.dirname(DEFAULT_SAMPLE), exist_ok=True)
    LOG.info("生成样例媒体: %s" % DEFAULT_SAMPLE)
    rc = run_inherit([FFMPEG, "-y", "-hide_banner", "-loglevel", "error",
                      "-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30",
                      "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
                      "-t", "8", "-c:v", "libx264", "-preset", "veryfast",
                      "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest", DEFAULT_SAMPLE])
    if rc != 0 or not os.path.isfile(DEFAULT_SAMPLE):
        raise CliError("生成样例媒体失败（ffmpeg 退出码 %d）" % rc, E_ENV)
    return DEFAULT_SAMPLE


def cmd_selftest(opts, argv):
    args = expand_equals(argv)
    media, out_dir, frames, no_rpc = None, "selftest_out", DEFAULT_SELFTEST_FRAMES, False
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--frames":
            frames = parse_number(take(args, i, a, USAGE_SELFTEST), a, MIN_FRAMES, MAX_FRAMES,
                                  usage=USAGE_SELFTEST)
            i += 2
        elif a == "--out":
            out_dir = take(args, i, a, USAGE_SELFTEST)
            i += 2
        elif a == "--no-rpc":
            no_rpc = True
            i += 1
        elif a.startswith("-"):
            raise UsageError("未知选项: %s" % a, USAGE_SELFTEST)
        else:
            if media is not None:
                raise UsageError("只能给一个媒体文件，多给了: %s" % a, USAGE_SELFTEST)
            media = a
            i += 1
    require_dist()
    media = require_media(media) if media else ensure_sample()
    exe_args = ["--selftest", "--frames", str(frames), "--out", out_dir,
                "--rpc-port", "0" if no_rpc else str(opts.port)]
    cmd = [EXE] + exe_args + [media]
    LOG.info("自检: " + " ".join(cmd))
    rc = run_inherit(cmd, cwd=DIST, env=player_env())
    report = os.path.join(DIST, out_dir, "selftest_report.json")
    if os.path.isfile(report):
        LOG.info("报告: %s" % report)
    return rc


def cmd_plugins(opts, argv):
    """默认离线读取 dist/plugins（走 exe 的 --list-plugins）；
    `--remote` 改为向运行中的播放器要 plugin.list（含 ABI 与加载状态）。"""
    args = expand_equals(argv)
    remote = "--remote" in args
    for a in args:
        if a not in ("--remote",):
            raise UsageError("未知选项: %s" % a, "用法: %s plugins [--remote]" % CMD_NAME)
    if remote:
        def body(c):
            return c.call_ok("plugin.list").get("plugins", [])
        rows = with_rpc(opts, body)
        if opts.json:
            LOG.result({"ok": True, "plugins": rows})
        else:
            wname = max([disp_width(str(p.get("name", ""))) for p in rows] + [10])
            for p in rows:
                LOG.info("  #%s %s %s %s %s" % (
                    pad(str(p.get("slot")), 2), pad(str(p.get("name")), wname),
                    pad(str(p.get("version")), 7), pad(str(p.get("state")), 8),
                    p.get("file")))
        return E_OK
    require_dist()
    return run_inherit([EXE, "--list-plugins"], env=player_env())


def cmd_manifest(opts, argv):
    reject_extra(expand_equals(argv), "用法: %s manifest" % CMD_NAME)
    require_dist()
    return run_inherit([EXE, "--write-plugin-manifest"], env=player_env())


USAGE_BUILD = "用法: %s build [build.py 参数...]（--release --smoke --apitest --clean --debug --core --plugins a,b --list）" % CMD_NAME


def cmd_build(opts, argv):
    """透传给 build.py：本 CLI 不重复实现构建逻辑，只做转发与退出码传递。"""
    if not os.path.isfile(BUILD_PY):
        raise CliError("找不到 %s" % BUILD_PY, E_ENV)
    passthrough = expand_equals(argv)
    if passthrough and passthrough[0] == "--":
        passthrough = passthrough[1:]
    known = ("--clean", "--core", "--smoke", "--apitest", "--release", "--no-package",
             "--debug", "--list", "--plugins")
    for a in passthrough:
        if a.startswith("-") and a.split("=")[0] not in known:
            raise UsageError("未知选项: %s" % a, USAGE_BUILD)
    return run_inherit([sys.executable, BUILD_PY] + passthrough)


def cmd_host_js(opts, argv):
    """启动进程外 JS 插件宿主（Node + Cordis）。令牌由宿主自己读令牌文件。"""
    args = expand_equals(argv)
    extra, i = [], 0
    while i < len(args):
        if args[i] == "--":
            extra = args[i + 1:]
            break
        extra.append(args[i])
        i += 1
    if not os.path.isfile(HOST_JS):
        raise CliError("找不到 %s" % HOST_JS, E_ENV)
    node = os.environ.get("PP_NODE", "").strip() or shutil.which("node") or "node"
    deps = os.path.join(ROOT, "host-js", "node_modules", "cordis")
    if not os.path.isdir(deps):
        LOG.warn("host-js/node_modules 里没有 cordis，先在 host-js/ 下执行 npm install")
    cmd = [node, HOST_JS, "--port", str(opts.port)] + extra
    LOG.info("启动 JS 插件宿主: " + " ".join(cmd))
    return run_inherit(cmd, cwd=os.path.join(ROOT, "host-js"))


# ---- doctor ----------------------------------------------------------------

class Check:
    def __init__(self, name, status, detail):
        self.name, self.status, self.detail = name, status, detail


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fp:
        for chunk in iter(lambda: fp.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_manifest():
    """解析 plugins/manifest.sha256 -> {文件名: 哈希}（与 sha256sum 格式兼容）。"""
    path = os.path.join(PLUGIN_DIR, "manifest.sha256")
    if not os.path.isfile(path):
        return None
    entries = {}
    with open(path, "r", encoding="utf-8") as fp:
        for line in fp:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) >= 2 and re.fullmatch(r"[0-9a-fA-F]{64}", parts[0]):
                entries[parts[-1]] = parts[0].lower()
    return entries


def check_manifest_integrity():
    """真实复现宿主的加载判定：清单存在即强制校验，未列出/哈希不符 = 会被拒绝加载。"""
    entries = read_manifest()
    if entries is None:
        return Check("插件完整性清单", "WARN",
                     "plugins/manifest.sha256 不存在（宿主不校验；发布前建议生成）")
    dlls = [f for f in os.listdir(PLUGIN_DIR) if f.lower().endswith(".dll")] \
        if os.path.isdir(PLUGIN_DIR) else []
    bad, unlisted = [], []
    for f in sorted(dlls):
        if f not in entries:
            unlisted.append(f)
            continue
        if sha256_file(os.path.join(PLUGIN_DIR, f)) != entries[f]:
            bad.append(f)
    missing = [f for f in entries if f not in dlls]
    if bad or unlisted or missing:
        detail = "校验失败 ->"
        if bad:
            detail += " 哈希不符 %s;" % ",".join(bad)
        if unlisted:
            detail += " 未登记 %s;" % ",".join(unlisted)
        if missing:
            detail += " 清单有但文件缺失 %s;" % ",".join(missing)
        return Check("插件完整性清单", "FAIL", detail.rstrip(";"))
    return Check("插件完整性清单", "OK", "%d 个插件哈希与清单一致" % len(dlls))


def check_control_channel(opts):
    try:
        s = socket.create_connection(("127.0.0.1", opts.port), timeout=1.5)
        s.close()
    except OSError as exc:
        return Check("控制通道", "WARN", "127.0.0.1:%d 未监听 (%s)" % (opts.port, exc))
    return Check("控制通道", "OK", "127.0.0.1:%d 已监听" % opts.port)


def check_cli_on_path():
    exe = shutil.which(CMD_NAME)
    if exe:
        return Check("CLI 命令", "OK", "命令 %s -> %s" % (CMD_NAME, exe))
    return Check("CLI 命令", "WARN", "PATH 中找不到 %s，先执行: %s install-cli" % (CMD_NAME, CMD_NAME))


def cmd_doctor(opts, argv):
    reject_extra(expand_equals(argv), "用法: %s doctor [--json] [--port N]" % CMD_NAME)
    checks = []

    checks.append(Check("Python", "OK" if sys.version_info >= (3, 9) else "FAIL",
                        "%d.%d.%d @ %s" % (sys.version_info[0], sys.version_info[1],
                                           sys.version_info[2], sys.executable)))

    checks.append(Check("仓库根", "OK" if os.path.isdir(ROOT) else "FAIL", ROOT))
    checks.append(Check("CLI 脚本", "OK" if os.path.isfile(os.path.abspath(__file__)) else "FAIL",
                        os.path.join("tools", "pp_cli.py")))

    # dist / exe
    if not os.path.isdir(DIST):
        checks.append(Check("dist/", "FAIL", "不存在，尚未构建"))
    else:
        checks.append(Check("dist/", "OK", DIST))
        if os.path.isfile(EXE):
            checks.append(Check("内核 exe", "OK", "%.0f KB" % (os.path.getsize(EXE) / 1024.0)))
        else:
            checks.append(Check("内核 exe", "FAIL", "dist/pluginplayer.exe 缺失"))

        lack = [d for d in RUNTIME_DLLS if not os.path.isfile(os.path.join(DIST, d))]
        all_dll = sorted(f for f in os.listdir(DIST) if f.lower().endswith(".dll")) \
            if os.path.isdir(DIST) else []
        if lack:
            checks.append(Check("运行时 DLL", "FAIL", "缺少 %s" % ",".join(lack)))
        else:
            checks.append(Check("运行时 DLL", "OK", "%d 个 DLL 齐备" % len(all_dll)))

        pl = [f for f in os.listdir(PLUGIN_DIR) if f.lower().endswith(".dll")] \
            if os.path.isdir(PLUGIN_DIR) else []
        checks.append(Check("插件目录", "OK" if pl else "FAIL",
                            "dist/plugins 下 %d 个 DLL" % len(pl) if pl else "dist/plugins 里没有 DLL"))
        if os.path.isdir(PLUGIN_DIR):
            checks.append(check_manifest_integrity())

    # 工具链与依赖
    gxx = os.environ.get("PP_GXX", r"F:/mingw64/mingw64/bin/g++.exe")
    if os.path.isfile(gxx):
        rc, txt = run_capture([gxx, "--version"], timeout=30)
        checks.append(Check("编译器 g++", "OK" if rc == 0 else "FAIL",
                            (txt or "").splitlines()[0] if rc == 0 else "调用失败"))
    else:
        checks.append(Check("编译器 g++", "WARN", "未找到 %s（仅构建时需要）" % gxx))

    vdirs = ["sdl3/include", "sdl3/lib", "ffmpeg/include", "ffmpeg/lib"]
    lack = [d for d in vdirs if not os.path.isdir(os.path.join(VENDOR, d))]
    checks.append(Check("vendor 依赖", "OK" if not lack else "WARN",
                        "头文件/导入库齐备" if not lack else "缺少 vendor/%s（跑 tools/fetch_deps.py）" % ",".join(lack)))
    checks.append(Check("ffmpeg.exe", "OK" if os.path.isfile(FFMPEG) else "WARN",
                        FFMPEG if os.path.isfile(FFMPEG) else "缺失（生成样例媒体与转码时需要）"))

    node = os.environ.get("PP_NODE", "").strip() or shutil.which("node")
    if node:
        rc, txt = run_capture([node, "--version"], timeout=30)
        checks.append(Check("Node.js", "OK" if rc == 0 else "WARN",
                            (txt or "").strip() if rc == 0 else "调用失败（仅 host-js 需要）"))
    else:
        checks.append(Check("Node.js", "WARN", "PATH 中没有 node（仅 host-js 需要）"))
    hj = os.path.isdir(os.path.join(ROOT, "host-js", "node_modules"))
    checks.append(Check("host-js 依赖", "OK" if hj else "WARN",
                        "node_modules 已就位" if hj else "host-js/node_modules 缺失（跑 npm install）"))

    tok = os.path.isfile(opts.token_path)
    checks.append(Check("控制通道令牌", "OK" if tok else "WARN",
                        opts.token_path if tok else "令牌文件不存在（播放器启动后才会生成）"))
    checks.append(check_control_channel(opts))
    checks.append(check_cli_on_path())

    fails = [c for c in checks if c.status == "FAIL"]
    warns = [c for c in checks if c.status == "WARN"]
    if opts.json:
        LOG.result({
            "ok": not fails,
            "checks": [{"name": c.name, "status": c.status, "detail": c.detail} for c in checks],
            "fail": len(fails), "warn": len(warns),
        })
    else:
        width = max(disp_width(c.name) for c in checks)
        for c in checks:
            LOG.info("  %-6s %s  %s" % ("[" + c.status + "]", pad(c.name, width), c.detail))
        LOG.info("")
        LOG.info("体检结果: 失败 %d 项，告警 %d 项，共 %d 项" % (len(fails), len(warns), len(checks)))
    return E_ENV if fails else E_OK


# ---- install-cli / uninstall-cli ------------------------------------------

def norm_path(p):
    return os.path.normcase(os.path.abspath(p)).rstrip("\\/")


def is_on_path(d):
    target = norm_path(d)
    for entry in os.environ.get("PATH", "").split(os.pathsep):
        entry = entry.strip().strip('"')
        if entry and norm_path(entry) == target:
            return True
    return False


def candidate_shim_dirs():
    """候选安装目录，按"已在本机 PATH 上"优先。

    首选 <python 前缀>\\Scripts —— Python 自己的用户级脚本目录，通常已在 PATH
    （本机 F:\\Python314\\Scripts 就在），因此默认不需要改任何环境变量。
    """
    cands = []
    env = os.environ.get("PP_BIN_DIR", "").strip()
    if env:
        cands.append(os.path.abspath(env))
    try:
        py_dir = os.path.dirname(os.path.abspath(sys.executable))
        if py_dir:
            cands.append(os.path.join(py_dir, "Scripts"))
            cands.append(py_dir)
    except (AttributeError, OSError):
        pass
    local = os.environ.get("LOCALAPPDATA", "").strip() or os.path.expanduser("~")
    cands.append(os.path.join(local, "PluginPlayer", "bin"))
    seen, uniq = set(), []
    for c in cands:
        if norm_path(c) not in seen:
            seen.add(norm_path(c))
            uniq.append(c)
    return uniq


def try_writable(d):
    try:
        os.makedirs(d, exist_ok=True)
    except OSError:
        return False
    probe = os.path.join(d, ".pp_write_probe")
    try:
        with open(probe, "w", encoding="ascii") as fp:
            fp.write("x")
        os.remove(probe)
        return True
    except OSError:
        return False


def pick_shim_dir(explicit):
    if explicit:
        d = os.path.abspath(explicit)
        if not try_writable(d):
            raise CliError("目录不可写: %s" % d, E_ENV)
        return d, is_on_path(d)
    cands = candidate_shim_dirs()
    for d in cands:
        if is_on_path(d) and try_writable(d):
            return d, True
    for d in cands:
        if try_writable(d):
            return d, False
    raise CliError("找不到可写的安装目录，请用 --shim-dir 指定", E_ENV)


def shim_text(root_expr, python_exe):
    """cmd 垫片。两条硬约束：

    1) **纯 ASCII + CRLF** —— cmd.exe 按当前代码页（本机 GBK）解读 .bat，
       文件里混入中文在非中文代码页的机器上会乱码/截断（项目既有约定）。
    2) **PP_ROOT 一律带尾部分隔符**，脚本里统一写 "%PP_ROOT%tools\\pp_cli.py"。
       这样绝对路径版（PATH 垫片）与 %~dp0 版（仓库根垫片）能共用同一份脚本体。

    解析 Python 的顺序：PP_PYTHON 环境变量 -> 安装时记下的绝对路径 ->
    `py -3` -> `python`。调用时一律给解释器加引号（路径可能含空格），
    但 `py -3` 这种"命令 + 参数"形式不能整体加引号，所以拆成 PP_PY / PP_PYARG。
    """
    return (
        "@echo off\r\n"
        "rem %s v1 - generated by tools/pp_cli.py install-cli; safe to delete.\r\n"
        "rem PluginPlayer command-line interface shim.\r\n"
        "setlocal\r\n"
        "set \"PP_ROOT=%s\"\r\n"
        "set \"PP_PY=%s\"\r\n"
        "set \"PP_PYARG=\"\r\n"
        "if defined PP_PYTHON set \"PP_PY=%%PP_PYTHON%%\"\r\n"
        "if defined PP_PY if not exist \"%%PP_PY%%\" set \"PP_PY=\"\r\n"
        "if defined PP_PY goto pp_run\r\n"
        "if exist \"%s\" set \"PP_PY=%s\"\r\n"
        "if defined PP_PY goto pp_run\r\n"
        "where py >nul 2>nul\r\n"
        "if not errorlevel 1 set \"PP_PY=py\"\r\n"
        "if defined PP_PY goto pp_run\r\n"
        "where python >nul 2>nul\r\n"
        "if not errorlevel 1 set \"PP_PY=python\"\r\n"
        "if not defined PP_PY (\r\n"
        "  echo [ERR] Python not found. Set PP_PYTHON to a python.exe path. 1>&2\r\n"
        "  exit /b 6\r\n"
        ")\r\n"
        ":pp_run\r\n"
        "if /i \"%%PP_PY%%\"==\"py\" set \"PP_PYARG=-3\"\r\n"
        "\"%%PP_PY%%\" %%PP_PYARG%% \"%%PP_ROOT%%tools\\pp_cli.py\" %%*\r\n"
        "set \"PP_RC=%%ERRORLEVEL%%\"\r\n"
        "exit /b %%PP_RC%%\r\n"
    ) % (SHIM_MARKER, root_expr, python_exe, python_exe, python_exe)


def write_text_exact(path, text, encoding="ascii"):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding=encoding, newline="") as fp:
        fp.write(text)
    return path


def with_sep(path):
    """统一成带尾部分隔符的形式，与 shim_text 的约定对齐。"""
    path = os.path.abspath(path)
    return path if path.endswith(os.sep) else path + os.sep


def write_shims(dir_path, root_expr, python_exe):
    """写入 pluginplayer.cmd / pp.cmd。返回已写入的路径列表。"""
    written = []
    body = shim_text(root_expr, python_exe)
    for name in (CMD_NAME + ".cmd", "pp.cmd"):
        written.append(write_text_exact(os.path.join(dir_path, name), body))
    return written


def cmd_install_cli(opts, argv):
    args = expand_equals(argv)
    shim_dir, add_to_path, dry = "", False, False
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--shim-dir":
            shim_dir = take(args, i, a, "用法: %s install-cli [--shim-dir 目录] [--add-to-path] [--dry-run]" % CMD_NAME)
            i += 2
        elif a == "--add-to-path":
            add_to_path = True
            i += 1
        elif a == "--dry-run":
            dry = True
            i += 1
        else:
            raise UsageError("未知选项: %s" % a,
                             "用法: %s install-cli [--shim-dir 目录] [--add-to-path] [--dry-run]" % CMD_NAME)

    target, on_path = pick_shim_dir(shim_dir)
    if dry:
        LOG.result({"ok": True, "dir": target, "on_path": on_path, "dry_run": True})
        if not opts.json:
            LOG.info("将安装到: %s（已在 PATH: %s）" % (target, "是" if on_path else "否"))
        return E_OK

    written = write_shims(target, with_sep(ROOT), sys.executable)
    # 仓库根再放一份 %~dp0 版：clone 下来就能 `cd /d <repo> && pluginplayer status`，
    # 不依赖 PATH 有没有装过。它用相对路径，整个仓库搬走也不会失效。
    repo_shims = write_shims(ROOT, "%~dp0", sys.executable)

    if not on_path and add_to_path:
        add_user_path(target)
        on_path = is_on_path(target) or True   # 写进注册表后，新开的终端才生效

    if opts.json:
        LOG.result({"ok": True, "dir": target, "files": written,
                    "repo_files": repo_shims, "on_path": on_path})
    else:
        for w in written:
            LOG.ok("已写入 %s" % w)
        for w in repo_shims:
            LOG.ok("已写入 %s" % w)
        LOG.info("")
        if on_path:
            LOG.ok("命令 %s 已可用（若当前窗口还找不到，新开一个 cmd 再试）" % CMD_NAME)
        else:
            LOG.warn("%s 不在当前 PATH 上，当前窗口无法直接调用" % target)
            LOG.info("  方式一（推荐，只改用户级 PATH，不动系统 PATH）:")
            LOG.info("    powershell -NoProfile -Command \"[Environment]::SetEnvironmentVariable('PATH', "
                     "[Environment]::GetEnvironmentVariable('PATH','User') + ';%s', 'User')\"" % target)
            LOG.info("  方式二: %s install-cli --add-to-path" % CMD_NAME)
            LOG.info("  方式三: 换一个已在 PATH 上的目录: %s install-cli --shim-dir <目录>" % CMD_NAME)
    return E_OK


def add_user_path(dir_path):
    """只写 HKCU\\Environment\\PATH（用户级），并用 winreg 追加，避免 setx 的
    1024 字符截断与"把系统 PATH 混进用户 PATH"的老坑。"""
    try:
        import winreg
    except ImportError:
        raise CliError("当前环境没有 winreg，无法自动改 PATH，请手工添加", E_ENV)
    key_path = r"Environment"
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key_path, 0,
                        winreg.KEY_READ | winreg.KEY_WRITE) as key:
        try:
            cur, kind = winreg.QueryValueEx(key, "PATH")
        except FileNotFoundError:
            cur, kind = "", winreg.REG_EXPAND_SZ
        entries = [e for e in cur.split(os.pathsep) if e.strip()]
        if any(norm_path(e.strip('"')) == norm_path(dir_path) for e in entries):
            LOG.info("用户级 PATH 里已经有 %s" % dir_path)
            return False
        LOG.info("原用户级 PATH: %s" % (cur or "(空)"))
        new = (cur.rstrip(";") + ";" if cur.strip() else "") + dir_path
        winreg.SetValueEx(key, "PATH", 0, kind or winreg.REG_EXPAND_SZ, new)
        LOG.ok("已追加到用户级 PATH: %s" % dir_path)
    LOG.info("已广播环境变更，但**当前窗口**不会自动生效，请新开一个终端。")
    return True


def cmd_uninstall_cli(opts, argv):
    args = expand_equals(argv)
    shim_dir = ""
    i = 0
    while i < len(args):
        if args[i] == "--shim-dir":
            shim_dir = take(args, i, args[i], "用法: %s uninstall-cli [--shim-dir 目录]" % CMD_NAME)
            i += 2
        else:
            raise UsageError("未知选项: %s" % args[i],
                             "用法: %s uninstall-cli [--shim-dir 目录]" % CMD_NAME)
    dirs = [os.path.abspath(shim_dir)] if shim_dir else candidate_shim_dirs()
    removed, skipped = [], []
    for d in dirs:
        for name in (CMD_NAME + ".cmd", "pp.cmd"):
            p = os.path.join(d, name)
            if not os.path.isfile(p):
                continue
            try:
                with open(p, "r", encoding="ascii", errors="replace") as fp:
                    head = fp.read(4096)
            except OSError:
                skipped.append(p)
                continue
            # 只删自己写的：必须带标记。别人手工放的同名文件不动。
            if SHIM_MARKER not in head:
                skipped.append(p + "（不是本工具生成的，保留）")
                continue
            try:
                os.remove(p)
                removed.append(p)
            except OSError as exc:
                skipped.append("%s (%s)" % (p, exc))
    if opts.json:
        LOG.result({"ok": True, "removed": removed, "skipped": skipped})
    else:
        for p in removed:
            LOG.ok("已删除 %s" % p)
        for p in skipped:
            LOG.warn("跳过 %s" % p)
        if not removed:
            LOG.info("没有找到本工具生成的命令垫片")
    return E_OK


def cmd_version(opts, argv):
    reject_extra(expand_equals(argv), "用法: %s version [--json]" % CMD_NAME)
    info = {
        "cli": CLI_VERSION,
        "root": ROOT,
        "exe": EXE if os.path.isfile(EXE) else None,
        "python": "%d.%d.%d" % sys.version_info[:3],
        "port": opts.port,
    }
    node = os.environ.get("PP_NODE", "").strip() or shutil.which("node")
    if node:
        rc, txt = run_capture([node, "--version"], timeout=20)
        if rc == 0:
            info["node"] = txt.strip()
    if opts.json:
        LOG.result(dict(info, ok=True))
    else:
        LOG.info("%s-cli %s" % (CMD_NAME, CLI_VERSION))
        LOG.info("  仓库根   : %s" % info["root"])
        LOG.info("  内核 exe : %s" % (info["exe"] or "(未构建)"))
        LOG.info("  Python   : %s" % info["python"])
        LOG.info("  Node     : %s" % info.get("node", "(无)"))
        LOG.info("  默认端口 : %d" % info["port"])
    return E_OK


# ============================================================================
# 6. 命令：控制通道
# ============================================================================

USAGE_REMOTE = "用法: %s %s [--port N] [--token HEX] [--token-file 路径] [--json]"


def cmd_status(opts, argv):
    args = expand_equals(argv)
    reject_extra(args, USAGE_REMOTE % (CMD_NAME, "status"))

    def body(c):
        info = c.call_ok("player.info")
        stats = c.call_ok("player.stats")
        # player.volume 传 value=-999 是"只查不改"（见 control.cpp：v <= -998 直接回值）。
        # player.mute 没有纯查询路径（不给 value 就是 toggle），所以状态里只报音量。
        vol = c.call_ok("player.volume", {"value": -999}).get("volume")
        return {"info": info, "stats": stats, "volume": vol}

    res = with_rpc(opts, body)
    info, stats = res["info"], res["stats"]
    if opts.json:
        LOG.result({"ok": True, "info": info, "stats": stats, "volume": res["volume"]})
        return E_OK
    pos, dur = stats.get("position", 0.0), stats.get("duration", 0.0)
    LOG.info("媒体   : %s" % info.get("path", "?"))
    LOG.info("容器   : %s  视频 %s %dx%d %.2ffps  音频 %s %dHz/%dch" % (
        info.get("format", "?"), info.get("video_codec", "?"), info.get("width", 0),
        info.get("height", 0), info.get("fps", 0.0), info.get("audio_codec", "?"),
        info.get("sample_rate", 0), info.get("channels", 0)))
    LOG.info("进度   : %s / %s  (%.1f%%)" % (fmt_time(pos), fmt_time(dur),
                                             (pos / dur * 100.0) if dur > 0 else 0.0))
    LOG.info("状态   : %s   倍速 %.2fx   音量 %.3f" % (
        "暂停" if stats.get("paused") else "播放", stats.get("speed", 1.0),
        res["volume"] if res["volume"] is not None else float("nan")))
    LOG.info("时钟   : 主 %.3f  视频 %.3f  音频 %.3f" % (stats.get("master_clock", 0.0),
                                                         stats.get("video_clock", 0.0),
                                                         stats.get("audio_clock", 0.0)))
    LOG.info("统计   : 解码 %d  显示 %d  丢帧 %d  实测 %.2f fps  vqueue %d  abuf %d" % (
        stats.get("decoded", 0), stats.get("displayed", 0), stats.get("dropped", 0),
        stats.get("fps", 0.0), stats.get("vqueue", 0), stats.get("abuf", 0)))
    if stats.get("eof"):
        LOG.info("状态   : 已到文件末尾 (eof)")
    return E_OK


def fmt_time(sec):
    sec = max(0.0, float(sec))
    h = int(sec // 3600)
    m = int((sec % 3600) // 60)
    s = sec % 60
    return ("%d:%02d:%05.2f" % (h, m, s)) if h else ("%d:%05.2f" % (m, s))


def _simple_call(opts, argv, method, usage):
    args = expand_equals(argv)
    if args:
        raise UsageError("未知选项: %s" % args[0], usage)
    res = with_rpc(opts, lambda c: c.call_ok(method))
    if opts.json:
        LOG.result(dict(res or {}, ok=True))
    else:
        LOG.ok("%s -> %s" % (method, json.dumps(res, ensure_ascii=False)))
    return E_OK


def cmd_play(opts, argv):
    return _simple_call(opts, argv, "player.play", USAGE_REMOTE % (CMD_NAME, "play"))


def cmd_pause(opts, argv):
    return _simple_call(opts, argv, "player.pause", USAGE_REMOTE % (CMD_NAME, "pause"))


def cmd_toggle(opts, argv):
    return _simple_call(opts, argv, "player.toggle", USAGE_REMOTE % (CMD_NAME, "toggle"))


def cmd_stop(opts, argv):
    return _simple_call(opts, argv, "player.stop", USAGE_REMOTE % (CMD_NAME, "stop"))


def cmd_quit(opts, argv):
    return _simple_call(opts, argv, "app.quit", USAGE_REMOTE % (CMD_NAME, "quit"))


USAGE_SEEK = "用法: %s seek <秒|+N|-N>    （+/- 表示相对当前位置）" % CMD_NAME


def cmd_seek(opts, argv):
    args = expand_equals(argv)
    if not args or args[0].startswith("--"):
        raise UsageError("seek 需要一个数值参数", USAGE_SEEK)
    if len(args) > 1:
        raise UsageError("seek 只接受一个参数，多给了: %s" % args[1], USAGE_SEEK)
    raw = args[0].strip()
    is_rel = raw[:1] in ("+", "-")
    value = parse_number(raw.lstrip("+"), "--seek", 0.0, 1e9, kind="float", usage=USAGE_SEEK)
    if raw[:1] == "-":
        value = -value
    params = {"delta": value} if is_rel else {"seconds": value}
    res = with_rpc(opts, lambda c: c.call_ok("player.seek", params))
    if opts.json:
        LOG.result(dict(res or {}, ok=True))
    else:
        LOG.ok("位置 -> %s" % fmt_time(res.get("position", 0.0)))
    return E_OK


USAGE_VOLUME = "用法: %s volume [0..1|+N|-N]   （不给参数则查询当前值）" % CMD_NAME


def cmd_volume(opts, argv):
    args = expand_equals(argv)
    if len(args) > 1:
        raise UsageError("volume 只接受一个参数，多给了: %s" % args[1], USAGE_VOLUME)
    if not args:
        res = with_rpc(opts, lambda c: c.call_ok("player.volume", {}))
        if opts.json:
            LOG.result(dict(res or {}, ok=True))
        else:
            LOG.ok("音量 = %.3f" % res.get("volume", 0.0))
        return E_OK
    raw = args[0].strip()
    if raw[:1] in ("+", "-"):
        delta = parse_number(raw.lstrip("+"), "--volume", 0.0, 1.0, kind="float", usage=USAGE_VOLUME)
        if raw[:1] == "-":
            delta = -delta

        def body(c):
            cur = c.call_ok("player.volume").get("volume", 0.0)
            return c.call_ok("player.volume", {"value": min(1.0, max(0.0, cur + delta))})
    else:
        v = parse_number(raw, "--volume", 0.0, 1.0, kind="float", usage=USAGE_VOLUME)

        def body(c):
            return c.call_ok("player.volume", {"value": v})
    res = with_rpc(opts, body)
    if opts.json:
        LOG.result(dict(res or {}, ok=True))
    else:
        LOG.ok("音量 -> %.3f" % res.get("volume", 0.0))
    return E_OK


USAGE_MUTE = "用法: %s mute [on|off|toggle]   （默认 toggle）" % CMD_NAME


def cmd_mute(opts, argv):
    args = expand_equals(argv)
    if len(args) > 1 or (args and args[0] not in ("on", "off", "toggle")):
        raise UsageError("mute 只接受 on|off|toggle，收到: %s" % (args[0] if args else ""), USAGE_MUTE)
    mode = args[0] if args else "toggle"
    params = {} if mode == "toggle" else {"value": 1 if mode == "on" else 0}
    res = with_rpc(opts, lambda c: c.call_ok("player.mute", params))
    if opts.json:
        LOG.result(dict(res or {}, ok=True))
    else:
        LOG.ok("静音 -> %s" % ("开" if res.get("muted") else "关"))
    return E_OK


USAGE_SPEED = "用法: %s speed <倍率>   （例如 1.5；不给参数则查询）" % CMD_NAME


def cmd_speed(opts, argv):
    args = expand_equals(argv)
    if len(args) > 1:
        raise UsageError("speed 只接受一个参数，多给了: %s" % args[1], USAGE_SPEED)
    if not args:
        res = with_rpc(opts, lambda c: c.call_ok("player.stats"))
        if opts.json:
            LOG.result({"ok": True, "speed": res.get("speed")})
        else:
            LOG.ok("倍速 = %.3f" % res.get("speed", 1.0))
        return E_OK
    v = parse_number(args[0], "--speed", MIN_SPEED, MAX_SPEED, kind="float", usage=USAGE_SPEED)
    res = with_rpc(opts, lambda c: c.call_ok("player.speed", {"value": v}))
    if opts.json:
        LOG.result(dict(res or {}, ok=True))
    else:
        LOG.ok("倍速 -> %.3f" % res.get("speed", 0.0))
    return E_OK


def cmd_commands(opts, argv):
    reject_extra(expand_equals(argv), USAGE_REMOTE % (CMD_NAME, "commands"))
    res = with_rpc(opts, lambda c: c.call_ok("command.list"))
    if opts.json:
        LOG.result({"ok": True, "count": res.get("count", 0), "text": res.get("text", "")})
    else:
        LOG.info(res.get("text", "").rstrip())
        LOG.info("")
        LOG.ok("共 %d 个命令" % res.get("count", 0))
    return E_OK


def cmd_services(opts, argv):
    reject_extra(expand_equals(argv), USAGE_REMOTE % (CMD_NAME, "services"))
    res = with_rpc(opts, lambda c: c.call_ok("service.list"))
    names = res.get("services", [])
    if opts.json:
        LOG.result({"ok": True, "services": names})
    else:
        for n in names:
            LOG.info("  %s" % n)
        LOG.ok("共 %d 个服务" % len(names))
    return E_OK


USAGE_CMD = ("用法: %s cmd <命令名> [参数...]\n"
             "  例: %s cmd effect.set gray\n"
             "      %s cmd snapshot D:/shots\n"
             "  先看有哪些命令: %s commands" % (CMD_NAME, CMD_NAME, CMD_NAME, CMD_NAME))


def cmd_invoke(opts, argv):
    args = expand_equals(argv)
    # 允许 `cmd effect.set gray --port 7912` 这种写法：全局选项就地吃掉
    rest, i = [], 0
    while i < len(args):
        if args[i] in COMMON_FLAGS:
            j = parse_common(args, i, opts, USAGE_CMD)
            i = j
            continue
        if args[i] == "--":
            rest += args[i + 1:]
            break
        if args[i].startswith("--"):
            raise UsageError("未知选项: %s" % args[i], USAGE_CMD)
        rest.append(args[i])
        i += 1
    if not rest:
        raise UsageError("cmd 需要命令名", USAGE_CMD)
    name, params = rest[0], rest[1:]
    payload = {"name": name}
    if params:
        payload["args"] = params
    res = with_rpc(opts, lambda c: c.call_ok("command.invoke", payload))
    if opts.json:
        LOG.result(dict(res or {}, ok=True))
    else:
        LOG.ok("命令 %s 返回 %s" % (res.get("command", name), res.get("return")))
    return E_OK if res.get("return", 0) == 0 else E_FAIL


USAGE_EVENTS = ("用法: %s events [选项]\n"
                "  选项: --topic T(可重复) --seconds N --count N --raw\n"
                "  默认订阅全部主题（topic=*），持续 10 秒。" % CMD_NAME)


def cmd_events(opts, argv):
    args = expand_equals(argv)
    topics, seconds, count, raw = [], 10.0, 0, False
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--topic":
            topics.append(take(args, i, a, USAGE_EVENTS))
            i += 2
        elif a == "--seconds":
            seconds = parse_number(take(args, i, a, USAGE_EVENTS), a, 0.1, 86400.0, kind="float",
                                   usage=USAGE_EVENTS)
            i += 2
        elif a == "--count":
            count = parse_number(take(args, i, a, USAGE_EVENTS), a, 1, 1000000, usage=USAGE_EVENTS)
            i += 2
        elif a == "--raw":
            raw = True
            i += 1
        else:
            raise UsageError("未知选项: %s" % a, USAGE_EVENTS)
    if not topics:
        topics = ["*"]
    got = with_rpc(opts, lambda c: c.stream_events(topics, seconds, count, raw))
    if not opts.json:
        LOG.ok("收到 %d 个事件" % got)
    return E_OK


USAGE_RPC = ("用法: %s rpc <方法> [--params JSON | --params-file 文件 | --p k=v ...]\n"
             "  例: %s rpc ping\n"
             "      %s rpc player.seek --p seconds=12.5\n"
             "      %s rpc command.invoke --params '{\"name\":\"stats\"}'" % (CMD_NAME, CMD_NAME,
                                                                           CMD_NAME, CMD_NAME))


def coerce(text):
    """k=v 里的 v：能当 JSON 字面量就当，否则当字符串（gray -> \"gray\"）。"""
    try:
        v = json.loads(text)
    except ValueError:
        return text
    if isinstance(v, float) and (v != v or v in (float("inf"), float("-inf"))):
        return text
    return v


def cmd_rpc(opts, argv):
    args = expand_equals(argv)
    if not args or args[0].startswith("-"):
        raise UsageError("rpc 需要方法名", USAGE_RPC)
    method = args[0]
    params, i = {}, 1
    while i < len(args):
        a = args[i]
        if a == "--params":
            raw = take(args, i, a, USAGE_RPC)
            try:
                params.update(json.loads(raw))
            except ValueError as exc:
                raise UsageError("--params 不是合法 JSON: %s" % exc, USAGE_RPC)
            i += 2
        elif a == "--params-file":
            p = take(args, i, a, USAGE_RPC)
            try:
                with open(p, "r", encoding="utf-8") as fp:
                    params.update(json.load(fp))
            except (OSError, ValueError) as exc:
                raise UsageError("读取 --params-file 失败: %s" % exc, USAGE_RPC)
            i += 2
        elif a == "--p":
            kv = take(args, i, a, USAGE_RPC)
            if "=" not in kv:
                raise UsageError("--p 需要 k=v 形式，收到: %s" % kv, USAGE_RPC)
            k, v = kv.split("=", 1)
            params[k] = coerce(v)
            i += 2
        else:
            raise UsageError("未知选项: %s" % a, USAGE_RPC)

    def body(c):
        resp = c.call(method, params)
        if not resp.get("ok"):
            raise CliError("远端返回错误 (%s): %s" % (method, resp.get("error", "?")), E_REMOTE)
        return resp.get("result")

    res = with_rpc(opts, body)
    if opts.json:
        LOG.result({"ok": True, "method": method, "result": res})
    else:
        LOG.info(json.dumps(res, ensure_ascii=False, indent=2))
    return E_OK


# ============================================================================
# 7. 命令表与解析
# ============================================================================

HELP_TAIL = """
全局选项（可放在命令前后）:
  --port N          控制通道端口（默认 %d）
  --token HEX       控制通道令牌（默认按 PP_RPC_TOKEN -> 令牌文件 顺序解析）
  --token-file 路径 指定令牌文件（默认 dist/config/rpc-token.txt）
  --timeout 秒      单次请求超时（默认 10）
  --json            输出机器可读 JSON
  -q, --quiet       少说话
  -v, --verbose     打印调试信息

示例:
  %s open 影片.mp4 --hidden --rpc-port 7911
  %s status --json
  %s seek +30
  %s cmd effect.set gray
  %s events --topic player.completed --seconds 30
  %s doctor
  %s install-cli                # 之后在任何目录都能直接敲 pluginplayer

退出码: 0 成功 / 1 一般失败 / 2 用法错误 / 3 连不上 / 4 认证失败 / 5 远端错误 / 6 环境缺失
""" % (DEFAULT_PORT, CMD_NAME, CMD_NAME, CMD_NAME, CMD_NAME, CMD_NAME, CMD_NAME, CMD_NAME)


def command_table():
    return {
        # ---- 本地编排 ----
        "open":        (cmd_open,        "open <媒体> [选项]        启动播放器播放媒体（默认命令）"),
        "selftest":    (cmd_selftest,    "selftest [媒体] [选项]    无人值守自检，产出 JSON 报告"),
        "plugins":     (cmd_plugins,     "plugins [--remote]        列出插件（默认离线读 dist/plugins）"),
        "manifest":    (cmd_manifest,    "manifest                  重新生成 plugins/manifest.sha256"),
        "build":       (cmd_build,       "build [参数...]           转发给 build.py 构建"),
        "host-js":     (cmd_host_js,     "host-js [选项]            启动进程外 JS 插件宿主（Node）"),
        "doctor":      (cmd_doctor,      "doctor                    环境与依赖体检（真校验，不看印象）"),
        "install-cli": (cmd_install_cli, "install-cli [选项]        把 pluginplayer 命令装进 PATH 目录"),
        "uninstall-cli": (cmd_uninstall_cli, "uninstall-cli             移除本工具写入的命令垫片"),
        "version":     (cmd_version,     "version                   版本与环境摘要"),
        # ---- 控制通道 ----
        "status":      (cmd_status,      "status                    查询播放状态（info + stats）"),
        "play":        (cmd_play,        "play                      远端继续播放"),
        "pause":       (cmd_pause,       "pause                     远端暂停"),
        "toggle":      (cmd_toggle,      "toggle                    远端播放/暂停切换"),
        "stop":        (cmd_stop,        "stop                      远端停止并回到起点"),
        "seek":        (cmd_seek,        "seek <秒|+N|-N>           定位（+/- 为相对）"),
        "volume":      (cmd_volume,      "volume [0..1|+N|-N]       音量，不给参数则查询"),
        "mute":        (cmd_mute,        "mute [on|off|toggle]      静音"),
        "speed":       (cmd_speed,       "speed [倍率]              变速"),
        "cmd":         (cmd_invoke,      "cmd <命令名> [参数...]    调用插件命令"),
        "commands":    (cmd_commands,    "commands                  列出插件命令"),
        "services":    (cmd_services,    "services                  列出已注册服务"),
        "events":      (cmd_events,      "events [选项]             订阅并打印内核事件"),
        "rpc":         (cmd_rpc,         "rpc <方法> [选项]         原始 JSON-RPC 调用（逃生舱）"),
        "quit":        (cmd_quit,        "quit                      让远端播放器退出"),
    }


ALIASES = {}   # 目前没有别名；保留结构便于以后加（如 `pp st` -> status）


def help_detail():
    """`pluginplayer <命令> --help` 打出来的详细说明。"""
    return {
        "open": USAGE_OPEN + "\n\n"
                "行为: 前台运行 dist/pluginplayer.exe，直到播放器退出；退出码透传。\n"
                "  --port 会被翻译成播放器的 --rpc-port（--no-rpc 等价于 --rpc-port 0）。\n"
                "  控制通道令牌由播放器每次启动随机生成，写入 dist/config/rpc-token.txt；\n"
                "  启动后可直接 `%s status` 查看。" % CMD_NAME,
        "selftest": USAGE_SELFTEST + "\n\n"
                    "行为: 前台运行无人值守自检，退出码即自检结果；产物在 dist/<out>/。",
        "plugins": "用法: %s plugins [--remote]\n\n"
                   "  不带 --remote: 走 pluginplayer.exe --list-plugins（离线，不占端口）。\n"
                   "  带 --remote:   向运行中的播放器要 plugin.list（含 ABI 与加载状态）。" % CMD_NAME,
        "manifest": "用法: %s manifest\n\n"
                    "重新生成 dist/plugins/manifest.sha256。清单存在时宿主强制校验，\n"
                    "新增或替换插件后必须重新生成，否则该插件会被拒绝加载。" % CMD_NAME,
        "build": USAGE_BUILD + "\n\n转发给 build.py，不做二次实现；退出码透传。",
        "host-js": "用法: %s host-js [node 参数...] [-- 额外参数]\n\n"
                   "启动 host-js/src/index.mjs（Node + Cordis）。宿主自己读令牌文件，\n"
                   "所以先用 --port 开好控制通道再启动它。" % CMD_NAME,
        "doctor": "用法: %s doctor [--json]\n\n"
                  "逐项真校验：文件是否存在、DLL 是否齐备、插件哈希是否与清单一致、\n"
                  "编译器/Node 是否可用、控制通道是否在听。FAIL 会退出码 6，WARN 不影响。" % CMD_NAME,
        "install-cli": "用法: %s install-cli [--shim-dir 目录] [--add-to-path] [--dry-run]\n\n"
                       "写入 pluginplayer.cmd / pp.cmd。默认目录优先挑「已在本机 PATH 上」的位置\n"
                       "（通常是 <Python>\\Scripts，本机已在 PATH，因此无需改环境变量）。\n"
                       "垫片是纯 ASCII + CRLF（cmd.exe 按 GBK 解读 .bat）。" % CMD_NAME,
        "uninstall-cli": "用法: %s uninstall-cli [--shim-dir 目录]\n\n"
                         "只删除带本工具标记的垫片；同名的手工文件不会被碰。" % CMD_NAME,
        "version": "用法: %s version [--json]" % CMD_NAME,
        "status": "用法: %s status [--json]\n\n一次拿 player.info + player.stats + 当前音量。" % CMD_NAME,
        "play": "用法: %s play        (远端 player.play)" % CMD_NAME,
        "pause": "用法: %s pause       (远端 player.pause)" % CMD_NAME,
        "toggle": "用法: %s toggle      (远端 player.toggle)" % CMD_NAME,
        "stop": "用法: %s stop        (远端停止并回到 0 秒)" % CMD_NAME,
        "quit": "用法: %s quit        (远端 app.quit，让播放器退出)" % CMD_NAME,
        "seek": USAGE_SEEK,
        "volume": USAGE_VOLUME,
        "mute": USAGE_MUTE + "\n\n细节: 控制通道没有「只查不改」的 mute 方法，不给参数即为 toggle。",
        "speed": USAGE_SPEED,
        "cmd": USAGE_CMD,
        "commands": "用法: %s commands\n\n列出全部插件命令（含用法与归属插件）。" % CMD_NAME,
        "services": "用法: %s services\n\n列出插件注册的服务。" % CMD_NAME,
        "events": USAGE_EVENTS,
        "rpc": USAGE_RPC + "\n\n逃生舱：任何控制通道方法都能这样直接调。参数值能解析成 JSON\n"
                           "字面量就当字面量（--p seconds=12.5 送数字，--p name=gray 送字符串）。",
    }


def print_help(command=None):
    table = command_table()
    if command and command in table:
        # 帮助永远要打出来，不受 --json/--quiet 影响，所以走 out() 而不是 LOG.info()
        detail = help_detail().get(command)
        out(detail if detail else table[command][1])
        return
    out("")
    out("%s-cli %s -- PluginPlayer 命令行接口" % (CMD_NAME, CLI_VERSION))
    out("")
    out("用法: %s <命令> [选项]" % CMD_NAME)
    out("")
    out("本地编排:")
    for name, (_, desc) in table.items():
        if name in ("status", "play", "pause", "toggle", "stop", "seek", "volume", "mute",
                    "speed", "cmd", "commands", "services", "events", "rpc", "quit"):
            continue
        out("  %s" % desc)
    out("")
    out("控制通道（需要播放器以 --rpc-port N 启动）:")
    for name, (_, desc) in table.items():
        if name in ("status", "play", "pause", "toggle", "stop", "seek", "volume", "mute",
                    "speed", "cmd", "commands", "services", "events", "rpc", "quit"):
            out("  %s" % desc)
    out(HELP_TAIL)


MEDIA_EXT = {".mp4", ".mkv", ".avi", ".mov", ".flv", ".wmv", ".webm", ".ts", ".m4v",
             ".mp3", ".m4a", ".aac", ".flac", ".wav", ".ogg", ".opus", ".wma"}


def looks_like_media(tok):
    if os.path.isfile(tok):
        return True
    return os.path.splitext(tok)[1].lower() in MEDIA_EXT


def suggest(word, names):
    try:
        import difflib
        near = difflib.get_close_matches(word, names, n=3, cutoff=0.5)
    except Exception:
        near = []
    return ("你是指: %s ?" % ", ".join(near)) if near else ""


def reject_extra(args, usage):
    """不接受参数的命令统一走这里：以 - 开头报"未知选项"，否则报"不接受参数"。
    两者分开报，用户一眼就知道是自己打错了选项，还是多给了位置参数。"""
    if not args:
        return
    a = args[0]
    if a.startswith("-"):
        raise UsageError("未知选项: %s" % a, usage)
    raise UsageError("不接受位置参数: %s" % a, usage)


def main(argv=None):
    setup_stdio()
    argv = list(sys.argv[1:] if argv is None else argv)

    if not argv:
        print_help()
        return E_OK

    # 顶层 --help / --version
    if argv[0] in ("--help", "-h"):
        print_help()
        return E_OK
    if argv[0] in ("--version", "-V"):
        return cmd_version(Opts(), [])
    if argv[0] == "help":
        print_help(argv[1] if len(argv) > 1 else None)
        return E_OK

    opts = Opts()
    table = command_table()

    try:
        # 全局选项在前：一直吃到第一个非选项 token —— 那才是命令。
        i = parse_common(argv, 0, opts, "")
        if i >= len(argv):
            print_help()
            return E_OK
        name = argv[i]
        rest = argv[i + 1:]

        if name not in table:
            # 隐式 open：`pluginplayer 影片.mp4` 直接播。
            if looks_like_media(name):
                opts.apply_stream_flags()
                return cmd_open(opts, strip_common(argv[i + 1:], opts,
                                                   USAGE_OPEN)) or E_OK
            hint = suggest(name, list(table))
            raise UsageError("未知命令: %s" % name,
                             (hint + "\n" if hint else "") + "用 `%s --help` 看全部命令" % CMD_NAME)

        opts.apply_stream_flags()
        # cmd 自己处理全局选项（它后面是插件命令的参数，语义更微妙，见 cmd_invoke）。
        if name != "cmd":
            rest = strip_common(rest, opts, "用法: %s %s [选项]（%s --help 看细节）"
                                % (CMD_NAME, name, CMD_NAME))

        if rest and rest[0] in ("--help", "-h"):
            print_help(name)
            return E_OK

        fn = table[name][0]
        return fn(opts, rest) or E_OK

    except UsageError as exc:
        LOG.fail(str(exc))
        if exc.hint:
            err("      " + exc.hint)
        return exc.code
    except CliError as exc:
        if LOG.json:
            LOG.result({"ok": False, "error": exc.message, "code": exc.code})
        else:
            LOG.fail(str(exc))
            if exc.hint:
                err("      " + exc.hint)
        return exc.code
    except KeyboardInterrupt:
        err("")
        err("[ERR] 已中断")
        return E_INTR
    except BrokenPipeError:
        return E_OK


if __name__ == "__main__":
    sys.exit(main())
