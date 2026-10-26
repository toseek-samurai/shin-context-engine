"""New updater delivery despite an application manifest unknown to the launcher."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile

tool=Path(sys.argv[1]);version=sys.argv[2]
with tempfile.TemporaryDirectory(prefix='shin-bootstrap-handoff-') as folder:
    base=Path(folder).resolve();prefix=base/'owned kit';home=base/'history';fixtures=base/'transport'
    for path in [prefix,home,fixtures]:path.mkdir(mode=0o700)
    # This is deliberately not a supported kit format. Only the downloaded
    # updater may interpret installation policy; the bootstrap cannot gate it.
    unknown=b'{"schema_version":"future-kit.v999","opaque":"preserve"}'
    (prefix/'shin-kit.json').write_bytes(unknown)
    def call(command=None,*args,ok=True):
        result=subprocess.run([str(tool),str(fixtures),str(prefix),str(home),*([command,*args] if command else [])],
                              capture_output=True,text=True,timeout=15)
        assert (result.returncode==0)==ok,(result.stdout,result.stderr)
        return json.loads(result.stdout or result.stderr)
    call()
    def package(protocol=1):
        # A newly fetched executable speaks the stable protocol but invents no
        # real installation: it records the exact received prefix and revision.
        script=f'''#!/bin/sh
set -eu
case "$1" in
--protocol) printf '%s\\n' '{{"schema_version":"shin.updater-protocol.v1","protocol":{protocol},"version":"9.0.0"}}' ;;
update-check) printf '%s\\n' '{{"ok":true,"available":true,"version":"9.0.0","revision":"fresh-engine-revision"}}' ;;
update-status) printf '%s\\n' '{{"ok":false,"phase":"review-required"}}' ;;
update-install)
  prefix= revision=
  shift
  while [ "$#" -gt 0 ]; do
    case "$1" in --prefix) prefix=$2;shift 2;; --revision) revision=$2;shift 2;; --progress|--progress-json) shift;; *) shift 2;; esac
  done
  [ "$revision" = fresh-engine-revision ]
  printf '%s' "$revision" > "$prefix/delegated"
  printf '%s\\n' '{{"step":2,"received":50,"total":100}}' >&2
  printf '{{"ok":true,"phase":"installed","fresh_updater":true,"pid":%s}}\\n' "$$"
  stage=$(cd "$(dirname "$0")/../../.." && pwd)
  rm -rf "$stage"
  ;;
*) exit 41;;
esac
'''.encode()
        manifest={'schema_version':'shin.updater.v1','protocol':1,'version':'9.0.0','files':[
            {'path':'bin/shin-updater','size':len(script),'sha256':hashlib.sha256(script).hexdigest(),'executable':True}]}
        with zipfile.ZipFile(fixtures/'updater.zip','w',compression=zipfile.ZIP_DEFLATED) as z:
            for name,payload,mode in [('bin/shin-updater',script,0o100755),('shin-updater.json',json.dumps(manifest).encode(),0o100644)]:
                entry=zipfile.ZipInfo('shin-updater-v1/'+name);entry.create_system=3;entry.external_attr=mode<<16;z.writestr(entry,payload)
        digest=hashlib.sha256((fixtures/'updater.zip').read_bytes()).hexdigest()
        name='shin-updater-v1-macos-arm64.zip';url='https://github.com/toseek-samurai/shin-context-engine/releases/'
        (fixtures/'updater.sha256').write_text(f'{digest}  {name}\n')
        release={'draft':False,'prerelease':False,'tag_name':'v9.0.0','html_url':url+'tag/v9.0.0','assets':[
            {'name':name,'state':'uploaded','digest':'sha256:'+digest,'size':(fixtures/'updater.zip').stat().st_size,'browser_download_url':url+'download/v9.0.0/'+name},
            {'name':name+'.sha256','state':'uploaded','size':110,'browser_download_url':url+'download/v9.0.0/'+name+'.sha256'}]}
        (fixtures/'feed.json').write_text(json.dumps([release]))
    package()
    current=json.loads((fixtures/'feed.json').read_text())
    current[0]['tag_name']='v'+version;current[0]['html_url']=current[0]['html_url'].replace('v9.0.0','v'+version)
    for asset in current[0]['assets']:asset['browser_download_url']=asset['browser_download_url'].replace('v9.0.0','v'+version)
    (fixtures/'feed.json').write_text(json.dumps(current))
    equal=call('update-check');assert not equal['available'] and 'version' not in equal, 'current updater created a false update badge'
    package()
    offered=call('update-check');assert offered['version']=='9.0.0'
    call('update-install','--revision','stale',ok=False);assert not (prefix/'delegated').exists()
    saved=(fixtures/'updater.sha256').read_text();(fixtures/'updater.sha256').write_text('0'*64)
    call('update-install','--revision',offered['revision'],ok=False);assert not (prefix/'delegated').exists()
    (fixtures/'updater.sha256').write_text(saved)
    package(protocol=2);offered=call('update-check')
    call('update-install','--revision',offered['revision'],ok=False);assert not (prefix/'delegated').exists()
    package();offered=call('update-check')
    result=call('update-install','--revision',offered['revision'])
    assert result['fresh_updater'] and (prefix/'delegated').read_text()=='fresh-engine-revision'
    assert (prefix/'shin-kit.json').read_bytes()==unknown and not list(home.iterdir())
    child=subprocess.Popen([str(tool),str(fixtures),str(prefix),str(home),'exec-update-install','--revision',offered['revision'],'--progress'],
                           stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,env={**os.environ,'LANG':'C','LC_ALL':'C'})
    output,error=child.communicate(timeout=20)
    assert child.returncode==0 and json.loads(output)['pid']==child.pid, (output,error)
    assert 'MiB' in error, 'the CLI bootstrap hid its actual transfer progress'
    with tempfile.TemporaryDirectory(prefix='shin-updater.',dir='/tmp') as retained:
        stage=Path(retained).resolve();payload=stage/'payload';payload.mkdir(mode=0o700)
        with zipfile.ZipFile(fixtures/'updater.zip') as archive:archive.extractall(payload)
        executable=payload/'shin-updater-v1/bin/shin-updater';executable.chmod(0o755)
        call('pin',str(stage))
        (fixtures/'feed.json').unlink()  # Offline status cannot consult the feed.
        assert call('update-status',ok=False)['phase']=='review-required'
        executable.write_bytes(executable.read_bytes()+b'changed')
        assert call('update-status',ok=False)['code']=='update-bootstrap-invalid'
print('Fresh updater handoff: unknown kit format, exact review, corrupt download, protocol refusal, exec PID, CLI transfer progress and offline result passed')
