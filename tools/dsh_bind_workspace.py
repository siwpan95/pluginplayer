#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
dsh_bind_workspace.py — 把某个目录注册成 DeepSeek Harness（dsh）的工作区
==========================================================================

`start-dsh.bat` 在拉起 dsh Web UI **之前**调用本工具，把启动器所在目录
（也就是本项目）注册成 dsh 的工作区。这样 http://127.0.0.1:3080 打开后，
侧边栏「工作区」里直接就有本项目、新建会话默认以本项目为工作目录 ——
不必每次手工在 UI 里点「添加工作区…」。

两条通道（自动选择，都幂等）：

  1) dsh 正在跑（探测 127.0.0.1:3080）→ 走官方 HTTP API
     `POST /api/workspace.create`，与 UI 点「添加工作区…」是同一条路径。
  2) dsh 没跑（启动器的正常场景）→ 直接编辑 `$DSH_HOME/storages/workspace.json`
     （dsh 的持久化快照）。此时没有实例在跑，写文件不会与内存态打架。

为什么必须两条通道都要：启动器是在 dsh **启动之前**跑的，那会儿 3080 还没人
监听，API 通道够不着；而 API 通道覆盖"dsh 已经在跑、用户又双击了一次启动器"
的场景（比如从另一个目录启动）。两边都先判断"该路径是否已注册"，已注册则
什么都不做。

为什么写文件是安全的：workspace.json 是完整快照（unit / global / tables 三段），
本工具只在整体解析成功后做**成对追加**（`global.workspaceIds` 前置 + 
`tables.workspaces` 加一条），然后原子替换（同目录临时文件 + os.replace）。
解析失败、或结构不是 dsh 写出来的那种快照时，一个字都不改。

$DSH_HOME 解析：环境变量 DSH_HOME，否则 `~/.dsh`（与 dsh 自身规则一致）。

**失败不阻塞启动器**：任何失败只打印 `[dsh-bind] warn:`，退出码 0；
`--strict` 时失败返回 2（给"想拿它当检查用"的脚本留的开关）。

用法::

    F:\\Python314\\python.exe tools\\dsh_bind_workspace.py                 # 绑定本仓库根目录
    F:\\Python314\\python.exe tools\\dsh_bind_workspace.py "D:\\somewhere"  # 绑定指定目录

    --api-port PORT   探测/调用的端口（默认 3080；0 = 跳过 API 通道）
    --dsh-home DIR    覆盖 $DSH_HOME（测试用）
    --strict          绑定失败时退出 2（默认 0）

