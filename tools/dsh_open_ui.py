#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
dsh_open_ui.py — dsh Web UI 就绪后，用系统默认浏览器打开它
===========================================================

`start-dsh.bat` 在拉起 dsh 之后**异步**调用本工具（用 pythonw.exe，无控制台窗口）：
轮询 127.0.0.1:3080 直到有人监听，再把 `http://127.0.0.1:3080` 交给默认浏览器。
于是"双击启动器 → 界面自己出现在浏览器里"，不用手动敲地址。

为什么必须轮询：端口在 dsh 启动后要过几秒到几十秒才监听（本机实测 ~20-30 秒），
直接 `start http://...` 会先开出一个"无法访问"的页面。等不到就静默退出——
这是便利功能，失败不是错误，启动器不依赖它。

用法::

    F:\\Python314\\pythonw.exe tools\\dsh_open_ui.py            # 无窗口后台（bat 里就用这个）
    F:\\Python314\\python.exe   tools\\dsh_open_ui.py --port 3080 --timeout 120
"""

import argparse
import os
import socket
import sys
import time

TAG = "[dsh-open]"


def wait_and_open(port=3080, timeout=120.0, connect=None, open_url=None, sleep=time.sleep,
                  on_ready=None):
    """轮询端口，就绪后（先 on_ready，再）打开浏览器；返回是否打开了（依赖可注入，便于单测）。

    @param connect: 单参可调用，成功返回可 close 的对象，失败抛 OSError。
    @param open_url: 单参可调用，用系统方式打开 URL。
    @param on_ready: 可选，就绪后的回调（把本项目准备成当前项目）；抛错不影响打开。
    """
    connect = connect or (lambda p: socket.create_connection(("127.0.0.1", p), timeout=0.5))
    open_url = open_url or os.startfile
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            connect(port).close()
        except OSError:
            sleep(1)
            continue
        if on_ready is not None:
            try:
                on_ready(port)
            except Exception:  # noqa: BLE001 —— 准备上下文失败不该拦住打开界面
                pass
        open_url("http://127.0.0.1:%d" % port)
        return True
    return False


def prepare_project_context(port):
    """就绪后把本项目准备成 dsh 的"当前项目"：工作区 + 一个会话（失败只警告）。

    启动前那次调用只能走存储通道（dsh 还没跑）；这里 dsh 已经在跑，走官方 API，
    顺带把该工作区的会话补上 —— 打开界面就看到项目的入口。
    """
    import dsh_bind_workspace as bind  # 同目录兄弟模块（脚本目录已在 sys.path 上）

    state, channel, detail = bind.run_bind(bind.default_repo_root(), bind.resolve_dsh_home(), port)
    print("%s project context: %s via %s: %s" % (TAG, state, channel, detail))
    return state == "failed"


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Open the dsh Web UI in the default browser once it accepts connections.")
    parser.add_argument("--port", type=int, default=3080)
    parser.add_argument("--timeout", type=float, default=120.0,
                        help="seconds to wait for the port (default 120)")
    parser.add_argument("--no-prepare", action="store_true",
                        help="only open the browser; skip the project-context step")
    args = parser.parse_args(argv)
    on_ready = None if args.no_prepare else prepare_project_context
    if wait_and_open(args.port, args.timeout, on_ready=on_ready):
        print("%s opened http://127.0.0.1:%d" % (TAG, args.port))
    else:
        print("%s warn: 127.0.0.1:%d not ready within %.0fs" % (TAG, args.port, args.timeout))
    return 0  # 便利功能：永远返回 0，不阻塞、不打断启动器


if __name__ == "__main__":
    sys.exit(main())
