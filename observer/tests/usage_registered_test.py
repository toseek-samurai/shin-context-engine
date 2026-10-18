#!/usr/bin/env python3
"""Project enrollment must collect numerical usage without a second project list."""
import datetime
import hashlib
import json
import os
import pty
import select
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import time
import unittest

CE, USAGE, PROFILES = map(lambda p: Path(p).resolve(), sys.argv[1:4])
del sys.argv[1:4]

class RegisteredUsage(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='shin-auto-usage-')
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name).resolve()
        self.home = self.base/'ce'
        self.user = self.base/'user'; self.user.mkdir(mode=0o700)
        self.codex = self.user/'.codex'; self.codex.mkdir(mode=0o700)
        (self.codex/'sessions').mkdir()  # No archive directory on a fresh client.
        self.env = {k:v for k,v in os.environ.items() if k not in ('CODEX_HOME','HERMES_HOME','XDG_DATA_HOME')}
        self.env.update(HOME=str(self.user), PATH='')
        self.db = self.codex/'state_5.sqlite'
        with sqlite3.connect(self.db) as db:
            db.execute('CREATE TABLE threads(id,rollout_path,cwd,created_at_ms,created_at,updated_at_ms,updated_at)')
        self.addCleanup(self.stop)

    def stop(self):
        subprocess.run([str(CE),'service-stop','--home',str(self.home)],env=self.env,capture_output=True,timeout=20)
        deadline=time.monotonic()+10
        while (self.home/'service/rpc.sock').exists() and time.monotonic()<deadline:
            time.sleep(.02)
        self.assertFalse((self.home/'service/rpc.sock').exists(),'collector delayed service shutdown')

    def ce(self, command, root=None):
        args=[str(CE),command,'--home',str(self.home)]
        if root: args += ['--root',str(root)]
        if command=='project-add': args += ['--profile-dir',str(PROFILES)]
        r=subprocess.run(args,env=self.env,capture_output=True,text=True,timeout=45)
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        return json.loads(r.stdout)

    def project(self,name,tokens):
        root=self.base/name; root.mkdir()
        (root/'example.cpp').write_text('int sampleLimit() { return 7; }\n')
        now=datetime.datetime.now(datetime.timezone.utc)
        log=self.codex/'sessions'/f'{name}.jsonl'
        usage={'input_tokens':tokens,'output_tokens':2,'cached_input_tokens':0}
        rows=[{'type':'session_meta','payload':{'id':name}},
              {'type':'turn_context','payload':{'model':'fixture-model'}},
              {'type':'event_msg','timestamp':now.isoformat().replace('+00:00','Z'),
               'payload':{'type':'token_count','info':{'total_token_usage':usage,'last_token_usage':usage}}}]
        log.write_text(''.join(json.dumps(r)+'\n' for r in rows));log.chmod(0o600)
        with sqlite3.connect(self.db) as db:
            db.execute('INSERT INTO threads VALUES(?,?,?,?,?,?,?)',(name,str(log),str(root),0,0,int(now.timestamp()*1000),0))
        return root

    def state(self,check):
        deadline=time.monotonic()+20
        while time.monotonic()<deadline:
            try:
                state=json.loads((self.home/'usage/state.json').read_text())
                if check(state):return state
            except (OSError,ValueError):pass
            time.sleep(.1)
        self.fail('automatic usage did not converge: '+str({str(p.relative_to(self.home)):p.read_text() for p in self.home.glob('usage*/state.json')}))

    def collect(self):
        r=subprocess.run([str(USAGE),'--registry',str(self.home)],env=self.env,capture_output=True,text=True,timeout=15)
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        return json.loads((self.home/'usage/state.json').read_text())

    def check_default_tui(self):
        master, slave=pty.openpty()
        child=subprocess.Popen([str(CE),'stats','--home',str(self.home)],
            stdin=slave,stdout=slave,stderr=slave,env={**self.env,'TERM':'xterm-256color'})
        output=b''
        try:
            os.write(master,b'5')
            deadline=time.monotonic()+10
            while b'fixture-model' not in output and time.monotonic()<deadline:
                if select.select([master],[],[],.1)[0]: output+=os.read(master,65536)
            self.assertIn(b'fixture-model',output)
            os.write(master,b'q')
            deadline=time.monotonic()+5
            while child.poll() is None and time.monotonic()<deadline:
                if select.select([master],[],[],.05)[0]: os.read(master,65536)
            self.assertEqual(child.poll(),0)
        finally:
            if child.poll() is None:child.kill();child.wait(timeout=5)
            os.close(master);os.close(slave)

    def test_enroll_repeat_detach_restart_and_source_failure(self):
        one=self.project('first',100)
        response=self.ce('project-add',one)
        self.assertEqual(response['model_usage']['collection'],'automatic-background')
        first=self.state(lambda s:len(s['snapshot']['buckets'])==1)
        self.assertEqual(first['snapshot']['buckets'][0]['input_tokens'],100)
        self.assertEqual(first['snapshot']['sources']['codex']['state'],'available')
        self.check_default_tui()
        two=self.project('second',200);self.ce('project-add',two)
        second=self.state(lambda s:len(s['snapshot']['buckets'])==2)
        self.ce('project-add',two);self.stop()
        repeated=self.collect()
        self.assertEqual(second['snapshot']['buckets'],repeated['snapshot']['buckets'])
        # A missing detached rollout must not be read or counted as a new error.
        self.ce('service-start'); self.ce('detach',one)
        (self.codex/'sessions/first.jsonl').unlink()
        self.stop(); detached=self.collect()
        self.assertEqual(detached['snapshot']['sources']['codex']['source_records'],1)
        self.assertEqual(detached['snapshot']['sources']['codex']['skipped_sessions'],0)
        self.assertEqual(len(detached['snapshot']['buckets']),2)
        # Disappearing client history retains the previous cumulative buckets.
        self.db.rename(self.db.with_suffix('.offline'))
        unavailable=self.collect()
        self.assertEqual(unavailable['snapshot']['buckets'],detached['snapshot']['buckets'])
        self.assertEqual(unavailable['snapshot']['sources']['codex']['state'],'unavailable')
        self.ce('service-start');self.stop()  # Collector cancellation must not delay shutdown.
        self.assertEqual((self.home/'usage/state.json').stat().st_mode & 0o777,0o600)

if __name__=='__main__':unittest.main()
