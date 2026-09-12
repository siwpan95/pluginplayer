#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""test_verify.py — tools/verify.py 的解析与契约测试

门禁的价值完全取决于"它解析对了没有"：把文档解析歪了会变成假绿，
把实现解析歪了会变成假红（然后被人调松）。--selfcheck 验证的是"破坏能拦住"，
这里验证的是**解析器本身**在各种真实写法上算得对。
"""
import os
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import verify  # noqa: E402


class TestMethodNotationExpansion(unittest.TestCase):
    """ARCHITECTURE.md 的「方法一览」用的是压缩写法，展开错了就会假绿/假红。"""

    def test_expands_pipe_with_prefix(self):
        got = verify.expand_method_notation("`player.info|stats|seek`")
        self.assertEqual(got, {"player.info", "player.stats", "player.seek"})

    def test_expands_without_prefix_when_no_dot(self):
        got = verify.expand_method_notation("`red|green|blue`")
        self.assertEqual(got, {"red", "green", "blue"})

    def test_bare_names(self):
        got = verify.expand_method_notation("`auth`、`ping`")
        self.assertEqual(got, {"auth", "ping"})

    def test_full_documented_line(self):
        line = ("`auth`、`ping`、`player.info|stats|play|pause|toggle|stop|seek|volume|mute|speed|open`、"
                "`command.invoke|list`、`service.list`、`plugin.list`、`topic.subscribe`、`app.quit`")
        got = verify.expand_method_notation(line)
        self.assertIn("player.open", got)
        self.assertIn("command.list", got)
        self.assertIn("app.quit", got)
        self.assertEqual(len(got), 19)

    def test_ignores_parentheticals_and_empties(self):
        got = verify.expand_method_notation("`a`、`b`、、`c`")
        self.assertEqual(got, {"a", "b", "c"})


class TestDepRow(unittest.TestCase):

    SAMPLE = (
        "| 依赖 | 版本 | 来源 | 用途 |\n"
        "|---|---|---|---|\n"
        "| FFmpeg | 9.0.1 `full_build-shared` | 下载 | demux |\n"
        "| SDL3 | 3.4.16 | 本地 | 渲染 |\n"
        "\n"
        "正文里也提到 FFmpeg 9.0.1 与 SDL3 3.4.16（不该被当成表行）\n"
    )

    def test_finds_row(self):
        self.assertIn("9.0.1", verify.dep_row(self.SAMPLE, "FFmpeg"))
        self.assertIn("3.4.16", verify.dep_row(self.SAMPLE, "SDL3"))

    def test_missing_row_returns_none(self):
        self.assertIsNone(verify.dep_row(self.SAMPLE, "Cordis"))

    def test_row_only_matches_table_start(self):
        """关键：只能匹配行首的 `| 名字 |`，不能匹配正文里出现的名字 ——
        否则"版本号出现在别处"又会变成假绿。"""
        text = "正文里提到 FFmpeg 但没有表行\n"
        self.assertIsNone(verify.dep_row(text, "FFmpeg"))


class TestTreeTokens(unittest.TestCase):

    def test_plain_name(self):
        self.assertEqual(verify._tree_tokens("├─ src/"), ["src/"])

    def test_strips_trailing_description(self):
        got = verify._tree_tokens("│  ├─ player.cpp/h                 FFmpeg 解码、A/V 时钟同步、seek")
        self.assertEqual(got, ["player.cpp", "player.h"])

    def test_expands_combined_cpp_h(self):
        got = verify._tree_tokens("│  └─ config.cpp/h  util.cpp/h      配置持久化 / 日志")
        self.assertEqual(got, ["config.cpp", "config.h", "util.cpp", "util.h"])

    def test_cuts_parenthetical(self):
        """`AGENTS.md（CLAUDE.md 指向它）` 这种写法要切到括号为止，
        否则会当成一个不存在的文件名而误报。"""
        got = verify._tree_tokens("├─ AGENTS.md（CLAUDE.md 指向它）        仓库约定")
        self.assertEqual(got, ["AGENTS.md"])

    def test_strips_star_marker(self):
        self.assertEqual(verify._tree_tokens("│  ├─ sdk/player_plugin.h        ★ 插件 ABI"),
                         ["sdk/player_plugin.h"])

    def test_blank_lines(self):
        self.assertEqual(verify._tree_tokens("   "), [])
        self.assertEqual(verify._tree_tokens(""), [])


class TestNewlineHelpers(unittest.TestCase):

    def test_newline_count_counts_lone_lf(self):
        self.assertEqual(verify.newline_count(b"a\r\nb\r\n"), 0)
        self.assertEqual(verify.newline_count(b"a\nb\n"), 2)
        self.assertEqual(verify.newline_count(b"a\r\nb\n"), 1)

    def test_stray_cr_detection_catches_cr_cr_lf(self):
        """`\\r\\r\\n` 是 build.py 曾经生成过的坏行尾：
        只数裸 LF 看不出来（\\n 都配了 \\r），必须专门查孤立 CR。"""
        import re
        raw = b"@echo off\r\r\nsetlocal\r\r\n"
        self.assertEqual(raw.count(b"\n") - raw.count(b"\r\n"), 0)      # 旧检查：看起来没问题
        self.assertEqual(len(re.findall(rb"\r(?!\n)", raw)), 2)          # 新检查：抓到 2 个
        self.assertEqual(raw.count(b"\r\r\n"), 2)


class TestGateRegistry(unittest.TestCase):

    def test_names_unique(self):
        names = [n for n, _d, _f in verify.GATES]
        self.assertEqual(len(names), len(set(names)), "门禁名有重复")

    def test_every_gate_has_description_and_callable(self):
        for name, desc, fn in verify.GATES:
            self.assertTrue(desc.strip(), name)
            self.assertTrue(callable(fn), name)

    def test_expected_gates_present(self):
        names = {n for n, _d, _f in verify.GATES}
        for must in ("bat-ascii-crlf", "plugin-manifest", "release-payload",
                     "arg-bounds-align", "rpc-methods-align", "exit-codes-align",
                     "cli-commands-align", "versions-align", "third-party-notices",
                     "doc-links", "docs-index", "readme-tree", "no-credentials",
                     "plugin-command-count", "native-plugin-set", "js-plugin-count",
                     "portable-modules", "ci-entrypoints"):
            self.assertIn(must, names)

    def test_critical_gates_have_a_selfcheck_case(self):
        """每条门禁都该有"故意破坏"的用例，否则没人知道它拦不拦得住。
        新增门禁时忘了加用例，这里会红。"""
        covered = {gate for gate, _why, _apply in verify.selfcheck_cases()}
        for name, _d, _f in verify.GATES:
            self.assertIn(name, covered, "门禁 %s 没有 --selfcheck 破坏用例" % name)

    def test_runner_returns_text_not_crash(self):
        """跑一遍全部门禁（只读，不改仓库），确认没有门禁自身抛异常。"""
        problems = []
        for name, _d, fn in verify.GATES:
            try:
                fn(False)
            except (verify.Skip, verify.Fail):
                pass                       # 门禁的正常结论
            except Exception as exc:       # noqa: BLE001
                problems.append("%s: %s: %s" % (name, type(exc).__name__, exc))
        self.assertEqual(problems, [], "有门禁自身抛异常: %s" % problems)


class TestGatesSkipWithoutVendor(unittest.TestCase):
    """托管 CI 是 Linux、不会有 vendor/。这几条门禁在缺 vendor 时必须是 SKIP ——
    如果是 FAIL，云端流水线会因为"没下依赖"而红，而不是因为真的有问题；
    third-party-notices 更严重：生成器用 SystemExit 报"事实来源读不到"，
    不接住的话会把整个门禁进程掀掉（门禁只捕获 Exception，SystemExit 不是）。
    """

    def test_versions_align_skips(self):
        with tempfile.TemporaryDirectory() as d:
            with mock.patch.object(verify, "ROOT", d):
                with self.assertRaises(verify.Skip):
                    verify.g_versions(False)

    def test_notices_skips(self):
        import gen_third_party_notices as notices
        with tempfile.TemporaryDirectory() as d:
            with mock.patch.object(verify, "ROOT", d), \
                 mock.patch.object(notices, "FFVERSION_H", os.path.join(d, "missing.h")), \
                 mock.patch.object(notices, "SDL_VERSION_H", os.path.join(d, "missing2.h")):
                with self.assertRaises(verify.Skip):
                    verify.g_notices(False)

    def test_notices_survives_generator_systemexit(self):
        """即使前置判断被绕过，生成器的 SystemExit 也必须转成 SKIP 而不是崩。"""
        import gen_third_party_notices as notices
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, "THIRD_PARTY_NOTICES.md")
            open(out, "w", encoding="utf-8").write("whatever\n")
            with mock.patch.object(verify, "ROOT", d), \
                 mock.patch.object(notices, "OUT", out), \
                 mock.patch.object(notices, "FFVERSION_H", os.path.join(d, "missing.h")), \
                 mock.patch.object(notices, "SDL_VERSION_H", os.path.join(d, "missing2.h")):
                with self.assertRaises(verify.Skip):
                    verify.g_notices(False)

    def test_full_run_has_no_exception_on_empty_tree(self):
        """把 ROOT 指到空目录后跑一遍全部门禁：不能有门禁抛 Skip/Fail 之外的异常。"""
        problems = []
        with tempfile.TemporaryDirectory() as d, mock.patch.object(verify, "ROOT", d):
            for name, _desc, fn in verify.GATES:
                try:
                    fn(False)
                except (verify.Skip, verify.Fail):
                    pass
                except BaseException as exc:      # noqa: BLE001 - 就是要抓 SystemExit 之类
                    problems.append("%s: %s: %s" % (name, type(exc).__name__, exc))
        self.assertEqual(problems, [], "空仓库上这些门禁会炸: %s" % problems)


class TestWalkExclusions(unittest.TestCase):

    def test_does_not_walk_dependency_dirs(self):
        got = set(verify.walk_files((".py",)))
        bad = [f for f in got if f.startswith(("vendor/", "player/", "_extract/", "_deps/",
                                              "dist/", "build/"))
               or "/node_modules/" in f or f.endswith("__pycache__" )]
        self.assertEqual(bad, [], "遍历时漏掉了排除目录: %s" % bad[:5])

    def test_finds_known_files(self):
        got = set(verify.walk_files((".py",)))
        self.assertIn("tools/verify.py", got)
        self.assertIn("build.py", got)


class TestExitCodeAlignmentHelpers(unittest.TestCase):

    def test_section_extraction_stops_at_next_h2(self):
        text = "## 1. A\n内容A\n## 2. B\n内容B\n"
        import re
        m = re.search(r"## 1\. A(.*?)(?:\n## |\Z)", text, re.S)
        self.assertIn("内容A", m.group(1))
        self.assertNotIn("内容B", m.group(1))


if __name__ == "__main__":
    unittest.main(verbosity=2)
