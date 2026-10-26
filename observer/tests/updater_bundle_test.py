"""Stable updater envelope refusal tests; no installation or network."""
import hashlib
import json
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import zipfile

check=Path(sys.argv[1])

def run(*args, ok=True):
    result=subprocess.run([str(check),*map(str,args)],capture_output=True,text=True,timeout=5)
    assert (result.returncode==0)==ok,(args,result.stdout,result.stderr)
    return result

with tempfile.TemporaryDirectory(prefix='shin-updater-envelope-') as folder:
    base=Path(folder).resolve(); root=base/'shin-updater-v1'; (root/'bin').mkdir(parents=True,mode=0o700);root.chmod(0o700)
    payload=b'independent updater fixture'
    binary=root/'bin/shin-updater'; binary.write_bytes(payload);binary.chmod(0o755)
    manifest={'schema_version':'shin.updater.v1','protocol':1,'version':'2.0.5-dev.99','files':[
        {'path':'bin/shin-updater','sha256':hashlib.sha256(payload).hexdigest(),'size':len(payload),'executable':True}]}
    record=root/'shin-updater.json';record.write_text(json.dumps(manifest));record.chmod(0o644)
    run('bundle',root)
    (root/'extra').write_text('preserve');run('bundle',root,ok=False);run('bundle',root,'inside-kit');(root/'extra').unlink()
    binary.write_bytes(payload+b'changed');run('bundle',root,ok=False);binary.write_bytes(payload)
    binary.unlink();binary.symlink_to(record);run('bundle',root,ok=False);binary.unlink();binary.write_bytes(payload);binary.chmod(0o755)
    link=base/'hardlink';link.hardlink_to(binary);run('bundle',root,ok=False);link.unlink()
    original=record.read_bytes()
    manifest['protocol']=2;record.write_text(json.dumps(manifest));run('bundle',root,ok=False);record.write_bytes(original)
    for field,value in [('protocol','1'),('schema_version',[]),('files',{})]:
        malformed=json.loads(original);malformed[field]=value;record.write_text(json.dumps(malformed))
        assert 'stable protocol' in run('bundle',root,ok=False).stderr
    record.write_bytes(original)
    archive=base/'updater.zip'
    def zip_case(extra=None, mode=None, metadata=False):
        with zipfile.ZipFile(archive,'w',compression=zipfile.ZIP_DEFLATED) as out:
            out.write(record,'shin-updater-v1/shin-updater.json')
            out.write(binary,'shin-updater-v1/bin/shin-updater')
            if extra:
                entry=zipfile.ZipInfo(extra);entry.create_system=3
                entry.external_attr=((mode or (stat.S_IFREG|0o600))<<16)
                if metadata:entry.extra=b'\x6e\x75\x00\x00'
                out.writestr(entry,b'/tmp/escape')
    zip_case();run('archive',archive)
    for name in ['../escape','/tmp/escape','shin-updater-v1/../escape','other/root','shin-updater-v1/bin/shin-updater']:
        zip_case(name);run('archive',archive,ok=False)
    zip_case('shin-updater-v1/link',stat.S_IFLNK|0o777);run('archive',archive,ok=False)
    zip_case('shin-updater-v1/extra',metadata=True);run('archive',archive,ok=False)
    zip_case();archive.write_bytes(archive.read_bytes()[:-8]);run('archive',archive,ok=False)
print('Stable updater: hashes, protocol, complete inventory, kit independence, paths, links, duplicates, ZIP structure passed')
