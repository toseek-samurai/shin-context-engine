#!/usr/bin/env python3
"""Offline native onboarding, storage isolation and non-mutating reads."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import stat
import sqlite3
import time

TOOL = Path(sys.argv.pop(1)).resolve()
REGISTRY = Path(sys.argv.pop(1)).resolve()


class LocalProjectTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="shin-local-entry-")
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name).resolve()
        self.root = self.base / "project"
        self.root.mkdir()
        (self.root / "README.md").write_text("# Widget Limits\n\nThe retry ceiling is owned by readLimit.\n")
        (self.root / "limits.js").write_text("export function readLimit() { return 7; }\n")
        self.home = self.base / "private"

    def run_ce(self, command, *extra, expected=0, root=None, profile_dir=None, tool=TOOL):
        args = [str(tool), command, *extra, "--root", str(root or self.root), "--home", str(self.home)]
        if command == 'init':
            args.append('--no-service')
        if profile_dir is not None:
            args += ["--profile-dir", str(profile_dir)]
        result = subprocess.run(args, text=True, capture_output=True, timeout=45,
                                env={**os.environ, "PATH": "", "LC_ALL": "C"})
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        if expected == 2:
            self.assertFalse(result.stdout)
            data = json.loads(result.stderr)
            self.assertFalse(data["ok"])
            return data
        self.assertFalse(result.stderr, result.stderr)
        self.assertLessEqual(len(result.stdout.encode()), 65536)
        return json.loads(result.stdout)

    def init(self, **kwargs):
        return self.run_ce("init", "--offline", **kwargs)

    def workspace(self):
        return self.home / "projects" / hashlib.sha256(str(self.root).encode()).hexdigest()

    def file_map(self, root):
        return {p.relative_to(root).as_posix(): p.read_bytes()
                for p in root.rglob("*") if p.is_file()}

    def write_private(self, path, value):
        path.write_text(json.dumps(value))
        path.chmod(0o600)

    def test_uninitialized_status_and_query_do_not_create_state(self):
        self.assertEqual(self.run_ce("status", expected=1)["state"], "not-initialized")
        self.assertFalse(self.run_ce("query", "readLimit", expected=1)["local_ready"])
        self.assertFalse(self.home.exists())

    def test_init_query_and_repeat_without_git_or_path_tools(self):
        before = self.file_map(self.root)
        first = self.run_ce("init")
        self.assertTrue(first["local_ready"])
        self.assertFalse(first["canonical_ready"])
        self.assertEqual(first["project_quality"], "not-configured")
        self.assertTrue(first["rebuilt"])
        self.assertEqual(first["mode"], "offline")
        self.assertFalse(first["delivery_performed"])
        self.assertFalse(first["daemon_started"])
        again = self.init()
        self.assertFalse(again["rebuilt"])
        self.assertEqual(first["project_id"], again["project_id"])
        query = self.run_ce("query", "readLimit retry ceiling")
        self.assertEqual(query['work_performed']['full_generations'], 0)
        self.assertIn("return 7", query["source_context"])
        self.assertIn("README.md", json.dumps(query["document_context"]))
        self.assertFalse(query["answer_complete"])
        self.assertFalse(query["query_persisted"])
        self.assertLessEqual(len(json.dumps(query, separators=(",", ":")).encode()), 65536)
        self.assertEqual(before, self.file_map(self.root))
        self.assertEqual((self.home.stat().st_mode & 0o777), 0o700)
        self.assertEqual(((self.workspace() / "state.json").stat().st_mode & 0o777), 0o600)
        self.assertTrue((self.workspace() / "observer/source-cache").is_dir())

    def test_status_is_current_and_non_mutating(self):
        self.init()
        before = self.file_map(self.home)
        self.assertTrue(self.run_ce("status")["local_ready"])
        self.assertEqual(before, self.file_map(self.home))
        (self.root / "limits.js").write_text("export function readLimit() { return 11; }\n")
        self.assertFalse(self.run_ce("status", expected=1)["local_ready"])
        result = self.run_ce("query", "readLimit", expected=1)
        self.assertNotIn("source_context", result)
        self.assertEqual(before, self.file_map(self.home))
        self.assertTrue(self.init()["rebuilt"])
        self.assertIn("return 11", self.run_ce("query", "readLimit")["source_context"])

    def test_untracked_additions_and_deletions(self):
        self.init()
        source = self.root / "new.js"
        source.write_text("export function addedNow() { return 19; }\n")
        self.assertFalse(self.run_ce("status", expected=1)["local_ready"])
        self.init()
        self.assertIn("addedNow", self.run_ce("query", "addedNow")["source_context"])
        source.unlink()
        self.assertFalse(self.run_ce("status", expected=1)["local_ready"])

    def test_document_only_project(self):
        (self.root / "limits.js").unlink()
        self.init()
        result = self.run_ce("query", "retry ceiling")
        self.assertIn("README.md", json.dumps(result["document_context"]))

    def test_followup_inspect_file_and_record_share_query_snapshot(self):
        self.init()
        query = self.run_ce("query", "readLimit")
        snapshot = query["snapshot_id"]
        self.assertEqual(snapshot, query["document_context"]["snapshot_id"])
        before = self.file_map(self.home)
        inventory = self.run_ce("inspect", "limits.js", "--snapshot", snapshot)["source_inventory"]
        self.assertEqual(inventory["snapshot_id"], snapshot)
        record = next(row["id"] for row in inventory["records"] if row["name"] == "readLimit")
        for selection in (["limits.js"], ["--record", record]):
            read = self.run_ce("read", *selection, "--snapshot", snapshot)
            self.assertEqual(read['work_performed']['full_generations'], 0)
            self.assertEqual(read["snapshot_id"], snapshot)
            self.assertTrue(read["native_read_complete"])
            self.assertFalse(read["answer_complete"])
            self.assertIn("return 7", read["source_context"])
        self.assertEqual(before, self.file_map(self.home))

    def test_followup_requires_snapshot_and_one_selector_before_state_creation(self):
        valid = "fnv1a64:0000000000000000"
        cases = [("read", ["limits.js"]), ("inspect", ["limits.js"]),
                 ("read", ["limits.js", "--snapshot", "invalid"]),
                 ("read", ["--snapshot", valid]),
                 ("read", ["limits.js", "--record", "id", "--snapshot", valid]),
                 ("read", ["--record", "a", "--record", "b", "--snapshot", valid]),
                 ("inspect", ["--record", "id", "--snapshot", valid]),
                 ("read", ["limits.js", "--snapshot", valid, "--snapshot", valid]),
                 ("query", ["readLimit", "--snapshot", valid])]
        for command, args in cases:
            with self.subTest(command=command, args=args):
                self.assertFalse(self.run_ce(command, *args, expected=2)["ok"])
        self.assertFalse(self.home.exists())

    def test_followup_refuses_wrong_snapshot_outside_paths_and_unknown_records(self):
        self.init()
        snapshot = self.run_ce("query", "readLimit")["snapshot_id"]
        before = self.file_map(self.home)
        for command in ["read", "inspect"]:
            self.assertEqual(self.run_ce(command, "limits.js", "--snapshot", "fnv1a64:0000000000000000",
                                         expected=2)["error"]["code"], "source-snapshot-mismatch")
            for path in ["../limits.js", str(self.root / "limits.js"), "src/../limits.js"]:
                self.assertFalse(self.run_ce(command, path, "--snapshot", snapshot, expected=2)["ok"])
        self.assertFalse(self.run_ce("read", "--record", "not-a-record", "--snapshot", snapshot, expected=2)["ok"])
        self.assertFalse(self.run_ce("read", "missing.js", "--snapshot", snapshot, expected=2)["ok"])
        self.assertEqual(before, self.file_map(self.home))

    def test_followup_stale_source_and_old_generation_do_not_return_evidence(self):
        self.init()
        snapshot = self.run_ce("query", "readLimit")["snapshot_id"]
        (self.root / "limits.js").write_text("export function readLimit() { return 11; }\n")
        for command in ["read", "inspect"]:
            result = self.run_ce(command, "limits.js", "--snapshot", snapshot, expected=1)
            self.assertNotIn("source_context", result)
            self.assertNotIn("source_inventory", result)
        self.init()
        self.assertEqual(self.run_ce("read", "limits.js", "--snapshot", snapshot, expected=2)["error"]["code"],
                         "source-snapshot-mismatch")

    def test_followup_view_omission_is_not_complete_delivery(self):
        (self.root / "comments.js").write_text("/*x*/\n" * 3000)
        self.init()
        snapshot = self.run_ce("query", "readLimit")["snapshot_id"]
        read = self.run_ce("read", "comments.js", "--snapshot", snapshot)
        self.assertTrue(read["native_read_complete"])
        footer = json.loads(read["source_context"].split("\nEND ")[1])
        self.assertEqual(footer["omitted_for_view_budget"], [1])
        self.assertFalse(footer["all_native_bodies_shown"])
        self.assertFalse(read["answer_complete"])

    def test_empty_project_is_not_ready(self):
        for path in self.root.iterdir():
            path.unlink()
        self.assertFalse(self.init(expected=1)["local_ready"])
        self.assertFalse(self.run_ce("query", "anything", expected=2)["ok"])

    def test_existing_adoption_is_not_bypassed(self):
        for relative in ["docs/machine/shin-project-adoption-v1.json",
                         "docs/machine/shin-context-engine-adoption-v1.json",
                         "docs/shin-context-engine-adoption.json",
                         "docs/architecture/shin-context-engine-adoption.json"]:
            with self.subTest(marker=relative):
                marker = self.root / relative
                marker.parent.mkdir(parents=True, exist_ok=True)
                marker.write_text("malformed marker must still block bootstrap")
                result = self.init(expected=2)
                self.assertEqual(result["error"]["code"], "existing-project-adoption")
                self.assertFalse(self.home.exists())
                marker.unlink()

    def test_later_adoption_blocks_reads_and_reinitialization(self):
        self.init()
        before = self.file_map(self.home)
        marker = self.root / "docs/machine/shin-project-adoption-v1.json"
        marker.parent.mkdir(parents=True)
        marker.write_text("{}")
        for cmd, extra in [("status", []), ("query", ["readLimit"]), ("init", [])]:
            self.assertEqual(self.run_ce(cmd, *extra, expected=2)["error"]["code"], "existing-project-adoption")
        self.assertEqual(before, self.file_map(self.home))

    def test_nested_directory_does_not_bypass_parent_route(self):
        child = self.root / "component"
        child.mkdir()
        (child / "README.md").write_text("# Component\n")
        marker = self.root / "docs/machine/shin-project-adoption-v1.json"
        marker.parent.mkdir(parents=True)
        marker.write_text("{}")
        self.assertEqual(self.init(root=child, expected=2)["error"]["code"], "existing-project-adoption")
        self.assertFalse(self.home.exists())
        marker.unlink()
        self.init()
        self.assertEqual(self.init(root=child, expected=2)["error"]["code"], "ancestor-project-binding")

    def test_pending_ancestor_is_not_a_fresh_project(self):
        self.init()
        (self.workspace() / "state.json").rename(self.workspace() / "state.pending")
        child = self.root / "component"
        child.mkdir()
        (child / "README.md").write_text("# Component\n")
        self.assertEqual(self.init(root=child, expected=2)["error"]["code"], "ancestor-project-binding")

    def test_in_place_publication_prevents_parallel_bootstrap_without_marker(self):
        manifest = self.root / "docs/kb/generated/manifest.json"
        manifest.parent.mkdir(parents=True)
        manifest.write_text("broken publication is still owned state")
        self.assertEqual(self.init(expected=2)["error"]["code"], "existing-project-publication")
        self.assertFalse(self.home.exists())
        manifest.unlink()
        lock = self.root / "docs/kb/.shin-context.lock"
        lock.touch()
        self.assertEqual(self.init(expected=2)["error"]["code"], "existing-project-publication")
        self.assertFalse(self.home.exists())

    def test_pinned_registry_works_without_bundled_profiles(self):
        self.init(profile_dir=REGISTRY)
        isolated = self.base / "isolated-ce"
        shutil.copy2(TOOL, isolated)
        self.assertFalse(self.run_ce("init", tool=isolated)["rebuilt"])

    def test_missing_receipt_never_reclaims_existing_bootstrap_index(self):
        self.init()
        (self.workspace() / "state.json").unlink()
        before = self.file_map(self.home)
        self.assertEqual(self.init(expected=2)["error"]["code"], "unbound-project-state")
        self.assertEqual(before, self.file_map(self.home))

    def test_root_replacement_requires_review(self):
        self.init()
        self.root.rename(self.base / "old-project")
        self.root.mkdir()
        (self.root / "README.md").write_text("# Replacement\n")
        self.assertEqual(self.init(expected=2)["error"]["code"], "binding-identity-changed")

    def test_two_projects_are_isolated_and_symlink_spelling_reuses_root(self):
        first = self.init()
        second_root = self.base / "other"
        second_root.mkdir()
        (second_root / "README.md").write_text("# Other Project\n")
        second = self.init(root=second_root)
        self.assertNotEqual(first["project_id"], second["project_id"])
        self.assertNotEqual(first["index"], second["index"])
        alias = self.base / "alias"
        alias.symlink_to(self.root, target_is_directory=True)
        self.assertEqual(first["project_id"], self.run_ce("status", root=alias)["project_id"])

    def test_public_or_symlink_state_home_rejected_without_chmod(self):
        self.home.mkdir(mode=0o755)
        self.home.chmod(0o755)
        self.assertEqual(self.init(expected=2)["error"]["code"], "unsafe-directory")
        self.assertEqual(self.home.stat().st_mode & 0o777, 0o755)
        self.home.rmdir()
        elsewhere = self.base / "elsewhere"
        elsewhere.mkdir()
        self.home.symlink_to(elsewhere, target_is_directory=True)
        self.assertEqual(self.init(expected=2)["error"]["code"], "unsafe-home")
        self.assertFalse(list(elsewhere.iterdir()))

    def test_state_inside_project_rejected(self):
        self.home = self.root / "private"
        self.assertEqual(self.init(expected=2)["error"]["code"], "unsafe-home")
        self.assertFalse(self.home.exists())

    def test_pinned_profile_and_registry_changes_refused(self):
        registry = self.base / "profiles"
        registry.mkdir()
        shutil.copy(REGISTRY / "generic-source.json", registry / "generic-source.json")
        self.init(profile_dir=registry)
        self.assertFalse(self.init()["rebuilt"])
        profile = registry / "generic-source.json"
        profile.write_text(profile.read_text() + "\n")
        self.assertEqual(self.init(profile_dir=registry, expected=2)["error"]["code"],
                         "profile-policy-revision-conflict")
        self.assertEqual(self.init(profile_dir=REGISTRY, expected=2)["error"]["code"], "binding-profile-conflict")

    def test_corrupt_and_pending_binding_not_replaced(self):
        self.init()
        state = self.workspace() / "state.json"
        self.write_private(state, {"schema_version": "bad"})
        before = state.read_bytes()
        self.assertEqual(self.init(expected=2)["error"]["code"], "binding-invalid")
        self.assertEqual(state.read_bytes(), before)
        state.rename(self.workspace() / "state.pending")
        self.assertEqual(self.init(expected=2)["error"]["code"], "binding-uncommitted")

    def test_offline_config_cannot_silently_enable_delivery(self):
        self.init()
        path = self.workspace() / "config/state.json"
        config = json.loads(path.read_text())
        config["outbox"]["enabled"] = True
        self.write_private(path, config)
        self.assertEqual(self.run_ce("query", "readLimit", expected=2)["error"]["code"], "binding-config-conflict")
        self.assertEqual(self.init(expected=2)["error"]["code"], "binding-config-conflict")

    def test_busy_initializer_blocks_read_and_second_writer(self):
        self.init()
        with (self.workspace() / "observer.lock").open("rb") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.assertEqual(self.run_ce("status", expected=2)["error"]["code"], "project-busy")
            self.assertEqual(self.init(expected=2)["error"]["code"], "writer-busy")

    def test_engine_upgrade_requires_explicit_init(self):
        self.init()
        path = self.workspace() / "state.json"
        value = json.loads(path.read_text())
        value["core_version"] = "2.0.0-dev.1"
        self.write_private(path, value)
        self.assertFalse(self.run_ce("query", "readLimit", expected=1)["local_ready"])
        self.assertTrue(self.init()["rebuilt"])
        self.assertTrue(self.run_ce("status")["local_ready"])

    def test_sqlite_cache_is_recoverable(self):
        self.init()
        first = self.run_ce("query", "readLimit")
        shutil.rmtree(self.workspace() / "observer/source-cache")
        second = self.run_ce("query", "readLimit")
        self.assertEqual(first["source_context"], second["source_context"])

    def test_missing_observer_sample_needs_explicit_init(self):
        self.init()
        (self.workspace() / "observer/state.json").unlink()
        self.assertFalse(self.run_ce("status", expected=1)["local_ready"])
        self.assertFalse(self.run_ce("query", "readLimit", expected=1)["local_ready"])
        self.init()
        self.assertTrue(self.run_ce("status")["local_ready"])

    def test_unprepared_source_snapshot_is_not_locally_ready(self):
        self.init()
        path = self.workspace() / "state.json"
        state = json.loads(path.read_text())
        state["prepared_snapshot"] = ""
        self.write_private(path, state)
        self.assertFalse(self.run_ce("status", expected=1)["local_ready"])
        self.assertFalse(self.run_ce("query", "readLimit", expected=1)["local_ready"])
        self.init()
        self.assertTrue(self.run_ce("query", "readLimit")["local_ready"])

    def test_binding_and_config_symlinks_are_rejected(self):
        self.init()
        config = self.workspace() / "config/state.json"
        target = self.base / "unrelated.json"
        config.rename(target)
        config.symlink_to(target)
        before = target.read_bytes()
        self.assertFalse(self.run_ce("status", expected=2)["ok"])
        self.assertEqual(target.read_bytes(), before)

    def test_project_quality_suite_cannot_be_skipped_by_bootstrap(self):
        registry = self.base / "profiles"
        registry.mkdir()
        profile = json.loads((REGISTRY / "generic-source.json").read_text())
        profile["evalCases"] = [{"id": "mandatory", "query": "retry", "mustPaths": ["README.md"]}]
        (registry / "generic-source.json").write_text(json.dumps(profile))
        result = self.init(profile_dir=registry, expected=2)
        self.assertEqual(result["error"]["code"], "unsupported-local-profile")
        self.assertFalse((self.workspace() / "state.json").exists())

    def test_help_version_and_bad_flags_do_not_initialize(self):
        for option in ["--help", "--version"]:
            result = subprocess.run([str(TOOL), option], capture_output=True, text=True,
                                    env={**os.environ, "HOME": "/nonexistent", "PATH": ""}, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
        for command, args in [("login", []), ("init", ["--online"]),
                              ("init", ["--offline", "--offline"]),
                              ("query", ["x" * 4097])]:
            self.assertFalse(self.run_ce(command, *args, expected=2)["ok"])
        self.assertFalse(self.home.exists())


class StorageTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='shin-storage-')
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name).resolve()
        self.root = self.base / 'project'
        self.root.mkdir()
        (self.root / 'main.cpp').write_text('int storageFixture() { return 42; }\n')
        (self.root / 'AGENTS.md').write_text('Project content must remain unchanged.\n')
        (self.root / '.sentinel').write_text('private project data')
        (self.root / '.empty').mkdir()
        (self.root / '.alias').symlink_to('.sentinel')
        os.link(self.root / '.sentinel', self.root / '.hardlink')
        self.before = self.tree(self.root)
        self.home = self.base / 'home'
        self.addCleanup(self.stop)
        self.call('init', '--no-service', '--profile-dir', REGISTRY)
        self.workspace = self.home / 'projects' / hashlib.sha256(str(self.root).encode()).hexdigest()

    def stop(self):
        subprocess.run([str(TOOL), 'service-stop', '--home', str(self.home)], capture_output=True, timeout=10)

    def files(self, root):
        return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in root.rglob('*') if p.is_file() and not p.is_symlink()}

    def tree(self, root):
        result = {}
        for p in [root, *root.rglob('*')]:
            s = p.lstat()
            result[str(p.relative_to(root))] = (stat.S_IFMT(s.st_mode), s.st_mode, s.st_ino, s.st_nlink,
                s.st_mtime_ns, s.st_ctime_ns, os.readlink(p) if p.is_symlink() else
                hashlib.sha256(p.read_bytes()).hexdigest() if p.is_file() else None)
        return result

    def call(self, command, *args, error=None, root=None):
        result = subprocess.run([str(TOOL), command, '--home', str(self.home), '--root', str(root or self.root),
                                 *map(str,args)], capture_output=True, text=True, timeout=95)
        data = json.loads(result.stdout or result.stderr)
        if error:
            self.assertNotEqual(result.returncode, 0, data)
            self.assertEqual(data['error']['code'], error, data)
        else:
            self.assertEqual(result.returncode, 0, data)
            self.assertTrue(data['ok'], data)
        self.assertEqual(self.before, self.tree(self.root))
        return data

    def preview(self):
        return self.call('storage-status')

    def test_preview_active_and_defaults_are_read_only(self):
        before = self.files(self.home)
        p = self.preview()
        self.assertFalse(p['auto_clean'])
        self.assertEqual(p['quota_bytes'], 8 * 1024**3)
        self.assertEqual(p['reserve_free_bytes'], 2 * 1024**3)
        self.assertEqual(p['reclaimable_bytes'], 0)
        self.assertEqual(before, self.files(self.home))

    def test_sizes_reconcile_without_counting_project_files(self):
        shared = self.home / 'shared-fixture'
        shared.write_bytes(b'shared' * 31)
        before = self.files(self.home)
        result = self.preview()
        row = result['projects'][0]
        actual = sum(p.stat().st_size for p in self.home.rglob('*') if p.is_file())
        private = sum(p.stat().st_size for p in self.workspace.rglob('*') if p.is_file())
        index = sum(p.stat().st_size for p in (self.workspace / 'index').rglob('*') if p.is_file())
        cache = sum(p.stat().st_size for folder in ['source-cache','observer/source-cache']
                    for p in (self.workspace / folder).rglob('*') if p.is_file())
        self.assertEqual(result['size_kind'], 'logical-files')
        self.assertGreater(result['sampled_at_ms'], 0)
        self.assertEqual(result['used_bytes'], actual)
        self.assertEqual(row['used_bytes'], private)
        self.assertEqual(row['index_bytes'], index)
        self.assertEqual(row['cache_bytes'], cache)
        self.assertEqual(row['other_bytes'], private-index-cache)
        self.assertEqual(result['shared_bytes'], actual-private)
        self.assertEqual(result['project_bytes']+result['shared_bytes'], result['used_bytes'])
        self.assertEqual(before, self.files(self.home))

    def test_global_and_selected_sizes_keep_detached_projects(self):
        other = self.base / 'other-project'
        other.mkdir()
        (other / 'README.md').write_text('# Other project\n')
        self.call('init','--no-service','--profile-dir',REGISTRY,root=other)
        self.call('detach',root=other)
        run = subprocess.run([str(TOOL),'storage-status','--home',str(self.home)],
                             capture_output=True,text=True,timeout=10)
        self.assertEqual(run.returncode,0,run.stderr)
        total = json.loads(run.stdout)
        self.assertEqual(len(total['projects']),2)
        self.assertEqual(sum(p['used_bytes'] for p in total['projects']),total['project_bytes'])
        self.assertEqual(total['project_bytes']+total['shared_bytes'],total['used_bytes'])
        selected = self.preview()
        self.assertEqual(len(selected['projects']),1)
        self.assertEqual(selected['used_bytes'],total['used_bytes'])
        self.assertEqual(selected['shared_bytes'],total['shared_bytes'])
        row = next(p for p in total['projects'] if p['root']==str(other))
        self.assertTrue(row['detached'])
        self.assertGreater(row['cache_bytes'],0)
        self.assertGreater(row['reclaimable_bytes'],0)
        self.assertEqual(self.before,self.tree(self.root))

    def test_detach_cleanup_and_reconnect_preserve_project_index_and_history(self):
        index = self.files(self.workspace / 'index')
        binding = (self.workspace / 'state.json').read_bytes()
        self.call('project-add', '--profile-dir', REGISTRY)
        self.call('detach')
        p = self.preview()
        self.assertGreater(p['reclaimable_bytes'], 0)
        done = self.call('storage-clean', '--revision', p['revision'])
        self.assertEqual(done['reclaimed_bytes'], p['reclaimable_bytes'])
        self.assertEqual(self.preview()['reclaimable_bytes'], 0)
        self.assertEqual(index, self.files(self.workspace / 'index'))
        self.assertEqual(binding, (self.workspace / 'state.json').read_bytes())
        self.assertTrue(self.call('project-add', '--profile-dir', REGISTRY)['service_enrolled'])
        self.assertTrue(self.call('query', 'storageFixture')['local_ready'])

    def test_changed_preview_is_refused(self):
        self.call('detach')
        p = self.preview()
        cache = next((self.workspace / 'observer/source-cache').glob('*.sqlite'))
        cache.write_bytes(cache.read_bytes() + b'changed')
        self.call('storage-clean', '--revision', p['revision'], error='storage-plan-changed')
        self.assertTrue(cache.exists())

    def test_symbolic_cache_directory_cannot_touch_project(self):
        self.call('detach')
        cache = self.workspace / 'observer/source-cache'
        cache.rename(cache.with_name('saved-cache'))
        cache.symlink_to(self.root, target_is_directory=True)
        self.call('storage-status', error='unsafe-project-storage')
        self.assertEqual(self.before, self.tree(self.root))

    def test_hardlinked_cache_is_preserved(self):
        self.call('detach')
        cache = next((self.workspace / 'observer/source-cache').glob('*.sqlite'))
        os.link(cache, self.base / 'second-link')
        p = self.preview()
        self.assertEqual(p['reclaimable_bytes'], 0)
        self.assertTrue(cache.exists())

    def test_cache_with_unknown_content_is_preserved(self):
        self.call('detach')
        cache = next((self.workspace / 'observer/source-cache').glob('*.sqlite'))
        with sqlite3.connect(cache) as db:
            db.execute('CREATE TABLE foreign_data(value TEXT)')
            db.execute("INSERT INTO foreign_data VALUES('must remain')")
        p = self.preview()
        self.assertEqual(p['reclaimable_bytes'], 0)
        self.assertEqual(p['projects'][0]['reason'], 'unverified-files-preserved')
        self.assertTrue(cache.exists())

    def test_cache_format_and_schema_must_agree_before_cleanup(self):
        self.call('detach')
        cache = next((self.workspace / 'observer/source-cache').glob('*.sqlite'))
        with sqlite3.connect(cache) as db:
            header = json.loads(db.execute('SELECT payload FROM cache_meta WHERE id=0').fetchone()[0])
            self.assertEqual(header['binding']['format'], 'shin.source-cache.v3')
            header['binding']['format'] = 'shin.source-cache.v2'
            db.execute('UPDATE cache_meta SET payload=? WHERE id=0', (json.dumps(header),))
        self.assertEqual(self.preview()['reclaimable_bytes'], 0, 'format alone cannot authorize deletion')
        with sqlite3.connect(cache) as db:
            db.execute("CREATE VIRTUAL TABLE query_text USING fts5(body, tokenize='unicode61')")
            db.execute("CREATE VIRTUAL TABLE query_vocab USING fts5vocab(query_text, 'row')")
        preview = self.preview()
        self.assertGreater(preview['reclaimable_bytes'], 0, 'previous owned cache remains reclaimable')
        with sqlite3.connect(cache) as db:
            header['binding']['identity']['root'] = str(self.base / 'foreign-project')
            db.execute('UPDATE cache_meta SET payload=? WHERE id=0', (json.dumps(header),))
        self.assertEqual(self.preview()['reclaimable_bytes'], 0, 'matching format grants no foreign ownership')
        self.assertTrue(cache.exists())

    def test_busy_cache_is_not_removed(self):
        self.call('detach')
        cache = next((self.workspace / 'observer/source-cache').glob('*.sqlite'))
        p = self.preview()
        with cache.with_suffix('.lock').open('r+') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            result = subprocess.run([str(TOOL),'storage-clean','--home',str(self.home),'--root',str(self.root),
                                     '--revision',p['revision']],capture_output=True,text=True,timeout=10)
            self.assertNotEqual(result.returncode,0)
            refused = json.loads(result.stdout or result.stderr)
            self.assertEqual(refused['error']['code'], 'source-cache-busy', refused)
        self.assertTrue(cache.exists())
        self.assertEqual(self.before,self.tree(self.root))

    def test_offline_cleanup_preserves_unplanned_pending_files(self):
        self.call('detach')
        self.assertFalse((self.home / 'registry').exists())
        cache = next((self.workspace / 'observer/source-cache').glob('*.sqlite'))
        pending = cache.with_suffix('.pending')
        pending.write_bytes(b'Unverified pending data is not in the cleanup plan.')
        pending.chmod(0o600)
        original = self.tree(pending)
        preview = self.preview()
        cleaned = self.call('storage-clean', '--revision', preview['revision'])
        self.assertEqual(cleaned['reclaimed_bytes'], preview['reclaimable_bytes'])
        self.assertFalse(cache.exists())
        self.assertEqual(self.tree(pending), original)
        self.assertFalse((self.home / 'registry').exists())

    def test_partial_cleanup_reports_every_completed_removal(self):
        self.call('detach')
        second = next((self.workspace / 'observer/source-cache').glob('*.sqlite'))
        first_dir = self.workspace / 'source-cache'
        first_dir.mkdir(mode=0o700)
        first = first_dir / second.name
        first.write_bytes(second.read_bytes())
        first.chmod(0o600)
        removed_bytes = first.stat().st_size
        preview = self.preview()
        with second.with_suffix('.lock').open('r+') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            run = subprocess.run([str(TOOL), 'storage-clean', '--home', str(self.home), '--root', str(self.root),
                '--revision', preview['revision']], capture_output=True, text=True, timeout=10)
        result = json.loads(run.stdout or run.stderr)
        self.assertNotEqual(run.returncode, 0, result)
        self.assertFalse(result['ok'])
        self.assertEqual(result['reclaimed_bytes'], removed_bytes, result)
        self.assertEqual(result['reclaimable_bytes'], second.stat().st_size)
        actions = {a['path']: a for a in result['actions']}
        self.assertTrue(actions[str(first)]['applied'])
        self.assertFalse(actions[str(second)]['applied'])
        self.assertEqual(actions[str(second)]['error'], result['error']['code'])
        self.assertFalse(first.exists())
        self.assertTrue(second.exists())
        self.assertEqual(self.before, self.tree(self.root))

    def test_storage_inside_another_known_project_is_rejected(self):
        self.call('project-add','--profile-dir',REGISTRY,root=self.base,error='unsafe-project-storage')

    def test_quota_blocks_new_growth_without_deleting_existing_data(self):
        large = self.home / 'reserved-fixture'
        with large.open('wb') as out:
            out.truncate(8 * 1024**3)
        try:
            self.call('init','--no-service','--profile-dir',REGISTRY,error='storage-quota-exceeded')
            self.assertTrue((self.workspace / 'index/docs/kb/generated/manifest.json').exists())
        finally:
            large.unlink()

    def test_automatic_cleanup_uses_the_same_detached_boundary(self):
        self.call('project-add','--profile-dir',REGISTRY)
        self.call('detach')
        self.call('service-stop')
        until = time.monotonic() + 5
        while (self.home / 'service/rpc.sock').exists() and time.monotonic() < until:
            time.sleep(.1)
        self.assertGreater(self.preview()['reclaimable_bytes'], 0)
        self.call('storage-policy','--auto-clean','on')
        self.call('service-start')
        until = time.monotonic() + 10
        while time.monotonic() < until:
            p = self.preview()
            if p.get('last_cleanup_at_ms'):
                break
            time.sleep(.1)
        self.assertEqual(p.get('last_cleanup_error'), '')
        self.assertGreater(p.get('last_cleanup_bytes',0),0)
        self.assertEqual(p['reclaimable_bytes'],0)

    def test_automatic_policy_is_explicit_and_preserves_current_projects(self):
        self.assertFalse(self.preview()['auto_clean'])
        self.call('storage-policy','--auto-clean','on')
        self.assertTrue(self.preview()['auto_clean'])
        self.assertEqual(self.preview()['reclaimable_bytes'],0)
        self.call('storage-policy','--auto-clean','off')
        self.assertFalse(self.preview()['auto_clean'])


if __name__ == "__main__":
    unittest.main()
