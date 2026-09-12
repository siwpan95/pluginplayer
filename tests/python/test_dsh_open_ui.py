#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""test_dsh_open_ui.py — tools/dsh_open_ui.py 的单元测试

只测纯逻辑（轮询 + 打开时机），用注入的假 connect/open_url 替代真实网络与浏览器；
真实行为由 start-dsh.bat 的端到端验证覆盖。

用标准库 unittest，不引入 pytest（项目零三方依赖的约定）。
"""
import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import dsh_open_ui as open_ui  # noqa: E402


class FakeConn:
    def __init__(self, calls):
        self.calls = calls

    def close(self):
        self.calls.append("close")


class TestWaitAndOpen(unittest.TestCase):
    def test_opens_once_the_port_answers(self):
        calls = []

        def connect(port):
            calls.append(port)
            return FakeConn(calls)

        opened = []
        ok = open_ui.wait_and_open(3080, 5, connect=connect,
                                   open_url=opened.append, sleep=lambda _s: None)
        self.assertTrue(ok)
        self.assertEqual(opened, ["http://127.0.0.1:3080"])
        self.assertEqual(calls, [3080, "close"])

    def test_refused_port_then_success_waits(self):
        state = {"n": 0}

        def connect(port):
            state["n"] += 1
            if state["n"] < 3:
                raise OSError("connection refused")
            return FakeConn([])

        slept = []
        opened = []
        ok = open_ui.wait_and_open(3081, 60, connect=connect, open_url=opened.append,
                                   sleep=slept.append)
        self.assertTrue(ok)
        self.assertEqual(state["n"], 3)
        self.assertEqual(slept, [1, 1])
        self.assertEqual(opened, ["http://127.0.0.1:3081"])

    def test_times_out_without_opening(self):
        def connect(_port):
            raise OSError("connection refused")

        opened = []
        ok = open_ui.wait_and_open(3082, 0.0, connect=connect, open_url=opened.append,
                                   sleep=lambda _s: None)
        self.assertFalse(ok)
        self.assertEqual(opened, [])


if __name__ == "__main__":
    unittest.main()
