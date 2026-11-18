#!/usr/bin/env python3
"""Exact-kit upgrade with an initialized, live stdio MCP adapter.

Both kits and all project/state paths are disposable. No installed user kit is
selected. Legacy/changed-contract adapters must refuse and reconnect explicitly;
an unchanged adapter contract must continue in the same process.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tests'))
from integration_test import Fixture

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--previous-kit', type=Path, required=True)
parser.add_argument('--candidate-kit', type=Path, required=True)
parser.add_argument('--report', type=Path, required=True)
args = parser.parse_args()
old, candidate = args.previous_kit.resolve(), args.candidate_kit.resolve()
previous = json.loads((old/'shin-kit.json').read_text())
target = json.loads((candidate/'shin-kit.json').read_text())
assert previous['version'] != target['version']

def contract(kit):
    path = kit/'share/shin-context/mcp-runtime.json'
    return json.loads(path.read_text()).get('adapter_contract_sha256') if path.exists() else None

mode = 'legacy-reconnect' if not contract(old) else 'automatic' if contract(old) == contract(candidate) else 'contract-reconnect'
report = {'previous_version': previous['version'], 'candidate_version': target['version'],
          'mode': mode, 'live_installation_changed': False}
with tempfile.TemporaryDirectory(prefix='shin-mcp-up-', dir='/tmp') as directory:
    work = Path(directory).resolve()
    user = work/'user'; user.mkdir(mode=0o700)
    prefix, home, commands = (user/name for name in ('kit','data','bin'))
    fixture = Fixture(work, 'external')
    root = fixture.root
    (root/'quota.cpp').write_text('int updateQuota() { return 41; }\n')
    (root/'release.rules').write_text('releaseQuotaPolicy permits 83 units.\n')
    fixture.write_source('# Limits\n\nProject quota belongs to updateQuota.\n')
    profile = json.loads(fixture.profile_path.read_text())
    profile.update(bootstrapOnly=False, sourceFactRoots=['.'], sourceFactExtensions=['.cpp'],
        evalCases=[{'id':'quota','query':'updateQuota','mustPaths':['quota.cpp']}],
        packEvalCases=[{'id':'quota-pack','mustPaths':['quota.cpp'],'expectedComplete':True,'request':{
            'schema_version':'shin.task-request.v1','request_id':'quota-pack',
            'task':{'objective':'Locate quota.','must':[{'id':'owner','query':'updateQuota'}]},
            'budget':{'max_evidence_bytes':12000,'max_records':8,'per_query_records':8,'per_document_records':2}}}])
    fixture.profile_path.write_text(json.dumps(profile))
    selected_settings = user/'selected-agent.json'
    other_settings = user/'other-agent.json'
    other_settings.write_text('{"mcpServers":{}}\n')
    env = {**os.environ, 'HOME':str(user), 'PATH':'', 'LC_ALL':'C'}
    children = []

    def call(executable, *words):
        run = subprocess.run([str(executable), *map(str,words)], cwd=root, env=env,
                             capture_output=True, text=True, timeout=180)
        value = json.loads(run.stdout or run.stderr)
        assert run.returncode == 0 and value.get('ok'), (run.returncode,value)
        return value

    def rpc(child, method, params):
        child.sequence += 1
        child.stdin.write((json.dumps({'jsonrpc':'2.0','id':child.sequence,'method':method,'params':params})+'\n').encode())
        child.stdin.flush()
        end = time.monotonic()+35
        while time.monotonic()<end:
            if b'\n' not in child.pending:
                if not select.select([child.stdout],[],[],max(0,end-time.monotonic()))[0]:break
                # Force a short first read so this test always exercises framing.
                block=os.read(child.stdout.fileno(), 1 if not child.pending else 131072)
                assert block, 'MCP stdout ended before the expected response'
                child.pending+=block
                continue  # A pipe read can end before the newline.
            line,child.pending=child.pending.split(b'\n',1)
            value=json.loads(line)
            if value.get('id')==child.sequence:return value
        raise AssertionError('MCP response deadline')

    def adapter():
        server = json.loads(selected_settings.read_text())['mcpServers']['shin']
        child = subprocess.Popen([server['command'], *server['args']],
                                 stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=env)
        children.append(child); child.sequence=0; child.pending=b''
        initialized=rpc(child,'initialize',{'protocolVersion':'2025-11-25','capabilities':{},
            'clientInfo':{'name':'shin-kit-handoff-test','version':'1'}})
        assert initialized.get('result'),initialized
        child.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n');child.stdin.flush()
        return child

    def tool(child, name, arguments):
        value=rpc(child,'tools/call',{'name':name,'arguments':arguments})
        return json.loads(value['result']['content'][0]['text'])

    try:
        call(old/'bin/shin-ce','install','--prefix',prefix,'--bin-dir',commands,'--home',home)
        call(prefix/'bin/shin-context','--root',root,'--profile','observer-fixture','--profile-dir',fixture.profiles,
             '--out-dir',fixture.out,'build')
        call(prefix/'bin/shin-observer','sample','--config',fixture.config)
        call(prefix/'bin/shin-ce','init','--root',root,'--home',home,'--observer-config',fixture.config,'--manage-index')
        setup = call(prefix/'bin/shin-ce','mcp-config','--client','generic','--root',root,'--home',home)
        selected_settings.write_text(setup['configuration'])
        protected = {path:path.read_bytes() for path in (selected_settings,other_settings,home/'registry/state.json',
            home/'identity/state.json',fixture.out/'docs/kb/generated/manifest.json')}
        child=adapter(); original_pid=child.pid
        first=tool(child,'shin_query',{'query':'updateQuota','diagnostic':True})
        assert first.get('ok'),first
        fixture.profile_path.unlink()
        updated=call(candidate/'bin/shin-ce','update','--prefix',prefix)
        assert updated['version']==target['version'],updated
        assert child.poll() is None and child.pid==original_pid
        result=tool(child,'shin_query',{'query':'releaseQuotaPolicy','diagnostic':True})
        if mode=='automatic':
            assert result.get('ok') and result['version']==target['version'],result
            report['same_initialized_process']=True
        else:
            assert not result.get('ok'),result
            expected={'mcp-protocol-changed'} if mode=='contract-reconnect' else {'service-version-mismatch','service-response-invalid'}
            assert result['error']['code'] in expected,result
            report['explicit_refusal']=result['error']['code']
            child.terminate();child.communicate(timeout=10)
            child=adapter()
            result=tool(child,'shin_query',{'query':'releaseQuotaPolicy','diagnostic':True})
            assert result.get('ok') and result['version']==target['version'],result
        assert 'permits 83 units' in result['source_context']
        read=tool(child,'shin_read',{'path':'release.rules','snapshot':result['snapshot_id'],'diagnostic':True,
            'parent_request_id':result['request_trace']['request_id']})
        assert read.get('ok'),read
        assert all(path.read_bytes()==original for path,original in protected.items())
        assert not fixture.profile_path.exists()
        installed=call(prefix/'bin/shin-ce','installation','--prefix',prefix)
        assert installed['integrity_verified'] and not installed['recovery_pending'],installed
        report.update(ok=True,query=True,snapshot_bound_read=True,registry_preserved=True,
                      old_pid=original_pid,current_pid=child.pid,
                      candidate_sha256=hashlib.sha256((candidate/'shin-kit.json').read_bytes()).hexdigest())
    except BaseException as error:
        report.update(ok=False,error=str(error))
        raise
    finally:
        for child in children:
            if child.poll() is None:child.terminate()
            try:child.communicate(timeout=10)
            except subprocess.TimeoutExpired:child.kill();child.communicate()
        if (prefix/'.shin-installation.json').exists():
            if (home/'service/rpc.sock').exists():
                call(prefix/'bin/shin-ce','service-stop','--home',home)
                end=time.monotonic()+15
                while (home/'service/rpc.sock').exists():
                    assert time.monotonic()<end,'Disposable service did not stop'
                    time.sleep(.02)
            call(candidate/'bin/shin-ce','uninstall','--prefix',prefix)
        args.report.parent.mkdir(parents=True,exist_ok=True)
        args.report.write_text(json.dumps(report,indent=2)+'\n')
args.report.parent.mkdir(parents=True,exist_ok=True)
args.report.write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report))
