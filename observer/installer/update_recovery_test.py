#!/usr/bin/env python3
"""Exercise production release orchestration with exact kit bytes and private HTTP fixtures.

No real installation, global service or client settings are touched. The default
previous identity is a synthetic manifest fixture; --previous-kit uses a real kit.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import time

p=argparse.ArgumentParser()
p.add_argument('--helper',type=Path,required=True)
p.add_argument('--candidate-kit',type=Path,required=True)
p.add_argument('--archive',type=Path,required=True)
p.add_argument('--previous-kit',type=Path)
p.add_argument('--report',type=Path,required=True)
a=p.parse_args()
manifest=json.loads((a.candidate_kit/'shin-kit.json').read_text())
version=manifest['version']; name=f'shin-local-{version}-macos-arm64.zip'
archive_sha=hashlib.sha256(a.archive.read_bytes()).hexdigest()
report={'real_previous':bool(a.previous_kit),'scenarios':[]}

def interrupt_tree(process):
    # NSTask creates a separate process group for its child. Pin descendants
    # while the owned launcher is still alive; never signal by executable name.
    rows=subprocess.check_output(['/bin/ps','-axo','pid=,ppid='],text=True)
    edges=[tuple(map(int,line.split())) for line in rows.splitlines()]
    owned={process.pid}
    while True:
        added={pid for pid,parent in edges if parent in owned}-owned
        if not added:break
        owned.update(added)
    for sig in (signal.SIGSTOP,signal.SIGKILL):
        for pid in sorted(owned,reverse=True):
            try:os.kill(pid,sig)
            except ProcessLookupError:pass
    process.communicate(timeout=15)

def save(path,value):
    path.write_text(json.dumps(value));path.chmod(0o600)

# Native child HOME is NSHomeDirectory. Keep disposable paths under that account,
# while explicitly routing every command to this isolated installation/data home.
with tempfile.TemporaryDirectory(prefix='.shin-update-recovery-',dir=Path.home()) as tmp:
    base=Path(tmp).resolve();transport=base/'http';transport.mkdir()
    shutil.copyfile(a.archive,transport/'archive.zip')
    (transport/'archive.sha256').write_text(f'{archive_sha}  {name}\n')
    url=f'https://github.com/toseek-samurai/shin-context-engine/releases/'
    save(transport/'feed.json',[{'draft':False,'prerelease':'-' in version,'tag_name':'v'+version,
        'html_url':url+'tag/v'+version,'assets':[
        {'name':name,'state':'uploaded','digest':'sha256:'+archive_sha,'size':a.archive.stat().st_size,'browser_download_url':url+'download/v'+version+'/'+name},
        {'name':name+'.sha256','state':'uploaded','size':128,'browser_download_url':url+'download/v'+version+'/'+name+'.sha256'}]}])
    for scenario in ['retry-offline','interrupted-copy','missing-staging','interrupted-download','download-outage','cleanup-failure']:
        user=base/scenario;user.mkdir(mode=0o700)
        prefix=user/'kit';home=user/'data';commands=user/'bin'
        env={'HOME':str(user),'PATH':'/usr/bin:/bin','LC_ALL':'C'}
        def call(tool,*args,expected=0):
            r=subprocess.run([str(tool),*map(str,args)],env=env,capture_output=True,text=True,timeout=120)
            result=json.loads(r.stdout or r.stderr)
            assert r.returncode==expected,(args,r.returncode,result)
            return result
        def helper(command,*args,expected=0):
            return call(a.helper,transport,'shin-updater',command,'--prefix',prefix,'--home',home,*args,expected=expected)
        old=a.previous_kit or a.candidate_kit
        call(old/'bin/shin-ce','install','--prefix',prefix,'--home',home,'--bin-dir',commands)
        receipt_path=prefix/'.shin-installation.json';receipt=json.loads(receipt_path.read_text())
        control=Path(receipt['control'])/'state.json'
        if not a.previous_kit:
            previous=json.loads((prefix/'shin-kit.json').read_text());previous['version']='2.0.5-dev.0'
            save(prefix/'shin-kit.json',previous)
            receipt['manifest_sha256']=hashlib.sha256((prefix/'shin-kit.json').read_bytes()).hexdigest();save(receipt_path,receipt)
            state=json.loads(control.read_text());state['kit']=previous;state['receipt']=receipt;save(control,state)
        offer=helper('update-check');assert offer['version']==version
        if scenario=='download-outage':
            (transport/'fail-download').touch()
            failed=helper('update-install','--revision',offer['revision'],expected=1)
            assert failed['phase']=='failed' and failed['error']['code']=='update-network-unavailable',failed
            status=helper('update-status',expected=1);assert status['can_retry'],status
            (transport/'fail-download').unlink()
            result=helper('update-resume','--revision',status['review_revision'])
        elif scenario=='cleanup-failure':
            (transport/'lock-download').touch()
            result=helper('update-install','--revision',offer['revision'])
            (transport/'lock-download').unlink()
            assert result['phase']=='installed' and result['cleanup_warning']=='update-cache-cleanup-pending',result
            stage=Path(result['staging']);assert stage.exists() and result['stage_identity']
            os.chflags(stage/name,0)
            status=helper('update-status');assert status['ok'] and status['outcome']=='installed',status
            helper('update-acknowledge','--revision',status['review_revision'])
            assert not stage.exists()
        elif scenario=='retry-offline':
            downloaded=helper('update-download','--revision',offer['revision'])
            state_path=home/'updates/state.json';state=json.loads(state_path.read_text())
            state['phase']='review-required';state['ok']=False
            state['result']={'ok':False,'error':{'code':'installation-project-not-ready'}};save(state_path,state)
            status=helper('update-status',expected=1);assert status['can_retry'] and not status['native_recovery_pending']
            reviewed_receipt=receipt_path.read_bytes()
            receipt_path.write_bytes(reviewed_receipt+b'\n')
            try:
                refused=helper('update-resume','--revision',status['review_revision'],expected=2)
                assert refused['error']['code']=='update-offer-changed' and json.loads(state_path.read_text())==state
            finally:
                receipt_path.write_bytes(reviewed_receipt)
            (transport/'offline').touch();before=(transport/'requests').read_bytes()
            kit=Path(state['candidate']);target=kit/'share/shin-context/docs/local-kit.md'
            if not target.exists():
                target=next(kit.glob('share/**/local-kit.md'))
            original=target.read_bytes();target.write_bytes(original+b'\nchanged fixture\n')
            bad=helper('update-resume','--revision',status['review_revision'],expected=2)
            assert bad['error']['code']=='installation-file-changed',bad
            assert kit.exists() and json.loads(state_path.read_text())==state
            target.write_bytes(original)
            result=helper('update-resume','--revision',status['review_revision'])
            assert (transport/'requests').read_bytes()==before,'retained retry unexpectedly fetched network'
            (transport/'offline').unlink()
        else:
            if scenario=='interrupted-download':(transport/'pause-download').touch()
            process=subprocess.Popen([str(a.helper),str(transport),'shin-updater','update-install','--prefix',str(prefix),
                '--home',str(home),'--revision',offer['revision']],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
            try:
                deadline=time.monotonic()+60
                while True:
                    state=json.loads(control.read_text())
                    if (transport/'download-paused').exists() if scenario=='interrupted-download' else state.get('phase')=='updating':break
                    assert process.poll() is None,'update finished before interruption'
                    assert time.monotonic()<deadline
                    time.sleep(.001)
                interrupt_tree(process)
            finally:
                if process.poll() is None:interrupt_tree(process)
            status=helper('update-status',expected=1)
            if scenario=='interrupted-download':
                assert status['can_retry'] and not status['native_recovery_pending'],status
                partial_stage=Path(status['staging'])
                assert (partial_stage/name).stat().st_size==1024 and 'candidate' not in status
                (transport/'pause-download').unlink();(transport/'download-paused').unlink()
            else:assert status['can_resume'] and status['native_recovery_pending'],status
            if scenario=='missing-staging':
                stage=Path(status['staging']);assert stage.name.startswith('shin-release.') and stage.parent==Path('/private/tmp')
                shutil.rmtree(stage)
                status=helper('update-status',expected=1)
                feed_bytes=(transport/'feed.json').read_bytes();feed=json.loads(feed_bytes)
                feed[0]['assets'][0]['digest']='sha256:'+'0'*64;save(transport/'feed.json',feed)
                journal=control.read_bytes()
                bad=helper('update-resume','--revision',status['review_revision'],expected=2)
                assert bad['error']['code']=='update-release-invalid',bad
                assert control.read_bytes()==journal
                (transport/'feed.json').write_bytes(feed_bytes)
            result=helper('update-resume' ,'--revision',status['review_revision'])
        if scenario=='interrupted-download':assert not partial_stage.exists(),'incomplete transfer cache leaked'
        assert result['ok'] and result['phase']=='installed',result
        installed=call(prefix/'bin/shin-ce','installation','--prefix',prefix)
        assert installed['integrity_verified'] and not installed['recovery_pending'] and installed['version']==version
        assert len(result['timeline'])<=64
        report['scenarios'].append(scenario)
        print(scenario,'passed',flush=True)
report['ok']=True;a.report.write_text(json.dumps(report,indent=2))
