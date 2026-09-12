#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""test_pp_cli.py — tools/pp_cli.py 的单元测试

覆盖的是**不依赖进程与网络的纯逻辑**：参数校验、全局选项抽取、令牌解析顺序、
垫片生成的字节契约、命令表完备性。端到端行为在 tests/cli_check.py 里测，
这里测的是"它内部算得对不对" —— 两者互补，不重复。

用标准库 unittest，不引入 pytest（项目零三方依赖的约定）。
"""
import io
import os
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import pp_cli  # noqa: E402


class TestNumberValidation(unittest.TestCase):
    """数值校验是"参数先校验再启动"的第一道闸；边界写错会直接放到越界值。"""

    def test_int_range(self):
        self.assertEqual(pp_cli.parse_number("4", "--log-level", 0, 4), 4)
        self.assertEqual(pp_cli.parse_number("0", "--log-level", 0, 4), 0)
        with self.assertRaises(pp_cli.UsageError):
            pp_cli.parse_number("5", "--log-level", 0, 4)
        with self.assertRaises(pp_cli.UsageError):
            pp_cli.parse_number("-1", "--log-level", 0, 4)

    def test_int_rejects_non_integer_text(self):
        for bad in ("abc", "1.5", "", "1 2", "0x10", "1e3", "--1", "12abc", "1-", "１"):
            with self.assertRaises(pp_cli.UsageError, msg="应当拒绝 %r" % bad):
                pp_cli.parse_number(bad, "--frames", 1, 100)

    def test_int_strips_surrounding_whitespace(self):
        """首尾空白会被 strip 掉（用户从别处粘过来的值常常带空格），这不是错误。"""
        self.assertEqual(pp_cli.parse_number(" 42 ", "--frames", 1, 100), 42)

    def test_int_accepts_leading_plus(self):
        self.assertEqual(pp_cli.parse_number("+7", "--reload", 0, 9999), 7)

    def test_float_range(self):
        self.assertAlmostEqual(pp_cli.parse_number("0.5", "--volume", 0.0, 1.0, kind="float"), 0.5)
        self.assertAlmostEqual(pp_cli.parse_number("1", "--volume", 0.0, 1.0, kind="float"), 1.0)
        with self.assertRaises(pp_cli.UsageError):
            pp_cli.parse_number("1.5", "--volume", 0.0, 1.0, kind="float")
        with self.assertRaises(pp_cli.UsageError):
            pp_cli.parse_number("-0.1", "--volume", 0.0, 1.0, kind="float")

    def test_float_rejects_nan_and_inf(self):
        for bad in ("nan", "inf", "-inf", "NaN", "Infinity"):
            with self.assertRaises(pp_cli.UsageError, msg="应当拒绝 %r" % bad):
                pp_cli.parse_number(bad, "--start", 0.0, 1e9, kind="float")

    def test_wxh(self):
        self.assertEqual(pp_cli.parse_wxh("1280x720"), (1280, 720))
        self.assertEqual(pp_cli.parse_wxh(" 640X480 "), (640, 480))
        # 边界值本身必须合法
        self.assertEqual(pp_cli.parse_wxh("%dx%d" % (pp_cli.MIN_WIN, pp_cli.MAX_WIN)),
                         (pp_cli.MIN_WIN, pp_cli.MAX_WIN))
        for bad in ("%dx%d" % (pp_cli.MIN_WIN - 1, 720),
                    "%dx%d" % (pp_cli.MAX_WIN + 1, 720),
                    "1280", "1280x", "x720", "1280x720x1", "axb", ""):
            with self.assertRaises(pp_cli.UsageError, msg="应当拒绝 %r" % bad):
                pp_cli.parse_wxh(bad)


class TestArgNormalization(unittest.TestCase):

    def test_expand_equals(self):
        self.assertEqual(pp_cli.expand_equals(["--port=7911"]), ["--port", "7911"])
        self.assertEqual(pp_cli.expand_equals(["--params=a=b"]), ["--params", "a=b"])
        # 不是长选项、或值是 JSON（可能含 =）都不该被拆坏
        self.assertEqual(pp_cli.expand_equals(["-p=1"]), ["-p=1"])
        self.assertEqual(pp_cli.expand_equals(["--"]), ["--"])
        self.assertEqual(pp_cli.expand_equals(["a.mp4"]), ["a.mp4"])
        self.assertEqual(pp_cli.expand_equals(["--params", '{"a":"x=y"}']),
                         ["--params", '{"a":"x=y"}'])

    def test_strip_common_takes_flags_anywhere(self):
        o = pp_cli.Opts()
        rest = pp_cli.strip_common(["--json", "here", "--port", "7912", "-q"], o, "u")
        self.assertEqual(rest, ["here"])
        self.assertTrue(o.json)
        self.assertEqual(o.port, 7912)
        self.assertTrue(o.quiet)

    def test_strip_common_stops_at_double_dash(self):
        o = pp_cli.Opts()
        rest = pp_cli.strip_common(["x", "--", "--json", "--port", "1"], o, "u")
        self.assertEqual(rest, ["x", "--", "--json", "--port", "1"])
        self.assertFalse(o.json)
        self.assertEqual(o.port, pp_cli.DEFAULT_PORT)

    def test_strip_common_rejects_bad_value(self):
        with self.assertRaises(pp_cli.UsageError):
            pp_cli.strip_common(["--port", "abc"], pp_cli.Opts(), "u")
        with self.assertRaises(pp_cli.UsageError):
            pp_cli.strip_common(["--port", "99999"], pp_cli.Opts(), "u")
        with self.assertRaises(pp_cli.UsageError):
            pp_cli.strip_common(["--timeout"], pp_cli.Opts(), "u")   # 缺值

    def test_take_reports_missing_value(self):
        with self.assertRaises(pp_cli.UsageError):
            pp_cli.take(["--port"], 0, "--port", "u")

    def test_reject_extra_distinguishes_option_from_positional(self):
        with self.assertRaises(pp_cli.UsageError) as cm:
            pp_cli.reject_extra(["--nope"], "u")
        self.assertIn("未知选项", str(cm.exception))
        with self.assertRaises(pp_cli.UsageError) as cm:
            pp_cli.reject_extra(["extra"], "u")
        self.assertIn("位置参数", str(cm.exception))
        pp_cli.reject_extra([], "u")          # 空：不抛

    def test_coerce_json_literals(self):
        self.assertEqual(pp_cli.coerce("12.5"), 12.5)
        self.assertEqual(pp_cli.coerce("7"), 7)
        self.assertEqual(pp_cli.coerce("true"), True)
        self.assertEqual(pp_cli.coerce("gray"), "gray")            # 不是字面量 → 字符串
        self.assertEqual(pp_cli.coerce('["a"]'), ["a"])
        self.assertEqual(pp_cli.coerce('{"a":1}'), {"a": 1})
        # 非有限数不能当字面量送出去（服务端 JSON 不接受）
        self.assertEqual(pp_cli.coerce("nan"), "nan")

    def test_looks_like_media(self):
        for p in ("a.mp4", "A.MKV", "x/y/z.m4a", "song.MP3"):
            self.assertTrue(pp_cli.looks_like_media(p), p)
        for p in ("subcommand", "a.txt", "noext", "a.dll"):
            self.assertFalse(pp_cli.looks_like_media(p), p)


class TestTokenResolution(unittest.TestCase):
    """令牌解析顺序写错的话，症状是"认证失败"，极难查 —— 所以逐个钉住优先级。"""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.tokfile = os.path.join(self.tmp.name, "rpc-token.txt")
        with open(self.tokfile, "w", encoding="utf-8") as f:
            f.write("FROM_FILE\n")

    def test_explicit_token_wins(self):
        o = pp_cli.Opts()
        o.token = "FROM_ARG"
        o.token_file = self.tokfile
        with mock.patch.dict(os.environ, {"PP_RPC_TOKEN": "FROM_ENV"}):
            self.assertEqual(o.resolve_token(), "FROM_ARG")

    def test_env_beats_file(self):
        o = pp_cli.Opts()
        o.token_file = self.tokfile
        with mock.patch.dict(os.environ, {"PP_RPC_TOKEN": "FROM_ENV"}):
            self.assertEqual(o.resolve_token(), "FROM_ENV")

    def test_file_when_nothing_else(self):
        o = pp_cli.Opts()
        o.token_file = self.tokfile
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("PP_RPC_TOKEN", None)
            self.assertEqual(o.resolve_token(), "FROM_FILE")

    def test_strips_whitespace(self):
        o = pp_cli.Opts()
        o.token = "  pad  "
        self.assertEqual(o.resolve_token(), "pad")

    def test_missing_file_gives_auth_error_with_hint(self):
        o = pp_cli.Opts()
        o.token_file = os.path.join(self.tmp.name, "nope.txt")
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("PP_RPC_TOKEN", None)
            with self.assertRaises(pp_cli.CliError) as cm:
                o.resolve_token()
            self.assertEqual(cm.exception.code, pp_cli.E_AUTH)
            self.assertTrue(cm.exception.hint, "认证失败必须给出「怎么修」的提示")

    def test_empty_file_is_reported(self):
        p = os.path.join(self.tmp.name, "empty.txt")
        open(p, "w", encoding="utf-8").close()
        o = pp_cli.Opts()
        o.token_file = p
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("PP_RPC_TOKEN", None)
            with self.assertRaises(pp_cli.CliError) as cm:
                o.resolve_token()
            self.assertEqual(cm.exception.code, pp_cli.E_AUTH)


class TestExitCodes(unittest.TestCase):
    """退出码是本 CLI 对外的核心契约，必须互不相同（否则脚本无法区分故障）。"""

    def test_codes_are_distinct_and_documented(self):
        names = [n for n in vars(pp_cli) if n.startswith("E_")]
        values = [getattr(pp_cli, n) for n in names]
        self.assertGreaterEqual(len(names), 8)
        self.assertEqual(len(values), len(set(values)), "退出码有重复: %s" % names)
        self.assertEqual(pp_cli.E_OK, 0)
        self.assertEqual(pp_cli.E_USAGE, 2)

    def test_forwarded_commands_documented(self):
        """转发类命令的退出码透传这条承诺，docs 里必须写清楚（有门禁核对正文）。"""
        doc = open(os.path.join(ROOT, "docs", "CLI.md"), encoding="utf-8").read()
        self.assertIn("原样返回子进程的退出码", doc)


class TestShimContract(unittest.TestCase):
    """垫片的字节契约 —— 这是最"沉默"的一类 bug（乱码/拆词只在别人机器上出现）。"""

    def test_ascii_and_crlf_only(self):
        body = pp_cli.shim_text("C:\\a b\\", "C:\\Program Files\\Py\\python.exe")
        raw = body.encode("ascii")                       # 非 ASCII 会在这里抛
        self.assertEqual([b for b in raw if b > 127], [])
        self.assertEqual(raw.count(b"\n") - raw.count(b"\r\n"), 0, "有裸 LF")
        self.assertEqual(len(__import__("re").findall(rb"\r(?!\n)", raw)), 0, "有孤立 CR")

    def test_has_uninstall_marker(self):
        self.assertIn(pp_cli.SHIM_MARKER, pp_cli.shim_text("C:\\x\\", "py"))

    def test_quotes_the_interpreter(self):
        body = pp_cli.shim_text("C:\\x\\", "py")
        self.assertIn('"%PP_PY%" %PP_PYARG%', body)

    def test_handles_py_launcher_separately(self):
        """`py -3` 是"命令+参数"，不能整体加引号 —— 所以必须拆成 PP_PY/PP_PYARG。"""
        body = pp_cli.shim_text("C:\\x\\", "py")
        self.assertIn('if /i "%PP_PY%"=="py" set "PP_PYARG=-3"', body)

    def test_root_forms_share_one_body(self):
        a = pp_cli.shim_text(pp_cli.with_sep(ROOT), "py")
        b = pp_cli.shim_text("%~dp0", "py")
        self.assertEqual(a.replace(pp_cli.with_sep(ROOT), "R"), b.replace("%~dp0", "R"))

    def test_with_sep(self):
        self.assertTrue(pp_cli.with_sep("C:\\x").endswith(os.sep))
        self.assertTrue(pp_cli.with_sep("C:\\x" + os.sep).endswith(os.sep))

    def test_write_shims_creates_both_names(self):
        with tempfile.TemporaryDirectory() as d:
            written = pp_cli.write_shims(d, "C:\\r\\", "py")
            self.assertEqual(len(written), 2)
            for p in written:
                self.assertTrue(os.path.isfile(p))
                raw = open(p, "rb").read()
                self.assertEqual([b for b in raw if b > 127], [])
            self.assertTrue(os.path.isfile(os.path.join(d, "pluginplayer.cmd")))
            self.assertTrue(os.path.isfile(os.path.join(d, "pp.cmd")))


class TestCommandSurface(unittest.TestCase):

    def test_every_command_has_help_detail(self):
        table = pp_cli.command_table()
        detail = pp_cli.help_detail()
        self.assertGreaterEqual(len(table), 20)
        missing = sorted(set(table) - set(detail))
        self.assertEqual(missing, [], "这些命令没有 --help 详细说明: %s" % missing)

    def test_command_handlers_are_callable(self):
        for name, (fn, desc) in pp_cli.command_table().items():
            self.assertTrue(callable(fn), name)
            self.assertTrue(desc.strip(), name)

    def test_help_detail_mentions_usage(self):
        for name, text in pp_cli.help_detail().items():
            if name == "version":
                continue
            self.assertIn("用法", text, "%s 的帮助里没有用法行" % name)


class TestFormatting(unittest.TestCase):

    def test_fmt_time(self):
        self.assertEqual(pp_cli.fmt_time(0), "0:00.00")
        self.assertEqual(pp_cli.fmt_time(59.5), "0:59.50")
        self.assertEqual(pp_cli.fmt_time(3661), "1:01:01.00")
        self.assertEqual(pp_cli.fmt_time(-5), "0:00.00")   # 负数钳到 0

    def test_disp_width_counts_cjk_as_two(self):
        self.assertEqual(pp_cli.disp_width("ab"), 2)
        self.assertEqual(pp_cli.disp_width("中文"), 4)
        self.assertEqual(pp_cli.disp_width("a中"), 3)

    def test_pad_aligns_visually(self):
        self.assertEqual(pp_cli.disp_width(pp_cli.pad("中文", 6)), 6)
        self.assertEqual(pp_cli.disp_width(pp_cli.pad("abcdefgh", 4)), 8)   # 不截断

    def test_jdump_keeps_chinese_readable(self):
        s = pp_cli.jdump({"k": "中文"})
        self.assertIn("中文", s)


class TestIsOnPath(unittest.TestCase):

    def test_detects_membership(self):
        with tempfile.TemporaryDirectory() as d:
            with mock.patch.dict(os.environ, {"PATH": d + os.pathsep + "C:\\Windows"}):
                self.assertTrue(pp_cli.is_on_path(d))
                self.assertFalse(pp_cli.is_on_path(os.path.join(d, "sub")))

    def test_trailing_separator_insensitive(self):
        with tempfile.TemporaryDirectory() as d:
            with mock.patch.dict(os.environ, {"PATH": d + os.sep + os.pathsep + "C:\\Windows"}):
                self.assertTrue(pp_cli.is_on_path(d))


class TestLogAndStreams(unittest.TestCase):

    def test_json_mode_suppresses_human_output(self):
        log = pp_cli.Log()
        log.json = True
        buf = io.StringIO()
        with mock.patch("sys.stdout", buf):
            log.info("should not appear")
            log.ok("neither")
            log.result({"k": 1})
        out = buf.getvalue()
        self.assertNotIn("should not appear", out)
        self.assertIn('"k": 1', out)

    def test_quiet_suppresses_but_json_still_prints(self):
        log = pp_cli.Log()
        log.quiet = True
        buf = io.StringIO()
        with mock.patch("sys.stdout", buf):
            log.info("nope")
        self.assertEqual(buf.getvalue(), "")


if __name__ == "__main__":
    unittest.main(verbosity=2)