`directory` 省略时绑定**本工具所在仓库的根目录**（`tools/` 的上一级），
`start-dsh.bat` 就按这个形式调用。不要给这个参数传 `"%~dp0"` 这种
"带尾反斜杠又被引号包住"的值：Windows 的 C 运行时会把它解析成
`...\\pluginplayer"`（末尾引号被反斜杠转义），目录校验必然失败。
"""

import argparse
import json
import os
import socket
import sys
import urllib.request
import uuid
from datetime import datetime, timezone

PROBE_TIMEOUT = 0.5
API_TIMEOUT = 10.0
TAG = "[dsh-bind]"


class SkipBind(Exception):
    """无法安全完成绑定时的原因（消息即原因，调用方只负责打印）。"""


def resolve_dsh_home(explicit=None, environ=None):
    """$DSH_HOME 的解析：--dsh-home > DSH_HOME 环境变量 > ~/.dsh。"""
    if explicit:
        return os.path.abspath(explicit)
    env = os.environ if environ is None else environ
    home = env.get("DSH_HOME")
    if home:
        return os.path.abspath(home)
    return os.path.join(os.path.expanduser("~"), ".dsh")


def path_key(path):
    """路径身份键：realpath + normpath + 大小写折叠（Windows 路径身份不区分大小写）。"""
    return os.path.normcase(os.path.normpath(os.path.realpath(path)))


def utc_now_iso():
    """dsh 落盘用的时间戳形状：`2026-09-12T08:59:55.509Z`。"""
    now = datetime.now(timezone.utc)
    return now.strftime("%Y-%m-%dT%H:%M:%S.") + "%03dZ" % (now.microsecond // 1000)


def bind_storage(data, directory, now_iso=None, new_id=None):
    """在 workspace.json 的解析结果里追加一条工作区记录（纯函数，原地修改 data）。

    返回 (created, workspace_id, title)；已注册时 created=False、workspace_id=None。
    结构不是 dsh 写出来的快照时抛 SkipBind —— 不猜格式，一个字都不改。
    """
    unit = data.get("unit")
    if not isinstance(unit, dict) or unit.get("name") != "workspace":
        raise SkipBind("unrecognized storage unit (expected unit.name == 'workspace')")
    global_ = data.get("global")
    tables = data.get("tables")
    if not isinstance(global_, dict) or not isinstance(tables, dict):
        raise SkipBind("unrecognized storage shape (global/tables missing)")
    workspaces = tables.get("workspaces")
    order = global_.get("workspaceIds")
    if not isinstance(workspaces, dict) or not isinstance(order, list):
        raise SkipBind("unrecognized workspace table/order")

    real = os.path.realpath(directory)
    key = path_key(directory)
    for record in workspaces.values():
        if isinstance(record, dict) and isinstance(record.get("path"), str) \
                and path_key(record["path"]) == key:
            return False, None, record.get("title")

    if not os.path.isdir(real):
        raise SkipBind("not a directory: %s" % real)
    workspace_id = new_id or str(uuid.uuid4())
    stamp = now_iso or utc_now_iso()
    title = os.path.basename(real.rstrip("\\/")) or real
    workspaces[workspace_id] = {
        "path": real,
        "title": title,
        "sessionIds": [],
        "createdAt": stamp,
        "updatedAt": stamp,
    }
    # 与 dsh 的 createCanonical 一致：新工作区插到注册顺序的最前。
    order.insert(0, workspace_id)
    return True, workspace_id, title


def bind_via_storage(home, directory):
    """文件通道：编辑 $DSH_HOME/storages/workspace.json。返回 (created, detail)。"""
    path = os.path.join(home, "storages", "workspace.json")
    if not os.path.isfile(path):
        raise SkipBind("no workspace.json yet at %s (start dsh once first)" % path)
    with open(path, "r", encoding="utf-8") as handle:
        raw = handle.read()
    try:
        data = json.loads(raw)
    except ValueError as exc:
        raise SkipBind("workspace.json is not valid JSON: %s" % exc)
    if not isinstance(data, dict):
        raise SkipBind("workspace.json is not a JSON object")
    created, workspace_id, title = bind_storage(data, directory)
    if not created:
        return False, "already bound: %s" % (title or directory)
    tmp = "%s.tmp-%d" % (path, os.getpid())
    with open(tmp, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(json.dumps(data, ensure_ascii=False, indent=2) + "\n")
    os.replace(tmp, path)
    return True, "workspace %s (%s)" % (workspace_id, title)


def api_call(port, method, payload):
    """POST /api/<method>，带 dsh 要求的 client-request 信封。"""
    body = json.dumps({
        "type": "client-request",
        "rpcId": "dsh-bind-" + method,
        "method": method,
        "payload": payload,
    }).encode("utf-8")
    request = urllib.request.Request(
        "http://127.0.0.1:%d/api/%s" % (port, method),
        data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(request, timeout=API_TIMEOUT) as response:
        return json.loads(response.read().decode("utf-8"))


def bind_via_api(port, directory):
    """API 通道：等价于 UI 的「添加工作区…」。返回 (created, workspace_id, title)。"""
    reply = api_call(port, "workspace.create", {"path": os.path.realpath(directory)})
    result = reply.get("result") or {}
    if not result.get("ok"):
        error = result.get("error") or {}
        raise SkipBind("dsh API refused: %s %s" % (error.get("code"), error.get("message")))
    value = result.get("value") or {}
    workspace = value.get("workspace") or {}
    return bool(value.get("created")), workspace.get("workspaceId"), workspace.get("title")


def ensure_api_session(port, workspace_id, title, directory):
    """确保该工作区下有一个 cwd = 本项目的会话，让"打开界面就在项目里"成立。

    已有 cwd 匹配的会话时什么都不做（幂等）。返回 (created, session_id)。
    """
    reply = api_call(port, "session.list", {})
    items = ((reply.get("result") or {}).get("value") or {}).get("items") or []
    key = path_key(directory)
    for item in items:
        cwd = item.get("cwd")
        if isinstance(cwd, str) and path_key(cwd) == key:
            return False, item.get("sessionId")
    reply = api_call(port, "session.create", {"workspaceId": workspace_id})
    session_id = ((reply.get("result") or {}).get("value") or {}).get("sessionId")
    if not session_id:
        raise SkipBind("dsh refused to create a session for workspace %s" % workspace_id)
    # 标题 = 目录名：会话列表里一眼认出是哪个项目（重命名失败不影响可用性）。
    try:
        api_call(port, "session.rename", {
            "sessionId": session_id,
            "title": title or os.path.basename(os.path.realpath(directory))})
    except Exception:  # noqa: BLE001
        pass
    return True, session_id


def api_alive(port):
    """3080 有没有人在听 —— 有则说明 dsh 已经在跑。"""
    if port <= 0:
        return False
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=PROBE_TIMEOUT):
            return True
    except OSError:
        return False


def run_bind(directory, home, api_port):
    """选通道并执行。返回 (state, channel, detail)，state ∈ {bound, exists, failed}。

    API 通道（dsh 在跑）不只注册工作区，还会确保该工作区下有一个会话 —— 这正是
    "用启动器打开当前项目"的落地：打开界面就有项目的入口。文件通道（dsh 没跑）只写
    工作区快照；会话由 dsh 就绪后那次 API 调用补上（见 dsh_open_ui.py）。
    """
    if api_alive(api_port):
        channel = "api"

        def attempt():
            created, workspace_id, title = bind_via_api(api_port, directory)
            session_created, session_id = ensure_api_session(
                api_port, workspace_id, title, directory)
            detail = "workspace %s (%s); session %s %s" % (
                workspace_id, title, session_id,
                "created" if session_created else "reused")
            return created, detail
    else:
        channel = "storage"
        attempt = lambda: bind_via_storage(home, directory)  # noqa: E731
    try:
        created, detail = attempt()
    except SkipBind as exc:
        return "failed", channel, str(exc)
    except Exception as exc:  # noqa: BLE001 —— 绑定的任何异常都不许阻断启动器
        return "failed", channel, "%s: %s" % (type(exc).__name__, exc)
    return ("bound" if created else "exists"), channel, detail


def default_repo_root():
    """本工具所在仓库的根目录（tools/ 的上一级）—— 不传参数时的默认绑定目标。"""
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Bind a directory as a DeepSeek Harness (dsh) workspace (idempotent).")
    parser.add_argument("directory", nargs="?", default=None,
                        help="directory to bind; defaults to this tool's repository root")
    parser.add_argument("--api-port", type=int, default=3080,
                        help="dsh web port to probe/use; 0 skips the API channel (default: 3080)")
    parser.add_argument("--dsh-home", default=None, help="override $DSH_HOME")
    parser.add_argument("--strict", action="store_true",
                        help="exit 2 when the bind fails (default: warn and exit 0)")
    args = parser.parse_args(argv)

    directory = os.path.abspath(args.directory or default_repo_root())
    state, channel, detail = run_bind(directory, resolve_dsh_home(args.dsh_home), args.api_port)
    if state == "bound":
        print("%s bound %s via %s: %s" % (TAG, directory, channel, detail))
        return 0
    if state == "exists":
        print("%s already a workspace: %s" % (TAG, detail))
        return 0
    print("%s warn: cannot bind %s via %s: %s" % (TAG, directory, channel, detail))
    return 2 if args.strict else 0


if __name__ == "__main__":
    sys.exit(main())
