# SPDX-License-Identifier: GPL-3.0-or-later
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from tools import run_realism_history as history


class RealismHistoryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="g3t-history-unit-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.fake = self.root / "fake.dll"
        self.fake.write_bytes(b"unit fixture; never loaded")
        self.output = self.root / "results"
        self.git_calls = []
        self.commands = []

    def git(self, *arguments):
        self.git_calls.append(arguments)
        return "1234567890abcdef" if "rev-parse" in arguments else ""

    def runner(self, command, **_):
        self.commands.append(command)
        output = Path(command[command.index("--output-dir") + 1])
        self.assertFalse(any(output.iterdir()))
        summary = {"schema": 1, "cases": [{"name": "connect", "status": "FAIL",
                                          "reasons": ["observed refusal"]}]}
        (output / "summary.json").write_text(json.dumps(summary), encoding="utf-8")
        return subprocess.CompletedProcess(command, 1)

    def test_same_scenarios_and_runner_for_every_revision(self):
        with patch.object(history, "git", self.git), \
                patch.object(history.subprocess, "run", self.runner):
            result = history.run_history(self.fake, self.output, "all", 60)
        self.assertEqual(len(result["runs"]), 4)
        self.assertEqual(len(self.commands), 4)
        for command in self.commands:
            self.assertEqual(command[1], str(history.ROOT / "tools" / "run_steam_realism.py"))
            self.assertEqual(command[command.index("--scenario") + 1], "all")
            self.assertEqual(command[command.index("--seconds") + 1], "60")
            self.assertFalse(Path(command[command.index("--source-root") + 1])
                             .is_relative_to(history.ROOT))
        self.assertEqual(sum("add" in call for call in self.git_calls), 4)
        self.assertEqual(sum("remove" in call for call in self.git_calls), 4)

    def test_failures_are_retained_not_relabelled_expected_passes(self):
        with patch.object(history, "git", self.git), \
                patch.object(history.subprocess, "run", self.runner):
            result = history.run_history(self.fake, self.output, "metadata", 3)
        for run in result["runs"]:
            self.assertEqual(run["exit_code"], 1)
            self.assertEqual(run["summary"]["cases"][0]["status"], "FAIL")
        saved = json.loads((self.output / "matrix.json").read_text(encoding="utf-8"))
        self.assertEqual(saved, result)

    def test_removes_worktree_after_runner_error(self):
        with patch.object(history, "git", self.git), \
                patch.object(history.subprocess, "run", side_effect=OSError("launch refused")):
            with self.assertRaisesRegex(OSError, "launch refused"):
                history.run_history(self.fake, self.output)
        self.assertEqual(sum("add" in call for call in self.git_calls), 1)
        self.assertEqual(sum("remove" in call for call in self.git_calls), 1)

    def test_missing_summary_is_an_error_and_cleanup_still_runs(self):
        with patch.object(history, "git", self.git), \
                patch.object(history.subprocess, "run",
                             return_value=subprocess.CompletedProcess([], 0)):
            with self.assertRaisesRegex(RuntimeError, "produced no summary"):
                history.run_history(self.fake, self.output)
        self.assertEqual(sum("remove" in call for call in self.git_calls), 1)

    def test_failed_worktree_add_is_not_force_removed(self):
        with patch.object(history, "git", side_effect=RuntimeError("git refused")) as call:
            with self.assertRaisesRegex(RuntimeError, "git refused"):
                history.run_history(self.fake, self.output)
        self.assertEqual(call.call_count, 1)

    def test_partial_add_failure_cleans_registered_source(self):
        def partial_git(*arguments):
            self.git_calls.append(arguments)
            if "add" in arguments:
                source = Path(arguments[3])
                source.mkdir()
                (source / ".git").write_text("partial fixture", encoding="ascii")
                raise RuntimeError("checkout failed after registration")
            return ""
        with patch.object(history, "git", partial_git):
            with self.assertRaisesRegex(RuntimeError, "after registration"):
                history.run_history(self.fake, self.output)
        self.assertEqual(sum("remove" in call for call in self.git_calls), 1)

    def test_git_timeout_is_reported_explicitly(self):
        with patch.object(history.subprocess, "run",
                          side_effect=subprocess.TimeoutExpired(["git"], 120)):
            with self.assertRaisesRegex(RuntimeError, "exceeded 120 seconds"):
                history.git("worktree", "list")

    def test_rejects_stale_result_directories_before_adding_a_worktree(self):
        self.output.mkdir()
        (self.output / "matrix.json").write_text("{}", encoding="ascii")
        with patch.object(history, "git") as call:
            with self.assertRaisesRegex(RuntimeError, "fresh empty directory"):
                history.run_history(self.fake, self.output)
        call.assert_not_called()


if __name__ == "__main__":
    unittest.main()
