#!/usr/bin/env python3
"""Read-only manager definitions; live launchd proof is explicitly opt-in."""
import json
import hashlib
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import plistlib
import pwd
import re
import shutil
import signal
import subprocess
import sys
import time
import unittest

import managed_init_test as fixtures


@unittest.skipUnless(sys.platform == 'darwin', 'macOS manager definition')
class SupervisionTests(fixtures.ManagedInitTests):
    # Do not repeat inherited managed-init tests in this suite.
    def definition(self, expected=0, home=None):
        run = subprocess.run([str(fixtures.fixtures.TOOL), 'service-definition', '--home', str(home or self.home)],
                             capture_output=True, text=True, timeout=15, env={**os.environ, 'PATH': ''})
        self.assertEqual(run.returncode, expected, run.stdout + run.stderr)
        return json.loads(run.stdout or run.stderr)

    def test_definition_is_read_only_and_stable(self):
        self.home = self.base / 'home & <xml>'
        self.endpoint = self.home / 'service/rpc.sock'
        self.registry = self.home / 'registry/state.json'
        first = self.run_ce('init')
        registry = self.registry.read_bytes()
        result = self.definition()
        self.assertEqual(result, self.definition())
        unit = plistlib.loads(result['plist'].encode())
        self.assertEqual(unit, result['unit'])
        self.assertEqual(unit['ProgramArguments'], [str(fixtures.fixtures.TOOL), 'serve', '--registered', '--home', str(self.home)])
        self.assertFalse(unit['KeepAlive']['SuccessfulExit'])
        self.assertTrue(unit['RunAtLoad'])
        self.assertEqual(unit['Umask'], 0o77)
        self.assertGreaterEqual(unit['ThrottleInterval'], 10)
        self.assertFalse(result['installation_performed'])
        self.assertEqual(result['manager_state'], 'not-assessed')
        self.assertFalse(result['readiness_assessed'])
        self.assertEqual(registry, self.registry.read_bytes())
        self.assertEqual(first['service']['instance_id'], self.run_ce('service-status')['instance_id'])
        self.assertRegex(result['label'], r'^org\.shin\.context\.[0-9a-f]{32}$')

    def test_missing_registry_and_unsafe_home_refuse(self):
        missing = self.base / 'missing'
        self.assertFalse(self.definition(expected=2, home=missing)['ok'])
        self.assertFalse(missing.exists())
        self.run_ce('init')
        link = self.base / 'alias'
        link.symlink_to(self.home, target_is_directory=True)
        self.assertFalse(self.definition(expected=2, home=link)['ok'])
        self.home.chmod(0o755)
        try:
            self.assertFalse(self.definition(expected=2)['ok'])
        finally:
            self.home.chmod(0o700)

    def test_empty_enrollment_keeps_a_valid_manager_definition(self):
        self.run_ce('init')
        before = self.definition()
        self.run_ce('detach')
        after = self.definition()
        self.assertEqual(after['registered_projects'], 0)
        self.assertEqual(after['unit'], before['unit'])
        self.assertEqual(after['plist'], before['plist'])
        self.assertFalse(after['installation_performed'])

    def test_incomplete_installation_blocks_unmanaged_fallback(self):
        self.run_ce('init')
        self.stop()
        (self.home / 'supervision').mkdir(mode=0o700)
        refused = self.run_ce('service-start', expected=2)
        self.assertEqual(refused['error']['code'], 'service-install-incomplete')
        self.assertFalse(self.endpoint.exists())

    @unittest.skipUnless(os.environ.get('SHIN_TEST_LAUNCHD') == '1', 'explicit live-manager opt-in required')
    def test_persistent_install_start_resume_and_remove(self):
        initial = self.run_ce('init')
        registry = self.registry.read_bytes()
        root_files = {p.name: p.read_bytes() for p in self.root.iterdir()}
        definition = self.definition()
        agent = Path(pwd.getpwuid(os.getuid()).pw_dir) / 'Library/LaunchAgents' / (definition['label'] + '.plist')
        target = f"gui/{os.getuid()}/{definition['label']}"
        self.assertFalse(agent.exists())
        expected_plist = None
        def cleanup():
            if expected_plist is not None and agent.exists() and agent.read_bytes() == expected_plist + b'\n':
                agent.write_bytes(expected_plist)
            if (self.home / 'supervision/state.json').exists():
                self.run_ce('service-uninstall')
            self.assertFalse(agent.exists(), 'test LaunchAgent file leaked')
            self.assertEqual(subprocess.run(['/bin/launchctl', 'print', target], capture_output=True).returncode, 113)
        self.addCleanup(cleanup)
        agent.write_bytes(b'foreign-registration')
        agent.chmod(0o600)
        try:
            collision = self.run_ce('service-install', expected=2)
            self.assertEqual(collision['error']['code'], 'service-install-conflict')
            self.assertEqual(agent.read_bytes(), b'foreign-registration')
            self.assertEqual(self.run_ce('service-status')['instance_id'], initial['service']['instance_id'])
        finally:
            if agent.exists() and agent.read_bytes() == b'foreign-registration':
                agent.unlink()
        installed = self.run_ce('service-install')
        self.assertEqual(installed['phase'], 'installed')
        self.assertTrue(installed['service_running'])
        self.assertNotEqual(installed['instance_id'], initial['service']['instance_id'])
        expected_plist = agent.read_bytes()
        self.assertEqual(agent.stat().st_mode & 0o777, 0o600)
        self.assertIn('_serve-supervised', plistlib.loads(expected_plist)['ProgramArguments'])
        self.assertEqual(self.run_ce('service-installation')['phase'], 'installed')
        self.assertTrue(self.run_ce('service-installation')['kit_current'])
        self.assertEqual(self.run_ce('service-install')['instance_id'], installed['instance_id'])
        with ThreadPoolExecutor(max_workers=2) as pool:
            concurrent = list(pool.map(lambda _: self.run_ce('service-install'), range(2)))
        self.assertEqual({item['instance_id'] for item in concurrent}, {installed['instance_id']})
        repeated = self.run_ce('init')
        self.assertFalse(repeated['daemon_started'])
        self.assertEqual(repeated['service']['supervision'], 'launchd-user')
        self.assertEqual(repeated['service']['instance_id'], installed['instance_id'])
        other = self.project('other-managed', 19)
        enrolled = self.run_ce('init', root=other)
        self.assertEqual(enrolled['service']['instance_id'], installed['instance_id'])
        self.assertEqual(enrolled['service']['supervision'], 'launchd-user')
        self.assertIn('return 19;', self.run_ce('query', 'readLimit', '--service', root=other)['source_context'])
        registry = self.registry.read_bytes()
        self.stop()
        restarted = self.run_ce('service-start')
        self.assertEqual(restarted['supervision'], 'launchd-user')
        self.assertNotEqual(restarted['instance_id'], installed['instance_id'])
        self.assertEqual(set(restarted['projects']), {str(self.root), str(other)})
        self.assertIn('return 7;', self.run_ce('query', 'readLimit', '--service')['source_context'])
        alternate = self.base / 'alternate-ce'
        shutil.copy2(fixtures.fixtures.TOOL, alternate)
        changed = subprocess.run([str(alternate), 'service-start', '--home', str(self.home)], capture_output=True, text=True, timeout=30)
        self.assertEqual(changed.returncode, 2)
        self.assertEqual(json.loads(changed.stderr)['error']['code'], 'service-installed-kit-changed')
        alternate.chmod(0o777)
        unsafe = subprocess.run([str(alternate), 'service-start', '--home', str(self.home)], capture_output=True, text=True, timeout=30)
        self.assertEqual(json.loads(unsafe.stderr)['error']['code'], 'unsafe-executable')
        alternate.chmod(0o755)
        agent.write_bytes(expected_plist + b'\n')
        self.assertEqual(self.run_ce('service-start', expected=2)['error']['code'], 'service-install-changed')
        self.assertEqual(self.run_ce('service-uninstall', expected=2)['error']['code'], 'service-install-changed')
        self.assertEqual(agent.read_bytes(), expected_plist + b'\n')
        agent.write_bytes(expected_plist)
        state_path = self.home / 'supervision/state.json'
        state = json.loads(state_path.read_text())
        invalid = {**state, 'executable_sha256': '0' * 64}
        state_path.write_text(json.dumps(invalid))
        self.assertEqual(self.run_ce('service-start', expected=2)['error']['code'], 'service-installed-kit-changed')
        state['phase'] = 'installing'
        state_path.write_text(json.dumps(state))
        self.assertEqual(self.run_ce('service-start', expected=2)['error']['code'], 'service-install-incomplete')
        self.assertEqual(self.run_ce('service-install')['phase'], 'installed')
        # Simulate an unloaded user job, not a reboot: init must restore OS ownership.
        subprocess.run(['/bin/launchctl', 'bootout', target], capture_output=True, check=True, timeout=15)
        deadline = time.monotonic() + 10
        while self.endpoint.exists() and time.monotonic() < deadline:
            time.sleep(.05)
        restored = self.run_ce('init')
        self.assertEqual(restored['service']['supervision'], 'launchd-user')
        self.assertTrue(restored['daemon_started'])
        state = json.loads(state_path.read_text())
        state['phase'] = 'removing'
        state_path.write_text(json.dumps(state))
        self.assertEqual(self.run_ce('service-start', expected=2)['error']['code'], 'service-install-incomplete')
        history = self.home / 'service/history/telemetry.sqlite'
        self.assertTrue(history.exists())
        history_inode = history.stat().st_ino
        removed = self.run_ce('service-uninstall')
        self.assertEqual(removed['phase'], 'removed')
        self.assertFalse(agent.exists())
        self.assertFalse(self.endpoint.exists())
        self.assertEqual(history.stat().st_ino, history_inode)
        self.assertEqual(self.registry.read_bytes(), registry)
        self.assertEqual({p.name: p.read_bytes() for p in self.root.iterdir()}, root_files)
        self.assertEqual(self.run_ce('service-uninstall')['phase'], 'removed')
        agent.write_bytes(b'foreign-registration')
        agent.chmod(0o600)
        try:
            self.assertEqual(self.run_ce('service-start', expected=2)['error']['code'], 'service-manager-conflict')
            self.assertEqual(self.run_ce('service-uninstall', expected=2)['error']['code'], 'service-manager-conflict')
            self.assertEqual(agent.read_bytes(), b'foreign-registration')
        finally:
            if agent.exists() and agent.read_bytes() == b'foreign-registration':
                agent.unlink()
        self.assertEqual(self.run_ce('service-start')['supervision'], 'unmanaged')

    @unittest.skipUnless(os.environ.get('SHIN_TEST_LAUNCHD') == '1', 'explicit live-manager opt-in required')
    def test_forward_update_without_old_binary_and_pending_recovery(self):
        self.run_ce('init')
        other = self.project('other-update', 19)
        self.run_ce('init', root=other)
        old = self.base / 'old-ce'
        candidate = self.base / 'new-ce'
        for executable in (old, candidate):
            shutil.copy2(fixtures.fixtures.TOOL, executable)

        def invoke(tool, command, expected=0):
            run = subprocess.run([str(tool), command, '--home', str(self.home)],
                                 capture_output=True, text=True, timeout=75,
                                 env={**os.environ, 'PATH': '', 'LC_ALL': 'C'})
            self.assertEqual(run.returncode, expected, run.stdout + run.stderr)
            return json.loads(run.stdout or run.stderr)

        installed = invoke(old, 'service-install')
        state_path = self.home / 'supervision/state.json'
        initial = json.loads(state_path.read_text())
        agent = Path(initial['agent_path'])
        target = f"gui/{os.getuid()}/{initial['label']}"

        def cleanup():
            # Exact isolated test registration only, including deliberately pending receipts.
            subprocess.run(['/bin/launchctl', 'bootout', target], capture_output=True, timeout=20)
            deadline = time.monotonic() + 15
            while self.endpoint.exists() and time.monotonic() < deadline:
                time.sleep(.05)
            self.assertFalse(self.endpoint.exists())
            if agent.exists():
                self.assertEqual(plistlib.loads(agent.read_bytes())['Label'], initial['label'])
                agent.unlink()
            self.assertEqual(subprocess.run(['/bin/launchctl', 'print', target], capture_output=True).returncode, 113)
        self.addCleanup(cleanup)
        registry = self.registry.read_bytes()
        history = self.home / 'service/history/telemetry.sqlite'
        history_inode = history.stat().st_ino
        bindings = {p: p.read_bytes() for p in (self.home / 'projects').glob('*/state.json')}

        candidate.chmod(0o777)
        self.assertEqual(invoke(candidate, 'service-update', 2)['error']['code'], 'unsafe-executable')
        candidate.chmod(0o755)
        agent.write_bytes(initial['definition']['plist'].encode() + b'\n')
        self.assertEqual(invoke(candidate, 'service-update', 2)['error']['code'], 'service-install-changed')
        agent.write_text(initial['definition']['plist'])
        self.assertEqual(self.run_ce('service-status')['instance_id'], installed['instance_id'])
        away = self.base / 'temporarily-unavailable'
        self.root.rename(away)
        try:
            self.assertFalse(invoke(candidate, 'service-update', 2)['ok'])
            self.assertEqual(json.loads(state_path.read_text()), initial)
            self.assertEqual(self.run_ce('service-status')['instance_id'], installed['instance_id'])
        finally:
            away.rename(self.root)
        # The old executable is not required for shutdown, and is not backed up.
        old.unlink()
        result = invoke(candidate, 'service-update')
        self.assertTrue(result['changed'])
        self.assertEqual(result['ready_projects'], 2)
        self.assertFalse(result['backup_created'])
        self.assertFalse(result['rollback_available'])
        self.assertNotEqual(result['instance_id'], installed['instance_id'])
        self.assertFalse(old.exists())
        self.assertEqual(registry, self.registry.read_bytes())
        self.assertEqual(history_inode, history.stat().st_ino)
        self.assertEqual(bindings, {p: p.read_bytes() for p in bindings})
        self.assertIn('return 7;', self.run_ce('query', 'readLimit', '--service')['source_context'])
        self.assertIn('return 19;', self.run_ce('query', 'readLimit', '--service', root=other)['source_context'])
        self.assertFalse(invoke(candidate, 'service-update')['changed'])
        finished = json.loads(state_path.read_text())
        self.assertNotIn('previous_plist', finished)
        self.assertEqual(plistlib.loads(agent.read_bytes())['Program'], str(candidate))

        # Recover each durable boundary, including rename-before-receipt publication.
        for stage, published in [('stopping', False), ('stopping', True), ('starting', True)]:
            pending = {**finished, 'phase': 'updating', 'update_stage': stage,
                       'previous_plist': initial['definition']['plist'],
                       'registry_sha256': hashlib.sha256(registry).hexdigest()}
            subprocess.run(['/bin/launchctl', 'bootout', target], capture_output=True, timeout=20)
            deadline = time.monotonic() + 15
            while self.endpoint.exists() and time.monotonic() < deadline:
                time.sleep(.05)
            self.assertFalse(self.endpoint.exists())
            state_path.write_text(json.dumps(pending))
            agent.write_text((finished if published else initial)['definition']['plist'])
            observed = invoke(candidate, 'service-installation')
            self.assertEqual(observed['update_stage'], stage)
            self.assertEqual(observed['next_action'], 'service-update')
            self.assertEqual(invoke(candidate, 'service-start', 2)['error']['code'], 'service-install-incomplete')
            self.assertEqual(invoke(candidate, 'service-install', 2)['error']['code'], 'service-update-incomplete')
            self.assertEqual(invoke(fixtures.fixtures.TOOL, 'service-update', 2)['error']['code'], 'service-installed-kit-changed')
            self.assertEqual(invoke(candidate, 'service-update')['phase'], 'installed')
        # A later safe kit at the same destination may finish a pending update
        # without requiring a retained copy of the old binary or editing receipts.
        subprocess.run(['/bin/launchctl', 'bootout', target], capture_output=True, check=True, timeout=20)
        deadline = time.monotonic() + 15
        while self.endpoint.exists() and time.monotonic() < deadline:
            time.sleep(.05)
        self.assertFalse(self.endpoint.exists())
        replaced = {**pending, 'executable_sha256': '0' * 64}
        state_path.write_text(json.dumps(replaced))
        candidate.chmod(0o777)
        self.assertEqual(invoke(candidate, 'service-update', 2)['error']['code'], 'unsafe-executable')
        self.assertEqual(json.loads(state_path.read_text()), replaced)
        candidate.chmod(0o755)
        self.assertEqual(invoke(fixtures.fixtures.TOOL, 'service-update', 2)['error']['code'], 'service-installed-kit-changed')
        self.assertEqual(json.loads(state_path.read_text()), replaced)
        self.root.rename(away)
        try:
            self.assertFalse(invoke(candidate, 'service-update', 2)['ok'])
            self.assertEqual(json.loads(state_path.read_text()), replaced)
        finally:
            away.rename(self.root)
        self.assertEqual(invoke(candidate, 'service-update')['phase'], 'installed')
        self.assertEqual(registry, self.registry.read_bytes())
        self.assertEqual(history_inode, history.stat().st_ino)
        # Startup failure leaves an explicit pending operation, never a false success.
        subprocess.run(['/bin/launchctl', 'bootout', target], capture_output=True, check=True, timeout=20)
        deadline = time.monotonic() + 15
        while self.endpoint.exists() and time.monotonic() < deadline:
            time.sleep(.05)
        self.assertFalse(self.endpoint.exists())
        pending['update_stage'] = 'starting'
        state_path.write_text(json.dumps(pending))
        self.root.rename(away)
        try:
            self.assertFalse(invoke(candidate, 'service-update', 2)['ok'])
            self.assertEqual(json.loads(state_path.read_text())['phase'], 'updating')
        finally:
            away.rename(self.root)
        self.assertEqual(invoke(candidate, 'service-update')['phase'], 'installed')
        invoke(candidate, 'service-uninstall')

    @unittest.skipUnless(os.environ.get('SHIN_TEST_LAUNCHD') == '1', 'explicit live-manager opt-in required')
    def test_live_manager_recovers_and_respects_stop(self):
        self.run_ce('init')
        result = self.definition()
        self.stop()
        path = self.base / 'service.plist'
        path.write_text(result['plist'])
        path.chmod(0o600)
        domain = f'gui/{os.getuid()}'
        target = f"{domain}/{result['label']}"
        launchctl = '/bin/launchctl'
        def ctl(*args, required=True):
            completed = subprocess.run([launchctl, *args], capture_output=True, text=True, timeout=15)
            if required:
                self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            return completed
        def cleanup():
            ctl('bootout', target, required=False)
            deadline = time.monotonic() + 15
            while self.endpoint.exists() and time.monotonic() < deadline:
                time.sleep(.05)
            self.assertFalse(self.endpoint.exists(), 'test launchd service did not stop')
            self.assertNotEqual(ctl('print', target, required=False).returncode, 0, 'test job still loaded')
        self.addCleanup(cleanup)
        ctl('bootstrap', domain, str(path))
        def await_ready(previous=None):
            deadline = time.monotonic() + 35
            last = ''
            while time.monotonic() < deadline:
                run = subprocess.run([str(fixtures.fixtures.TOOL), 'service-status', '--home', str(self.home)],
                                     capture_output=True, text=True, timeout=15)
                last = run.stdout or run.stderr
                if run.returncode == 0:
                    state = json.loads(run.stdout)
                    prepared = any(row['root'] == str(self.root) and row['ready']
                                   for row in state['resident_projects'])
                    if state['instance_id'] != previous and prepared:
                        return state
                time.sleep(.1)
            self.fail('manager did not become ready: ' + last)
        first = await_ready()
        self.assertIn('return 7;', self.run_ce('query', 'readLimit', '--service')['source_context'])
        job = ctl('print', target).stdout
        match = re.search(r'^\s*pid = (\d+)\s*$', job, re.MULTILINE)
        self.assertIsNotNone(match, job)
        # Only the exact PID reported by the unique temporary launchd job is killed.
        os.kill(int(match[1]), signal.SIGKILL)
        (self.root / 'limits.js').write_text('export function readLimit() { return 77; }\n')
        recovered = await_ready(first['instance_id'])
        self.assertNotEqual(first['instance_id'], recovered['instance_id'])
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            run = subprocess.run([str(fixtures.fixtures.TOOL), 'query', 'readLimit', '--service',
                                  '--root', str(self.root), '--home', str(self.home)],
                                 capture_output=True, text=True, timeout=40)
            self.assertIn(run.returncode, (0, 1), run.stdout + run.stderr)
            response = json.loads(run.stdout)
            if run.returncode == 0:
                self.assertIn('return 77;', response['source_context'])
                break
            self.assertEqual(response['next_action'], 'wait-for-local-refresh', response)
            self.assertFalse(response.get('source_context'), 'stale source escaped during reconciliation')
            time.sleep(.1)
        else:
            self.fail('restarted service did not reconcile changed source')
        self.stop()
        # Wait longer than launchd's configured throttle: clean stop must stay stopped.
        time.sleep(result['unit']['ThrottleInterval'] + 2)
        self.assertFalse(self.endpoint.exists(), 'clean stop restarted unexpectedly')
        ctl('kickstart', target)
        final = await_ready(recovered['instance_id'])
        self.assertTrue(final['service_running'])
        self.assertIn('return 77;', self.run_ce('query', 'readLimit', '--service')['source_context'])
        print(json.dumps({'manager': 'launchd-user', 'abnormal_restart': True,
                          'clean_stop_preserved': True, 'explicit_restart': True,
                          'changed_source_after_crash': True, 'persistent_install': False}))


if __name__ == '__main__':
    names = [name for name in SupervisionTests.__dict__ if name.startswith('test_')]
    suite = unittest.TestSuite(SupervisionTests(name) for name in names)
    sys.exit(0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1)
