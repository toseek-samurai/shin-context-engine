#!/usr/bin/env python3
"""Project enrollment must collect numerical usage without a second project list."""
import datetime
import fcntl
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

    def test_explicit_host_events_are_private_bounded_and_separate_from_token_snapshot(self):
        root=self.project('marked',100)
        log=self.codex/'sessions/marked.jsonl'
        now=datetime.datetime.now(datetime.timezone.utc).isoformat().replace('+00:00','Z')
        with log.open('a') as stream:
            events=[{'type':'event_msg','payload':{'type':'task_started','turn_id':'raw-turn-private'}},
                    {'type':'turn_context','payload':{'model':'provider/model-one','turn_id':'raw-turn-private'}},
                    {'type':'compacted','payload':{'window_id':'raw-window-private','message':'PRIVATE SUMMARY',
                                                'replacement_history':['PRIVATE HISTORY']}},
                    {'type':'event_msg','payload':{'type':'task_complete','last_agent_message':'PRIVATE ANSWER'}}]
            for event in events: stream.write(json.dumps({**event,'timestamp':now})+'\n')
        self.ce('project-add',root);self.stop()
        state=self.collect()
        self.assertNotIn('context_events',state['snapshot'])  # Older kits can still read this exact token snapshot.
        context=state['context_events'];markers=context['events']
        self.assertEqual({m['kind'] for m in markers},{'turn-start','turn-model','compaction','turn-finish'})
        expected=hashlib.sha256(f'shin-session-v1:{self.home}:marked'.encode()).hexdigest()
        self.assertEqual({m['session_id'] for m in markers},{expected})
        self.assertEqual(next(m for m in markers if m['kind']=='turn-model')['model_id'],'provider/model-one')
        self.assertTrue(all('model_id' not in m for m in markers if m['kind']!='turn-model'))
        for private in ('PRIVATE','raw-turn-private','raw-window-private','marked',str(root)):
            self.assertNotIn(private,json.dumps(context))
        # A token gap is not a compaction; retained explicit events remain stable on a repeated sweep.
        self.assertEqual(self.collect()['context_events'],context)
        with log.open('a') as stream:
            for i in range(80): stream.write(json.dumps({'type':'compacted','timestamp':now,
                                                       'payload':{'window_id':f'raw-{i}'}})+'\n')
        bounded=self.collect()['context_events']
        self.assertEqual(len(bounded['events']),64)
        self.assertFalse(bounded['complete']);self.assertGreater(bounded['omitted'],0)
        self.assertEqual(self.collect()['snapshot']['buckets'],state['snapshot']['buckets'])

    def test_bad_optional_context_record_does_not_drop_valid_tokens(self):
        root=self.project('damaged-marker',100)
        self.ce('project-add',root);self.stop()
        self.assertEqual(sum(b['input_tokens'] for b in self.collect()['snapshot']['buckets']),100)
        stamp=datetime.datetime.now(datetime.timezone.utc).isoformat().replace('+00:00','Z')
        with (self.codex/'sessions/damaged-marker.jsonl').open('a') as stream:
            stream.write('{"type":"compacted","payload":\n')
            stream.write(json.dumps({'type':'event_msg','timestamp':stamp,'payload':{'type':'token_count','info':{
                'total_token_usage':{'input_tokens':150,'output_tokens':2},
                'last_token_usage':{'input_tokens':50,'output_tokens':0}}}})+'\n')
        state=self.collect()
        self.assertEqual(state['snapshot']['sources']['codex']['state'],'available')
        self.assertEqual(sum(b['input_tokens'] for b in state['snapshot']['buckets']),150)
        self.assertFalse(state['context_events']['complete'])
        self.assertGreater(state['context_events']['omitted'],0)

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

    def test_repeat_enrollment_waits_for_busy_refresh_recovery(self):
        root=self.project('busy',100)
        self.ce('project-add',root)
        workspace=self.home/'projects'/hashlib.sha256(str(root).encode()).hexdigest()
        with (workspace/'observer.lock').open('rb') as lock:
            fcntl.flock(lock,fcntl.LOCK_EX)
            (root/'example.cpp').write_text('int sampleLimit() { return 42; }\n')
            deadline=time.monotonic()+5
            while True:
                status=self.ce('service-status')
                row=status['telemetry']['project_states'][0]
                if row.get('error_code')=='writer-busy':break
                self.assertLess(time.monotonic(),deadline,status)
                time.sleep(.02)
            fcntl.flock(lock,fcntl.LOCK_UN)
        # Bootstrap can finish before the watcher's normal failure backoff.
        # Admission must wait for requalification, not accept the local build.
        response=self.ce('project-add',root)
        self.assertTrue(response['service_enrolled'],response)
        self.assertTrue(response['local_ready'],response)
        query=subprocess.run([str(CE),'query','sampleLimit','--service','--root',str(root),
                              '--home',str(self.home)],env=self.env,capture_output=True,text=True,timeout=10)
        self.assertEqual(query.returncode,0,query.stdout+query.stderr)
        self.assertIn('return 42',json.loads(query.stdout)['source_context'])

    def test_new_enrollment_waits_for_busy_controller(self):
        self.ce('project-add',self.project('first',100))
        root=self.project('second',200)
        prepared=subprocess.run([str(CE),'init','--offline','--no-service','--root',str(root),
                                 '--home',str(self.home),'--profile-dir',str(PROFILES)],
                                env=self.env,capture_output=True,text=True,timeout=30)
        self.assertEqual(prepared.returncode,0,prepared.stdout+prepared.stderr)
        workspace=self.home/'projects'/hashlib.sha256(str(root).encode()).hexdigest()
        watch=workspace/'watch';watch.mkdir(mode=0o700,exist_ok=True)
        with (watch/'observer.lock').open('a+b') as lock:
            os.chmod(lock.name,0o600)
            fcntl.flock(lock,fcntl.LOCK_EX)
            child=subprocess.Popen([str(CE),'project-add','--root',str(root),'--home',str(self.home),
                                    '--profile-dir',str(PROFILES)],env=self.env,
                                   stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            try:
                deadline=time.monotonic()+10
                while True:
                    status=self.ce('service-status')
                    if status['enrollment_errors'].get(str(root))=='writer-busy':break
                    self.assertLess(time.monotonic(),deadline,status)
                    time.sleep(.02)
                self.assertIsNone(child.poll(),'initializer rejected a temporary controller lock')
                fcntl.flock(lock,fcntl.LOCK_UN)
                out,err=child.communicate(timeout=30)
                self.assertEqual(child.returncode,0,out+err)
                self.assertTrue(json.loads(out)['service_enrolled'])
            finally:
                if child.poll() is None:child.kill();child.wait(timeout=5)
        query=subprocess.run([str(CE),'query','sampleLimit','--service','--root',str(root),
                              '--home',str(self.home)],env=self.env,capture_output=True,text=True,timeout=10)
        self.assertEqual(query.returncode,0,query.stdout+query.stderr)
        self.assertIn('return 7',json.loads(query.stdout)['source_context'])

if __name__=='__main__':unittest.main()
