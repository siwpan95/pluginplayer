#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
rpc_client.py — PluginPlayer 控制通道测试客户端（同时是「进程外插件」的最小示例）

控制通道**强制令牌认证**：连接后第一条消息必须是
    {"id":1,"method":"auth","params":{"token":"<64 位 hex>"}}
本脚本会自动按下面顺序找令牌：
    1) 命令行 --token HEX
    2) 环境变量 PP_RPC_TOKEN
    3) 命令行 --token-file PATH
    4) 默认文件 <本脚本目录>/../dist/config/rpc-token.txt

用法:
    F:/Python314/python.exe tools/rpc_client.py [port]
    F:/Python314/python.exe tools/rpc_client.py --token <hex>
    F:/Python314/python.exe tools/rpc_client.py 7911 --token-file dist/config/rpc-token.txt
"""
import argparse
import json
import os
import socket
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_TOKEN_FILE = os.path.join(ROOT, "dist", "config", "rpc-token.txt")

# 服务端硬上限（与 src/core/control.h 中的常量保持一致）
SERVER_MAX_LINE_BYTES = 4 * 1024 * 1024
SERVER_MAX_CLIENTS = 8


def resolve_token(token_arg, token_file):
    if token_arg:
        return token_arg.strip()
    env = os.environ.get("PP_RPC_TOKEN", "").strip()
    if env:
        return env
    path = token_file or DEFAULT_TOKEN_FILE
    try:
        with open(path, "r", encoding="utf-8") as f:
            t = f.read().strip()
        if t:
            return t
    except OSError as e:
        raise SystemExit(
            f"[ERR] 找不到控制通道令牌文件: {path} ({e})\n"
            f"      请用 --token <hex> 指定，或先启动播放器（启动时会生成该文件），\n"
            f"      或设置环境变量 PP_RPC_TOKEN。")
    raise SystemExit(f"[ERR] 令牌文件 {path} 为空")


class Conn:
    """一条未认证的裸连接。"""

    def __init__(self, port, timeout=10):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.buf = b""
        self.next_id = 1

    def send_raw(self, data):
        self.sock.sendall(data)

    def call(self, method, **params):
        req = {"id": self.next_id, "method": method, "params": params}
        self.next_id += 1
        self.sock.sendall((json.dumps(req) + "\n").encode("utf-8"))
        while True:
            line, self.buf = self._readline()
            if line is None:
                raise RuntimeError("连接已关闭")
            obj = json.loads(line)
            if obj.get("event"):
                print(f"  [event] {obj['event']}  {json.dumps(obj.get('data'), ensure_ascii=False)}")
                continue
            return obj

    def _readline(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                return None, self.buf
            self.buf += chunk
        i = self.buf.index(b"\n")
        line, self.buf = self.buf[:i], self.buf[i + 1:]
        return line.decode("utf-8", "replace").strip(), self.buf

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


class Player(Conn):
    """已认证的连接。"""

    def __init__(self, port, token, timeout=10):
        super().__init__(port, timeout=timeout)
        r = self.call("auth", token=token)
        if not r.get("ok"):
            raise RuntimeError(f"认证失败: {r}")
        self.auth_result = r


def main():
    ap = argparse.ArgumentParser(description="PluginPlayer 控制通道测试客户端")
    ap.add_argument("port", nargs="?", type=int, default=7911)
    ap.add_argument("--token", default="")
    ap.add_argument("--token-file", default="")
    args = ap.parse_args()

    token = resolve_token(args.token, args.token_file)
    port = args.port

    results = []

    def step(name, fn):
        try:
            r = fn()
            results.append((name, True, r))
            print(f"[PASS] {name}: {json.dumps(r, ensure_ascii=False)[:200]}")
        except Exception as e:
            results.append((name, False, str(e)))
            print(f"[FAIL] {name}: {e}")

    # ---------------- 认证边界（安全回归） ----------------
    def wrong_token_rejected():
        c = Conn(port)
        try:
            try:
                r = c.call("auth", token="0" * 64)
            except (RuntimeError, OSError) as e:
                # 服务端认证失败后立即断开（RST/EOF），这也是可接受的结果
                return {"rejected": True, "closed": str(e)}
            if r.get("ok"):
                raise AssertionError(f"错误令牌竟然通过了认证: {r}")
            return {"rejected": True, "error": r.get("error")}
        finally:
            c.close()

    def unauthenticated_call_rejected():
        c = Conn(port)
        try:
            try:
                r = c.call("player.stats")
            except (RuntimeError, OSError) as e:
                return {"rejected": True, "closed": str(e)}
            if r.get("ok"):
                raise AssertionError("未认证竟然可以调用 player.stats")
            return {"rejected": True, "error": r.get("error")}
        finally:
            c.close()

    step("auth.错误令牌被拒", wrong_token_rejected)
    step("auth.未认证调用被拒", unauthenticated_call_rejected)

    p = Player(port, token)
    print(f"[INFO] 认证通过 (port={port})")

    step("ping", lambda: p.call("ping"))
    step("player.info", lambda: p.call("player.info"))
    step("player.stats", lambda: p.call("player.stats"))
    step("topic.subscribe", lambda: p.call("topic.subscribe", topic="*"))
    step("service.list", lambda: p.call("service.list"))
    step("plugin.list", lambda: p.call("plugin.list"))

    p.call("player.pause")
    st = p.call("player.stats")["result"]
    step("player.pause", lambda: {"paused": st["paused"]})
    step("player.play", lambda: p.call("player.play"))

    step("player.seek(2.0)", lambda: p.call("player.seek", seconds=2.0))
    time.sleep(0.3)
    step("position after seek", lambda: {"position": p.call("player.stats")["result"]["position"]})

    step("player.volume", lambda: p.call("player.volume", value=0.5))
    step("player.speed", lambda: p.call("player.speed", value=1.5))
    step("player.speed 1.0", lambda: p.call("player.speed", value=1.0))

    step("command.list", lambda: {"count": p.call("command.list")["result"]["count"]})
    step("command.invoke stats", lambda: p.call("command.invoke", name="stats"))
    step("command.invoke effect.set gray", lambda: p.call("command.invoke", name="effect.set", args=["gray"]))
    step("command.invoke effect.clear", lambda: p.call("command.invoke", name="effect.clear"))
    step("command.invoke hotkey.list", lambda: p.call("command.invoke", name="hotkey.list"))
    step("command.invoke 不存在", lambda: p.call("command.invoke", name="no.such.command"))

    # ---------------- 资源边界（安全回归） ----------------
    def oversized_line_rejected():
        c = Conn(port)
        try:
            c.call("auth", token=token)
            # 发送超过服务端单条报文上限的、**不含换行**的数据
            c.send_raw(b"a" * (SERVER_MAX_LINE_BYTES + 512 * 1024))
            c.sock.settimeout(15)
            try:
                while True:
                    chunk = c.sock.recv(65536)
                    if not chunk:
                        return {"rejected": True, "reason": "服务端断开连接"}
                    c.buf += chunk
                    if b"\n" in c.buf:
                        line = c.buf.split(b"\n")[0].decode("utf-8", "replace")
                        return {"rejected": True, "reason": line[:200]}
            except socket.timeout:
                raise AssertionError("服务端未在超时内拒绝超长报文")
            except (ConnectionResetError, ConnectionAbortedError) as e:
                return {"rejected": True, "reason": f"连接被重置: {e}"}
        finally:
            c.close()

    def concurrency_capped():
        conns = []
        try:
            refused = 0
            for _ in range(SERVER_MAX_CLIENTS + 4):
                try:
                    c = Conn(port, timeout=5)
                except OSError:
                    refused += 1
                    break
                # 探测是否被立即拒绝（会收到上限提示并被关闭）
                c.sock.settimeout(2)
                try:
                    chunk = c.sock.recv(65536)
                except socket.timeout:
                    chunk = b""
                except OSError:
                    chunk = b""
                if chunk:
                    refused += 1
                    c.close()
                    break
                conns.append(c)
            if refused == 0:
                raise AssertionError(
                    f"并发上限未生效：成功建立了 {len(conns)} 条连接（上限 {SERVER_MAX_CLIENTS}）")
            return {"capped": True, "accepted_before_refuse": len(conns),
                    "server_limit": SERVER_MAX_CLIENTS}
        finally:
            for c in conns:
                c.close()

    step("资源.超长报文被拒", oversized_line_rejected)
    step("资源.并发连接上限", concurrency_capped)

    p.close()

    ok = sum(1 for _, o, _ in results if o)
    print(f"\n=== 控制通道: {ok}/{len(results)} 通过 ===")
    return 0 if ok == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
