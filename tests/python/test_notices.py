#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""test_notices.py — tools/gen_third_party_notices.py 的单元测试

这份生成物承担的是**许可合规**（随包 FFmpeg 是 GPL v3）。把它的口径测住，
等于把"发布物的许可声明"测住：载荷过滤错了会让声明里的文件数/体积对不上，
许可行漏了会让义务说不清楚。
"""
import os
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import gen_third_party_notices as notices  # noqa: E402


class TestRuntimeArtifactFilter(unittest.TestCase):
    """过滤口径必须和 build.py --release 的清理规则一致，
    否则声明里的"发布载荷 N 个文件"会把日志/自检产物也数进去。"""

    def test_dirs(self):
        self.assertTrue(notices.is_runtime_artifact("logs/pluginplayer.log"))

    def test_prefixed_dirs(self):
        self.assertTrue(notices.is_runtime_artifact("selftest_out/report.json"))
        self.assertTrue(notices.is_runtime_artifact("selftest_out_ci/frame_001.png"))
        self.assertTrue(notices.is_runtime_artifact("selftest_out_short/x.png"))
        self.assertTrue(notices.is_runtime_artifact("test_assets/sample.mp4"))

    def test_logs_anywhere(self):
        self.assertTrue(notices.is_runtime_artifact("deep/nested/thing.log"))

    def test_token_file(self):
        self.assertTrue(notices.is_runtime_artifact("config/rpc-token.txt"))

    def test_release_files_survive(self):
        for rel in ("pluginplayer.exe", "avcodec-63.dll", "plugins/osd_stats.dll",
                    "plugins/manifest.sha256", "config/pluginplayer.ini", "SDL3.dll"):
            self.assertFalse(notices.is_runtime_artifact(rel), rel)


class TestDistPayload(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        d = self.tmp.name
        for rel, size in (("pluginplayer.exe", 100), ("avcodec-63.dll", 200),
                          ("plugins/osd_stats.dll", 50), ("logs/x.log", 10),
                          ("selftest_out/a.json", 10), ("config/rpc-token.txt", 5)):
            p = os.path.join(d, rel.replace("/", os.sep))
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "wb") as f:
                f.write(b"x" * size)
        self.dist = d

    def test_payload_excludes_artifacts(self):
        with mock.patch.object(notices, "DIST", self.dist):
            files = notices.dist_payload()
        rels = sorted(r for r, _s in files)
        self.assertEqual(rels, ["avcodec-63.dll", "pluginplayer.exe", "plugins/osd_stats.dll"])

    def test_include_artifacts_shows_everything(self):
        with mock.patch.object(notices, "DIST", self.dist):
            files = notices.dist_payload(include_artifacts=True)
        self.assertEqual(len(files), 6)

    def test_missing_dist_returns_none(self):
        with mock.patch.object(notices, "DIST", os.path.join(self.tmp.name, "nope")):
            self.assertIsNone(notices.dist_payload())

    def test_classify_buckets(self):
        with mock.patch.object(notices, "DIST", self.dist):
            ff, sdl, mingw, plugins, other = notices.classify_dist(notices.dist_payload())
        self.assertEqual([r for r, _s in ff], ["avcodec-63.dll"])
        self.assertEqual([r for r, _s in plugins], ["plugins/osd_stats.dll"])
        self.assertEqual([r for r, _s in other], ["pluginplayer.exe"])
        self.assertEqual(sdl, [])
        self.assertEqual(mingw, [])

    def test_classify_recognizes_mingw_and_sdl(self):
        files = [("SDL3.dll", 10), ("libgcc_s_seh-1.dll", 10),
                 ("libstdc++-6.dll", 10), ("libwinpthread-1.dll", 10),
                 ("swscale-10.dll", 10), ("swresample-7.dll", 10)]
        ff, sdl, mingw, _plugins, other = notices.classify_dist(files)
        self.assertEqual(len(sdl), 1)
        self.assertEqual(len(mingw), 3)
        self.assertEqual(len(ff), 2)
        self.assertEqual(other, [])


class TestVersionParsing(unittest.TestCase):

    def test_ffmpeg_version(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "ffversion.h")
            open(p, "w", encoding="utf-8").write(
                '/* generated */\n#define FFMPEG_VERSION "9.0.1-full_build-www.gyan.dev"\n')
            with mock.patch.object(notices, "FFVERSION_H", p):
                self.assertEqual(notices.ffmpeg_version(), "9.0.1-full_build-www.gyan.dev")

    def test_ffmpeg_version_missing_fails_loud(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "ffversion.h")
            open(p, "w", encoding="utf-8").write("nothing here\n")
            with mock.patch.object(notices, "FFVERSION_H", p):
                with self.assertRaises(SystemExit):
                    notices.ffmpeg_version()

    def test_sdl_version(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "SDL_version.h")
            open(p, "w", encoding="utf-8").write(
                "#define SDL_MAJOR_VERSION   3\n"
                "#define SDL_MINOR_VERSION   4\n"
                "#define SDL_MICRO_VERSION   16\n")
            with mock.patch.object(notices, "SDL_VERSION_H", p):
                self.assertEqual(notices.sdl_version(), "3.4.16")

    def test_abi_version_strips_u_suffix(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "player_plugin.h")
            open(p, "w", encoding="utf-8").write("#define PP_ABI_VERSION 1u\n")
            with mock.patch.object(notices, "ABI_H", p):
                self.assertEqual(notices.abi_version(), "1")


class TestSelfLicenseRef(unittest.TestCase):

    def test_without_license_no_markdown_link(self):
        """没有 LICENSE 时必须**不**输出 [LICENSE](LICENSE) 链接 ——
        否则 doc-links 门禁会因为死链失败（这个坑第一版就踩过）。"""
        with mock.patch.object(notices, "ROOT", os.path.join(ROOT, "docs")):  # 该目录下没有 LICENSE
            text = notices._self_license_ref()
        self.assertNotIn("](LICENSE)", text)
        self.assertIn("LICENSE", text)
        self.assertIn("GPL v3", text)

    def test_with_license_links(self):
        with tempfile.TemporaryDirectory() as d:
            open(os.path.join(d, "LICENSE"), "w", encoding="utf-8").write("GPLv3\n")
            with mock.patch.object(notices, "ROOT", d):
                text = notices._self_license_ref()
        self.assertIn("](LICENSE)", text)


class TestLicenseKind(unittest.TestCase):
    """声明正文里有一句"因此 LICENSE 不能是专有许可"，那在没有 LICENSE 时是待办、
    在已有 GPLv3 时是已满足的约束。生成器必须**读内容**判断，否则条件一变就写成错话。"""

    def test_detects_gplv3(self):
        with tempfile.TemporaryDirectory() as d:
            open(os.path.join(d, "LICENSE"), "w", encoding="utf-8").write(
                "                    GNU GENERAL PUBLIC LICENSE\n"
                "                       Version 3, 29 June 2007\n")
            with mock.patch.object(notices, "ROOT", d):
                kind, is_v3 = notices.license_kind()
        self.assertTrue(is_v3)
        self.assertIn("GPL-3.0", kind)

    def test_detects_non_gplv3(self):
        with tempfile.TemporaryDirectory() as d:
            open(os.path.join(d, "LICENSE"), "w", encoding="utf-8").write("MIT License\n")
            with mock.patch.object(notices, "ROOT", d):
                kind, is_v3 = notices.license_kind()
                text = notices._self_license_ref()
        self.assertFalse(is_v3)
        self.assertIn("不兼容", text, "非 GPLv3 的 LICENSE 必须被明确警告")

    def test_missing_license(self):
        with tempfile.TemporaryDirectory() as d:
            with mock.patch.object(notices, "ROOT", d):
                kind, is_v3 = notices.license_kind()
        self.assertIsNone(kind)
        self.assertFalse(is_v3)

    def test_repo_license_is_gplv3(self):
        """本仓库现在**确实**是 GPLv3 —— 这条断言把"对外公开分发的前置条件"钉住：
        一旦 LICENSE 被换成不兼容的许可，测试立刻红。"""
        kind, is_v3 = notices.license_kind()
        self.assertTrue(is_v3, "仓库的 LICENSE 不是 GPLv3 原文（实际: %s）" % kind)


class TestDocument(unittest.TestCase):

    def setUp(self):
        self.doc = notices.build_document()

    def test_states_gpl_conclusion_up_front(self):
        head = self.doc[:2500]
        self.assertIn("GPL v3", head)
        self.assertIn("Corresponding Source", self.doc)

    def test_lists_ffmpeg_source_commit(self):
        self.assertIn("bf1b838f2a", self.doc, "必须给出对应源码的定位（GPL 义务）")

    def test_has_no_generated_at_timestamp(self):
        """写时间戳会让 --check 在不同时间/环境误报过期。"""
        self.assertNotIn("生成于", self.doc)
        self.assertNotIn("Python 3.", self.doc)

    def test_is_deterministic(self):
        self.assertEqual(self.doc, notices.build_document())

    def test_markdown_tables_have_separator_rows(self):
        """没有 `|---|` 的表格不会渲染 —— 生成器写错过一次，钉住。"""
        import re
        for block in re.findall(r"(?:^\|.*\n)+", self.doc, re.M):
            self.assertIn("|---", block, "有表格缺分隔行:\n%s" % block[:200])

    def test_mentions_all_redistributed_components(self):
        for kw in ("FFmpeg", "SDL3", "Cordis", "MinGW"):
            self.assertIn(kw, self.doc, kw)

    def test_dev_only_section_distinguishes_reference_from_dependency(self):
        self.assertIn("不随发布物分发", self.doc)
        self.assertIn("potplayer", self.doc)


class TestFreshnessCheck(unittest.TestCase):

    def test_check_mode_reports_stale(self):
        """--check 的核心行为：文件内容与重新生成的结果不一致就返回 1。"""
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, "THIRD_PARTY_NOTICES.md")
            with mock.patch.object(notices, "OUT", out):
                sys_argv = sys.argv
                try:
                    sys.argv = ["gen", "--check"]
                    self.assertEqual(notices.main(), 1)      # 文件不存在
                    with open(out, "w", encoding="utf-8") as f:
                        f.write("stale\n")
                    self.assertEqual(notices.main(), 1)      # 内容不对
                    with open(out, "w", encoding="utf-8") as f:
                        f.write(notices.build_document())
                    self.assertEqual(notices.main(), 0)      # 一致
                finally:
                    sys.argv = sys_argv


if __name__ == "__main__":
    unittest.main(verbosity=2)
