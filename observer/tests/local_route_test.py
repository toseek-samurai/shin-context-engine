#!/usr/bin/env python3
"""Qualify read-only attachment and explicitly granted canonical publication."""
import fcntl
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

from integration_test import Fixture, write_private_json

CE, OBSERVER, CORE = (Path(sys.argv.pop(1)).resolve() for _ in range(3))
QUALIFICATION = Path(sys.argv.pop(1)).resolve()


class LocalRouteTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="shin-route-", dir='/tmp')
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name).resolve()
        self.f = Fixture(self.base, "canonical")
        self.home = self.base / "cli"
        self.addCleanup(self.stop_service)
        self.f.write_source("# Route Contract\n\nThe retry ceiling is owned by readLimit.\n")
        (self.f.root / "src").mkdir()
        (self.f.root / "src/limits.js").write_text("export function readLimit() { return 7; }\n")
        self.profile = json.loads(self.f.profile_path.read_text())
        self.profile.update(bootstrapOnly=False, sourceFactRoots=["src"], sourceFactExtensions=[".js"],
            evalCases=[{"id": "route-doc", "query": "retry ceiling", "mustPaths": ["README.md"]}],
            packEvalCases=[{"id": "route-pack", "mustPaths": ["README.md"], "expectedComplete": True,
                "request": {"schema_version": "shin.task-request.v1", "request_id": "route-pack",
                    "task": {"objective": "Locate the retry ceiling", "must": [{"id": "rule", "query": "retry ceiling"}]},
                    "budget": {"max_evidence_bytes": 8192, "max_records": 4,
                               "per_query_records": 4, "per_document_records": 2}}}])
        self.build()
        self.invoke(OBSERVER, "sample", "--config", str(self.f.config))

    def invoke(self, tool, *args, expected=0, one_shot=True):
        if args and args[0] == 'init' and one_shot:
            args = (*args, '--no-service')
        run = subprocess.run([str(tool), *args], capture_output=True, text=True, timeout=45,
                             env={**os.environ, "PATH": "", "LC_ALL": "C"}, umask=0o077)
        self.assertEqual(run.returncode, expected, run.stdout + run.stderr)
        if expected == 2:
            self.assertFalse(run.stdout)
            return json.loads(run.stderr)
        self.assertFalse(run.stderr, run.stderr)
        return json.loads(run.stdout)

    def stop_service(self):
        endpoint = self.home / 'service/rpc.sock'
        if endpoint.exists():
            self.ce('service-stop')
            deadline = time.monotonic() + 10
            while endpoint.exists() and time.monotonic() < deadline:
                time.sleep(0.02)
            self.assertFalse(endpoint.exists(), 'test service failed to stop')

    def build(self):
        self.f.profile_path.write_text(json.dumps(self.profile))
        self.invoke(CORE, "--profile", "observer-fixture", "--profile-dir", str(self.f.profiles),
                    "--root", str(self.f.root), "--out-dir", str(self.f.out), "build")

    def ce(self, command, *args, expected=0, root=None, home=None, one_shot=True):
        return self.invoke(CE, command, *args, "--root", str(root or self.f.root), "--home", str(home or self.home),
                           expected=expected, one_shot=one_shot)

    def attach(self, *args, expected=0):
        return self.ce("attach", "--observer-config", str(self.f.config), *args, expected=expected)

    def workspace(self):
        return self.home / "projects" / hashlib.sha256(str(self.f.root).encode()).hexdigest()

    def canonical_files(self):
        return {str(p.relative_to(self.f.base)): (p.read_bytes(), p.stat().st_mtime_ns, p.stat().st_mode)
                for p in self.f.base.rglob("*") if p.is_file()}

    def marker(self):
        source = Path(__file__).resolve().parents[2] / "tests/fixtures/adoption/root/docs/machine/shin-project-adoption-v1.json"
        value = json.loads(source.read_text())
        value["profile"]["id"] = "observer-fixture"
        value["index"]["key"] = "index"
        path = self.f.root / "docs/machine/shin-project-adoption-v1.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value))
        (self.f.root / "AGENTS.md").write_text(
            "# Route Contract\n\nContext epoch: `fixture-shin-v1.12-2026-07-19`.\n\n"
            "Use `docs/machine/shin-project-adoption-v1.json` and profile `observer-fixture`.\n")
        self.profile["agentStartPaths"] += ["AGENTS.md", "docs/machine/shin-project-adoption-v1.json"]
        self.build()
        return path, value

    def test_storage_sizes_include_external_canonical_index_once(self):
        self.attach()
        before = self.canonical_files()
        report = self.ce('storage-status')
        row = report['projects'][0]
        def size(root):
            return sum(p.stat().st_size for p in root.rglob('*') if p.is_file())
        self.assertEqual(row['index_bytes'],size(self.f.out))
        self.assertEqual(row['used_bytes'],size(self.workspace())+size(self.f.out))
        self.assertEqual(report['used_bytes'],size(self.home)+size(self.f.out))
        self.assertEqual(report['shared_bytes']+report['project_bytes'],report['used_bytes'])
        self.assertEqual(before,self.canonical_files())

    def test_watch_cannot_claim_canonical_publication_authority(self):
        self.attach()
        before = self.canonical_files()
        result = self.ce('watch', '--samples', '1')
        self.assertEqual(result['watch']['refresh_authority'], 'external-publication-read-only')
        self.assertEqual(result['watch']['publications'], 0)
        self.assertEqual(before, self.canonical_files())
        self.assertTrue((self.workspace() / 'watch').exists())

    def managed(self, service=False):
        return self.ce('init', '--observer-config', str(self.f.config), '--manage-index', one_shot=not service)

    def relocation_target(self, name='project-profiles'):
        target = self.f.root / name
        target.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(self.f.profile_path, target / self.f.profile_path.name)
        return target

    def relocation_evidence(self, history=True):
        paths = [self.workspace() / 'state.json', self.grant_path(), self.f.config,
                 self.home / 'registry/state.json']
        if history:
            paths.append(self.home / 'service/history/telemetry.sqlite')
        return {p: (p.stat().st_ino, p.read_bytes()) for p in paths if p.exists()}

    def test_profile_relocation_preserves_authority_histories_and_supports_reverse(self):
        self.marker()
        self.managed()
        self.ce('refresh-release')
        config = self.f.config_data()
        config['max_snapshot_bytes'] *= 2
        write_private_json(self.f.config, config)
        self.config_accept()
        self.managed(service=True)
        self.stop_service()
        self.ce('watch', '--samples', '1')
        target = self.relocation_target()
        registry = (self.home / 'registry/state.json').read_bytes()
        history = self.home / 'service/history/telemetry.sqlite'
        history_before = history.stat().st_ino, history.read_bytes()
        owner = self.owner_path().read_bytes()
        def index_files():
            return {str(p.relative_to(self.f.out)): (p.read_bytes(), p.stat().st_mtime_ns)
                    for p in self.f.out.rglob('*') if p.is_file()}
        index_before = index_files()
        before = {name: json.loads((self.workspace() / name / 'state.json').read_text())
                  for name in ('publication', 'profile-revisions', 'marker-policy', 'config-policy', 'qualification')}
        observer_before = json.loads((self.f.state / 'state.json').read_text())
        binding_before = json.loads((self.workspace() / 'state.json').read_text())
        result = self.ce('profile-relocate', '--profile-dir', str(target))
        self.assertEqual(result['schema_version'], 'shin.ce-response.v1')
        self.assertEqual(result['tool'], 'shin-ce')
        self.assertEqual(result['version'], (Path(__file__).resolve().parents[1] / 'VERSION').read_text().strip())
        self.assertEqual(result['core_version'], (Path(__file__).resolve().parents[2] / 'VERSION').read_text().strip())
        self.assertTrue(result['changed'])
        self.assertFalse(result['index_mutated'])
        self.assertTrue(result['history_preserved'])
        self.assertFalse(result['readiness_assessed'])
        self.assertEqual(registry, (self.home / 'registry/state.json').read_bytes())
        self.assertEqual(history_before, (history.stat().st_ino, history.read_bytes()))
        self.assertEqual(owner, self.owner_path().read_bytes())
        self.assertEqual(index_before, index_files())
        self.assertEqual(json.loads(self.f.config.read_text())['profile_dir'], str(target))
        binding_after = json.loads((self.workspace() / 'state.json').read_text())
        for field in ('observer_config_hash', 'observer_binding_key'):
            binding_before.pop(field)
            binding_after.pop(field)
        self.assertEqual(binding_before, binding_after)
        for name, fields in {'publication': ['route'], 'profile-revisions': ['authority'],
                             'marker-policy': ['route_key'], 'config-policy': ['route_key', 'approved_config_hash'],
                             'qualification': []}.items():
            after = json.loads((self.workspace() / name / 'state.json').read_text())
            for field in fields:
                before[name].pop(field)
                after.pop(field)
            self.assertEqual(before[name], after, name)
        observer_after = json.loads((self.f.state / 'state.json').read_text())
        for field in ('binding', 'binding_key'):
            observer_before.pop(field)
            observer_after.pop(field)
        self.assertEqual(observer_before, observer_after)
        self.assertFalse(self.ce('profile-relocate', '--profile-dir', str(target))['changed'])
        self.ce('service-start')
        self.assertIn('return 7;', self.ce('query', 'readLimit')['source_context'])
        self.stop_service()
        self.assertTrue(self.ce('profile-relocate', '--profile-dir', str(self.f.profiles))['changed'])
        self.assertEqual(json.loads(self.f.config.read_text())['profile_dir'], str(self.f.profiles))
        self.ce('service-start')
        self.assertTrue(self.ce('query', 'readLimit')['canonical_ready'])

    def test_profile_relocation_refuses_live_service_different_policy_and_symlink(self):
        self.managed(service=True)
        target = self.relocation_target()
        before = self.relocation_evidence(history=False)
        refused = self.ce('profile-relocate', '--profile-dir', str(target), expected=2)
        self.assertEqual(refused['error']['code'], 'profile-relocation-requires-stopped-service')
        self.assertEqual(before, self.relocation_evidence(history=False))
        self.stop_service()
        before = self.relocation_evidence()
        copied = target / self.f.profile_path.name
        copied.write_text(copied.read_text() + '\n')
        self.assertEqual(self.ce('profile-relocate', '--profile-dir', str(target), expected=2)['error']['code'], 'profile-relocation-conflict')
        copied.unlink()
        copied.symlink_to(self.f.profile_path)
        self.ce('profile-relocate', '--profile-dir', str(target), expected=2)
        copied.unlink()
        shutil.copyfile(self.f.profile_path, copied)
        alias = self.f.root / 'profile-alias'
        alias.symlink_to(target, target_is_directory=True)
        self.ce('profile-relocate', '--profile-dir', str(alias), expected=2)
        self.assertEqual(before, self.relocation_evidence())

    def test_profile_relocation_resumes_mixed_writes_and_attributed_pending_state(self):
        self.managed(service=True)
        self.stop_service()
        target = self.relocation_target()
        self.ce('profile-relocate', '--profile-dir', str(target))
        journal_path = self.workspace() / 'profile-location/state.json'
        journal = json.loads(journal_path.read_text())
        payload = journal_path.parent / journal['payload']
        journal['phase'] = 'prepared'
        write_private_json(journal_path, journal)
        self.f.config.write_bytes((payload / 'config-before.json').read_bytes())
        for name, directory in [('binding', self.workspace()), ('observer', self.f.state)]:
            (directory / 'state.json').write_bytes((payload / (name + '-before.json')).read_bytes())
        write_private_json(self.f.state / 'state.pending', json.loads((payload / 'observer-after.json').read_text()))
        self.assertTrue(self.ce('profile-relocate', '--profile-dir', str(target))['changed'])
        self.assertFalse((self.f.state / 'state.pending').exists())
        self.assertEqual(json.loads(journal_path.read_text())['phase'], 'complete')
        self.ce('service-start')
        self.assertTrue(self.ce('query', 'readLimit')['canonical_ready'])

    def test_profile_relocation_does_not_approve_unrelated_configuration_or_overwrite_conflict(self):
        self.managed(service=True)
        self.stop_service()
        target = self.relocation_target()
        original = self.f.config.read_bytes()
        config = json.loads(original)
        config['poll_interval_ms'] *= 2
        write_private_json(self.f.config, config)
        before = self.relocation_evidence()
        self.assertEqual(self.ce('profile-relocate', '--profile-dir', str(target), expected=2)['error']['code'], 'profile-relocation-conflict')
        self.assertEqual(before, self.relocation_evidence())
        self.f.config.write_bytes(original)
        self.ce('profile-relocate', '--profile-dir', str(target))
        journal_path = self.workspace() / 'profile-location/state.json'
        journal = json.loads(journal_path.read_text())
        journal['phase'] = 'unknown'
        write_private_json(journal_path, journal)
        before = self.relocation_evidence()
        self.assertEqual(self.ce('profile-relocate', '--profile-dir', str(target), expected=2)['error']['code'], 'profile-relocation-conflict')
        self.assertEqual(before, self.relocation_evidence())
        journal['phase'] = 'prepared'
        write_private_json(journal_path, journal)
        grant = json.loads(self.grant_path().read_text())
        grant['approved_profile_hash'] = '0' * 16
        write_private_json(self.grant_path(), grant)
        before = self.relocation_evidence()
        self.assertEqual(self.ce('profile-relocate', '--profile-dir', str(target), expected=2)['error']['code'], 'profile-relocation-conflict')
        self.assertEqual(before, self.relocation_evidence())

    def test_detach_revokes_only_granted_publication_and_keeps_canonical_index(self):
        self.managed(service=True)
        before = self.ce('query', 'readLimit')
        binding = (self.workspace() / 'state.json').read_bytes()
        sources = {str(p.relative_to(self.f.root)): p.read_bytes() for p in self.f.root.rglob('*') if p.is_file()}
        detached = self.ce('detach')
        self.assertEqual(detached['state'], 'detached')
        self.assertTrue(detached['data_preserved'])
        self.assertEqual(json.loads((self.workspace() / 'publication/state.json').read_text())['phase'], 'revoked')
        self.assertEqual((self.workspace() / 'state.json').read_bytes(), binding)
        self.assertEqual({str(p.relative_to(self.f.root)): p.read_bytes() for p in self.f.root.rglob('*') if p.is_file()}, sources)
        self.assertEqual(self.ce('query', 'readLimit', expected=2)['error']['code'], 'project-detached')
        self.ce('detach')
        connected = self.ce('init', one_shot=False)
        self.assertEqual(connected['publication'], 'revoked')
        self.assertEqual(connected['refresh_management'], 'external')
        self.assertEqual(self.ce('query', 'readLimit')['snapshot_id'], before['snapshot_id'])

    def test_quality_reuse_keeps_live_checks_and_readers_do_not_write_receipt(self):
        first = self.managed()
        self.assertFalse(first['readiness']['quality_reuse']['reused'])
        receipt = self.workspace() / 'qualification/state.json'
        saved = (receipt.read_bytes(), receipt.stat().st_mtime_ns)
        for command, args in [('status', []), ('query', ['readLimit'])]:
            value = self.ce(command, *args)
            readiness = value['readiness']
            self.assertTrue(readiness['quality_reuse']['reused'])
            self.assertEqual({c['id'] for c in readiness['live_checks']},
                             {'verified-generation', 'profile-check', 'doctor'})
            self.assertEqual(value['work_performed']['full_generations'], 0)
            self.assertTrue(all(c['ok'] for c in readiness['live_checks']))
            self.assertEqual(saved, (receipt.read_bytes(), receipt.stat().st_mtime_ns))
        (self.f.root / 'src/limits.js').write_text('export function readLimit() { return 119; }\n')
        stale = self.ce('query', 'readLimit', expected=1)
        self.assertFalse(stale['canonical_ready'])
        self.assertNotIn('source_context', stale)
        self.assertEqual(saved, (receipt.read_bytes(), receipt.stat().st_mtime_ns))
        self.ce('watch', '--samples', '2')
        self.assertNotEqual(saved[0], receipt.read_bytes())
        current = self.ce('query', 'readLimit')
        self.assertTrue(current['readiness']['quality_reuse']['reused'])
        self.assertIn('return 119;', current['source_context'])

    def test_quality_reuse_miss_and_corrupt_receipt_rerun_full_gate(self):
        self.managed()
        receipt = self.workspace() / 'qualification/state.json'
        saved = json.loads(receipt.read_text())
        wrong = {**saved, 'key': '0' * 64}
        write_private_json(receipt, wrong)
        self.assertFalse(self.ce('status')['readiness']['quality_reuse']['reused'])
        self.assertEqual(json.loads(receipt.read_text()), wrong)
        self.ce('watch', '--samples', '1')
        self.assertEqual(json.loads(receipt.read_text())['key'], saved['key'])
        write_private_json(receipt, {})
        before = (receipt.read_bytes(), receipt.stat().st_mtime_ns)
        checked = self.ce('status')['readiness']
        self.assertFalse(checked['quality_reuse']['reused'])
        self.assertEqual(checked['quality_reuse']['cache_status'], 'invalid')
        self.assertEqual(before, (receipt.read_bytes(), receipt.stat().st_mtime_ns))
        tick = self.ce('watch', '--samples', '1')['watch']
        self.assertEqual(tick['phase'], 'stopped')
        self.assertEqual(tick['recoveries'], 1)
        self.assertEqual(tick['last_recovery'], 'quality-qualification')
        self.assertTrue(self.ce('status')['readiness']['quality_reuse']['reused'])

    def test_quality_reuse_does_not_accept_modified_generated_artifact(self):
        self.managed()
        target = self.f.out / 'docs/kb/generated/documents.jsonl'
        original = target.read_bytes()
        target.write_bytes(original + b' ')
        refused = self.ce('query', 'readLimit', expected=2)
        self.assertNotIn('source_context', refused)
        target.write_bytes(original)
        self.assertTrue(self.ce('status')['readiness']['quality_reuse']['reused'])

    def test_managed_refresh_reuses_only_its_strictly_validated_candidate(self):
        self.profile['evalResourcePolicy'] = 'advisory'
        self.profile['evalCases'][0]['maxCandidateRows'] = 1
        self.marker()
        self.managed()
        before = self.ce('query', 'readLimit')['snapshot_id']
        (self.f.root / 'src/limits.js').write_text('export function readLimit() { return 81; }\n')
        tick = self.ce('watch', '--samples', '2')['watch']['last_result']
        self.assertEqual(tick['phase'], 'current')
        self.assertTrue(tick['rebuilt'])
        self.assertEqual(tick['quality_reuse']['cache_status'], 'published-candidate')
        self.assertEqual(tick['refresh_work']['source_cache_builds'], 1)
        self.assertGreater(tick['refresh_work']['source_files'], 0)
        self.assertGreater(tick['refresh_work']['source_bytes'], 0)
        self.assertGreater(tick['refresh_work']['source_chunks'], 0)
        for metric in ('source_read_scan_ms', 'source_chunking_ms', 'source_index_ms', 'source_persist_ms'):
            self.assertIsInstance(tick['refresh_work'][metric], (int, float))
            self.assertGreaterEqual(tick['refresh_work'][metric], 0)
        for metric in ('qualification_snapshot_ms', 'qualification_generation_ms',
                       'qualification_profile_ms', 'qualification_doctor_ms', 'qualification_adoption_ms',
                       'validation_profile-check_ms', 'validation_check_ms', 'validation_doctor_ms',
                       'validation_vacuum_ms', 'validation_eval_ms', 'validation_pack-eval_ms',
                       'validation_adoption-check_ms'):
            self.assertIsInstance(tick['refresh_work'][metric], (int, float))
            self.assertGreaterEqual(tick['refresh_work'][metric], 0)
        idle = self.ce('watch', '--samples', '1')['watch']['last_result']
        self.assertFalse(idle['rebuilt'])
        self.assertIn('qualification_generation_ms', idle['refresh_work'])
        self.assertNotIn('validation_check_ms', idle['refresh_work'])
        self.assertGreater(idle['refresh_work']['qualification_reused_output_bytes'], 0)
        self.assertGreater(idle['refresh_work']['qualification_reused_output_files'], 0)
        self.assertTrue(list((self.workspace() / 'source-cache').glob('*.sqlite')),
                        'candidate cache should be persisted under the published identity')
        # One candidate generation plus strict validation's four exact audits;
        # the previous refresh repeated ready/validate and performed nine.
        self.assertLessEqual(tick['work_performed']['full_generations'], 5)
        self.assertGreater(tick['work_performed']['full_generations'], 0)
        current = self.ce('status')
        self.assertTrue(current['readiness']['quality_reuse']['reused'])
        self.assertTrue(current['readiness']['qualified_resources']['warnings'])
        self.assertIn('adoption-check', {c['id'] for c in current['readiness']['live_checks']})
        query = self.ce('query', 'readLimit')
        self.assertIn('return 81;', query['source_context'])
        self.assertNotEqual(query['snapshot_id'], before)
        self.assertEqual(query['work_performed']['full_generations'], 0)

    def test_followups_do_not_regenerate_qualified_output(self):
        first = self.managed()
        self.assertGreater(first['work_performed']['full_generations'], 0)
        query = self.ce('query', 'readLimit')
        snapshot = query['snapshot_id']
        for command in ['inspect', 'read']:
            value = self.ce(command, 'src/limits.js', '--snapshot', snapshot)
            self.assertEqual(value['work_performed']['full_generations'], 0)
            self.assertTrue(value['local_ready'])
        tick = self.ce('watch', '--samples', '1')['watch']['last_result']
        self.assertEqual(tick['work_performed']['full_generations'], 0)
        (self.workspace() / 'qualification/state.json').unlink()
        self.assertGreater(self.ce('status')['work_performed']['full_generations'], 0)

    def test_qualified_generation_refuses_changed_unindexed_link_target(self):
        target = self.f.root / 'linked.txt'
        target.write_text('link target outside the indexed extensions\n')
        self.f.write_source('# Route Contract\n\nThe retry ceiling is owned by readLimit.\n\n[link](linked.txt)\n')
        self.build()
        self.managed()
        self.assertEqual(self.ce('query', 'readLimit')['work_performed']['full_generations'], 0)
        target.unlink()
        value = self.ce('query', 'readLimit', expected=1)
        self.assertFalse(value['canonical_ready'])
        self.assertNotIn('source_context', value)

    def test_qualified_generation_refuses_extra_payload_and_preserved_mtime_edit(self):
        self.managed()
        extra = self.f.out / 'docs/kb/generated/extra.json'
        extra.write_text('{}')
        self.ce('query', 'readLimit', expected=2)
        extra.unlink()
        source = self.f.root / 'src/limits.js'
        before = source.stat()
        source.write_text(source.read_text().replace('7', '8'))
        os.utime(source, ns=(before.st_atime_ns, before.st_mtime_ns))
        self.assertFalse(self.ce('query', 'readLimit', expected=1)['canonical_ready'])

    def test_quality_reuse_preserves_advisory_warnings_and_marker_check(self):
        self.profile['evalResourcePolicy'] = 'advisory'
        self.profile['evalCases'][0]['maxCandidateRows'] = 1
        self.marker()
        self.managed()
        cached = self.ce('status')['readiness']
        self.assertTrue(cached['quality_reuse']['reused'])
        self.assertIn('adoption-check', {c['id'] for c in cached['live_checks']})
        resources = cached['qualified_resources']
        self.assertEqual(resources['resource_check']['policy'], 'advisory')
        self.assertGreater(resources['resource_check']['warning_queries'], 0)
        self.assertTrue(resources['warnings'])

    def previous_format_route(self, with_marker=False):
        previous = os.environ.get('SHIN_PREVIOUS_KIT')
        if not previous:
            self.skipTest('Cross-format release proof requires SHIN_PREVIOUS_KIT')
        old = Path(previous).resolve() / 'bin'
        if with_marker:
            marker, value = self.marker()
            value['engine'] = {'minimumVersion': '2.0.4', 'generatorIdentity': 'shin-generator-v21-cpp-syntax'}
            marker.write_text(json.dumps(value))
        self.invoke(old / 'shin-context', '--profile', 'observer-fixture',
                    '--profile-dir', str(self.f.profiles), '--root', str(self.f.root),
                    '--out-dir', str(self.f.out), 'build')
        self.invoke(old / 'shin-observer', 'sample', '--config', str(self.f.config))
        manifest = json.loads((self.f.out / 'docs/kb/generated/manifest.json').read_text())
        self.assertEqual(manifest['generator_version'], 'shin-generator-v21-cpp-syntax')
        self.invoke(old / 'shin-ce', 'init', '--observer-config', str(self.f.config),
                    '--manage-index', '--root', str(self.f.root), '--home', str(self.home), one_shot=False)
        self.invoke(old / 'shin-ce', 'query', 'readLimit', '--service',
                    '--root', str(self.f.root), '--home', str(self.home))
        self.invoke(old / 'shin-ce', 'service-stop', '--root', str(self.f.root), '--home', str(self.home))
        endpoint = self.home / 'service/rpc.sock'
        deadline = time.monotonic() + 10
        while endpoint.exists() and time.monotonic() < deadline: time.sleep(.02)
        self.assertFalse(endpoint.exists())
        return old

    def test_previous_format_upgrade_preserves_grant_sources_history_and_retry(self):
        self.previous_format_route()
        grant_path = self.workspace() / 'publication/state.json'
        binding_path = self.workspace() / 'state.json'
        grant = json.loads(grant_path.read_text())
        binding = json.loads(binding_path.read_text())
        sources = {p: p.read_bytes() for p in self.f.root.rglob('*') if p.is_file()}
        history = self.home / 'service/history/telemetry.sqlite'
        history_before = (history.stat().st_ino, history.read_bytes())
        stale = self.ce('init', '--manage-index', expected=1)
        self.assertFalse(stale['local_ready'])
        self.ce('watch', '--samples', '2')
        current = self.ce('init', '--manage-index')
        manifest = json.loads((self.f.out / 'docs/kb/generated/manifest.json').read_text())
        self.assertEqual(manifest['generator_version'], 'shin-generator-v22-compact-lexical')
        self.assertFalse((self.f.out / 'docs/kb/generated/terms.jsonl').exists())
        self.assertTrue((self.f.out / 'docs/kb/generated/terms.lex').is_file())
        after = json.loads(grant_path.read_text())
        self.assertEqual(after['route'], grant['route'])
        self.assertEqual(after['token'], grant['token'])
        final_binding = json.loads(binding_path.read_text())
        for key in binding:
            if key != 'prepared_snapshot': self.assertEqual(final_binding[key], binding[key])
        self.assertEqual(sources, {p: p.read_bytes() for p in self.f.root.rglob('*') if p.is_file()})
        self.assertEqual(history_before, (history.stat().st_ino, history.read_bytes()))
        generated = self.f.out / 'docs/kb/generated'
        files = {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in generated.iterdir() if p.is_file()}
        self.ce('watch', '--samples', '2')
        self.assertEqual(files, {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in generated.iterdir() if p.is_file()})
        self.ce('service-start')
        query = self.ce('query', 'readLimit')
        self.assertIn('return 7;', query['source_context'])
        self.assertEqual(final_binding['prepared_snapshot'], query['snapshot_id'])
        self.stop_service()
        (self.f.root / 'src/limits.js').write_text('export function readLimit() { return 99; }\n')
        self.ce('watch', '--samples', '2')
        self.ce('service-start')
        self.assertIn('return 99;', self.ce('query', 'readLimit')['source_context'])
        self.assertEqual(json.loads(grant_path.read_text())['route'], grant['route'])

    def test_previous_format_failed_qualification_preserves_head_then_recovers(self):
        self.previous_format_route()
        generated = self.f.out / 'docs/kb/generated'
        before = {p.name: p.read_bytes() for p in generated.iterdir() if p.is_file()}
        grant_path = self.workspace() / 'publication/state.json'
        grant = json.loads(grant_path.read_text())
        source = self.f.root / 'README.md'; original = source.read_bytes()
        source.write_text('# Other document\n\nUnrelated content.\n')
        failed = self.ce('watch', '--samples', '2')
        self.assertFalse(self.ce('init', '--manage-index', expected=1)['local_ready'])
        self.assertEqual(before, {p.name: p.read_bytes() for p in generated.iterdir() if p.is_file()})
        self.assertEqual(json.loads(grant_path.read_text())['last_snapshot'], grant['last_snapshot'])
        source.write_bytes(original)
        self.ce('watch', '--samples', '2')
        self.assertTrue(self.ce('init', '--manage-index')['local_ready'])
        self.assertEqual(json.loads(grant_path.read_text())['token'], grant['token'])

    @unittest.skipUnless(os.environ.get('SHIN_TEST_LAUNCHD') == '1', 'isolated manager proof opt-in')
    def test_previous_format_supervised_update_preserves_registry_and_history(self):
        old = self.previous_format_route()
        self.invoke(old / 'shin-ce', 'service-install', '--home', str(self.home))
        state = self.home / 'supervision/state.json'
        original = json.loads(state.read_text())
        agent = Path(original['agent_path']); target = f"gui/{os.getuid()}/{original['label']}"
        def cleanup():
            subprocess.run(['/bin/launchctl', 'bootout', target], capture_output=True, timeout=20)
            endpoint = self.home / 'service/rpc.sock'; deadline = time.monotonic() + 15
            while endpoint.exists() and time.monotonic() < deadline: time.sleep(.05)
            self.assertFalse(endpoint.exists())
            if agent.exists():
                import plistlib
                self.assertEqual(plistlib.loads(agent.read_bytes())['Label'], original['label'])
                agent.unlink()
        self.addCleanup(cleanup)
        self.invoke(old / 'shin-ce', 'service-stop', '--home', str(self.home))
        endpoint = self.home / 'service/rpc.sock'; deadline = time.monotonic() + 10
        while endpoint.exists() and time.monotonic() < deadline: time.sleep(.02)
        self.assertFalse(endpoint.exists())
        registry = (self.home / 'registry/state.json').read_bytes()
        history = self.home / 'service/history/telemetry.sqlite'; inode = history.stat().st_ino
        grant_path = self.workspace() / 'publication/state.json'; grant = json.loads(grant_path.read_text())
        source = {p: p.read_bytes() for p in self.f.root.rglob('*') if p.is_file()}
        rejected = self.ce('service-update', expected=2)
        self.assertEqual(rejected['error']['code'], 'service-update-project-not-ready')
        self.assertEqual(original, json.loads(state.read_text()))
        self.ce('watch', '--samples', '2')
        updated = self.ce('service-update')
        self.assertTrue(updated['changed']); self.assertEqual(updated['ready_projects'], 1)
        self.assertFalse(self.ce('service-update')['changed'])
        self.assertIn('return 7;', self.ce('query', 'readLimit')['source_context'])
        self.assertEqual(registry, (self.home / 'registry/state.json').read_bytes())
        self.assertEqual(inode, history.stat().st_ino)
        self.assertEqual(source, {p: p.read_bytes() for p in self.f.root.rglob('*') if p.is_file()})
        after = json.loads(grant_path.read_text())
        self.assertEqual(grant['route'], after['route']); self.assertEqual(grant['token'], after['token'])

    def test_previous_format_rollback_uses_explicit_writer_handoff(self):
        old = self.previous_format_route()
        grant_path = self.workspace() / 'publication/state.json'; before = json.loads(grant_path.read_text())
        history = self.home / 'service/history/telemetry.sqlite'; original = (history.stat().st_ino, history.read_bytes())
        self.ce('watch', '--samples', '2')
        self.ce('refresh-release')
        self.invoke(old / 'shin-context', '--profile', 'observer-fixture',
                    '--profile-dir', str(self.f.profiles), '--root', str(self.f.root),
                    '--out-dir', str(self.f.out), 'build')
        self.invoke(old / 'shin-observer', 'sample', '--config', str(self.f.config))
        restored = self.invoke(old / 'shin-ce', 'init', '--observer-config', str(self.f.config),
                    '--manage-index', '--root', str(self.f.root), '--home', str(self.home))
        self.assertTrue(restored['local_ready'])
        manifest = json.loads((self.f.out / 'docs/kb/generated/manifest.json').read_text())
        self.assertEqual(manifest['generator_version'], 'shin-generator-v21-cpp-syntax')
        after = json.loads(grant_path.read_text())
        self.assertEqual(before['route'], after['route'])
        self.assertNotEqual(before['token'], after['token']) # Explicit release/regrant rotates permission.
        self.assertEqual(original, (history.stat().st_ino, history.read_bytes()))

    def test_previous_format_marker_requires_exact_acceptance_before_refresh(self):
        old = self.previous_format_route(with_marker=True)
        marker = self.f.root / 'docs/machine/shin-project-adoption-v1.json'
        original_marker = marker.read_bytes()
        grant_path = self.workspace() / 'publication/state.json'; grant = json.loads(grant_path.read_text())
        head = (self.f.out / 'docs/kb/generated/manifest.json').read_bytes()
        failed = self.ce('watch', '--samples', '2')['watch']
        self.assertEqual(failed['last_result']['error']['code'], 'refresh-validation-failed')
        self.assertEqual(head, (self.f.out / 'docs/kb/generated/manifest.json').read_bytes())
        self.assertEqual(grant['last_snapshot'], json.loads(grant_path.read_text())['last_snapshot'])
        self.assertEqual(self.marker_accept(marker, expected=2)['error']['code'], 'invalid-marker-revision')
        value = json.loads(marker.read_text()); value['engine']['generatorIdentity'] = 'shin-generator-v22-compact-lexical'
        marker.write_text(json.dumps(value))
        self.assertEqual(self.ce('watch', '--samples', '1', expected=2)['error']['code'], 'route-revision-conflict')
        self.assertEqual(head, (self.f.out / 'docs/kb/generated/manifest.json').read_bytes())
        self.assertTrue(self.marker_accept(marker)['changed'])
        self.ce('watch', '--samples', '2')
        self.assertTrue(self.ce('init', '--manage-index')['local_ready'])
        after = json.loads(grant_path.read_text())
        self.assertEqual(grant['route'], after['route']); self.assertEqual(grant['token'], after['token'])
        self.assertEqual(after['approved_marker_hash'], hashlib.sha256(marker.read_bytes()).hexdigest())

        # Reverse approval while the original grant is still active; a revoked
        # grant cannot authorize marker-accept. The old kit validates the old pin.
        marker.write_bytes(original_marker)
        self.invoke(old / 'shin-ce', 'marker-accept', hashlib.sha256(original_marker).hexdigest(),
                    '--root', str(self.f.root), '--home', str(self.home))
        self.ce('refresh-release')
        self.invoke(old / 'shin-context', '--profile', 'observer-fixture', '--profile-dir', str(self.f.profiles),
                    '--root', str(self.f.root), '--out-dir', str(self.f.out), 'build')
        self.invoke(old / 'shin-observer', 'sample', '--config', str(self.f.config))
        restored = self.invoke(old / 'shin-ce', 'init', '--manage-index', '--observer-config', str(self.f.config),
                    '--root', str(self.f.root), '--home', str(self.home))
        self.assertTrue(restored['local_ready'])
        self.assertEqual(marker.read_bytes(), original_marker)
        after = json.loads(grant_path.read_text())
        self.assertEqual(after['route'], grant['route']); self.assertNotEqual(after['token'], grant['token'])

    def test_managed_same_format_core_upgrade_keeps_original_grant_and_requalifies(self):
        self.managed()
        binding_path = self.workspace() / 'state.json'
        grant_path = self.workspace() / 'publication/state.json'
        binding = json.loads(binding_path.read_text())
        grant = json.loads(grant_path.read_text())
        binding['core_version'] = grant['route']['core_version'] = '2.0.0-dev.1'
        write_private_json(binding_path, binding)
        write_private_json(grant_path, grant)
        (self.workspace() / 'qualification/state.json').unlink()
        checked = self.ce('status')
        self.assertTrue(checked['local_ready'])
        self.assertGreater(checked['work_performed']['full_generations'], 0)
        (self.f.root / 'src/limits.js').write_text('export function readLimit() { return 71; }\n')
        self.ce('watch', '--samples', '2')
        self.assertIn('return 71;', self.ce('query', 'readLimit')['source_context'])
        self.assertEqual(json.loads(binding_path.read_text())['core_version'], '2.0.0-dev.1')
        self.assertEqual(json.loads(grant_path.read_text())['route'], grant['route'])
        self.assertEqual(json.loads(grant_path.read_text())['token'], grant['token'])
        changed = json.loads(binding_path.read_text())
        changed['generator_version'] = 'different-format'
        changed_grant = json.loads(grant_path.read_text())
        changed_grant['route']['generator_version'] = 'different-format'
        write_private_json(binding_path, changed)
        write_private_json(grant_path, changed_grant)
        self.assertEqual(self.ce('status', expected=2)['error']['code'], 'refresh-engine-changed')

    def test_failed_writer_does_not_commit_new_quality_qualification(self):
        self.managed()
        other = self.base / 'other-home'
        self.ce('init', '--observer-config', str(self.f.config), '--manage-index', home=other, expected=2)
        self.assertFalse(list(other.glob('projects/*/qualification/state.json')))
        receipt = self.workspace() / 'qualification/state.json'
        wrong = {**json.loads(receipt.read_text()), 'key': '0' * 64}
        write_private_json(receipt, wrong)
        watch = self.workspace() / 'watch'
        watch.mkdir(mode=0o700)
        write_private_json(watch / 'state.json', {'oversized': 'x' * 70000})
        self.assertEqual(self.ce('init', expected=2)['error']['code'], 'output-limit')
        self.assertEqual(json.loads(receipt.read_text()), wrong)

    def test_malformed_quality_receipts_recover_but_unsafe_storage_refuses(self):
        self.managed()
        receipt = self.workspace() / 'qualification/state.json'
        saved = json.loads(receipt.read_text())
        for key, value in [('key', 'bad'), ('manifest_sha256', 'g' * 64),
                           ('snapshot_id', 'fnv1a64:bad'), ('qualified_at_ms', -1),
                           ('resource', {'unexpected': True})]:
            with self.subTest(key=key):
                write_private_json(receipt, {**saved, key: value})
                before = receipt.read_bytes()
                self.assertEqual(self.ce('status')['readiness']['quality_reuse']['cache_status'], 'invalid')
                self.assertEqual(receipt.read_bytes(), before)
                self.ce('watch', '--samples', '1')
                self.assertTrue(self.ce('status')['readiness']['quality_reuse']['reused'])
        write_private_json(receipt, saved)
        copy = self.base / 'qualification-copy.json'
        shutil.copyfile(receipt, copy)
        receipt.unlink()
        receipt.symlink_to(copy)
        self.assertFalse(self.ce('status', expected=2)['ok'])
        self.ce('watch', '--samples', '1', expected=2)
        self.assertTrue(receipt.is_symlink())
        self.assertEqual(json.loads(copy.read_text()), saved)

    def test_truncated_quality_cache_recovers_during_service_without_init(self):
        self.managed(service=True)
        self.await_service_query('return 7;')
        receipt = self.workspace() / 'qualification/state.json'
        before = self.canonical_files()
        receipt.write_text('{"schema_version":')
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            try:
                restored = json.loads(receipt.read_text())
            except json.JSONDecodeError:
                restored = {}
            if restored.get('schema_version') == 'shin.local-qualification.v2':
                break
            time.sleep(.05)
        else:
            self.fail('service did not recover the derived qualification')
        self.await_service_query('return 7;')
        self.assertEqual(before, self.canonical_files())

    def test_invalid_quality_cache_cannot_bypass_failed_quality_or_stale_source(self):
        self.managed()
        receipt = self.workspace() / 'qualification/state.json'
        receipt.write_text('{')
        (self.f.root / 'src/limits.js').write_text('export function readLimit() { return 911; }\n')
        failed = self.ce('query', 'readLimit', expected=1)
        self.assertFalse(failed['canonical_ready'])
        self.assertNotIn('source_context', failed)
        self.assertEqual(receipt.read_text(), '{')
        self.ce('watch', '--samples', '2')
        self.assertIn('return 911;', self.ce('query', 'readLimit')['source_context'])

    def test_derived_quality_recovery_preserves_failed_project_gate(self):
        self.managed()
        receipt = self.workspace() / 'qualification/state.json'
        receipt.write_text('{')
        before = {p: p.read_bytes() for p in (self.f.out / 'docs/kb/generated').iterdir() if p.is_file()}
        self.f.write_source('# Route Contract\n\nUnrelated replacement text.\n')
        watched = self.ce('watch', '--samples', '2')['watch']
        self.assertGreater(watched['failures'], 0)
        self.assertEqual(watched['recoveries'], 0)
        self.assertEqual(receipt.read_text(), '{')
        self.assertEqual(before, {p: p.read_bytes() for p in before})
        self.assertNotIn('source_context', self.ce('query', 'readLimit', expected=1))

    def test_oversized_and_interrupted_derived_receipts_recover(self):
        self.managed()
        receipt = self.workspace() / 'qualification/state.json'
        receipt.write_text('x' * (256 * 1024 + 1))
        pending = receipt.with_name('state.pending')
        write_private_json(pending, {'incomplete': True})
        self.assertEqual(self.ce('status')['readiness']['quality_reuse']['cache_status'], 'invalid')
        self.assertTrue(pending.exists(), 'reader must not repair storage')
        self.ce('watch', '--samples', '1')
        self.assertFalse(pending.exists())
        self.assertTrue(self.ce('status')['readiness']['quality_reuse']['reused'])

    def test_derived_quality_recovery_refuses_hardlink_and_unsafe_permissions(self):
        self.managed()
        receipt = self.workspace() / 'qualification/state.json'
        receipt.write_text('{')
        other = self.base / 'hardlink.json'
        os.link(receipt, other)
        self.ce('watch', '--samples', '1', expected=2)
        self.assertEqual(other.read_text(), '{')
        other.unlink()
        receipt.chmod(0o644)
        self.ce('watch', '--samples', '1', expected=2)
        self.assertEqual(receipt.read_text(), '{')
        self.assertEqual(receipt.stat().st_mode & 0o777, 0o644)

    def test_service_recreates_corrupt_source_sqlite_without_init(self):
        self.managed(service=True)
        self.await_service_query('return 7;')
        caches = list((self.workspace() / 'source-cache').glob('*.sqlite'))
        self.assertEqual(len(caches), 1)
        before = self.canonical_files()
        caches[0].write_bytes(b'damaged derived sqlite')
        self.await_service_query('return 7;')
        self.assertTrue(caches[0].read_bytes().startswith(b'SQLite format 3\0'))
        self.assertEqual(before, self.canonical_files())

    def test_executable_replacement_cannot_relabel_quality_qualification(self):
        self.managed()
        receipt = self.workspace() / 'qualification/state.json'
        receipt.unlink()
        binary = self.base / 'qualified-ce'
        shutil.copy2(CE, binary)
        with (self.f.out / 'docs/kb/.shin-context.lock').open('rb') as publication:
            fcntl.flock(publication, fcntl.LOCK_EX)
            process = subprocess.Popen([str(binary), 'init', '--no-service', '--root', str(self.f.root),
                                        '--home', str(self.home)], stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True)
            try:
                # The qualification key is captured before this publication
                # lock is opened. Observe that barrier, not process startup time.
                lock_path = str(self.f.out / 'docs/kb/.shin-context.lock')
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    self.assertIsNone(process.poll())
                    if sys.platform == 'darwin':
                        descriptors = subprocess.run(['/usr/sbin/lsof', '-a', '-p', str(process.pid), '-Fn'],
                            capture_output=True, text=True, timeout=5).stdout.splitlines()
                        opened = 'n' + lock_path in descriptors
                    else:
                        descriptors = Path(f'/proc/{process.pid}/fd')
                        opened = False
                        for fd in descriptors.iterdir():
                            try:
                                opened = os.readlink(fd) == lock_path
                            except FileNotFoundError:
                                # Other descriptors can close between listing and readlink.
                                continue
                            if opened:
                                break
                    if opened:
                        break
                    time.sleep(.02)
                else:
                    self.fail('writer did not reach the publication lock barrier')
                self.assertIsNone(process.poll())
                replacement = self.base / 'replacement'
                replacement.write_bytes(binary.read_bytes() + b'changed-build')
                replacement.chmod(0o755)
                replacement.replace(binary)
                fcntl.flock(publication, fcntl.LOCK_UN)
                output, error = process.communicate(timeout=15)
                self.assertEqual(process.returncode, 2, output + error)
                self.assertEqual(json.loads(error)['error']['code'], 'qualification-engine-changed')
                self.assertFalse(receipt.exists())
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate(timeout=5)

    def grant_path(self):
        return self.workspace() / 'publication/state.json'

    def owner_path(self):
        return self.f.out / 'docs/kb/.shin-context-owner'

    def profile_hash(self):
        result = self.invoke(CORE, '--profile', 'observer-fixture', '--profile-dir', str(self.f.profiles),
                             '--root', str(self.f.root), '--out-dir', str(self.f.out), 'profile-policy')
        return result['revision']['profile_config_hash']

    def profile_accept(self, profile_hash, *, expected=0):
        return self.ce('profile-accept', profile_hash, expected=expected)

    def marker_accept(self, marker, *, expected=0):
        return self.ce('marker-accept', hashlib.sha256(marker.read_bytes()).hexdigest(), expected=expected)

    def test_marker_accept_preserves_binding_grant_and_waits_for_refresh(self):
        marker, value = self.marker()
        self.managed()
        binding_path = self.workspace() / 'state.json'
        binding = binding_path.read_bytes()
        grant = json.loads(self.grant_path().read_text())
        owner = self.owner_path().read_bytes()
        old_hash = hashlib.sha256(marker.read_bytes()).hexdigest()
        qualification = self.workspace() / 'qualification/state.json'
        old_quality_key = json.loads(qualification.read_text())['key']
        manifest = self.generated_manifest().read_bytes()

        value['engine']['minimumVersion'] = '2.0.0'
        marker.write_text(json.dumps(value))
        new_hash = hashlib.sha256(marker.read_bytes()).hexdigest()
        self.assertEqual(self.ce('query', 'readLimit', expected=2)['error']['code'], 'route-revision-conflict')
        refused = self.ce('watch', '--samples', '1', expected=2)
        self.assertFalse(refused['ok'])
        self.assertEqual(refused['error']['code'], 'route-revision-conflict')
        wrong = self.ce('marker-accept', '0' * 64, expected=2)
        self.assertEqual(wrong['error']['code'], 'marker-revision-changed')
        self.assertEqual(json.loads(self.grant_path().read_text()), grant)
        for invalid in ('', 'a' * 63, 'A' * 64, 'g' * 64):
            args = (invalid,) if invalid else ()
            self.assertEqual(self.ce('marker-accept', *args, expected=2)['error']['code'], 'invalid-marker-revision')

        accepted = self.marker_accept(marker)
        self.assertTrue(accepted['changed'])
        self.assertEqual(accepted['previous_marker_hash'], old_hash)
        self.assertEqual(accepted['approved_marker_hash'], new_hash)
        self.assertFalse(accepted['readiness_assessed'])
        self.assertFalse(accepted['index_mutated'])
        revised = json.loads(self.grant_path().read_text())
        self.assertEqual(revised['schema_version'], 'shin.local-publication.v3')
        self.assertEqual(len(revised), 8)
        self.assertEqual(revised['approved_marker_hash'], new_hash)
        for field in ('route', 'token', 'last_snapshot', 'pending_snapshot', 'approved_profile_hash'):
            self.assertEqual(revised[field], grant[field])
        self.assertEqual(binding_path.read_bytes(), binding)
        self.assertEqual(self.owner_path().read_bytes(), owner)
        self.assertEqual(self.generated_manifest().read_bytes(), manifest)
        self.assertEqual(json.loads(qualification.read_text())['key'], old_quality_key)
        receipt = self.grant_path().read_bytes()
        self.assertFalse(self.marker_accept(marker)['changed'])
        self.assertEqual(self.grant_path().read_bytes(), receipt)

        refreshed = self.ce('watch', '--samples', '2')['watch']['last_result']
        self.assertTrue(refreshed['ok'])
        self.assertNotEqual(json.loads(qualification.read_text())['key'], old_quality_key)
        self.assertTrue(self.ce('status')['canonical_ready'])
        self.assertIn('return 7;', self.ce('query', 'readLimit')['source_context'])
        self.assertEqual(json.loads(self.grant_path().read_text())['route'], grant['route'])
        self.assertEqual(json.loads(self.grant_path().read_text())['token'], grant['token'])

    def test_marker_accept_rejects_marker_identity_and_entry_gate_changes(self):
        marker, value = self.marker()
        self.managed()
        receipt = self.grant_path().read_bytes()
        original = json.loads(json.dumps(value))
        mutations = [('project', 'id', 'other-project'), ('profile', 'id', 'other-profile'),
                     ('index', 'key', 'other-index'), ('agent', 'entryPath', 'OTHER.md'),
                     ('agent', 'profileReferenceRequired', False), ('qualityGates', 'evalStrict', False)]
        for section, field, replacement in mutations:
            with self.subTest(section=section, field=field):
                value = json.loads(json.dumps(original))
                value[section][field] = replacement
                marker.write_text(json.dumps(value))
                refused = self.marker_accept(marker, expected=2)
                self.assertIn(refused['error']['code'], {'invalid-marker-revision', 'marker-identity-conflict'})
                self.assertEqual(self.grant_path().read_bytes(), receipt)
        marker.write_text(json.dumps(original))
        entry = self.f.root / 'AGENTS.md'
        entry.write_text('# Missing adoption references\n')
        self.assertEqual(self.marker_accept(marker, expected=2)['error']['code'], 'invalid-marker-revision')
        self.assertEqual(self.grant_path().read_bytes(), receipt)

    def test_marker_accept_requires_grant_and_preserves_route_conflict_fences(self):
        marker, value = self.marker()
        self.attach()
        self.assertEqual(self.marker_accept(marker, expected=2)['error']['code'], 'refresh-grant-required')
        self.managed()
        receipt = self.grant_path().read_bytes()
        config = self.f.config.read_bytes()
        self.f.config.write_bytes(config + b' ')
        self.assertEqual(self.marker_accept(marker, expected=2)['error']['code'], 'route-revision-conflict')
        self.f.config.write_bytes(config)
        self.assertEqual(self.grant_path().read_bytes(), receipt)
        self.ce('refresh-release')
        self.assertEqual(self.marker_accept(marker, expected=2)['error']['code'], 'refresh-grant-required')
        self.assertEqual(self.ce('marker-accept', hashlib.sha256(marker.read_bytes()).hexdigest(),
                                 home=self.base / 'other-home', expected=2)['error']['code'], 'refresh-grant-required')

    def test_marker_v3_survives_profile_accept_release_and_detach_reinit(self):
        marker, value = self.marker()
        self.managed()
        value['engine']['minimumVersion'] = '2.0.0'
        marker.write_text(json.dumps(value))
        marker_hash = self.marker_accept(marker)['approved_marker_hash']
        self.ce('watch', '--samples', '2')
        self.profile['sourceFactMaxFiles'] = 128
        self.f.profile_path.write_text(json.dumps(self.profile))
        profile_hash = self.profile_hash()
        self.profile_accept(profile_hash)
        self.ce('watch', '--samples', '2')
        original = json.loads(self.grant_path().read_text())
        self.assertEqual(original['schema_version'], 'shin.local-publication.v3')
        self.assertEqual(original['approved_marker_hash'], marker_hash)
        self.assertEqual(original['approved_profile_hash'], profile_hash)
        self.ce('refresh-release')
        config = self.f.config_data()
        config['max_snapshot_bytes'] *= 2
        write_private_json(self.f.config, config)
        self.assertTrue(self.config_accept()['changed'])
        self.managed()
        self.ce('detach')
        self.managed()
        reconnected = json.loads(self.grant_path().read_text())
        self.assertEqual(reconnected['schema_version'], 'shin.local-publication.v3')
        self.assertEqual(reconnected['approved_marker_hash'], marker_hash)
        self.assertEqual(reconnected['approved_profile_hash'], profile_hash)
        self.assertEqual(reconnected['route'], original['route'])
        self.assertTrue(self.ce('status')['canonical_ready'])

    def test_legacy_marker_identity_must_be_seeded_from_original_bytes(self):
        marker, value = self.marker()
        self.managed()
        shutil.rmtree(self.workspace() / 'marker-policy')
        original = marker.read_bytes()
        value['engine']['minimumVersion'] = '2.0.0'
        marker.write_text(json.dumps(value))
        self.assertEqual(self.marker_accept(marker, expected=2)['error']['code'], 'marker-identity-unavailable')
        marker.write_bytes(original)
        # A first capture interrupted before its atomic state commit can resume
        # only from bytes matching the original pinned hash.
        policy = self.workspace() / 'marker-policy'
        policy.mkdir(mode=0o700)
        pending = policy / 'state.pending'
        pending.write_text('{"interrupted":')
        pending.chmod(0o600)
        self.assertFalse(self.marker_accept(marker)['changed'])
        value['engine']['minimumVersion'] = '2.0.0'
        marker.write_text(json.dumps(value))
        self.assertTrue(self.marker_accept(marker)['changed'])

    def test_profile_and_marker_metadata_can_be_approved_without_reverting_either(self):
        marker, value = self.marker()
        self.managed()
        binding = (self.workspace() / 'state.json').read_bytes()
        owner = self.owner_path().read_bytes()
        self.profile['sourceFactMaxFiles'] = 128
        self.f.profile_path.write_text(json.dumps(self.profile))
        value['engine']['minimumVersion'] = '2.0.0'
        marker.write_text(json.dumps(value))
        self.assertEqual(self.marker_accept(marker, expected=2)['error']['code'], 'route-revision-conflict')
        self.profile_accept(self.profile_hash())
        self.assertEqual(self.ce('query', 'readLimit', expected=2)['error']['code'], 'route-revision-conflict')
        self.marker_accept(marker)
        self.assertEqual((self.workspace() / 'state.json').read_bytes(), binding)
        self.ce('watch', '--samples', '2')
        self.assertTrue(self.ce('status')['canonical_ready'])
        before, after = json.loads(binding), json.loads((self.workspace() / 'state.json').read_text())
        before.pop('prepared_snapshot')
        after.pop('prepared_snapshot')
        self.assertEqual(after, before)
        self.assertEqual(self.owner_path().read_bytes(), owner)

    def test_marker_accept_fences_resident_reader_until_approved_refresh(self):
        marker, value = self.marker()
        self.managed(service=True)
        self.assertIn('return 7;', self.ce('query', 'readLimit')['source_context'])
        approved = hashlib.sha256(marker.read_bytes()).hexdigest()
        unapproved_grant = json.loads(self.grant_path().read_text())
        value['engine']['minimumVersion'] = '2.0.0'
        marker.write_text(json.dumps(value))
        run = subprocess.run([str(CE), 'query', 'readLimit', '--service', '--root', str(self.f.root),
                              '--home', str(self.home)], capture_output=True, text=True, timeout=30)
        self.assertNotEqual(run.returncode, 0)
        self.assertFalse(json.loads(run.stdout or run.stderr).get('source_context'))
        reviewed = hashlib.sha256(marker.read_bytes()).hexdigest()
        self.assertNotEqual(reviewed, approved)
        # The live publisher and explicit approval share an exclusive writer.
        # Refusal happens before mutation; it does not approve or bypass fences.
        with (self.workspace() / 'observer.lock').open('rb') as writer:
            fcntl.flock(writer, fcntl.LOCK_EX)
            refused = self.marker_accept(marker, expected=2)
            self.assertEqual(refused['error']['code'], 'writer-busy')
            self.assertEqual(json.loads(self.grant_path().read_text()), unapproved_grant)
        approval_deadline = time.monotonic() + 10
        while True:
            run = subprocess.run([str(CE), 'marker-accept', reviewed, '--root', str(self.f.root),
                                  '--home', str(self.home)], capture_output=True, text=True, timeout=30,
                                 env={**os.environ, 'PATH': '', 'LC_ALL': 'C'}, umask=0o077)
            if run.returncode == 0:
                self.assertFalse(run.stderr, run.stderr)
                accepted = json.loads(run.stdout)
                self.assertEqual(accepted['previous_marker_hash'], approved)
                self.assertEqual(accepted['approved_marker_hash'], reviewed)
                break
            self.assertEqual(run.returncode, 2, run.stdout + run.stderr)
            self.assertFalse(run.stdout)
            self.assertEqual(json.loads(run.stderr)['error']['code'], 'writer-busy')
            self.assertEqual(json.loads(self.grant_path().read_text()), unapproved_grant)
            self.assertLess(time.monotonic(), approval_deadline, 'publisher did not release the writer')
            time.sleep(.02)
        self.assertEqual(hashlib.sha256(marker.read_bytes()).hexdigest(), reviewed)
        self.assertEqual(json.loads(self.grant_path().read_text())['approved_marker_hash'], reviewed)
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            run = subprocess.run([str(CE), 'query', 'readLimit', '--service', '--root', str(self.f.root),
                                  '--home', str(self.home)], capture_output=True, text=True, timeout=30)
            result = json.loads(run.stdout or run.stderr)
            if run.returncode == 0:
                self.assertTrue(result['canonical_ready'])
                self.assertIn('return 7;', result['source_context'])
                break
            self.assertFalse(result.get('source_context'))
            time.sleep(.1)
        else:
            self.fail('resident did not refresh after marker approval: ' + str(result))

    def generated_manifest(self):
        return self.f.out / 'docs/kb/generated/manifest.json'

    def test_membership_revision_auto_accepts_only_after_validation(self):
        self.managed()
        before = json.loads(self.grant_path().read_text())
        manifest = self.generated_manifest().read_bytes()
        (self.f.root / 'next').mkdir()
        (self.f.root / 'next/new.js').write_text('export function newLimit() { return 81; }\n')
        self.profile['sourceFactRoots'] = ['src', 'next']
        self.f.profile_path.write_text(json.dumps(self.profile))
        pending = self.ce('status')
        self.assertTrue(pending['ok'])
        self.assertTrue(pending['local_ready'])
        self.assertFalse(pending['canonical_ready'])
        self.assertEqual(pending['source_coverage']['state'], 'partial')
        self.assertEqual(before, json.loads(self.grant_path().read_text()))
        self.assertEqual(manifest, self.generated_manifest().read_bytes())
        report = self.ce('watch', '--samples', '2')
        self.assertEqual(report['watch']['last_result']['phase'], 'current')
        after = json.loads(self.grant_path().read_text())
        self.assertEqual(after['approved_profile_hash'], self.profile_hash())
        self.assertEqual(after['route'], before['route'])
        self.assertEqual(after['token'], before['token'])
        self.assertIn('return 81;', self.ce('query', 'newLimit')['source_context'])

    def test_membership_publication_recovers_approval_with_verified_head(self):
        self.managed()
        before = json.loads(self.grant_path().read_text())
        old_manifest = self.generated_manifest().read_bytes()
        # First produce a genuinely qualified candidate and its private baseline.
        (self.f.root / 'next').mkdir()
        (self.f.root / 'next/new.js').write_text('export function newLimit() { return 81; }\n')
        self.profile['sourceFactRoots'] = ['src', 'next']
        self.f.profile_path.write_text(json.dumps(self.profile))
        self.ce('watch', '--samples', '2')
        committed = json.loads(self.grant_path().read_text())
        self.assertEqual(committed['schema_version'], 'shin.local-publication.v4')
        # Simulate process death after the index swap but before receipt commit.
        pending = {**committed, 'last_snapshot': before['last_snapshot'],
                   'approved_profile_hash': before['approved_profile_hash'],
                   'pending_snapshot': committed['last_snapshot'],
                   'pending_profile_hash': committed['approved_profile_hash']}
        write_private_json(self.grant_path(), pending)
        self.ce('watch', '--samples', '1')
        recovered = json.loads(self.grant_path().read_text())
        self.assertEqual(recovered, committed)
        self.assertIn('return 81;', self.ce('query', 'newLimit')['source_context'])
        # Simulate death before publication: old head must retain old approval.
        # The original verified head is reproduced only in this isolated fixture.
        owner = self.owner_path().read_bytes()
        self.owner_path().unlink()
        self.profile['sourceFactRoots'] = ['src']
        self.build()
        self.assertEqual(self.generated_manifest().read_bytes(), old_manifest)
        self.owner_path().write_bytes(owner)
        self.owner_path().chmod(0o600)
        self.profile['sourceFactRoots'] = ['src', 'next']
        self.f.profile_path.write_text(json.dumps(self.profile))
        write_private_json(self.grant_path(), pending)
        self.ce('watch', '--samples', '1')
        recovered = json.loads(self.grant_path().read_text())
        self.assertEqual(recovered['approved_profile_hash'], before['approved_profile_hash'])
        self.assertEqual(recovered['last_snapshot'], before['last_snapshot'])
        self.assertEqual(recovered['pending_profile_hash'], '')
        self.assertEqual(recovered['pending_snapshot'], '')

    def test_failed_membership_candidate_keeps_verified_partial_context(self):
        self.managed()
        before = self.grant_path().read_bytes()
        manifest = self.generated_manifest().read_bytes()
        self.profile['rootFiles'] = []
        self.f.profile_path.write_text(json.dumps(self.profile))
        report = self.ce('watch', '--samples', '2')
        last = report['watch']['last_result']
        self.assertEqual(last['phase'], 'partial')
        self.assertEqual(last['profile_revision']['state'], 'rejected')
        self.assertIn('gates', last['candidate_diagnostic'])
        self.assertEqual(before, self.grant_path().read_bytes())
        self.assertEqual(manifest, self.generated_manifest().read_bytes())
        status = self.ce('status')
        snapshot = status['snapshot_id']
        read = self.ce('read', 'src/limits.js', '--snapshot', snapshot)
        self.assertIn('return 7;', read['source_context'])
        (self.f.root / 'src/limits.js').write_text('export function readLimit() { return 991; }\n')
        omitted = self.ce('read', 'src/limits.js', '--snapshot', snapshot)
        self.assertTrue(omitted['ok'])
        self.assertFalse(omitted['native_read_complete'])
        self.assertNotIn('return 7', omitted['source_context'])
        self.assertNotIn('return 991', omitted['source_context'])
        query = self.ce('query', 'retry ceiling readLimit')
        self.assertTrue(query['ok'])
        self.assertNotIn('return 7', json.dumps(query))
        self.assertNotIn('ranked_paths', query['document_context'])
        self.assertNotIn('verification_paths', query['document_context'])
        self.assertEqual(query['document_context']['query_coverage']['state'], 'partial')
        self.assertFalse(query['canonical_ready'])
        self.assertEqual(query['source_coverage']['state'], 'partial')
        self.profile['rootFiles'] = ['README.md']
        self.f.profile_path.write_text(json.dumps(self.profile))
        self.assertEqual(self.ce('watch', '--samples', '2')['watch']['last_result']['phase'], 'current')

    def check_source_limit_retains_verified_context(self, oversized):
        self.profile.update(sourceFactMaxFiles=8 if oversized else 1, sourceFactMaxFileBytes=128)
        growth = self.f.root / 'src/zz-growth.js'
        if oversized:
            growth.write_text('export function growth() { return 1; }\n')
        self.build()
        self.managed()
        grant = self.grant_path().read_bytes()
        manifest = self.generated_manifest().read_bytes()
        profile = self.f.profile_path.read_bytes()
        if oversized:
            growth.write_text('export function growth() { return 1; }\n' + ' ' * 160)
        else:
            growth.write_text('export function growth() { return 1; }\n')
        report = self.ce('watch', '--samples', '1')
        last = report['watch']['last_result']
        self.assertEqual(last['phase'], 'partial')
        self.assertTrue(last['retrieval_ready'])
        self.assertFalse(last['canonical_ready'])
        self.assertEqual(last['profile_revision']['state'], 'approved')
        self.assertEqual(last['source_coverage']['reason'], 'source-observation-limited')
        self.assertEqual(last['source_coverage']['source_counts']['skipped_files' if oversized else 'capped_files'], 1)
        self.assertEqual(grant, self.grant_path().read_bytes())
        self.assertEqual(manifest, self.generated_manifest().read_bytes())
        self.assertEqual(profile, self.f.profile_path.read_bytes())
        status = self.ce('status')
        snapshot = status['snapshot_id']
        self.assertEqual(status['source_coverage']['reason'], 'source-observation-limited')
        # Old installed controllers persisted only the refusal, without counts.
        # Read-only qualification by a candidate kit must also recover safely.
        state_path = self.workspace() / 'watch/state.json'
        saved = json.loads(state_path.read_text())
        legacy = dict(saved)
        legacy['last_result'] = {'ok': False, 'error': {'code': 'incomplete-source-observation'}}
        write_private_json(state_path, legacy)
        self.assertTrue(self.ce('status')['retrieval_ready'])
        write_private_json(state_path, saved)
        self.assertIn('return 7;', self.ce('read', 'src/limits.js', '--snapshot', snapshot)['source_context'])
        (self.f.root / 'src/limits.js').write_text('export function readLimit() { return 991; }\n')
        withheld = self.ce('read', 'src/limits.js', '--snapshot', snapshot)
        self.assertFalse(withheld['native_read_complete'])
        self.assertNotIn('return 7', withheld['source_context'])
        self.assertNotIn('return 991', withheld['source_context'])
        query = self.ce('query', 'retry ceiling')
        self.assertIn('README.md', json.dumps(query['document_context']))
        self.assertEqual(query['source_coverage']['reason'], 'source-observation-limited')
        self.assertFalse(query['canonical_ready'])
        self.assertFalse(self.ce('read', 'src/zz-growth.js', '--snapshot', snapshot)['native_read_complete'])
        if oversized:
            growth.write_text('export function growth() { return 1; }\n')
        else:
            growth.unlink()
        restored = self.ce('watch', '--samples', '2')['watch']['last_result']
        self.assertEqual(restored['phase'], 'current')
        current = self.ce('status')
        self.assertTrue(current['canonical_ready'])
        ready_query = self.ce('query', 'retry ceiling')
        self.assertIn('return 991;', self.ce('read', 'src/limits.js', '--snapshot', ready_query['snapshot_id'])['source_context'])
        self.assertEqual(profile, self.f.profile_path.read_bytes())

    def test_source_limit_qualification_during_first_watch_check(self):
        self.profile['sourceFactMaxFiles'] = 1
        self.build()
        self.managed()
        grant = self.grant_path().read_bytes()
        manifest = self.generated_manifest().read_bytes()
        (self.f.root / 'src/zz-growth.js').write_text('export function growth() { return 1; }\n')
        result = self.invoke(QUALIFICATION, str(CE), str(self.home), '--first-watch-check', str(self.f.root))
        status = result['results'][0]
        self.assertTrue(status['local_ready'], status)
        self.assertTrue(status['retrieval_ready'])
        self.assertFalse(status['canonical_ready'])
        self.assertEqual(status['source_coverage']['reason'], 'source-observation-limited')
        self.assertEqual(grant, self.grant_path().read_bytes())
        self.assertEqual(manifest, self.generated_manifest().read_bytes())

    def test_source_file_cap_keeps_verified_partial_context(self):
        self.check_source_limit_retains_verified_context(False)

    def test_oversized_source_keeps_verified_partial_context(self):
        self.check_source_limit_retains_verified_context(True)

    def test_source_limit_service_reports_partial_and_refreshes_counts(self):
        self.profile['sourceFactMaxFiles'] = 1
        self.build()
        self.managed(service=True)
        before = self.generated_manifest().read_bytes()
        growth = self.f.root / 'src/zz-growth.js'
        growth.write_text('export function growth() { return 1; }\n')
        def wait_coverage(capped):
            deadline = time.monotonic() + 15
            last = {}
            while time.monotonic() < deadline:
                service = self.ce('service-status')
                last = next(p for p in service['telemetry']['project_states'] if p['root'] == str(self.f.root))
                coverage = last.get('source_coverage') or {}
                if (last.get('query_ready') and coverage.get('reason') == 'source-observation-limited' and
                        coverage.get('source_counts', {}).get('capped_files') == capped):
                    self.assertFalse(last['active_issue'])
                    return
                time.sleep(0.05)
            self.fail(json.dumps(last))
        wait_coverage(1)
        self.assertTrue(self.ce('query', 'retry ceiling')['retrieval_ready'])
        (self.f.root / 'src/zz-second.js').write_text('export function second() { return 2; }\n')
        wait_coverage(2)
        status = self.ce('status')
        self.assertEqual(status['source_coverage']['source_counts']['capped_files'], 2)
        self.assertIn('return 7;', self.ce('read', 'src/limits.js', '--snapshot', status['snapshot_id'])['source_context'])
        self.assertEqual(before, self.generated_manifest().read_bytes())

    def test_removed_module_without_profile_edit_keeps_safe_context(self):
        self.managed()
        (self.f.root / 'src/limits.js').unlink()
        (self.f.root / 'src').rmdir()
        report = self.ce('watch', '--samples', '2')
        self.assertEqual(report['watch']['last_result']['phase'], 'partial')
        query = self.ce('query', 'retry ceiling')
        self.assertTrue(query['ok'])
        self.assertFalse(query['canonical_ready'])
        self.assertIn('README.md', json.dumps(query['document_context']))
        self.assertNotIn('return 7', json.dumps(query))
        (self.f.root / 'src').mkdir()
        (self.f.root / 'src/limits.js').write_text('export function readLimit() { return 8; }\n')
        self.assertEqual(self.ce('watch', '--samples', '2')['watch']['last_result']['phase'], 'current')

    def test_membership_automation_preserves_protected_policy(self):
        self.managed()
        before = self.grant_path().read_bytes()
        original = dict(self.profile)
        for field, value in [('evalCases', []), ('sourceFactMaxFiles', 900),
                             ('secretScanExcludePrefixes', ['src']), ('sourceFactRoots', ['../outside'])]:
            with self.subTest(field=field):
                modified = {**original, field: value}
                self.f.profile_path.write_text(json.dumps(modified))
                self.assertEqual(self.ce('status', expected=2)['error']['code'], 'route-revision-conflict')
                self.assertEqual(self.grant_path().read_bytes(), before)
        self.f.profile_path.write_text(json.dumps(original))

    def test_profile_accept_recovers_source_overflow_without_manual_build(self):
        self.profile['sourceFactMaxFiles'] = 1
        self.build()
        self.managed()
        binding_path = self.workspace() / 'state.json'
        binding = binding_path.read_bytes()
        old_grant = json.loads(self.grant_path().read_text())
        manifest_path = self.generated_manifest()
        manifest_before = manifest_path.read_bytes()
        source_facts_before = json.loads(manifest_before)['source_fact_counts']
        self.assertEqual(source_facts_before['scanned_files'], 1)

        (self.f.root / 'src/newfile.js').write_text(
            'export function queryNewFile() { return "new source"; }\n')
        self.profile['sourceFactMaxFiles'] = 8
        self.f.profile_path.write_text(json.dumps(self.profile))
        self.assertEqual(self.ce('query', 'queryNewFile', expected=2)['error']['code'], 'route-revision-conflict')

        wrong = 'fnv1a64:' + '0' * 16
        rejected = self.profile_accept(wrong, expected=2)
        self.assertEqual(rejected['error']['code'], 'profile-revision-changed')
        self.assertEqual(json.loads(self.grant_path().read_text()), old_grant)

        approved_hash = self.profile_hash()
        accepted = self.profile_accept(approved_hash)
        self.assertTrue(accepted['ok'])
        self.assertFalse(accepted['local_ready'])
        self.assertFalse(accepted['readiness_assessed'])
        self.assertFalse(accepted['index_mutated'])
        self.assertEqual(accepted['approved_profile_hash'], approved_hash)
        self.assertEqual(binding, binding_path.read_bytes())
        self.assertEqual(manifest_before, manifest_path.read_bytes())
        grant = json.loads(self.grant_path().read_text())
        self.assertEqual(grant['schema_version'], 'shin.local-publication.v2')
        self.assertEqual(grant['approved_profile_hash'], approved_hash)
        self.assertEqual(grant['route'], old_grant['route'])
        self.assertEqual(grant['token'], old_grant['token'])
        receipt = self.grant_path().read_bytes()
        again = self.profile_accept(approved_hash)
        self.assertFalse(again['changed'])
        self.assertEqual(receipt, self.grant_path().read_bytes())

        refreshed = self.ce('watch', '--samples', '2')
        current_counts = json.loads(manifest_path.read_text())['source_fact_counts']
        self.assertTrue(refreshed['watch']['last_result']['ok'])
        self.assertEqual(current_counts['candidate_files'], 2)
        self.assertEqual(current_counts['scanned_files'], 2)
        result = self.ce('query', 'queryNewFile')
        self.assertIn('queryNewFile', result['source_context'])
        delivered = ''.join(json.loads(line.split(' ', 1)[1]) for line in result['source_context'].splitlines()
                            if line.startswith('L') and line[1:2].isdigit())
        self.assertIn('return "new source"', delivered)

        self.profile['summary'] = 'An unapproved follow-up revision.'
        self.f.profile_path.write_text(json.dumps(self.profile))
        self.assertEqual(self.ce('query', 'queryNewFile', expected=2)['error']['code'], 'route-revision-conflict')

    def test_profile_review_is_read_only_and_exact_consent_resumes_refresh(self):
        self.managed()
        self.ce('watch', '--samples', '1')  # capture the already-approved baseline
        (self.f.root / 'extra.md').write_text('# New guide\n')
        self.profile['rootFiles'].append('extra.md')
        self.profile['documentExcludePrefixes'] = ['docs/history']
        self.profile['packEvalCases'][0]['mustNotPaths'] = ['docs/history']
        self.f.profile_path.write_text(json.dumps(self.profile))
        before = self.canonical_files()
        grant = self.grant_path().read_bytes()
        review = self.ce('profile-review')['profile_review']
        self.assertTrue(review['can_accept'])
        self.assertTrue(review['checks_preserved'])
        self.assertEqual(review['revision'], self.profile_hash())
        self.assertEqual({x['id'] for x in review['categories']}, {'documents', 'exclusions', 'checks'})
        excluded = next(x for x in review['categories'] if x['id'] == 'exclusions')
        self.assertEqual(excluded['added_paths'], ['docs/history'])
        self.assertEqual(before, self.canonical_files())
        self.assertEqual(grant, self.grant_path().read_bytes())
        accepted = self.ce('profile-accept', review['revision'], '--review-token', review['review_token'])
        self.assertFalse(accepted['index_mutated'])
        self.assertFalse(accepted['local_ready'])
        self.assertEqual(before, self.canonical_files())
        self.assertFalse(self.ce('profile-review')['profile_review']['can_accept'])
        self.assertTrue(self.ce('watch', '--samples', '2')['watch']['last_result']['ok'])
        self.assertTrue(self.ce('query', 'readLimit')['ok'])

    def test_profile_review_accepts_document_selection_with_sources_and_added_check(self):
        self.managed()
        self.ce('watch', '--samples', '1')
        (self.f.root / 'firestore').mkdir()
        (self.f.root / 'firestore/owner.js').write_text('export function firestoreOwner() { return 9; }\n')
        (self.f.root / 'firestore.md').write_text('# Firestore\n\nfirestoreOwner owns native execution.\n')
        self.profile['rootFiles'].append('firestore.md')
        self.profile['documentIncludePrefixes'] = ['README.md', 'firestore.md']
        self.profile['sourceFactRoots'].append('firestore')
        self.profile['evalCases'].append({'id': 'firestore-owner', 'query': 'firestoreOwner',
                                         'mustPaths': ['firestore/owner.js']})
        self.f.profile_path.write_text(json.dumps(self.profile))
        before = self.canonical_files()
        grant = self.grant_path().read_bytes()
        review = self.ce('profile-review')['profile_review']
        self.assertTrue(review['can_accept'])
        self.assertTrue(review['checks_preserved'])
        self.assertEqual({x['id'] for x in review['categories']},
                         {'documents', 'document-selection', 'sources', 'checks'})
        selection = next(x for x in review['categories'] if x['id'] == 'document-selection')
        self.assertIn('firestore.md', selection['added_paths'])
        self.assertEqual(before, self.canonical_files())
        self.assertEqual(grant, self.grant_path().read_bytes())
        accepted = self.ce('profile-accept', review['revision'], '--review-token', review['review_token'])
        self.assertFalse(accepted['local_ready'])
        self.assertEqual(before, self.canonical_files())
        self.assertTrue(self.ce('watch', '--samples', '2')['watch']['last_result']['ok'])
        result = self.ce('query', 'firestoreOwner')
        self.assertTrue(result['canonical_ready'])
        self.assertIn('return 9', result['source_context'])

    def test_profile_review_rejects_stale_consent_and_preserves_grant(self):
        self.managed()
        self.ce('watch', '--samples', '1')
        self.profile['summary'] = 'First reviewed change'
        self.f.profile_path.write_text(json.dumps(self.profile))
        review = self.ce('profile-review')['profile_review']
        self.profile['summary'] = 'Another change after preview'
        self.f.profile_path.write_text(json.dumps(self.profile))
        before = self.grant_path().read_bytes()
        result = self.ce('profile-accept', self.profile_hash(), '--review-token', review['review_token'], expected=2)
        self.assertEqual(result['error']['code'], 'profile-review-changed')
        self.assertEqual(before, self.grant_path().read_bytes())
        self.ce('refresh-release')
        self.assertEqual(self.ce('profile-review', expected=2)['error']['code'], 'refresh-grant-required')

    def test_profile_review_unknown_policy_and_missing_baseline_never_claim_safe(self):
        self.managed()
        self.ce('watch', '--samples', '1')
        self.profile['sourceFactMaxFiles'] = 128
        self.f.profile_path.write_text(json.dumps(self.profile))
        review = self.ce('profile-review')['profile_review']
        self.assertFalse(review['can_accept'])
        self.assertIn('other', [x['id'] for x in review['categories']])
        self.assertEqual(self.ce('profile-accept', review['revision'], '--review-token', review['review_token'], expected=2)['error']['code'], 'profile-review-changed')
        shutil.rmtree(self.workspace() / 'profile-revisions')
        review = self.ce('profile-review')['profile_review']
        self.assertFalse(review['baseline_available'])
        self.assertFalse(review['can_accept'])
        self.assertFalse(review['checks_preserved'])

    def test_profile_review_reports_changed_checks_and_refuses_marker_change(self):
        marker, _ = self.marker()
        self.managed()
        self.ce('watch', '--samples', '1')
        self.profile['evalCases'][0]['mustPaths'] = ['AGENTS.md']
        self.f.profile_path.write_text(json.dumps(self.profile))
        review = self.ce('profile-review')['profile_review']
        self.assertFalse(review['checks_preserved'])
        self.assertEqual(review['categories'], [{'id': 'checks', 'added': 1, 'removed': 1, 'changed': 0}])
        marker.write_text(marker.read_text() + ' ')
        self.assertEqual(self.ce('profile-review', expected=2)['error']['code'], 'route-revision-conflict')

    def test_profile_accept_requires_current_hash_and_active_grant(self):
        self.attach()
        original_profile = self.f.profile_path.read_bytes()
        self.profile['summary'] = 'Changed before publisher grant.'
        self.f.profile_path.write_text(json.dumps(self.profile))
        approved_hash = self.profile_hash()
        unmanaged = self.profile_accept(approved_hash, expected=2)
        self.assertEqual(unmanaged['error']['code'], 'refresh-grant-required')

        self.f.profile_path.write_bytes(original_profile)
        self.managed()
        self.profile['summary'] = 'Changed after publisher grant.'
        self.f.profile_path.write_text(json.dumps(self.profile))
        approved_hash = self.profile_hash()
        wrong = self.profile_accept('fnv1a64:' + '1' * 16, expected=2)
        self.assertEqual(wrong['error']['code'], 'profile-revision-changed')
        self.assertEqual(self.profile_accept(approved_hash)['approved_profile_hash'], approved_hash)
        self.ce('refresh-release')
        revoked = self.profile_accept(approved_hash, expected=2)
        self.assertEqual(revoked['error']['code'], 'refresh-grant-required')

    def test_profile_accept_preserves_pending_publication_intent(self):
        self.managed()
        grant = json.loads(self.grant_path().read_text())
        grant['pending_snapshot'] = grant['last_snapshot']
        write_private_json(self.grant_path(), grant)
        self.profile['sourceFactMaxFiles'] = 128
        self.f.profile_path.write_text(json.dumps(self.profile))
        revision = self.profile_hash()
        before = self.canonical_files()
        accepted = self.profile_accept(revision)
        self.assertFalse(accepted['index_mutated'])
        self.assertEqual(before, self.canonical_files())
        self.assertEqual(json.loads(self.grant_path().read_text())['pending_snapshot'], grant['pending_snapshot'])
        refreshed = self.ce('watch', '--samples', '2')
        self.assertTrue(refreshed['watch']['last_result']['ok'])
        self.assertEqual(json.loads(self.grant_path().read_text())['pending_snapshot'], '')

    def test_profile_accept_refuses_changed_config_identity_and_root(self):
        marker, _ = self.marker()
        self.managed()
        approved_hash = self.profile_hash()
        receipt = self.grant_path().read_bytes()
        config = self.f.config.read_bytes()
        marker_bytes = marker.read_bytes()

        self.f.config.write_bytes(config + b' ')
        self.assertEqual(self.profile_accept(approved_hash, expected=2)['error']['code'], 'route-revision-conflict')
        self.f.config.write_bytes(config)
        marker.write_bytes(marker_bytes + b' ')
        self.assertFalse(self.profile_accept(approved_hash)['changed'])
        self.assertEqual(self.ce('query', 'readLimit', expected=2)['error']['code'], 'route-revision-conflict')
        value = json.loads(marker_bytes)
        value['project']['id'] = 'different-project'
        marker.write_text(json.dumps(value))
        self.assertEqual(self.profile_accept(approved_hash, expected=2)['error']['code'], 'marker-identity-conflict')
        marker.write_bytes(marker_bytes)
        self.assertEqual(receipt, self.grant_path().read_bytes())

        self.f.root.rename(self.base / 'source-before-replacement')
        self.f.root.mkdir(mode=0o700)
        refused = self.profile_accept(approved_hash, expected=2)
        self.assertIn(refused['error']['code'], {'binding-identity-changed', 'route-revision-conflict'})
        self.assertEqual(receipt, self.grant_path().read_bytes())

    def test_profile_accept_quality_failure_preserves_active_index(self):
        self.managed()
        manifest = self.generated_manifest()
        old_manifest = manifest.read_bytes()
        old_documents = (manifest.parent / 'documents.jsonl').read_bytes()
        self.profile['evalCases'][0]['query'] = 'unfindable_approved_revision_requirement'
        self.f.profile_path.write_text(json.dumps(self.profile))
        approved_hash = self.profile_hash()
        self.profile_accept(approved_hash)
        accepted_receipt = self.grant_path().read_bytes()

        failed = self.ce('watch', '--samples', '2')['watch']
        self.assertEqual(failed['publications'], 0)
        self.assertFalse(failed['last_result']['ok'])
        self.assertEqual(failed['last_result']['error']['code'], 'refresh-validation-failed')
        self.assertEqual(old_manifest, manifest.read_bytes())
        self.assertEqual(old_documents, (manifest.parent / 'documents.jsonl').read_bytes())
        self.assertEqual(accepted_receipt, self.grant_path().read_bytes())
        self.assertFalse(self.ce('query', 'readLimit', expected=1).get('source_context'))

    def test_profile_accept_survives_release_and_regrant(self):
        self.managed()
        binding_path = self.workspace() / 'state.json'
        binding = binding_path.read_bytes()
        self.profile['sourceFactMaxFiles'] = 8
        self.f.profile_path.write_text(json.dumps(self.profile))
        approved_hash = self.profile_hash()
        self.profile_accept(approved_hash)
        self.ce('watch', '--samples', '2')
        binding = binding_path.read_bytes()
        original_grant = json.loads(self.grant_path().read_text())

        self.ce('refresh-release')
        self.assertEqual(self.managed()['publication'], 'managed')
        regranted = json.loads(self.grant_path().read_text())
        self.assertEqual(regranted['phase'], 'active')
        self.assertEqual(regranted['approved_profile_hash'], approved_hash)
        self.assertEqual(regranted['route'], original_grant['route'])
        self.assertNotEqual(regranted['token'], original_grant['token'])
        self.assertEqual(binding, binding_path.read_bytes())
        self.assertIn('return 7;', self.ce('query', 'readLimit')['source_context'])

    def test_managed_canonical_service_refreshes_and_preserves_external_observer(self):
        observer = (self.f.state / 'state.json').read_bytes()
        first = self.managed(service=True)
        self.assertEqual(first['publication'], 'managed')
        self.assertEqual(first['refresh_management'], 'local-service')
        instance = first['service']['instance_id']
        old = json.loads(self.grant_path().read_text())
        (self.f.root / 'src/limits.js').write_text('export function readLimit() { return 83; }\n')
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            run = subprocess.run([str(CE), 'query', 'readLimit', '--service', '--root', str(self.f.root),
                                  '--home', str(self.home)], capture_output=True, text=True, timeout=30)
            value = json.loads(run.stdout or run.stderr)
            if run.returncode == 0:
                self.assertIn('return 83;', value['source_context'])
                break
            self.assertFalse(value.get('source_context'))
            time.sleep(.1)
        else:
            self.fail('canonical service did not publish: ' + str(value))
        self.assertEqual(value['index'], str(self.f.out))
        self.assertEqual(observer, (self.f.state / 'state.json').read_bytes())
        current = json.loads(self.grant_path().read_text())
        self.assertNotEqual(current['last_snapshot'], old['last_snapshot'])
        self.assertEqual(current['pending_snapshot'], '')
        self.assertEqual(self.ce('service-status')['instance_id'], instance)
        self.stop_service()
        self.ce('service-start')
        self.assertIn('return 83;', self.await_service_query('return 83;')['source_context'])
        self.stop_service()
        self.assertEqual(self.ce('refresh-release')['publication'], 'read-only')
        self.assertFalse(self.owner_path().exists())
        self.assertEqual(self.ce('refresh-release')['publication'], 'read-only')
        before = (self.f.out / 'docs/kb/generated/manifest.json').read_bytes()
        (self.f.root / 'src/limits.js').write_text('export function readLimit() { return 91; }\n')
        stopped = self.ce('watch', '--samples', '2')
        self.assertFalse(stopped['watch']['last_result']['ok'])
        self.assertEqual(stopped['watch']['publications'], 0)
        self.assertEqual(before, (self.f.out / 'docs/kb/generated/manifest.json').read_bytes())

    def test_managed_quality_failure_preserves_active_generation_then_recovers(self):
        self.managed()
        active = (self.f.out / 'docs/kb/generated/manifest.json').read_bytes()
        observer = (self.f.state / 'state.json').read_bytes()
        original = (self.f.root / 'README.md').read_text()
        (self.f.root / 'README.md').write_text('# Unrelated\n\nNo relevant evidence here.\n')
        failed = self.ce('watch', '--samples', '2')
        self.assertFalse(failed['watch']['last_result']['ok'])
        self.assertEqual(failed['watch']['last_result']['error']['code'], 'refresh-validation-failed')
        self.assertEqual(active, (self.f.out / 'docs/kb/generated/manifest.json').read_bytes())
        self.assertEqual(observer, (self.f.state / 'state.json').read_bytes())
        self.assertFalse(self.ce('query', 'readLimit', expected=1).get('source_context'))
        (self.f.root / 'README.md').write_text(original)
        (self.f.root / 'src/limits.js').write_text('export function readLimit() { return 29; }\n')
        self.ce('watch', '--samples', '2')
        self.assertIn('return 29;', self.ce('query', 'readLimit')['source_context'])

    def test_managed_repairs_corrupt_or_missing_generated_payload_without_init(self):
        self.managed()
        generated = self.f.out / 'docs/kb/generated'
        target = generated / 'documents.jsonl'
        original = target.read_bytes()
        manifest = (generated / 'manifest.json').read_bytes()
        for missing in [False, True]:
            with self.subTest(missing=missing):
                if missing:
                    target.unlink()
                else:
                    target.write_bytes(b'corrupt payload')
                self.assertNotIn('source_context', self.ce('query', 'readLimit', expected=2))
                watched = self.ce('watch', '--samples', '2')['watch']
                self.assertEqual(watched['failures'], 0)
                self.assertEqual(watched['publications'], 1)
                self.assertEqual(watched['recoveries'], 1)
                self.assertEqual(watched['last_recovery'], 'primary-index')
                self.assertEqual(target.read_bytes(), original)
                self.assertEqual((generated / 'manifest.json').read_bytes(), manifest)
                self.assertIn('return 7;', self.ce('query', 'readLimit')['source_context'])

    def test_primary_repair_preserves_bad_quality_candidate_and_damaged_base(self):
        self.managed()
        target = self.f.out / 'docs/kb/generated/documents.jsonl'
        target.write_bytes(b'corrupt payload')
        original = (self.f.root / 'README.md').read_text()
        (self.f.root / 'README.md').write_text('# Unrelated\n\nNo relevant evidence here.\n')
        receipt = self.grant_path().read_bytes()
        watched = self.ce('watch', '--samples', '2')['watch']
        self.assertEqual(watched['publications'], 0)
        self.assertGreater(watched['failures'], 0)
        self.assertEqual(watched['last_result']['error']['code'], 'refresh-validation-failed')
        self.assertEqual(target.read_bytes(), b'corrupt payload')
        self.assertEqual(self.grant_path().read_bytes(), receipt)
        (self.f.root / 'README.md').write_text(original)
        self.ce('watch', '--samples', '2')
        self.assertIn('return 7;', self.ce('query', 'readLimit')['source_context'])

    def test_primary_repair_refuses_foreign_extra_files_and_manifest_changes(self):
        self.managed()
        generated = self.f.out / 'docs/kb/generated'
        target = generated / 'documents.jsonl'
        target.write_bytes(b'corrupt payload')
        foreign = generated / 'user-notes.txt'
        foreign.write_text('not ours')
        self.ce('watch', '--samples', '1', expected=2)
        self.assertEqual(foreign.read_text(), 'not ours')
        foreign.unlink()
        manifest = generated / 'manifest.json'
        manifest.write_bytes(manifest.read_bytes() + b' ')
        self.ce('watch', '--samples', '1', expected=2)
        self.assertEqual(target.read_bytes(), b'corrupt payload')

    def test_primary_repair_refuses_missing_manifest_and_incomplete_sources(self):
        self.managed()
        generated = self.f.out / 'docs/kb/generated'
        target = generated / 'documents.jsonl'
        target.write_bytes(b'corrupt payload')
        manifest = generated / 'manifest.json'
        content = manifest.read_bytes()
        manifest.unlink()
        self.ce('watch', '--samples', '1', expected=2)
        self.assertFalse(manifest.exists())
        manifest.write_bytes(content)
        manifest.chmod(0o600)
        (self.f.root / 'src/large.js').write_text(' ' * (1024 * 1024 + 1))
        result = self.ce('watch', '--samples', '1')['watch']
        self.assertEqual(result['publications'], 0)
        self.assertFalse(result['last_result']['ok'])
        self.assertEqual(result['last_result']['observation']['status'], 'incomplete')
        self.assertEqual(target.read_bytes(), b'corrupt payload')

    def test_primary_repair_refuses_unmanaged_route(self):
        self.attach()
        target = self.f.out / 'docs/kb/generated/documents.jsonl'
        target.write_bytes(b'corrupt payload')
        self.ce('watch', '--samples', '1', expected=2)
        self.assertEqual(target.read_bytes(), b'corrupt payload')

    def test_primary_repair_refuses_links_and_shared_write_access(self):
        self.managed()
        target = self.f.out / 'docs/kb/generated/documents.jsonl'
        target.write_bytes(b'corrupt payload')
        original_mode = target.stat().st_mode & 0o777
        other = self.base / 'external.jsonl'
        for mode in ['symlink', 'hardlink', 'shared-write']:
            with self.subTest(mode=mode):
                if mode == 'symlink':
                    target.rename(other)
                    target.symlink_to(other)
                elif mode == 'hardlink':
                    os.link(target, other)
                else:
                    target.chmod(0o666)
                self.ce('watch', '--samples', '1', expected=2)
                self.assertEqual(target.read_bytes(), b'corrupt payload')
                if mode == 'symlink':
                    self.assertTrue(target.is_symlink())
                    target.unlink()
                    other.rename(target)
                elif mode == 'hardlink':
                    self.assertEqual(other.read_bytes(), b'corrupt payload')
                    other.unlink()
                else:
                    self.assertEqual(target.stat().st_mode & 0o777, 0o666)
                    target.chmod(original_mode)

    def test_service_repairs_primary_payload_without_init_or_restart(self):
        self.managed(service=True)
        self.await_service_query('return 7;')
        target = self.f.out / 'docs/kb/generated/documents.jsonl'
        original = target.read_bytes()
        target.write_bytes(b'corrupt payload')
        self.await_service_query('return 7;')
        self.assertEqual(target.read_bytes(), original)
        state = self.ce('status', '--service')['watch']['state']
        self.assertEqual(state['recoveries'], 1)
        self.assertEqual(state['last_recovery'], 'primary-index')

    def test_primary_repair_rechecks_damaged_payload_at_commit_fence(self):
        case = self.profile['evalCases'][0]
        self.profile['evalCases'] = [{**case, 'id': f'limit-{i}'} for i in range(128)]
        self.build()
        self.managed()
        target = self.f.out / 'docs/kb/generated/documents.jsonl'
        target.write_bytes(b'first corruption')
        grant = self.grant_path().read_bytes()
        candidate = self.workspace() / 'refresh/candidate/docs/kb/generated/manifest.json'
        process = subprocess.Popen([str(CE), 'watch', '--samples', '2', '--root', str(self.f.root),
            '--home', str(self.home)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline and not candidate.exists():
                self.assertIsNone(process.poll())
                time.sleep(.001)
            self.assertTrue(candidate.exists())
            with (self.f.out / 'docs/kb/.shin-context-writer.lock').open('a+b') as writer:
                fcntl.flock(writer, fcntl.LOCK_EX)
                self.assertTrue(candidate.exists(), 'repair completed before acquiring the commit fence')
                target.write_bytes(b'second corruption')
                fcntl.flock(writer, fcntl.LOCK_UN)
            output, error = process.communicate(timeout=20)
            self.assertEqual(process.returncode, 0, output + error)
            state = json.loads(output)['watch']
            self.assertEqual(state['publications'], 0)
            self.assertEqual(state['last_result']['error']['code'], 'refresh-base-changed')
            self.assertEqual(target.read_bytes(), b'second corruption')
            self.assertEqual(self.grant_path().read_bytes(), grant)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate(timeout=5)

    def test_managed_rejects_other_home_manual_build_and_foreign_publication(self):
        self.managed()
        self.ce('init', '--observer-config', str(self.f.config), '--manage-index',
                home=self.base / 'other-home', expected=2)
        owner = self.owner_path().read_bytes()
        denied = subprocess.run([str(CORE), '--profile', 'observer-fixture', '--profile-dir', str(self.f.profiles),
                                 '--root', str(self.f.root), '--out-dir', str(self.f.out), 'build'], capture_output=True)
        self.assertNotEqual(denied.returncode, 0)
        # Model a legacy/non-cooperating writer, not a supported user operation.
        self.owner_path().unlink()
        (self.f.root / 'src/limits.js').write_text('export function readLimit() { return 42; }\n')
        self.build()
        self.owner_path().write_bytes(owner)
        self.owner_path().chmod(0o600)
        failed = self.ce('watch', '--samples', '1', expected=2)
        self.assertEqual(failed['error']['code'], 'refresh-base-changed')
        self.ce('refresh-release')
        self.assertFalse(self.owner_path().exists())

    def test_managed_receipt_retry_and_release_after_policy_change(self):
        self.managed()
        record = json.loads(self.grant_path().read_text())
        self.owner_path().unlink()
        record['phase'] = 'granting'
        write_private_json(self.grant_path(), record)
        self.assertEqual(self.ce('status', expected=2)['error']['code'], 'refresh-grant-incomplete')
        self.assertEqual(self.managed()['publication'], 'managed')
        record['phase'] = 'active'
        record['pending_snapshot'] = record['last_snapshot']
        write_private_json(self.grant_path(), record)
        self.ce('watch', '--samples', '1')
        self.assertEqual(json.loads(self.grant_path().read_text())['pending_snapshot'], '')
        self.profile['sourceLabel'] = 'Changed policy'
        self.f.profile_path.write_text(json.dumps(self.profile))
        self.assertEqual(self.ce('status', expected=2)['error']['code'], 'route-revision-conflict')
        self.ce('refresh-release')
        self.assertFalse(self.owner_path().exists())

    def test_managed_pack_failure_preserves_generation(self):
        self.profile['packEvalCases'][0]['request']['task']['must'][0]['query'] = 'dedicated pack evidence'
        original = (self.f.root / 'README.md').read_text()
        (self.f.root / 'README.md').write_text(original + '\nDedicated pack evidence.\n')
        self.build()
        self.managed()
        active = (self.f.out / 'docs/kb/generated/manifest.json').read_bytes()
        (self.f.root / 'README.md').write_text(original)
        failed = self.ce('watch', '--samples', '2')
        self.assertFalse(failed['watch']['last_result']['ok'])
        self.assertEqual(failed['watch']['last_result']['error']['code'], 'refresh-validation-failed')
        self.assertEqual(active, (self.f.out / 'docs/kb/generated/manifest.json').read_bytes())
        self.assertFalse(self.ce('query', 'readLimit', expected=1).get('source_context'))

    def test_managed_marker_destination_and_incomplete_source(self):
        self.marker()
        self.managed()
        (self.f.root / 'src/limits.js').write_text('export function readLimit() { return 53; }\n')
        self.ce('watch', '--samples', '2')
        self.assertIn('return 53;', self.ce('query', 'readLimit')['source_context'])
        active = (self.f.out / 'docs/kb/generated/manifest.json').read_bytes()
        (self.f.root / 'src/large.js').write_text(' ' * (1024 * 1024 + 1))
        failed = self.ce('watch', '--samples', '2')
        retained = failed['watch']['last_result']
        self.assertTrue(retained['ok'])
        self.assertEqual(retained['phase'], 'partial')
        self.assertFalse(retained['canonical_ready'])
        self.assertEqual(retained['source_coverage']['source_counts']['skipped_files'], 1)
        self.assertEqual(failed['watch']['publications'], 0)
        self.assertEqual(active, (self.f.out / 'docs/kb/generated/manifest.json').read_bytes())
        snapshot = self.ce('query', 'readLimit')['snapshot_id']
        self.assertIn('return 53;', self.ce('read', 'src/limits.js', '--snapshot', snapshot)['source_context'])
        self.assertFalse(self.ce('read', 'src/large.js', '--snapshot', snapshot)['native_read_complete'])

    def test_managed_live_release_and_interrupted_release(self):
        self.managed(service=True)
        deadline = time.monotonic() + 10
        while True:
            run = subprocess.run([str(CE), 'refresh-release', '--root', str(self.f.root),
                                  '--home', str(self.home)], capture_output=True, text=True, timeout=30)
            if run.returncode == 0:
                break
            self.assertEqual(json.loads(run.stderr)['error']['code'], 'writer-busy')
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.02)
        self.assertFalse(self.owner_path().exists())
        active = (self.f.out / 'docs/kb/generated/manifest.json').read_bytes()
        (self.f.root / 'src/limits.js').write_text('export function readLimit() { return 61; }\n')
        time.sleep(.3)
        self.stop_service()
        self.ce('watch', '--samples', '2')
        self.assertEqual(active, (self.f.out / 'docs/kb/generated/manifest.json').read_bytes())
        record = json.loads(self.grant_path().read_text())
        record['phase'] = 'revoking'
        write_private_json(self.grant_path(), record)
        self.assertEqual(self.ce('status', expected=2)['error']['code'], 'service-unavailable')
        self.assertEqual(self.ce('watch', '--samples', '1', expected=2)['error']['code'], 'refresh-grant-incomplete')
        self.ce('refresh-release')
        self.assertEqual(json.loads(self.grant_path().read_text())['phase'], 'revoked')

    def test_interrupted_candidate_is_reclaimed_on_service_restart(self):
        self.interrupted_candidate_recovery(primary_repair=False)

    def test_interrupted_primary_repair_resumes_on_service_restart(self):
        self.interrupted_candidate_recovery(primary_repair=True)

    def interrupted_candidate_recovery(self, primary_repair):
        # Keep candidate validation active long enough to take its commit fence
        # after the preliminary ownership reconciliation has released that fence.
        case = self.profile['evalCases'][0]
        self.profile['evalCases'] = [{**case, 'id': f'limit-{i}', 'queryVariants': ['retry ceiling readLimit']} for i in range(128)]
        self.build()
        self.managed(service=True)
        manifest = self.f.out / 'docs/kb/generated/manifest.json'
        accepted = manifest.read_bytes()
        staging = self.workspace() / 'refresh'
        candidate = staging / 'candidate'
        if primary_repair:
            (self.f.out / 'docs/kb/generated/documents.jsonl').write_bytes(b'corrupt payload')
        (self.f.root / 'src/limits.js').write_text('export function readLimit() { return 88; }\n')
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if (candidate / 'docs/kb/generated/manifest.json').exists():
                break
            time.sleep(.001)
        else:
            self.fail('candidate was not prepared')
        with (self.f.out / 'docs/kb/.shin-context-writer.lock').open('a+b') as writer:
            fcntl.flock(writer, fcntl.LOCK_EX)
            self.assertTrue(candidate.exists(), 'candidate completed before the test acquired its fence')
            self.stop_service()
            self.assertTrue(candidate.exists(), 'interrupted candidate was not retained for recovery')
            self.assertEqual(manifest.read_bytes(), accepted)
            self.assertEqual(json.loads((staging / 'state.json').read_text())['phase'], 'prepared')
            fcntl.flock(writer, fcntl.LOCK_UN)
        self.ce('service-start')
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            run = subprocess.run([str(CE), 'query', 'readLimit', '--service', '--root', str(self.f.root),
                                  '--home', str(self.home)], capture_output=True, text=True, timeout=30)
            value = json.loads(run.stdout or run.stderr)
            if run.returncode == 0:
                self.assertIn('return 88;', value['source_context'])
                break
            self.assertFalse(value.get('source_context'))
            time.sleep(.05)
        else:
            self.fail('candidate recovery did not publish the current source: ' + str(value))
        self.assertFalse(candidate.exists())
        self.assertEqual(json.loads((staging / 'state.json').read_text())['phase'], 'idle')
        self.assertFalse(list(self.workspace().glob('refresh-*')))

    def test_unowned_candidate_is_not_removed(self):
        self.managed()
        staging = self.workspace() / 'refresh'
        candidate = staging / 'candidate'
        candidate.mkdir(parents=True, mode=0o700)
        staging.chmod(0o700)
        (candidate / 'foreign.txt').write_text('must remain')
        failed = self.ce('watch', '--samples', '1', expected=2)
        self.assertEqual(failed['error']['code'], 'refresh-storage-unowned')
        self.assertEqual((candidate / 'foreign.txt').read_text(), 'must remain')

    def test_attach_and_query_leave_canonical_files_unchanged(self):
        before = self.canonical_files()
        first = self.attach()
        self.assertTrue(first["canonical_ready"])
        self.assertTrue(first["local_ready"])
        self.assertEqual(first["index"], str(self.f.out))
        self.assertEqual(first["profile"], "observer-fixture")
        self.assertEqual(first["binding_kind"], "existing-route")
        self.assertFalse(first["rebuilt"])
        state = (self.workspace() / "state.json").read_bytes()
        self.attach()
        self.assertEqual(state, (self.workspace() / "state.json").read_bytes())
        self.assertTrue(self.ce("status")["canonical_ready"])
        result = self.ce("query", "readLimit retry ceiling")
        self.assertIn("return 7", result["source_context"])
        self.assertIn("README.md", json.dumps(result["document_context"]))
        self.assertFalse(result["observer_state_mutated"])
        self.assertEqual(before, self.canonical_files())
        self.assertTrue((self.workspace() / "source-cache").exists())
        self.assertFalse((self.workspace() / "index").exists())
        self.assertFalse((self.f.state / "source-cache").exists())

    def test_repeat_attach_detects_saved_configuration(self):
        self.attach()
        before = self.canonical_files()
        receipt = (self.workspace() / "state.json").read_bytes()
        self.assertTrue(self.ce("attach")["local_ready"])
        self.assertEqual(receipt, (self.workspace() / "state.json").read_bytes())
        self.assertEqual(before, self.canonical_files())

    def test_init_connects_existing_route_once_then_reuses_it_without_service(self):
        before = self.canonical_files()
        first = self.ce('init', '--observer-config', str(self.f.config))
        self.assertEqual(first['command'], 'init')
        self.assertEqual(first['binding_kind'], 'existing-route')
        self.assertTrue(first['canonical_ready'])
        receipt = (self.workspace() / 'state.json').read_bytes()
        for result in [first, self.ce('init')]:
            self.assertTrue(result['local_ready'])
            self.assertEqual(result['refresh_management'], 'external')
            self.assertFalse(result['service_enrolled'])
            self.assertFalse(result['daemon_started'])
            self.assertFalse(result['index_mutated'])
        self.assertFalse((self.home / 'registry').exists())
        self.assertFalse((self.home / 'service').exists())
        self.assertEqual(receipt, (self.workspace() / 'state.json').read_bytes())
        self.assertEqual(before, self.canonical_files())
        self.assertIn('return 7', self.ce('query', 'readLimit')['source_context'])

    def test_init_reuses_legacy_attachment_and_cannot_override_profile(self):
        self.attach()
        before = self.canonical_files()
        self.assertTrue(self.ce('init', one_shot=False)['canonical_ready'])
        result = self.ce('init', '--profile-dir', str(self.f.profiles), expected=2)
        self.assertEqual(result['error']['code'], 'invalid-arguments')
        self.assertEqual(before, self.canonical_files())

    def test_init_config_cannot_convert_bootstrap_or_switch_saved_config(self):
        self.ce('init')
        state = (self.workspace() / 'state.json').read_bytes()
        result = self.ce('init', '--observer-config', str(self.f.config), expected=2)
        self.assertEqual(result['error']['code'], 'binding-mode-conflict')
        self.assertEqual(state, (self.workspace() / 'state.json').read_bytes())
        other_home = self.base / 'attached-home'
        self.ce('init', '--observer-config', str(self.f.config), home=other_home)
        other = self.base / 'other-config.json'
        write_private_json(other, self.f.config_data())
        result = self.ce('init', '--observer-config', str(other), home=other_home, expected=2)
        self.assertEqual(result['error']['code'], 'route-config-conflict')

    def test_init_does_not_bypass_existing_quality_gate(self):
        self.profile['evalCases'][0]['query'] = 'unfindable_quality_requirement'
        self.build()
        before = self.canonical_files()
        result = self.ce('init', '--observer-config', str(self.f.config), expected=1, one_shot=False)
        self.assertFalse(result['canonical_ready'])
        self.assertFalse((self.workspace() / 'state.json').exists())
        self.assertFalse((self.home / 'service').exists())
        self.assertEqual(before, self.canonical_files())

    def await_service_query(self, text):
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            run = subprocess.run([str(CE), 'query', 'readLimit', '--service', '--root', str(self.f.root),
                                  '--home', str(self.home)], capture_output=True, text=True, timeout=40)
            result = json.loads(run.stdout or run.stderr)
            if result.get('ok') and text in result.get('source_context', ''):
                return result
            self.assertNotIn('source_context', result)
            time.sleep(0.1)
        self.fail('canonical service did not prepare the published snapshot')

    def test_service_adopts_new_publication_without_init_or_canonical_writes(self):
        before = self.canonical_files()
        first = self.ce('init', '--observer-config', str(self.f.config), one_shot=False)
        self.assertTrue(first['service_enrolled'])
        self.assertTrue(first['daemon_started'])
        self.assertEqual(first['refresh_management'], 'external')
        original = self.await_service_query('return 7')
        self.assertTrue(self.ce('status', '--service')['watch']['running'])
        self.assertEqual(before, self.canonical_files())
        (self.f.root / 'src/limits.js').write_text('export function readLimit() { return 29; }\n')
        # An external publisher has not rebuilt yet. Wait for the completed
        # freshness check instead of racing its intermediate project-updating state.
        blocked = None
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            projects = self.ce('service-status')['telemetry']['project_states']
            blocked = next((p for p in projects if p['root'] == str(self.f.root) and p['active_issue']), None)
            if blocked:
                break
            time.sleep(.05)
        self.assertIsNotNone(blocked, 'stale external publication was not reported')
        stale = self.ce('query', 'readLimit', '--service', expected=2)
        self.assertEqual(stale['error']['code'], 'project-check-failed')
        gates = stale['error']['diagnostic']['gates']
        self.assertTrue(any(gate['id'] == 'worktree-diff' and
                            'worktree-not-current' in gate.get('reasons', []) for gate in gates), stale)
        self.assertNotIn('source_context', stale)
        self.build()
        published = self.canonical_files()
        current = self.await_service_query('return 29')
        self.assertNotEqual(current['snapshot_id'], original['snapshot_id'])
        self.assertEqual(published, self.canonical_files())
        self.assertEqual(self.ce('read', 'src/limits.js', '--snapshot', original['snapshot_id'],
                                 '--service', expected=2)['error']['code'], 'source-snapshot-mismatch')
        self.assertEqual(self.ce('service-status')['instance_id'], first['service']['instance_id'])
        self.assertEqual(self.ce('status', '--service')['watch']['state']['publications'], 0)

    def test_service_quality_failure_isolated_from_local_project_and_recovers(self):
        self.ce('init', '--observer-config', str(self.f.config), one_shot=False)
        other = self.base / 'local-project'
        other.mkdir()
        (other / 'README.md').write_text('# Local\n')
        (other / 'limits.js').write_text('export function readLimit() { return 99; }\n')
        self.ce('init', root=other, one_shot=False)
        (self.f.root / 'README.md').write_text('# Missing requirement\n')
        self.build()
        bad = self.canonical_files()
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            projects = self.ce('service-status')['telemetry']['project_states']
            failed = next((p for p in projects if p['root'] == str(self.f.root) and p['active_issue']), None)
            if failed:
                break
            time.sleep(.05)
        self.assertIsNotNone(failed, 'stable quality failure was not reported')
        refused = self.ce('query', 'readLimit', '--service', expected=2)
        self.assertEqual(refused['error']['code'], 'project-check-failed')
        self.assertNotIn('source_context', refused)
        self.assertIn('return 99', self.ce('query', 'readLimit', '--service', root=other)['source_context'])
        self.assertEqual(bad, self.canonical_files())
        self.f.write_source('# Route Contract\n\nThe retry ceiling is owned by readLimit.\n')
        self.build()
        restored = self.canonical_files()
        self.await_service_query('return 7')
        self.assertEqual(restored, self.canonical_files())

    def test_service_restart_keeps_external_policy_and_rejects_revision_changes(self):
        self.ce('init', '--observer-config', str(self.f.config), one_shot=False)
        self.stop_service()
        self.ce('service-start')
        self.await_service_query('return 7')
        self.f.config.write_text(self.f.config.read_text() + '\n')
        changed = self.canonical_files()
        result = self.ce('query', 'readLimit', '--service', expected=2)
        self.assertEqual(result['error']['code'], 'route-revision-conflict')
        self.assertEqual(changed, self.canonical_files())

    def test_bound_followup_preserves_canonical_and_private_state(self):
        self.attach()
        snapshot = self.ce("query", "readLimit")["snapshot_id"]
        before = self.canonical_files()
        private_before = {str(p): p.read_bytes() for p in self.home.rglob("*") if p.is_file()}
        inventory = self.ce("inspect", "src/limits.js", "--snapshot", snapshot)["source_inventory"]
        record = next(row["id"] for row in inventory["records"] if row["name"] == "readLimit")
        for selection in (["src/limits.js"], ["--record", record]):
            result = self.ce("read", *selection, "--snapshot", snapshot)
            self.assertTrue(result["canonical_ready"])
            self.assertIn("return 7", result["source_context"])
        self.assertEqual(before, self.canonical_files())
        self.assertEqual(private_before, {str(p): p.read_bytes() for p in self.home.rglob("*") if p.is_file()})

    def test_followup_cannot_bypass_canonical_quality(self):
        self.attach()
        snapshot = self.ce("query", "readLimit")["snapshot_id"]
        (self.f.root / "README.md").write_text("# Removed the evaluated requirement\n")
        self.build()
        for command in ["read", "inspect"]:
            result = self.ce(command, "src/limits.js", "--snapshot", snapshot, expected=1)
            self.assertFalse(result["canonical_ready"])
            self.assertNotIn("source_context", result)
            self.assertNotIn("source_inventory", result)

    def test_first_attach_does_not_guess_configuration(self):
        self.assertEqual(self.ce("attach", expected=2)["error"]["code"], "invalid-arguments")
        self.assertFalse(self.home.exists())

    def test_concurrent_init_and_attach_commit_only_one_mode(self):
        before = self.canonical_files()
        def run(command):
            return subprocess.run([str(CE), *command, "--root", str(self.f.root), "--home", str(self.home)],
                                  capture_output=True, text=True, timeout=45,
                                  env={**os.environ, "PATH": "", "LC_ALL": "C"})
        with ThreadPoolExecutor(max_workers=2) as pool:
            results = list(pool.map(run, [["init", "--no-service"], ["attach", "--observer-config", str(self.f.config)]]))
        self.assertIn(sorted(item.returncode for item in results), [[0, 2], [0, 0]],
                         [(item.returncode, item.stdout, item.stderr) for item in results])
        for item in results:
            if item.returncode == 2:
                self.assertIn(json.loads(item.stderr)["error"]["code"],
                              ["writer-busy", "binding-changed", "binding-mode-conflict"])
        if all(item.returncode == 0 for item in results):
            self.assertTrue(all(json.loads(item.stdout)['binding_kind'] == 'existing-route' for item in results))
        self.assertTrue(self.ce("status")["local_ready"])
        self.assertEqual(before, self.canonical_files())

    def test_initial_writer_owns_incomplete_binding_until_unlock(self):
        self.home.mkdir(mode=0o700)
        self.workspace().parent.mkdir(mode=0o700)
        self.workspace().mkdir(mode=0o700)
        lock_path = self.workspace() / "observer.lock"
        lock_path.touch(mode=0o600)
        before = self.canonical_files()
        commands = [("init", []), ("attach", ["--observer-config", str(self.f.config)])]
        for name, abandoned_code in [("state.pending", "binding-uncommitted"),
                                     ("partial-index", "unbound-project-state")]:
            with self.subTest(state=name):
                pending = self.workspace() / name
                pending.write_bytes(b"incomplete first publication")
                pending.chmod(0o600)
                private_before = {str(p): p.read_bytes() for p in self.home.rglob("*") if p.is_file()}
                try:
                    with lock_path.open("rb") as lock:
                        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                        for command, args in commands:
                            self.assertEqual(self.ce(command, *args, expected=2)["error"]["code"], "writer-busy")
                    for command, args in commands:
                        self.assertEqual(self.ce(command, *args, expected=2)["error"]["code"], abandoned_code)
                    self.assertEqual(private_before,
                                     {str(p): p.read_bytes() for p in self.home.rglob("*") if p.is_file()})
                    self.assertEqual(before, self.canonical_files())
                finally:
                    pending.unlink()

    def test_mode_detection_precedes_bundle_discovery(self):
        self.attach()
        isolated = self.base / "isolated-ce"
        shutil.copy2(CE, isolated)
        before = self.canonical_files()
        result = self.invoke(isolated, "init", "--root", str(self.f.root), "--home", str(self.home))
        self.assertTrue(result['canonical_ready'])
        self.assertEqual(before, self.canonical_files())

    def test_attach_cannot_bypass_parent_cli_binding(self):
        parent = self.f.root.parent
        self.ce("init", root=parent)
        before = self.canonical_files()
        result = self.attach(expected=2)
        self.assertEqual(result["error"]["code"], "ancestor-project-binding")
        self.assertFalse(self.workspace().exists())
        self.assertEqual(before, self.canonical_files())

    def test_missing_receipt_does_not_adopt_or_replace_orphaned_cache(self):
        self.attach()
        (self.workspace() / "state.json").unlink()
        before = {str(p): p.read_bytes() for p in self.home.rglob("*") if p.is_file()}
        for command, args in [("init", []), ("attach", ["--observer-config", str(self.f.config)])]:
            self.assertEqual(self.ce(command, *args, expected=2)["error"]["code"], "unbound-project-state")
        self.assertEqual(before, {str(p): p.read_bytes() for p in self.home.rglob("*") if p.is_file()})

    def test_refresh_remains_canonical_and_requires_new_preparation(self):
        self.attach()
        (self.f.root / "src/limits.js").write_text("export function readLimit() { return 9; }\n")
        before = self.canonical_files()
        self.assertFalse(self.ce("status", expected=1)["canonical_ready"])
        self.assertNotIn("source_context", self.ce("query", "readLimit", expected=1))
        self.assertFalse(self.ce("init", expected=1, one_shot=False)['canonical_ready'])
        self.assertFalse(self.attach(expected=1)["canonical_ready"])
        self.assertEqual(before, self.canonical_files())
        self.build()
        result = self.ce("status", expected=1)
        self.assertTrue(result["canonical_ready"])
        self.assertEqual(result["state"], "needs-init")
        self.assertEqual(result['next_action'], 'shin-ce init')
        self.ce('init')
        self.assertIn("return 9", self.ce("query", "readLimit")["source_context"])

    def test_missing_observer_sample_is_not_created_by_attach(self):
        (self.f.state / "state.json").unlink()
        before = self.canonical_files()
        result = self.attach(expected=1)
        self.assertTrue(result["canonical_ready"])
        self.assertEqual(result["state"], "needs-observer-sample")
        self.assertEqual(before, self.canonical_files())
        self.assertFalse((self.workspace() / "state.json").exists())

    def test_eval_failure_is_not_bootstrapped_away(self):
        self.profile["evalCases"][0]["query"] = "unfindable_quality_requirement"
        self.build()
        result = self.attach(expected=1)
        self.assertFalse(result["canonical_ready"])
        self.assertFalse((self.workspace() / "state.json").exists())

    def test_pack_failure_is_not_bootstrapped_away(self):
        self.profile["packEvalCases"][0]["request"]["task"]["must"][0]["query"] = "unfindable_pack_requirement"
        self.build()
        self.assertFalse(self.attach(expected=1)["canonical_ready"])

    def test_automatic_marker_and_index_key_alignment(self):
        path, value = self.marker()
        self.assertEqual(self.attach()["marker"], path.relative_to(self.f.root).as_posix())
        value["index"]["key"] = "another-index"
        path.write_text(json.dumps(value))
        self.build()
        fresh_home = self.base / "other-cli"
        result = self.ce("attach", "--observer-config", str(self.f.config), home=fresh_home, expected=1)
        self.assertFalse(result["canonical_ready"])
        self.assertIn("adoption", json.dumps(result["readiness"]))

    def test_marker_revision_and_new_marker_require_review(self):
        self.attach()
        self.marker()
        self.assertEqual(self.ce("status", expected=2)["error"]["code"], "route-marker-conflict")

    def test_marker_bytes_are_pinned(self):
        path, _ = self.marker()
        self.attach()
        path.write_text(path.read_text() + "\n")
        self.assertEqual(self.attach(expected=2)["error"]["code"], "route-revision-conflict")

    def test_config_and_profile_revisions_are_pinned(self):
        self.attach()
        self.f.profile_path.write_text(self.f.profile_path.read_text() + "\n")
        self.assertEqual(self.ce("status", expected=2)["error"]["code"], "route-revision-conflict")
        self.build()
        self.f.config.write_text(self.f.config.read_text() + "\n")
        self.assertEqual(self.attach(expected=2)["error"]["code"], "route-revision-conflict")
        self.assertEqual(self.ce("attach", expected=2)["error"]["code"], "route-revision-conflict")

    def config_accept(self, expected=0, revision=None):
        revision = revision or hashlib.sha256(self.f.config.read_bytes()).hexdigest()
        return self.ce('config-accept', revision, expected=expected)

    def test_snapshot_budget_preflight_does_not_bind_or_claim_failed_init(self):
        original = self.f.config.read_bytes()
        config = json.loads(original)
        config.update(max_snapshot_bytes=1024, max_file_bytes=1024)
        write_private_json(self.f.config, config)
        failed = self.ce('init', '--observer-config', str(self.f.config), '--manage-index', expected=2)
        self.assertEqual(failed['error']['code'], 'snapshot-memory-budget-exceeded')
        self.assertFalse((self.workspace() / 'state.json').exists())
        self.assertFalse((self.workspace() / 'publication').exists())
        self.f.config.write_bytes(original)
        self.assertTrue(self.managed()['local_ready'])

    def test_config_approval_requires_release_and_preserves_route_and_index(self):
        self.marker()
        self.managed()
        binding = json.loads((self.workspace() / 'state.json').read_text())
        manifest = (self.f.out / 'docs/kb/generated/manifest.json').read_bytes()
        config = self.f.config_data()
        config['max_snapshot_bytes'] *= 2
        write_private_json(self.f.config, config)
        self.assertEqual(self.config_accept(expected=2)['error']['code'], 'config-requires-release')
        self.ce('refresh-release')
        approved = self.config_accept()
        self.assertTrue(approved['changed'])
        self.assertFalse(approved['local_ready'])
        self.assertFalse(approved['index_mutated'])
        self.assertTrue(approved['data_preserved'])
        revised = json.loads((self.workspace() / 'state.json').read_text())
        self.assertEqual(revised, binding)
        self.assertEqual((self.f.out / 'docs/kb/generated/manifest.json').read_bytes(), manifest)
        self.assertFalse(self.config_accept()['changed'])
        self.assertTrue(self.managed()['local_ready'])
        self.assertTrue(self.ce('query', 'readLimit')['local_ready'])

    def test_config_approval_is_part_of_resident_authority(self):
        self.marker()
        self.managed()
        self.ce('refresh-release')
        config = self.f.config_data()
        config['max_snapshot_bytes'] *= 2
        write_private_json(self.f.config, config)
        self.config_accept()
        self.managed(service=True)
        self.assertTrue(self.ce('query', 'readLimit')['local_ready'])
        approval = self.workspace() / 'config-policy/state.json'
        saved = approval.read_bytes()
        record = json.loads(saved)
        record['approved_config_hash'] = '0' * 64
        write_private_json(approval, record)
        refused = self.ce('query', 'readLimit', expected=2)
        self.assertEqual(refused['error']['code'], 'route-revision-conflict')
        approval.write_bytes(saved)

    def test_config_approval_pins_exact_revision(self):
        self.attach()
        before = (self.workspace() / 'state.json').read_bytes()
        self.assertEqual(self.config_accept(expected=2, revision='invalid')['error']['code'], 'invalid-config-revision')
        self.assertEqual(self.config_accept(expected=2, revision='0' * 64)['error']['code'], 'config-revision-changed')
        self.assertEqual((self.workspace() / 'state.json').read_bytes(), before)

    def test_config_approval_cannot_change_outbox_authority_or_profile(self):
        self.attach()
        original = self.f.config.read_bytes()
        before = (self.workspace() / 'state.json').read_bytes()
        config = json.loads(original)
        config['outbox']['enabled'] = not config['outbox']['enabled']
        write_private_json(self.f.config, config)
        self.assertEqual(self.config_accept(expected=2)['error']['code'], 'route-revision-conflict')
        self.f.config.write_bytes(original)
        self.f.profile_path.write_text(self.f.profile_path.read_text() + '\n')
        self.assertEqual(self.config_accept(expected=2)['error']['code'], 'route-revision-conflict')
        self.assertEqual((self.workspace() / 'state.json').read_bytes(), before)

    def test_config_approval_cannot_change_marker(self):
        marker, _ = self.marker()
        self.attach()
        before = (self.workspace() / 'state.json').read_bytes()
        marker.write_text(marker.read_text() + '\n')
        self.assertEqual(self.config_accept(expected=2)['error']['code'], 'route-revision-conflict')
        self.assertEqual((self.workspace() / 'state.json').read_bytes(), before)

    def test_config_approval_still_enforces_reduced_load_bounds(self):
        self.managed()
        self.ce('refresh-release')
        config = self.f.config_data()
        config.update(max_snapshot_bytes=1024, max_file_bytes=1024)
        write_private_json(self.f.config, config)
        self.assertTrue(self.config_accept()['changed'])
        self.assertEqual(self.ce('init', '--manage-index', expected=2)['error']['code'], 'snapshot-memory-budget-exceeded')
        self.assertEqual(json.loads((self.workspace() / 'publication/state.json').read_text())['phase'], 'revoked')

    def test_config_approval_requires_an_existing_route(self):
        self.assertEqual(self.config_accept(expected=2)['error']['code'], 'binding-unavailable')

    def test_config_approval_resumes_empty_or_pending_first_setup(self):
        self.attach()
        config = self.f.config_data()
        config['max_snapshot_bytes'] *= 2
        write_private_json(self.f.config, config)
        directory = self.workspace() / 'config-policy'
        directory.mkdir(mode=0o700)
        self.assertTrue(self.config_accept()['changed'])
        receipt = directory / 'state.json'
        receipt.rename(directory / 'state.pending')
        self.assertEqual(self.ce('status', expected=2)['error']['code'], 'config-identity-conflict')
        self.assertTrue(self.config_accept()['changed'])
        self.assertFalse((directory / 'state.pending').exists())
        self.assertFalse(self.config_accept()['changed'])

    def test_config_approval_preserves_unknown_or_malformed_setup(self):
        self.attach()
        config = self.f.config_data()
        config['max_snapshot_bytes'] *= 2
        write_private_json(self.f.config, config)
        directory = self.workspace() / 'config-policy'
        directory.mkdir(mode=0o700)
        foreign = directory / 'foreign'
        foreign.write_text('preserve')
        self.assertEqual(self.config_accept(expected=2)['error']['code'], 'config-identity-conflict')
        self.assertEqual(foreign.read_text(), 'preserve')
        foreign.unlink()
        receipt = directory / 'state.json'
        write_private_json(receipt, {'wrong': True})
        before = receipt.read_bytes()
        self.assertEqual(self.config_accept(expected=2)['error']['code'], 'config-identity-conflict')
        self.assertEqual(receipt.read_bytes(), before)

    def test_root_mismatch_and_storage_overlap_precede_writes(self):
        other = self.base / "other-source"
        other.mkdir()
        result = self.ce("attach", "--observer-config", str(self.f.config), root=other, expected=2)
        self.assertEqual(result["error"]["code"], "route-root-mismatch")
        self.assertFalse(self.home.exists())
        for directory in [self.f.state / "cli", self.f.out / "cli", self.f.profiles / "cli"]:
            with self.subTest(directory=directory):
                self.assertEqual(self.ce("attach", "--observer-config", str(self.f.config), home=directory, expected=2)["error"]["code"], "unsafe-home")
                self.assertFalse(directory.exists())

    def test_config_symlink_is_not_followed(self):
        link = self.base / "config-link.json"
        link.symlink_to(self.f.config)
        self.assertFalse(self.ce("attach", "--observer-config", str(link), expected=2)["ok"])
        self.assertFalse(self.home.exists())

    def test_config_switch_and_observer_state_replacement_refused(self):
        self.attach()
        other = self.base / "other-config.json"
        write_private_json(other, self.f.config_data())
        self.assertEqual(self.ce("attach", "--observer-config", str(other), expected=2)["error"]["code"], "route-config-conflict")
        self.f.state.rename(self.base / "old-state")
        self.f.state.mkdir(mode=0o700)
        self.assertEqual(self.ce("status", expected=2)["error"]["code"], "route-revision-conflict")

    def test_index_and_source_directory_replacement_refused(self):
        self.attach()
        self.f.out.rename(self.base / "old-index")
        self.f.out.mkdir(mode=0o700)
        self.assertEqual(self.ce("status", expected=2)["error"]["code"], "route-revision-conflict")
        self.f.root.rename(self.base / "old-source")
        self.f.root.mkdir(mode=0o700)
        self.assertEqual(self.ce("status", expected=2)["error"]["code"], "binding-identity-changed")

    def test_observer_delivery_policy_not_changed_or_executed(self):
        config = self.f.config_data()
        config["outbox"]["enabled"] = True
        write_private_json(self.f.config, config)
        (self.f.state / "state.json").unlink()
        self.invoke(OBSERVER, "sample", "--config", str(self.f.config))
        before = self.canonical_files()
        result = self.attach()
        self.assertFalse(result["delivery_performed"])
        self.assertEqual(result["synchronization"], "not-assessed")
        self.ce("query", "readLimit")
        self.assertEqual(before, self.canonical_files())

    def test_initializer_lock_blocks_status_and_attach(self):
        self.attach()
        with (self.workspace() / "observer.lock").open("rb") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.assertEqual(self.ce("status", expected=2)["error"]["code"], "project-busy")
            self.assertEqual(self.attach(expected=2)["error"]["code"], "writer-busy")

    def test_managed_current_read_during_workspace_refresh_preserves_freshness(self):
        self.managed()
        before = self.canonical_files()
        with (self.workspace() / "observer.lock").open("rb") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            result = self.ce("query", "readLimit")
            self.assertTrue(result["canonical_ready"])
            self.assertIn("return 7", result["source_context"])
            self.assertEqual(before, self.canonical_files())
            source = self.f.root / "src/limits.js"
            source.write_text("export function readLimit() { return 89; }\n")
            changed = self.canonical_files()
            stale = self.ce("query", "readLimit", expected=1)
            self.assertFalse(stale["canonical_ready"])
            self.assertNotIn("source_context", stale)
            self.assertEqual(changed, self.canonical_files())

    def test_publication_writer_blocks_context(self):
        self.attach()
        with (self.f.out / "docs/kb/.shin-context.lock").open("rb") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with subprocess.Popen([str(CE), "query", "readLimit", "--root", str(self.f.root),
                                   "--home", str(self.home)], stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True,
                                  env={**os.environ, "PATH": "", "LC_ALL": "C"}) as process:
                try:
                    with self.assertRaises(subprocess.TimeoutExpired):
                        process.communicate(timeout=0.3)
                finally:
                    fcntl.flock(lock, fcntl.LOCK_UN)
                    try:
                        output, error = process.communicate(timeout=45)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.communicate()
                        raise
                self.assertEqual(process.returncode, 0, output + error)
                self.assertFalse(error)
                self.assertTrue(json.loads(output)["canonical_ready"])

    def test_engine_revision_requires_explicit_reattach(self):
        self.attach()
        path = self.workspace() / "state.json"
        binding = json.loads(path.read_text())
        binding["core_version"] = "2.0.0-dev.1"
        write_private_json(path, binding)
        self.assertFalse(self.ce("query", "readLimit", expected=1)["local_ready"])
        self.assertTrue(self.attach()["local_ready"])

    def test_existing_bootstrap_binding_is_not_replaced(self):
        self.ce("init")
        path = self.workspace() / "state.json"
        before = path.read_bytes()
        self.assertEqual(self.attach(expected=2)["error"]["code"], "binding-mode-conflict")
        self.assertEqual(self.ce("attach", expected=2)["error"]["code"], "binding-mode-conflict")
        self.assertEqual(path.read_bytes(), before)

    def test_pending_initial_binding_and_corrupt_binding_are_preserved(self):
        self.attach()
        path = self.workspace() / "state.json"
        value = json.loads(path.read_text())
        del value["observer_config_hash"]
        write_private_json(path, value)
        before = path.read_bytes()
        self.assertFalse(self.ce("status", expected=2)["ok"])
        self.assertEqual(path.read_bytes(), before)
        path.rename(self.workspace() / "state.pending")
        self.assertEqual(self.attach(expected=2)["error"]["code"], "binding-uncommitted")

    def test_bootstrap_profile_cannot_be_attached_as_canonical(self):
        self.profile["bootstrapOnly"] = True
        self.build()
        self.assertEqual(self.attach(expected=2)["error"]["code"], "route-not-permanent")


if __name__ == "__main__":
    unittest.main()
