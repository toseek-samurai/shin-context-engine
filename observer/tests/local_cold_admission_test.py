#!/usr/bin/env python3
"""Bound cold admission independently of the long service/recovery suite."""
import fcntl
import json
import os
import signal
import time
import unittest

import local_service_test as service


class ColdAdmissionTests(unittest.TestCase):
    setUp = service.LocalServiceTests.setUp
    init = service.LocalServiceTests.init
    run_ce = service.LocalServiceTests.run_ce
    start = service.LocalServiceTests.start
    connect = service.LocalServiceTests.connect
    receive = service.LocalServiceTests.receive
    frame = service.LocalServiceTests.frame
    raw = service.LocalServiceTests.raw

    def test_cold_demand_wakes_idle_requalification(self):
        self.start()
        deadline = time.monotonic() + 25
        while True:
            status = self.raw('service-status')['result']
            row = status['telemetry']['project_states'][0]
            if row['phase'] == 'current' and row.get('retry_after_ms') == 15000:
                break
            self.assertLess(time.monotonic(), deadline, status)
            time.sleep(.03)
        # Lose only this disposable process, leaving sources, authority and the
        # qualified publication untouched. An idle refresh is nearly 15s away.
        reader = status['resident_projects'][0]
        os.kill(reader['pid'], signal.SIGTERM)
        deadline = time.monotonic() + 2
        while self.raw('service-status')['result']['resident_projects']:
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.02)
        with self.connect() as client:
            client.settimeout(3)
            started = time.monotonic()
            client.sendall((json.dumps(self.frame('query', query='readLimit', request_id='idle-cold')) + '\n').encode())
            response = self.receive(client)
            self.assertTrue(response['ok'], response)
            self.assertIn('return 7', response['result']['source_context'])
            self.assertLess(time.monotonic() - started, 3)
        metric = self.raw('service-status')['result']['telemetry']['request_metrics']['recent'][-1]
        self.assertEqual(metric['reader_state'], 'cold')
        self.assertGreater(metric['wait_ms'], 0)

    def test_cold_demand_survives_short_refusal_behind_another_refresh(self):
        other = self.base / 'other'
        other.mkdir()
        (other / 'limits.js').write_text('export function readLimit() { return 91; }\n')
        (other / 'README.md').write_text('# Limits\n\nreadLimit owns the ceiling.\n')
        self.init(root=other)
        self.roots.append(other)
        self.start()
        deadline = time.monotonic() + 25
        while True:
            status = self.raw('service-status')['result']
            if all(r['phase'] == 'current' and r.get('retry_after_ms') == 15000
                   for r in status['telemetry']['project_states']):
                break
            self.assertLess(time.monotonic(), deadline, status)
            time.sleep(.03)
        workspace = service.fixtures.LocalProjectTests.workspace(self)
        with (workspace / 'index/docs/kb/.shin-context.lock').open('rb') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            (self.root / 'limits.js').write_text('export function readLimit() { return 73; }\n')
            deadline = time.monotonic() + 3
            while True:
                status = self.raw('service-status')['result']
                active = next(r for r in status['telemetry']['project_states'] if r['root'] == str(self.root))
                if active['phase'] == 'checking':
                    break
                self.assertLess(time.monotonic(), deadline, status)
                time.sleep(.02)
            reader = next(r for r in status['resident_projects'] if r['root'] == str(other))
            os.kill(reader['pid'], signal.SIGTERM)
            deadline = time.monotonic() + 2
            while any(r['root'] == str(other) for r in self.raw('service-status')['result']['resident_projects']):
                self.assertLess(time.monotonic(), deadline)
                time.sleep(.02)
            started = time.monotonic()
            response = self.raw('query', root=other, query='readLimit')
            self.assertEqual(response['error']['code'], 'project-updating', response)
            self.assertLess(time.monotonic() - started, 7)
            fcntl.flock(lock, fcntl.LOCK_UN)
        # No second project request to wake it: the initial demand must remain
        # queued after the explicit refusal, without restarting the whole service.
        deadline = time.monotonic() + 3
        while True:
            status = self.raw('service-status')['result']
            if any(r['root'] == str(other) and r['ready'] for r in status['resident_projects']):
                break
            self.assertLess(time.monotonic(), deadline, status)
            time.sleep(.02)
        response = self.raw('query', root=other, query='readLimit')
        self.assertTrue(response['ok'], response)
        self.assertIn('return 91', response['result']['source_context'])


if __name__ == '__main__':
    unittest.main()
