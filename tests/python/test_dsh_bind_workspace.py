#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""test_dsh_bind_workspace.py — tools/dsh_bind_workspace.py 的单元测试

覆盖文件通道的纯逻辑：快照结构校验、幂等判定（大小写/斜杠归一）、成对追加
（order 前置 + 记录字段齐全）、不认识的快照一个字都不改。
API 通道的行为在真实 dsh 实例上端到端验证（workspace.create 调用）；
这里不假装能用假服务器测它。

用标准库 unittest，不引入 pytest（项目零三方依赖的约定）。
"""
import json
import os
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import dsh_bind_workspace as bind  # noqa: E402


def snapshot():
    """一份最小但形状正确的 workspace.json 快照（字段照抄 dsh 落盘的结果）。"""
    return {
        "unit": {"name": "workspace", "version": 2},
        "global": {"initialized": True, "workspaceIds": ["aaaa"], "archivedSessionIds": []},
        "tables": {"workspaces": {
            "aaaa": {
                "path": "G:\\docs", "title": "docs", "sessionIds": [],
                "createdAt": "2026-08-28T03:41:19.775Z",
                "updatedAt": "2026-08-28T17:13:06.743Z",
            },
        }},
    }


class TestBindStorage(unittest.TestCase):
    """文件通道的核心：成对追加、幂等、拒绝陌生快照。"""

    def test_inserts_record_and_prepends_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            data = snapshot()
            created, workspace_id, title = bind.bind_storage(
                data, tmp, now_iso="2026-09-12T09:00:00.000Z", new_id="new-id")
            self.assertTrue(created)
            self.assertEqual(workspace_id, "new-id")
            self.assertEqual(title, os.path.basename(os.path.realpath(tmp)))
            self.assertEqual(data["global"]["workspaceIds"][0], "new-id")
            self.assertEqual(data["global"]["workspaceIds"][1], "aaaa")
            record = data["tables"]["workspaces"]["new-id"]
            self.assertEqual(record["sessionIds"], [])
            self.assertEqual(record["path"], os.path.realpath(tmp))
            self.assertEqual(record["createdAt"], "2026-09-12T09:00:00.000Z")
            self.assertEqual(record["updatedAt"], "2026-09-12T09:00:00.000Z")

    def test_existing_path_is_idempotent(self):
        with tempfile.TemporaryDirectory() as tmp:
            data = snapshot()
            data["tables"]["workspaces"]["aaaa"]["path"] = os.path.realpath(tmp)
            before = json.dumps(data, sort_keys=True)
            created, workspace_id, _ = bind.bind_storage(data, tmp)
            self.assertFalse(created)
            self.assertIsNone(workspace_id)
            self.assertEqual(json.dumps(data, sort_keys=True), before)

    def test_path_identity_ignores_case_and_separators(self):
        with tempfile.TemporaryDirectory() as tmp:
            data = snapshot()
            data["tables"]["workspaces"]["aaaa"]["path"] = \
                os.path.normcase(os.path.realpath(tmp)).replace("\\", "/")
            created, _, _ = bind.bind_storage(data, tmp)
            self.assertFalse(created)

    def test_rejects_foreign_snapshot(self):
        for data in (
            {},
            {"unit": {"name": "other"}},
            {"unit": {"name": "workspace"}, "global": {}, "tables": {}},
            {"unit": {"name": "workspace"}, "global": {"workspaceIds": []}, "tables": {"workspaces": []}},
        ):
            with self.assertRaises(bind.SkipBind, msg=repr(data)):
                bind.bind_storage(data, ".")

    def test_missing_directory_is_skipped(self):
        with self.assertRaises(bind.SkipBind):
            bind.bind_storage(snapshot(), os.path.join(tempfile.gettempdir(), "pp-no-such-dir"))


class TestHelpers(unittest.TestCase):
    def test_dsh_home_precedence(self):
        self.assertEqual(bind.resolve_dsh_home("C:/x", {"DSH_HOME": "D:/y"}),
                         os.path.abspath("C:/x"))
        self.assertEqual(bind.resolve_dsh_home(None, {"DSH_HOME": "D:/y"}),
                         os.path.abspath("D:/y"))
        self.assertTrue(bind.resolve_dsh_home(None, {}).endswith(".dsh"))

    def test_timestamp_shape(self):
        self.assertRegex(bind.utc_now_iso(), r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$")

    def test_default_repo_root_is_the_tools_parent(self):
        root = bind.default_repo_root()
        self.assertTrue(os.path.isfile(os.path.join(root, "tools", "dsh_bind_workspace.py")))


if __name__ == "__main__":
    unittest.main()
