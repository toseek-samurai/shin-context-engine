"""Persistent local identities; no login, transport, Git or server enrollment."""
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import uuid

TOOL = Path(sys.argv.pop(1)).resolve()
PROFILES = Path(sys.argv.pop(1)).resolve()


class IdentityTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="shin-local-identity-")
        self.addCleanup(tmp.cleanup)
        self.base = Path(tmp.name).resolve()
        self.home = self.base / "home"
        self.root = self.project("checkout")

    def project(self, name):
        root = self.base / name
        root.mkdir(parents=True)
        (root / "README.md").write_text("# Widget\n\nWidget retry ceiling is seven.\n")
        (root / "widget.js").write_text("export const retryCeiling = 7;\n")
        return root

    def run_ce(self, command="init", root=None, home=None, status=0):
        args = [str(TOOL), command, "--root", str(root or self.root), "--home", str(home or self.home)]
        if command == "init": args += ["--no-service", "--profile-dir", str(PROFILES)]
        if command == "query": args += ["retryCeiling"]
        run = subprocess.run(args, text=True, capture_output=True, timeout=30, env={**os.environ, "PATH": ""})
        self.assertEqual(run.returncode, status, run.stdout + run.stderr)
        return json.loads(run.stderr if status == 2 else run.stdout)

    def workspace(self, root=None, home=None):
        return (home or self.home) / "projects" / hashlib.sha256(str(root or self.root).encode()).hexdigest()

    def files(self, path):
        return {str(p.relative_to(path)): (p.read_bytes(), p.stat().st_mode & 0o777)
                for p in path.rglob("*") if p.is_file()}

    def write(self, path, value):
        path.write_text(json.dumps(value)); path.chmod(0o600)

    def test_read_before_init_does_not_create_state(self):
        self.run_ce("identity", status=2)
        self.assertFalse(self.home.exists())

    def test_stable_across_init_changes_and_read_only_inspection(self):
        first = self.run_ce()["identity"]
        for key in ("installation_id", "local_workspace_id"):
            self.assertEqual(uuid.UUID(first[key]).version, 4)
        self.assertNotEqual(first["installation_id"], first["local_workspace_id"])
        self.assertIsNone(first["server_binding"])
        self.assertFalse(first["authentication"])
        self.assertFalse(first["content_currentness_assessed"])
        self.assertEqual(self.run_ce()["identity"], first)
        before = self.files(self.home)
        self.assertEqual(self.run_ce("identity")["identity"], first)
        self.assertEqual(before, self.files(self.home))
        (self.root / "widget.js").write_text("export const retryCeiling = 9;\n")
        self.assertEqual(self.run_ce("identity")["identity"], first)
        self.assertEqual(self.run_ce()["identity"], first)

    def test_different_installations_same_directory_do_not_share_identity(self):
        first = self.run_ce()["identity"]
        other = self.run_ce(home=self.base / "second-home")["identity"]
        self.assertNotEqual(first["installation_id"], other["installation_id"])
        self.assertNotEqual(first["local_workspace_id"], other["local_workspace_id"])

    def test_equal_names_and_content_are_separate_working_copies(self):
        roots = [self.project(f"copy-{i}/same-name") for i in range(4)]
        with ThreadPoolExecutor(max_workers=4) as pool:
            identities = list(pool.map(lambda root: self.run_ce(root=root)["identity"], roots))
        self.assertEqual(len({x["installation_id"] for x in identities}), 1)
        self.assertEqual(len({x["local_workspace_id"] for x in identities}), 4)
        for root, identity in zip(roots, identities):
            self.assertEqual(self.run_ce(root=root)["identity"], identity)

    def test_identity_reads_do_not_conflict_with_other_project_enrollment(self):
        expected = self.run_ce()["identity"]
        roots = [self.project(f"parallel-{i}") for i in range(4)]
        def reads():
            for _ in range(30): self.assertEqual(self.run_ce("identity")["identity"],expected)
        with ThreadPoolExecutor(max_workers=3) as pool:
            reader = pool.submit(reads)
            list(pool.map(lambda root: self.run_ce(root=root),roots))
            reader.result()

    def test_interrupted_catalog_commit_keeps_the_same_working_copy_id(self):
        original = self.run_ce()["identity"]
        catalog = self.home / "identity/state.json"
        state = json.loads(catalog.read_text()); state["workspaces"] = {}
        self.write(catalog,state)
        before = self.files(self.home)
        self.assertEqual(self.run_ce("identity",status=2)["error"]["code"],"local-identity-unavailable")
        self.assertEqual(before,self.files(self.home))
        self.assertEqual(self.run_ce()["identity"],original)

    def test_copied_home_is_rejected_without_repair(self):
        self.run_ce()
        copied = self.base / "copied"
        shutil.copytree(self.home, copied)
        before = self.files(copied)
        for command in ("identity", "init"):
            self.assertEqual(self.run_ce(command, home=copied, status=2)["error"]["code"], "local-identity-conflict")
        self.assertEqual(before, self.files(copied))

    def test_other_installation_or_workspace_stamp_cannot_be_substituted(self):
        self.run_ce()
        other_root = self.project("other")
        self.run_ce(root=other_root)
        own_file = self.workspace() / "identity/state.json"
        foreign_file = self.workspace(other_root) / "identity/state.json"
        own_file.write_bytes(foreign_file.read_bytes())
        before = self.files(self.home)
        for command in ("identity", "init"):
            self.assertEqual(self.run_ce(command, status=2)["error"]["code"], "local-identity-conflict")
        self.assertEqual(before, self.files(self.home))

    def test_changed_installation_id_is_not_adopted_by_existing_workspace(self):
        self.run_ce()
        path = self.home / "identity/state.json"
        value = json.loads(path.read_text()); value["installation_id"] = str(uuid.uuid4())
        self.write(path, value)
        before = self.files(self.home)
        self.assertEqual(self.run_ce(status=2)["error"]["code"], "local-identity-conflict")
        self.assertEqual(before, self.files(self.home))

    def test_missing_installation_does_not_regenerate_even_for_new_project(self):
        self.run_ce()
        shutil.rmtree(self.home / "identity")
        other = self.project("new-project")
        before = self.files(self.home)
        for root in (self.root, other):
            self.assertEqual(self.run_ce(root=root, status=2)["error"]["code"], "local-identity-unavailable")
        self.assertEqual(before, self.files(self.home))

    def test_missing_committed_records_are_not_replaced(self):
        self.run_ce()
        for directory in (self.home / "identity", self.workspace() / "identity"):
            with self.subTest(directory=directory):
                file = directory / "state.json"
                original = file.read_bytes(); file.unlink()
                before = self.files(self.home)
                self.assertEqual(self.run_ce(status=2)["error"]["code"], "local-identity-unavailable")
                self.assertEqual(before, self.files(self.home))
                file.write_bytes(original); file.chmod(0o600)

    def test_missing_entire_workspace_record_does_not_generate_another_id(self):
        self.run_ce()
        shutil.rmtree(self.workspace() / "identity")
        before = self.files(self.home)
        self.assertEqual(self.run_ce(status=2)["error"]["code"], "local-identity-unavailable")
        self.assertEqual(before, self.files(self.home))

    def test_valid_but_changed_workspace_uuid_is_rejected_by_installation_pin(self):
        self.run_ce()
        path = self.workspace() / "identity/state.json"
        value = json.loads(path.read_text()); value["local_workspace_id"] = str(uuid.uuid4())
        self.write(path,value)
        before = self.files(self.home)
        self.assertEqual(self.run_ce(status=2)["error"]["code"], "local-identity-conflict")
        self.assertEqual(before,self.files(self.home))

    def test_unsafe_identity_files_are_not_repaired(self):
        self.run_ce()
        file = self.home / "identity/state.json"
        original = file.read_bytes()
        file.chmod(0o644)
        self.run_ce(status=2)
        self.assertEqual(file.stat().st_mode & 0o777, 0o644)
        file.chmod(0o600)
        alias = self.base / "alias"
        file.rename(alias); file.symlink_to(alias)
        self.run_ce(status=2)
        self.assertTrue(file.is_symlink())
        self.assertEqual(alias.read_bytes(), original)

    def test_detach_reconnect_preserves_identity(self):
        first = self.run_ce()["identity"]
        self.run_ce("detach")
        self.assertEqual(self.run_ce("identity", status=2)["error"]["code"], "project-detached")
        self.assertEqual(self.run_ce()["identity"], first)

    def test_legacy_local_binding_gains_identity_without_relabeling(self):
        self.run_ce()
        # Model a pre-feature offline home; this fixture has no online grant.
        shutil.rmtree(self.workspace() / "identity")
        shutil.rmtree(self.home / "identity")
        binding = (self.workspace() / "state.json").read_bytes()
        config = (self.workspace() / "config/state.json").read_bytes()
        index = self.files(self.workspace() / "index")
        self.run_ce("identity", status=2)
        self.run_ce()
        self.assertEqual((self.workspace() / "state.json").read_bytes(), binding)
        self.assertEqual((self.workspace() / "config/state.json").read_bytes(), config)
        self.assertEqual(self.files(self.workspace() / "index"), index)


if __name__ == "__main__": unittest.main()
