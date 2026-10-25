#!/usr/bin/env python3
"""Model-free admission, command isolation and output-limit checks."""
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from codex_patch_guard import dispatch, initialize, locked_state


class CodexGuardTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="shin-codex-unit-", dir="/private/tmp")
        self.directory = Path(self.temp.name).resolve()
        self.root = self.directory / "source"
        self.root.mkdir()
        self.state = self.directory / "state.json"
        initialize(self.state, self.root)

    def tearDown(self):
        self.temp.cleanup()

    def admit(self, number, command="printf ok", tool="Bash"):
        return dispatch(self.state, {"hook_event_name": "PreToolUse", "tool_name": tool,
            "tool_use_id": str(number), "tool_input": {"command": command}})["hookSpecificOutput"]

    def execute(self, number, command):
        result = self.admit(number, command)
        self.assertEqual(result["permissionDecision"], "allow")
        return subprocess.run(["/bin/bash", "-c", result["updatedInput"]["command"]],
                              capture_output=True, cwd=self.root, timeout=5)

    def test_parallel_calls_cannot_overspend(self):
        with ThreadPoolExecutor(max_workers=8) as workers:
            results = list(workers.map(self.admit, range(20)))
        self.assertEqual(sum(r["permissionDecision"] == "allow" for r in results), 12)
        self.assertEqual(len(json.loads(self.state.read_text())["calls"]), 12)

    def test_duplicate_call_is_denied(self):
        self.assertEqual(self.admit(0)["permissionDecision"], "allow")
        self.assertEqual(self.admit(0)["permissionDecision"], "deny")

    def test_concurrent_executions_cannot_overspend_output_bytes(self):
        admitted = [self.admit(i, "python3 -c \"print('x' * 9000)\"") for i in range(9)]
        children = [subprocess.Popen(["/bin/bash", "-c", row["updatedInput"]["command"]],
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE) for row in admitted]
        results = [child.communicate(timeout=10) for child in children]
        self.assertEqual(sum(len(out) for out, _ in results), 65536)
        state = json.loads(self.state.read_text())
        self.assertEqual(state["bytes"], 65536)
        self.assertEqual(len(state["outputs"]), 8)
        self.assertEqual(sum(child.returncode == 0 for child in children), 8)

    def test_non_shell_tools_cannot_execute(self):
        self.assertEqual(self.admit(0, tool="apply_patch")["permissionDecision"], "deny")
        self.assertEqual(self.admit(1, tool="spawn_agent")["permissionDecision"], "deny")

    def test_deadline_refuses_new_commands(self):
        with locked_state(self.state) as state:
            state["seconds"] = 0
        self.assertEqual(self.admit(0)["permissionDecision"], "deny")

    def test_output_is_accounted_before_delivery_and_reexecution_is_denied(self):
        result = self.execute(0, "python3 -c \"print('\\u2028' * 5000)\"")
        self.assertEqual(result.returncode, 0, result.stderr)
        result.stdout.decode("utf8")
        self.assertLessEqual(len(result.stdout), 8192)
        self.assertIn(b"pilot output clipped", result.stdout)
        state = json.loads(self.state.read_text())
        self.assertEqual(state["bytes"], len(result.stdout))
        self.assertTrue(state["outputs"]["0"]["clipped"])
        guard = Path(__file__).with_name("codex_patch_guard.py")
        duplicate = subprocess.run([sys.executable, "-B", guard, self.state, "execute", "0"], capture_output=True)
        self.assertEqual(duplicate.returncode, 2)

    def test_cumulative_output_is_bounded(self):
        for number in range(8):
            result = self.execute(number, "python3 -c \"print('x' * 9000)\"")
            self.assertEqual(len(result.stdout), 8192)
        self.assertEqual(json.loads(self.state.read_text())["bytes"], 65536)
        self.assertEqual(self.admit(9)["permissionDecision"], "deny")

    def test_source_edits_allowed_private_state_and_network_denied(self):
        import shlex
        script = "import pathlib,socket; pathlib.Path('own').write_text('yes'); " + \
            "checks=[lambda:pathlib.Path(" + repr(str(self.state)) + ").read_text()," + \
            "lambda:pathlib.Path(" + repr(str(self.state)) + ").write_text('tamper')," + \
            "lambda:socket.socket().bind(('127.0.0.1',0))]\nfor check in checks:\n try: check()\n" + \
            " except PermissionError: continue\n raise AssertionError('unexpected permission')\n"
        result = self.execute(0, "python3 -B -c " + shlex.quote(script))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.root / "own").read_text(), "yes")

    def test_short_command_deadline_kills_sleep(self):
        with locked_state(self.state) as state:
            state["seconds"] = 0.6
        result = self.execute(0, "sleep 3; printf should-not-happen")
        self.assertNotIn(b"should-not-happen", result.stdout)
        self.assertEqual(json.loads(self.state.read_text())["stopped"], "pilot-command-deadline")

    def test_closed_output_does_not_cancel_valid_command(self):
        result = self.execute(0, "exec 1>&- 2>&-; sleep 0.05; printf finished > own")
        self.assertEqual(result.returncode, 0)
        self.assertEqual((self.root / "own").read_text(), "finished")


if __name__ == "__main__":
    unittest.main()
