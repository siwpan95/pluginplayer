#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
cli_check.py — PluginPlayer 命令行接口（tools/pp_cli.py）回归测试
================================================================

项目里没有单元测试框架，这个脚本刻意做成"直接跑、看退出码"的形式，
与 build.py --smoke / selftest.bat 的风格一致：

    F:\\Python314\\python.exe tests\\cli_check.py

覆盖两类东西：
  A. 静态契约 —— 命令垫片必须是纯 ASCII + CRLF（cmd.exe 按 GBK 解读 .bat，
     混入中文或 LF 会在别的机器上乱码/截断）。直接 import pp_cli 检查生成器。
  B. 真实行为 —— 把 CLI 当子进程跑，断言**退出码**与输出内容。
     退出码是本 CLI 对外契约的核心（0/1/2/3/4/5/6），必须逐个钉住。

需要 dist/ 的用例在 dist/ 不存在时记为 SKIP 并**不计入通过数**
（跳过 ≠ 通过，这条与项目 selftest 的口径一致）。
需要运行中播放器的用例同理。所有输出一律以 UTF-8 解码，避免受控制台代码页影响。

退出码: 0 = 全部通过；1 = 有失败项。
"""

import json
import os
import re
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TOOLS = os.path.join(ROOT, "tools")
CLI = os.path.join(TOOLS, "pp_cli.py")
DIST = os.path.join(ROOT, "dist")
EXE = os.path.join(DIST, "pluginplayer.exe")
TOKEN_FILE = os.path.join(DIST, "config", "rpc-token.txt")
DEFAULT_SAMPLE = os.path.join(DIST, "test_assets", "sample.mp4")

sys.path.insert(0, TOOLS)   # PYTHONSAFEPATH 下脚本目录不会自动进 sys.path
import pp_cli  # noqa: E402

RESULTS = []


def run_cli(*args, expect=None, timeout=60):
    """跑 CLI，返回 (退出码, stdout+stderr 文本)。"""
    cmd = [sys.executable, "-X", "utf8", CLI] + [str(a) for a in args]
    p = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       timeout=timeout)
    text = p.stdout.decode("utf-8", "replace")
    return p.returncode, text


def case(name, fn):
    try:
        ok, detail = fn()
    except AssertionError as exc:
        ok, detail = False, str(exc)
    except Exception as exc:                       # noqa: BLE001 - 测试要报出任何异常
        ok, detail = False, "%s: %s" % (type(exc).__name__, exc)
    if ok is None:
        RESULTS.append((name, "SKIP", detail))
        print("[SKIP] %-46s %s" % (name, detail))
    elif ok:
        RESULTS.append((name, "PASS", detail))
        print("[PASS] %-46s %s" % (name, detail))
    else:
        RESULTS.append((name, "FAIL", detail))
        print("[FAIL] %-46s %s" % (name, detail))


def free_port():
    """拿一个当前一定没人监听的端口（bind 后立刻释放）。"""
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def player_running():
    if not os.path.isfile(TOKEN_FILE):
        return False
    try:
        s = socket.create_connection(("127.0.0.1", pp_cli.DEFAULT_PORT), timeout=1.0)
        s.close()
        return True
    except OSError:
        return False


# ---------------------------------------------------------------------------
# A. 静态契约：垫片
# ---------------------------------------------------------------------------

def t_shim_ascii_crlf():
    body = pp_cli.shim_text("C:\\some\\where\\", "C:\\Program Files\\Python\\python.exe")
    raw = body.encode("ascii")                       # 非 ASCII 会直接抛 UnicodeEncodeError
    bad = [b for b in raw if b > 127]
    assert not bad, "垫片混入了非 ASCII 字节 %r" % bad[:8]
    lone = raw.count(b"\n") - raw.count(b"\r\n")
    assert lone == 0, "垫片有 %d 个裸 LF（必须全 CRLF）" % lone
    assert pp_cli.SHIM_MARKER in body, "垫片缺少卸载用的标记串"
    # 含空格的解释器路径必须被引号包住，否则 cmd 会拆词
    assert '"%PP_PY%" %PP_PYARG%' in body, "调用行没有给解释器加引号（路径含空格会断）"
    assert body.count("\r\n") >= 20
    return True, "纯 ASCII + 全 CRLF + 标记齐备（%d 字节）" % len(raw)


def t_shim_root_forms():
    """PATH 版（绝对路径）与仓库根版（%~dp0）必须共用同一份脚本体，
    只有 PP_ROOT 的赋值行不同 —— 否则两份垫片会各自漂移。"""
    a = pp_cli.shim_text(pp_cli.with_sep(ROOT), "x")
    b = pp_cli.shim_text("%~dp0", "x")
    assert a.replace(pp_cli.with_sep(ROOT), "R") == b.replace("%~dp0", "R"), \
        "两种 root 形式生成的脚本体不一致"
    assert pp_cli.with_sep("G:\\x") == "G:\\x\\", "with_sep 没有补尾部分隔符"
    return True, "绝对路径版与 %~dp0 版脚本一致"


# ---------------------------------------------------------------------------
# B. 真实行为：退出码
# ---------------------------------------------------------------------------

def t_help():
    rc, txt = run_cli("--help")
    assert rc == 0, "退出码 %d" % rc
    for kw in ("本地编排", "控制通道", "退出码"):
        assert kw in txt, "帮助里没有 %r" % kw
    return True, "退出码 0，含全部命令分组"


def t_help_bare_is_ok():
    """裸调用打帮助并退 0（git/docker 的惯例；不是错误）。"""
    rc, txt = run_cli()
    assert rc == 0, "退出码 %d" % rc
    assert "用法" in txt
    return True, "退出码 0"


def t_version():
    rc, txt = run_cli("--version")
    assert rc == 0, "退出码 %d" % rc
    assert "pluginplayer-cli" in txt and ROOT.replace("/", "\\") in txt.replace("/", "\\"), txt
    return True, "退出码 0，打印仓库根"


def t_cmd_help_detail():
    rc, txt = run_cli("status", "--help")
    assert rc == 0, "退出码 %d" % rc
    assert "player.info" in txt, "子命令帮助太薄: %r" % txt
    return True, "退出码 0，含子命令细节"


def t_unknown_command():
    rc, txt = run_cli("definitely_not_a_command")
    assert rc == pp_cli.E_USAGE, "退出码 %d，应为 %d" % (rc, pp_cli.E_USAGE)
    assert "未知命令" in txt
    return True, "退出码 2（用法错误）"


def t_typo_suggestion():
    rc, txt = run_cli("opne")
    assert rc == pp_cli.E_USAGE, "退出码 %d" % rc
    assert "open" in txt, "没有给出相近命令建议: %r" % txt
    return True, "退出码 2 + 建议 open"


def t_bad_number():
    rc, txt = run_cli("seek", "abc")
    assert rc == pp_cli.E_USAGE, "退出码 %d" % rc
    assert "非法的" in txt
    return True, "退出码 2（数值校验先于连接）"


def t_bad_size():
    """与 src/core/main.cpp 同一条边界：越界必须在启动播放器**之前**被挡掉。"""
    rc, txt = run_cli("open", "--size", "99999x99999", "whatever.mp4")
    assert rc == pp_cli.E_USAGE, "退出码 %d" % rc
    assert "160..8192" in txt, txt
    return True, "退出码 2（--size 越界，未启动进程）"


def t_missing_media():
    rc, txt = run_cli("open", os.path.join(ROOT, "no_such_media.mp4"))
    assert rc == pp_cli.E_USAGE, "退出码 %d" % rc
    assert "不存在" in txt
    return True, "退出码 2（媒体不存在）"


def t_unknown_option():
    rc, txt = run_cli("manifest", "--nope")
    assert rc == pp_cli.E_USAGE, "退出码 %d" % rc
    assert "未知选项" in txt
    return True, "退出码 2"


def t_rpc_port_hint():
    """open 用全局 --port，不该让人误写播放器的 --rpc-port；写错要有明确指引。"""
    rc, txt = run_cli("open", "--rpc-port", "7912", "x.mp4")
    assert rc == pp_cli.E_USAGE, "退出码 %d" % rc
    assert "--port" in txt and "翻译" in txt, txt
    return True, "退出码 2 + 指向 --port 的提示"


def t_connect_refused():
    rc, txt = run_cli("status", "--port", free_port())
    assert rc == pp_cli.E_CONN, "退出码 %d，应为 %d" % (rc, pp_cli.E_CONN)
    assert "连不上控制通道" in txt
    return True, "退出码 3（未监听）"


def t_bad_auth():
    """令牌不对必须是 4，而不是笼统的失败 —— 这两种故障的修法完全不同。"""
    port = free_port()
    # 起一个只会立刻关闭连接的监听器，用来确认 CLI 把"认证失败"与"连不上"分开
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(1)
    try:
        rc, txt = run_cli("status", "--port", port, "--token", "0" * 64, "--timeout", "3")
    finally:
        srv.close()
    assert rc in (pp_cli.E_AUTH, pp_cli.E_FAIL), "退出码 %d（应为 4=认证失败，或 1=超时）" % rc
    assert "认证失败" in txt or "超时" in txt, txt
    return True, "退出码 %d（认证/超时路径与连不上区分开）" % rc


def t_doctor_json():
    rc, txt = run_cli("--json", "doctor")
    assert rc in (0, pp_cli.E_ENV), "退出码 %d" % rc
    doc = json.loads(txt)                            # --json 必须是干净可解析的 JSON
    assert "checks" in doc and isinstance(doc["checks"], list), txt[:300]
    names = [c["name"] for c in doc["checks"]]
    for kw in ("Python", "内核 exe", "插件完整性清单"):
        assert kw in names, "体检缺少 %r（现有 %s）" % (kw, names)
    assert doc["ok"] == (rc == 0), "ok 字段与退出码不一致"
    return True, "%d 项检查，JSON 合法，ok 与退出码一致" % len(doc["checks"])


def t_manifest_integrity_real():
    """真算一遍 SHA-256，而不是只看文件在不在。"""
    import hashlib
    pdir = os.path.join(DIST, "plugins")
    mf = os.path.join(pdir, "manifest.sha256")
    if not os.path.isfile(mf):
        return None, "dist/plugins/manifest.sha256 不存在（先跑 build.py --release）"
    entries = pp_cli.read_manifest()
    assert entries, "清单解析为空"
    dlls = sorted(f for f in os.listdir(pdir) if f.lower().endswith(".dll"))
    assert dlls, "dist/plugins 下没有 DLL"
    for f in dlls:
        assert f in entries, "清单没登记 %s" % f
        h = hashlib.sha256()
        with open(os.path.join(pdir, f), "rb") as fp:
            for chunk in iter(lambda: fp.read(1 << 20), b""):
                h.update(chunk)
        assert h.hexdigest() == entries[f], "%s 哈希与清单不符" % f
    return True, "%d 个插件哈希与清单逐一相符" % len(dlls)


def t_plugins_offline():
    if not os.path.isfile(EXE):
        return None, "dist/pluginplayer.exe 不存在（先构建）"
    rc, txt = run_cli("plugins")
    assert rc == 0, "退出码 %d\n%s" % (rc, txt[-800:])
    m = re.search(r"插件目录共 (\d+) 个 DLL：成功 (\d+)，失败 (\d+)", txt)
    assert m, "输出里没有加载报告:\n%s" % txt[-800:]
    found, ok, bad = (int(x) for x in m.groups())
    assert found >= 5 and bad == 0 and ok == found, \
        "加载不完整: 发现 %d / 成功 %d / 失败 %d" % (found, ok, bad)
    return True, "5+ 插件全部加载成功（发现 %d 成功 %d 失败 %d）" % (found, ok, bad)


def t_install_cli_dry_run():
    rc, txt = run_cli("--json", "install-cli", "--dry-run")
    assert rc == 0, "退出码 %d" % rc
    doc = json.loads(txt)
    assert doc.get("dry_run") is True and os.path.isabs(doc["dir"]), txt
    assert doc.get("on_path") in (True, False)
    return True, "退出码 0，目标 %s（在 PATH: %s）" % (doc["dir"], doc["on_path"])


def t_uninstall_keeps_foreign_files():
    """卸载只删带标记的垫片 —— 绝不误删用户手工放的同名文件。"""
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        foreign = os.path.join(td, "pluginplayer.cmd")
        with open(foreign, "w", encoding="ascii") as fp:
            fp.write("@echo off\r\nrem my own launcher\r\n")
        rc, txt = run_cli("uninstall-cli", "--shim-dir", td)
        assert rc == 0, "退出码 %d" % rc
        assert os.path.isfile(foreign), "把别人的同名文件删了（严重）"
        assert "跳过" in txt, "没有说明跳过了非本工具生成的文件: %r" % txt
    return True, "退出码 0，非同源文件保留"


def t_install_uninstall_roundtrip():
    """在同一目录里装完再卸掉，必须恢复原状。"""
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        rc, _ = run_cli("install-cli", "--shim-dir", td)
        assert rc == 0, "安装退出码 %d" % rc
        for n in ("pluginplayer.cmd", "pp.cmd"):
            assert os.path.isfile(os.path.join(td, n)), "没写出 %s" % n
        rc, _ = run_cli("uninstall-cli", "--shim-dir", td)
        assert rc == 0, "卸载退出码 %d" % rc
        left = [f for f in os.listdir(td)]
        assert not left, "卸载后残留: %s" % left
    return True, "装→卸后目录为空"


def t_status_live():
    """有播放器在跑时，验证 status --json 的字段（含 volume 查询）。"""
    if not player_running():
        return None, "没有正在运行的播放器（先 open --hidden --rpc-port 7911 <媒体>）"
    rc, txt = run_cli("--json", "status")
    assert rc == 0, "退出码 %d\n%s" % (rc, txt[:400])
    doc = json.loads(txt)
    assert doc.get("ok") is True, txt[:300]
    for k in ("info", "stats", "volume"):
        assert k in doc, "status 缺字段 %r" % k
    assert doc["stats"].get("duration", 0) > 0, "duration 应为正数: %s" % doc["stats"]
    return True, "退出码 0，info/stats/volume 齐备（duration=%.2fs）" % doc["stats"]["duration"]


def t_seek_live():
    if not player_running():
        return None, "没有正在运行的播放器"
    rc, txt = run_cli("seek", "+1")
    assert rc == 0, "退出码 %d\n%s" % (rc, txt[:400])
    assert "位置" in txt, txt
    rc, txt = run_cli("--json", "seek", "2")
    assert rc == 0, "退出码 %d" % rc
    assert "position" in json.loads(txt), txt
    return True, "退出码 0，相对/绝对定位均可用"


def t_commands_live():
    if not player_running():
        return None, "没有正在运行的播放器"
    rc, txt = run_cli("commands")
    assert rc == 0, "退出码 %d" % rc
    for kw in ("effect.set", "snapshot", "stats", "quality.brightness", "radio.status"):
        assert kw in txt, "命令列表缺少 %r" % kw
    m = re.search(r"共 (\d+) 个命令", txt)
    assert m and int(m.group(1)) == 35, "命令数应为 35，实际 %s" % (m.group(1) if m else "?")
    return True, "退出码 0，35 个命令"


def t_invoke_live():
    if not player_running():
        return None, "没有正在运行的播放器"
    rc, txt = run_cli("cmd", "effect.status")
    assert rc == 0, "退出码 %d\n%s" % (rc, txt[:400])
    rc, txt = run_cli("cmd", "no.such.command")
    assert rc == pp_cli.E_REMOTE, "不存在的命令应退 %d，实际 %d" % (pp_cli.E_REMOTE, rc)
    return True, "真实命令退 0，不存在的命令退 5"


def t_tonemap_live():
    """video_tonemap 插件接线：命令可用、mode 切换不崩、切换后播放器仍活。

    帧处理量的像素级验证在 tests/unit/test_tonemap.cpp（115 项单测）；
    这里的 command.invoke 只回 return 码，日志不随 RPC 返回，故只做 rc 级断言。
    """
    if not player_running():
        return None, "没有正在运行的播放器"
    rc, txt = run_cli("cmd", "tonemap.status")
    assert rc == 0, "tonemap.status 退出码 %d\n%s" % (rc, txt[:400])
    rc, _ = run_cli("cmd", "tonemap.mode", "on")
    assert rc == 0, "tonemap.mode on 失败"
    rc, _ = run_cli("cmd", "tonemap.mode", "auto")
    assert rc == 0, "tonemap.mode auto 失败"
    rc, _ = run_cli("cmd", "tonemap.mode", "bogus")
    assert rc == 1, "命令存在但 return=-1 时 CLI 应退 E_FAIL(1)，实际 %d" % rc
    rc, txt = run_cli("cmd", "tonemap.status")
    assert rc == 0, "切换后 tonemap.status 退出码 %d" % rc
    return True, "mode on/auto 切换后播放器存活、命令均可用"


def t_events_live():
    if not player_running():
        return None, "没有正在运行的播放器"
    rc, txt = run_cli("events", "--seconds", "2", "--count", "3", timeout=30)
    assert rc == 0, "退出码 %d" % rc
    assert "app.tick" in txt, "没收到 app.tick 事件:\n%s" % txt[-400:]
    return True, "退出码 0，收到 app.tick"


def t_sample_autogen():
    """selftest 不给媒体时应能自己找到/生成样例（这条决定了 selftest 是否零参数可用）。"""
    if not os.path.isfile(EXE):
        return None, "dist/pluginplayer.exe 不存在（先构建）"
    if not os.path.isfile(os.path.join(ROOT, "vendor", "ffmpeg", "bin", "ffmpeg.exe")):
        return None, "vendor/ffmpeg 缺失，无法生成样例"
    p = pp_cli.ensure_sample()
    assert os.path.isfile(p) and os.path.getsize(p) > 10000, "样例无效: %s" % p
    return True, "%s（%.1f KB）" % (p, os.path.getsize(p) / 1024.0)


def t_forwarded_args_after_dashdash():
    """`--` 之后必须原样透传给播放器，CLI 不再二次解析（DSH 的语法归属原则）。

    这里不真的启动播放器：把 require_dist/run_inherit 换掉，直接看 cmd_open
    拼出来的 argv 长什么样。这样既不弹窗也不依赖 dist/。
    """
    o = pp_cli.Opts()
    o.quiet = True
    media = os.path.abspath(__file__)          # 只要是个存在的文件即可
    captured = {}

    orig_run, orig_dist = pp_cli.run_inherit, pp_cli.require_dist
    orig_quiet = pp_cli.LOG.quiet
    pp_cli.LOG.quiet = True                    # 别把 cmd_open 的"启动:"预告打到测试输出里
    pp_cli.require_dist = lambda: None
    pp_cli.run_inherit = lambda argv, cwd=None, env=None: captured.setdefault("argv", argv)
    try:
        # 1) -- 之前：越界值必须被 CLI 挡下
        try:
            pp_cli.cmd_open(o, ["--size", "99999x99999", media])
            raise AssertionError("-- 之前的越界 --size 竟然没被拦")
        except pp_cli.UsageError:
            pass
        # 2) -- 之后：同样的值原样透传，CLI 不校验
        pp_cli.cmd_open(o, [media, "--", "--size", "99999x99999"])
    finally:
        pp_cli.run_inherit, pp_cli.require_dist = orig_run, orig_dist
        pp_cli.LOG.quiet = orig_quiet

    argv = captured.get("argv")
    assert argv, "cmd_open 没有走启动路径"
    assert argv[-2:] == ["--size", "99999x99999"], "透传参数没有原样落在末尾: %s" % argv
    assert argv[-3] == media, "媒体路径应在透传参数之前: %s" % argv
    return True, "-- 之前拦下、之后原样透传"


def t_quit_open_connection_fast():
    """app.quit 后**客户端保持连接**时，进程必须在数秒内退出（rc=0）。

    回归背景：旧实现 kIdleTimeoutMs=30000，而 Windows 下跨线程 ::shutdown() 并不
    唤醒 pending recv —— 实测 quit 后进程卡满整整一个 recv 超时周期（30.5s）才退。
    修复后 kIdleTimeoutMs=1000，退出延迟应 ≤2s。这里自起一个隐藏实例，发完
    app.quit 故意不关 socket（正是旧实现会卡住的场景），掐表断言。"""
    if not os.path.isfile(EXE):
        return None, "dist/pluginplayer.exe 不存在（先构建）"
    port = free_port()
    token = "ABCDEF0123456789"
    env = dict(os.environ)
    env["SDL_VIDEODRIVER"] = "dummy"          # 无头跑，不弹窗
    env["SDL_AUDIODRIVER"] = "dummy"
    logf = os.path.join(os.environ.get("TEMP", "."),
                        "pp_quit_regression_%d.log" % port)
    p = subprocess.Popen(
        [EXE, "--hidden", "--log-level", "2",
         "--rpc-port", str(port), "--rpc-token", token],
        cwd=DIST, env=env,
        stdout=open(logf, "wb"), stderr=subprocess.STDOUT)
    s = None
    try:
        for _ in range(40):                    # 等控制通道监听（最多 10s）
            if p.poll() is not None:
                tail = open(logf, encoding="utf-8", errors="replace").read()[-400:]
                raise AssertionError("播放器提前退出 rc=%s\n%s" % (p.returncode, tail))
            try:
                s = socket.create_connection(("127.0.0.1", port), timeout=1.0)
                break
            except OSError:
                time.sleep(0.25)
        assert s, "控制通道迟迟没有监听 127.0.0.1:%d" % port
        s.sendall(('{"id":1,"method":"auth","params":{"token":"%s"}}\n' % token)
                  .encode("utf-8"))
        # 注意：未订阅任何主题的连接会收到**全部**广播事件（topics.empty() == 全部），
        # 所以认证应答之前可能先到 app.tick。必须跳过事件行，不能把第一行当应答。
        s.settimeout(5.0)
        buf = b""
        auth = None
        deadline = time.time() + 5.0
        while time.time() < deadline:
            while b"\n" not in buf:
                try:
                    chunk = s.recv(4096)
                except socket.timeout:
                    chunk = b""
                if not chunk:
                    break
                buf += chunk
            if b"\n" not in buf:
                break
            i = buf.index(b"\n")
            line = buf[:i].decode("utf-8", "replace").strip()
            buf = buf[i + 1:]
            if not line:
                continue
            try:
                obj = json.loads(line)
            except ValueError:
                continue
            if isinstance(obj, dict) and "event" in obj:
                continue
            auth = obj
            break
        assert auth and auth.get("ok"), "认证失败: %s" % (auth,)

        t0 = time.time()
        s.sendall(b'{"id":2,"method":"app.quit","params":{}}\n')
        for _ in range(60):                    # 最多等 6s，且**不关 socket**
            rc = p.poll()
            if rc is not None:
                elapsed = time.time() - t0
                assert rc == 0, "退出码应为 0，实际 %d" % rc
                assert elapsed < 5.0, "quit 后 %.1fs 才退出（回归：应 ≤2s）" % elapsed
                return True, "保持连接时 quit 后 %.1fs 退出（rc=0）" % elapsed
            time.sleep(0.1)
        raise AssertionError("quit 后 6 秒仍未退出（回归：卡满一个 idle 周期）")
    finally:
        try:
            s.close()
        except Exception:
            pass
        if p.poll() is None:
            p.kill()
        try:
            os.remove(logf)
        except OSError:
            pass


def main():
    print("=" * 78)
    print("PluginPlayer CLI 回归测试 (tools/pp_cli.py)")
    print("仓库根: %s" % ROOT)
    print("=" * 78)

    case("垫片: 纯 ASCII + CRLF + 标记", t_shim_ascii_crlf)
    case("垫片: 绝对路径版与 %~dp0 版一致", t_shim_root_forms)

    case("帮助: --help", t_help)
    case("帮助: 裸调用打帮助", t_help_bare_is_ok)
    case("版本: --version", t_version)
    case("帮助: 子命令 --help", t_cmd_help_detail)

    case("错误: 未知命令 -> 2", t_unknown_command)
    case("错误: 拼错给建议 -> 2", t_typo_suggestion)
    case("错误: 非法数值 -> 2", t_bad_number)
    case("错误: --size 越界 -> 2", t_bad_size)
    case("错误: 媒体不存在 -> 2", t_missing_media)
    case("错误: 未知选项 -> 2", t_unknown_option)
    case("错误: --rpc-port 误用给提示 -> 2", t_rpc_port_hint)
    case("错误: 端口未监听 -> 3", t_connect_refused)
    case("错误: 认证/超时与连不上分开", t_bad_auth)
    case("错误: -- 之后原样透传", t_forwarded_args_after_dashdash)

    case("doctor: --json 合法且字段齐", t_doctor_json)
    case("插件完整性: 逐文件重算 SHA-256", t_manifest_integrity_real)
    case("plugins: 离线加载报告", t_plugins_offline)
    case("selftest: 样例可自动生成", t_sample_autogen)

    case("install-cli: --dry-run", t_install_cli_dry_run)
    case("install/uninstall: 装→卸回归", t_install_uninstall_roundtrip)
    case("uninstall-cli: 不删他人文件", t_uninstall_keeps_foreign_files)

    case("控制通道: status", t_status_live)
    case("控制通道: seek", t_seek_live)
    case("控制通道: commands", t_commands_live)
    case("控制通道: cmd 调用与失败码", t_invoke_live)
    case("控制通道: video_tonemap 接线", t_tonemap_live)
    case("控制通道: events", t_events_live)
    case("控制通道: quit 保持连接也快速退出", t_quit_open_connection_fast)

    npass = sum(1 for _, s, _ in RESULTS if s == "PASS")
    nfail = sum(1 for _, s, _ in RESULTS if s == "FAIL")
    nskip = sum(1 for _, s, _ in RESULTS if s == "SKIP")
    print()
    print("==== CLI 回归: %d/%d 项通过，跳过 %d 项，失败 %d 项 ====" %
          (npass, npass + nfail, nskip, nfail))
    if nskip:
        print("（跳过项需要先 build.py --release 或先启动播放器，不计入通过数）")
    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main())
