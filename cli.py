#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""pluginplayer · 命令行接口。

由 allin 仓 `tools/publish_area_cli.py` 生成（2026-11-21）。
本文件只暴露**能从本仓自身事实推导**的能力，不含臆造的业务子命令：

    info        版本 / 入口 / 自检 / 依赖的机器可读概览（json）
    version     打印版本
    selfcheck   跑本仓无头自检（selfcheck.py）
    launch      启动本仓入口（）

来源事实：
    版本 = 未声明
    入口 = 无可识别入口
    自检 = 无

直接运行：`python cli.py [子命令]`
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
NAME = 'pluginplayer'
VERSION = ''
VERSION_SOURCE = '未声明'
ENTRY = ''
HAS_SELFCHECK = False


def _deps_count() -> int:
    """requirements.txt 里非注释非空行数（没有该文件则为 0）。"""
    req = ROOT / "requirements.txt"
    if not req.is_file():
        return 0
    n = 0
    for line in req.read_text(encoding="utf-8", errors="replace").splitlines():
        s = line.strip()
        if s and not s.startswith("#"):
            n += 1
    return n


def cmd_info(_args: argparse.Namespace) -> int:
    print(json.dumps({
        "name": NAME,
        "version": VERSION,
        "version_source": VERSION_SOURCE,
        "entry": ENTRY,
        "selfcheck": HAS_SELFCHECK,
        "requirements": _deps_count(),
        "root": str(ROOT),
    }, ensure_ascii=False, indent=2))
    return 0


def cmd_version(_args: argparse.Namespace) -> int:
    print(VERSION or "unknown")
    return 0


def _run(script: str, argv: list[str]) -> int:
    target = ROOT / script
    if not target.is_file():
        print(f"未找到 {script}", file=sys.stderr)
        return 3
    return subprocess.call([sys.executable, str(target), *argv])


def cmd_selfcheck(args: argparse.Namespace) -> int:
    if not HAS_SELFCHECK:
        print("本仓没有 selfcheck.py", file=sys.stderr)
        return 3
    return _run("selfcheck.py", args.passthrough)


def cmd_launch(args: argparse.Namespace) -> int:
    if not ENTRY:
        print("本仓无可识别的入口脚本（main.py / app.py / gui.py / run.py）", file=sys.stderr)
        return 3
    return _run(ENTRY, args.passthrough)


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog=NAME,
        description=f"{NAME} 命令行接口（由 allin publish_area_cli.py 生成）",
    )
    p.add_argument("--version", action="version", version=f"{NAME} {VERSION or 'unknown'}")
    sub = p.add_subparsers(dest="command")

    sub.add_parser("info", help="机器可读的仓概览（json）").set_defaults(func=cmd_info)
    sub.add_parser("version", help="打印版本").set_defaults(func=cmd_version)

    sc = sub.add_parser("selfcheck", help="跑本仓无头自检")
    sc.add_argument("passthrough", nargs=argparse.REMAINDER, help="透传给 selfcheck.py 的参数")
    sc.set_defaults(func=cmd_selfcheck)

    lc = sub.add_parser("launch", help="启动本仓入口脚本")
    lc.add_argument("passthrough", nargs=argparse.REMAINDER, help="透传给入口脚本的参数")
    lc.set_defaults(func=cmd_launch)
    return p


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if not getattr(args, "func", None):
        parser.print_help()
        return 0
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
