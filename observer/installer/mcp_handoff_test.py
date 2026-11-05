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
import tempfile
import time

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
    prefix, home, commands, root = (user/name for name in ('kit','data','bin','project'))
    root.mkdir(); (root/'quota.cpp').write_text('int updateQuota() { return 41; }\n')
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
                block=os.read(child.stdout.fileno(),131072)
                assert block,child.stderr.read().decode()
                child.pending+=block
            line,child.pending=child.pending.split(b'\n',1)
            value=json.loads(line)
            if value.get('id')==child.sequence:return value
        raise AssertionError('MCP response deadline')

    def adapter():
        child = subprocess.Popen([str(prefix/'bin/shin-mcp'),'--root',str(root),'--home',str(home)],
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
        call(prefix/'bin/shin-ce','init','--root',root,'--home',home)
        child=adapter(); original_pid=child.pid
        first=tool(child,'shin_query',{'query':'updateQuota','diagnostic':True})
        assert first.get('ok'),first
        registry=(home/'registry/state.json').read_bytes()
        updated=call(candidate/'bin/shin-ce','update','--prefix',prefix)
        assert updated['version']==target['version'],updated
        assert child.poll() is None and child.pid==original_pid
        result=tool(child,'shin_query',{'query':'updateQuota','diagnostic':True})
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
            result=tool(child,'shin_query',{'query':'updateQuota','diagnostic':True})
            assert result.get('ok') and result['version']==target['version'],result
        read=tool(child,'shin_read',{'path':'quota.cpp','snapshot':result['snapshot_id'],'diagnostic':True,
            'parent_request_id':result['request_trace']['request_id']})
        assert read.get('ok'),read
        assert (home/'registry/state.json').read_bytes()==registry
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
