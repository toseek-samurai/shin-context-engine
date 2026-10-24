#!/usr/bin/env python3
"""Lossless client settings edits, preservation, idempotence and stale-write refusal."""
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import tomllib

ce = Path(sys.argv[1]).resolve()
# This standalone test models private per-user settings regardless of host umask.
# Explicit unsafe-permission cases below retain the refusal boundary.
os.umask(0o077)

def run(*args, ok=True, env=None):
    p = subprocess.run([str(ce), *map(str,args)], capture_output=True, text=True, timeout=20, env=env)
    assert (p.returncode == 0) == ok, (args,p.stdout,p.stderr)
    return json.loads(p.stdout if ok else p.stderr)

with tempfile.TemporaryDirectory(prefix='shin-client-connect-',dir='/tmp') as temp:
    base=Path(temp).resolve(); root=base/'project café'; root.mkdir(); old=base/'old project'; old.mkdir(); home=base/'data'
    server=run('mcp-config','--root',root,'--home',home)['server']
    old_server={**server,'args':['--root',str(old),'--home',str(home)],'enabled':False}
    args=json.dumps(old_server['args'],ensure_ascii=False); command=json.dumps(server['command'])
    fixtures={
        'codex': '# keep café\nmodel = "unchanged"\n[mcp_servers.other]\ncommand = "other"\n[mcp_servers.shin]\ncommand = '+command+'\nargs = '+args+' # keep suffix\nenabled = false\n[mcp_servers.shin.env]\nKEEP = "private-value"\n',
        'opencode': '{\n// keep café\n"model":"unchanged", "nested":[{"mcp":{"shin":0}}],\n"mcp":{"other":{"type":"remote","url":"https://example.test/a//b"}, "shin":{"type":"local","command":'+json.dumps([server['command'],*old_server['args']],ensure_ascii=False)+',"enabled":false,"environment":{"KEEP":"private-value"},},},\n}\n',
        'command-code': json.dumps({'preferences':{'keep':True},'mcpServers':{'other':{'command':'other'},'shin':old_server}},indent=2)+'\n',
        'hermes': '# keep café\nmodel: unchanged\nmcp_servers:\n  other:\n    command: other\n  shin:\n    command: '+command+'\n    args:\n      - "--root"\n      - '+json.dumps(str(old),ensure_ascii=False)+'\n      - "--home"\n      - '+json.dumps(str(home))+'\n    enabled: false\n    env: {KEEP: private-value}\n# keep footer\nterminal:\n  backend: local\n',
    }
    extensions={'codex':'toml','opencode':'jsonc','command-code':'json','hermes':'yaml'}
    for client, original in fixtures.items():
        directory=base/client;directory.mkdir();config=directory/('config.'+extensions[client]);config.write_text(original);config.chmod(0o644)
        def preview():return run('client-connect','--client',client,'--root',root,'--home',home,'--config',config)
        def apply(plan):return run('client-connect','--client',client,'--root',root,'--home',home,'--config',config,'--revision',plan['revision'])
        plan=preview();assert plan['changed'] and not plan['client_modified'];assert config.read_text()==original
        result=apply(plan);assert result['client_modified'] and result['status']=='configured'
        backup=Path(result['backup']);assert backup.read_text()==original and stat.S_IMODE(backup.stat().st_mode)==0o600
        assert stat.S_IMODE(config.stat().st_mode)==0o644
        updated=config.read_text();assert 'private-value' in updated or client=='command-code'
        if client=='codex':
            value=tomllib.loads(updated);assert value['model']=='unchanged' and value['mcp_servers']['other']['command']=='other'
            assert value['mcp_servers']['shin']['args']==old_server['args']+['--root',str(root)]
            assert '# keep café' in updated and '# keep suffix' in updated
        elif client=='command-code':
            value=json.loads(updated);assert value['preferences']=={'keep':True}
            assert value['mcpServers']['shin']['args']==old_server['args']+['--root',str(root)]
        elif client=='opencode':
            assert '// keep café' in updated and 'https://example.test/a//b' in updated
            assert '"nested":[{"mcp":{"shin":0}}]' in updated
        else:
            assert updated.startswith('# keep café\nmodel: unchanged\nmcp_servers:\n  other:\n    command: other\n')
            assert updated.endswith('    env: {KEEP: private-value}\n# keep footer\nterminal:\n  backend: local\n')
            assert json.dumps(str(root),ensure_ascii=False) in updated
        again=apply(preview());assert not again['changed'] and not again['client_modified'];assert config.read_text()==updated
        # A later editor change invalidates the exact revision, never clobbers it.
        config.write_text(original);plan=preview();changed=original+'\n';config.write_text(changed)
        error=run('client-connect','--client',client,'--root',root,'--home',home,'--config',config,'--revision',plan['revision'],ok=False)
        assert error['error']['code']=='client-config-changed' and config.read_text()==changed
    # Creating settings preserves unrelated comments and supports inline TOML/YAML mappings.
    for client, source in [('codex','mcp_servers = { other = { command = "other" } } # café\n'),
                           ('codex','model="unchanged"\n'),('opencode','{/* keep */ "mcp": {},}'),
                           ('command-code','{"mcpServers":{"other":{"command":"other"}}}'),
                           ('hermes','mcp_servers: {other: {command: other}} # keep\n'),
                           ('hermes','model: unchanged\n'),('hermes','{}')]:
        config=base/('new-'+client+'.'+extensions[client]);config.write_text(source)
        plan=run('client-connect','--client',client,'--root',root,'--home',home,'--config',config)
        run('client-connect','--client',client,'--root',root,'--home',home,'--config',config,'--revision',plan['revision'])
        assert not run('client-connect','--client',client,'--root',root,'--home',home,'--config',config)['changed']
    for client in fixtures:
        config=base/'missing'/client/('config.'+extensions[client])
        plan=run('client-connect','--client',client,'--root',root,'--home',home,'--config',config)
        assert not config.parent.exists()
        run('client-connect','--client',client,'--root',root,'--home',home,'--config',config,'--revision',plan['revision'])
        assert config.exists() and stat.S_IMODE(config.stat().st_mode)==0o600
    # Unsupported/ambiguous input is not rewritten, and diagnostic output never contains values.
    for client, source in [('codex','secret = "do-not-echo"\n[bad'),('opencode','{"secret":"do-not-echo","mcp":{},"mcp":{}}'),
                           ('hermes','secret: &keep do-not-echo\ncopy: *keep\n'),
                           ('command-code','{"mcpServers":{"shin":{"command":"other","args":[]}}}')]:
        config=base/('bad.'+extensions[client]);config.write_text(source)
        error=run('client-connect','--client',client,'--root',root,'--home',home,'--config',config,ok=False)
        assert config.read_text()==source and 'do-not-echo' not in json.dumps(error)
    # Inline UTF-8 before the edited node must not corrupt source offsets.
    for client, source in [
        ('codex', 'mcp_servers = { shin = { command = '+command+', note="café", args='+args+' } } # keep\n'),
        ('hermes', 'mcp_servers: {shin: {note: "café", command: '+command+', args: '+args+'}} # keep\n')]:
        config=base/('unicode.'+extensions[client]);config.write_text(source)
        plan=run('client-connect','--client',client,'--root',root,'--home',home,'--config',config)
        run('client-connect','--client',client,'--root',root,'--home',home,'--config',config,'--revision',plan['revision'])
        assert '# keep' in config.read_text() and 'café' in config.read_text()
    # Custom locations are discovered without creating them; one bad override
    # must not hide every other supported agent.
    env={**os.environ,'CODEX_HOME':str(base/'custom-codex'),'XDG_CONFIG_HOME':str(base/'custom-xdg'),'HERMES_HOME':str(base/'custom-hermes')}
    targets=run('client-targets',env=env)['targets']
    assert any(t['client']=='codex' and t['config']==str(base/'custom-codex/config.toml') for t in targets)
    assert any(t['client']=='opencode' and t['config']==str(base/'custom-xdg/opencode/opencode.json') for t in targets)
    assert not (base/'custom-codex').exists()
    env['CODEX_HOME']='relative-path'
    targets=run('client-targets',env=env)['targets']
    assert any(t['client']=='codex' and t['choose_config_required'] for t in targets)
    assert any(t['client']=='opencode' and t['config'] for t in targets)
    # Legacy implicit-root or different-home settings are not silently retargeted.
    for entry in [
        {**server, 'args':['--home',str(home)]},
        {**server, 'args':['--root',str(old),'--home',str(base/'other-data')]},
        {**server, 'transport':'http'},
    ]:
        config=base/'legacy.json';original=json.dumps({'mcpServers':{'shin':entry}});config.write_text(original)
        run('client-connect','--client','command-code','--root',root,'--home',home,'--config',config,ok=False)
        assert config.read_text()==original
    # Two reviewed plans cannot overwrite each other. The losing caller can
    # re-preview and add its root without losing the winner or existing roots.
    config=base/'concurrent.json';config.write_text(json.dumps({'mcpServers':{'shin':old_server}}))
    other=base/'other project';other.mkdir()
    commands=[]
    for requested in [root,other]:
        words=['client-connect','--client','command-code','--root',str(requested),'--home',str(home),'--config',str(config)]
        plan=run(*words);commands.append([str(ce),*words,'--revision',plan['revision']])
    children=[subprocess.Popen(words,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True) for words in commands]
    responses=[child.communicate(timeout=20) for child in children]
    assert sum(child.returncode==0 for child in children)==1,responses
    for requested in [root,other]:
        words=['client-connect','--client','command-code','--root',requested,'--home',home,'--config',config]
        plan=run(*words);run(*words,'--revision',plan['revision'])
    saved=json.loads(config.read_text())['mcpServers']['shin']['args']
    assert all(saved.count(str(p))==1 for p in [old,root,other])
    # Do not follow links or replace shared inodes.
    unsafe_parent=base/'group-writable';unsafe_parent.mkdir();config=unsafe_parent/'config.json';config.write_text('{}')
    plan=run('client-connect','--client','command-code','--root',root,'--home',home,'--config',config)
    unsafe_parent.chmod(0o770)
    error=run('client-connect','--client','command-code','--root',root,'--home',home,'--config',config,'--revision',plan['revision'],ok=False)
    assert error['error']['code']=='unsafe-directory' and config.read_text()=='{}'
    config=base/'linked.json';target=base/'target.json';target.write_text('{}');config.symlink_to(target)
    run('client-connect','--client','command-code','--root',root,'--home',home,'--config',config,ok=False)
    assert target.read_text()=='{}'
    config.unlink();os.link(target,config)
    plan=run('client-connect','--client','command-code','--root',root,'--home',home,'--config',config)
    run('client-connect','--client','command-code','--root',root,'--home',home,'--config',config,'--revision',plan['revision'],ok=False)
    assert target.read_text()=='{}' and config.read_text()=='{}'
    assert not home.exists(), 'agent configuration must not create a service or enroll projects'
print('Four formats: preservation, comments, backups, idempotence, stale edits, new files and unsafe-path refusals passed')
