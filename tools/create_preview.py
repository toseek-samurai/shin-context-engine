#!/usr/bin/env python3
"""Prepare a preview on main and dispatch the existing qualified release pipeline."""
import json
import os
from pathlib import Path
import re
import subprocess

from publish_preview import VERSION, require

POLICY = 'docs/machine/shin-development-v1.json'
BOT = ['-c', 'user.name=github-actions[bot]', '-c',
       'user.email=41898282+github-actions[bot]@users.noreply.github.com']


def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args], text=True, timeout=30).strip()


def identity(version):
    require(re.fullmatch(VERSION, version), 'Expected a canonical preview version')
    return tuple(int(p) for p in re.split(r'\.|-dev\.', version))


def prepare(root, notes=''):
    """Return (tag, changed paths); caller owns the commit and remote mutations."""
    root = Path(root)
    current = (root / 'observer/VERSION').read_text().strip()
    identity(current)
    core = (root / 'VERSION').read_text().strip()
    policy = json.loads((root / POLICY).read_text())
    require(policy['developmentLane']['coreVersion'] == core and
            policy['developmentLane']['observerSourceVersion'] == current,
            'Component versions differ from the development policy')
    require('\0' not in notes and len(notes.encode('utf-8')) <= 8192, 'Release notes exceed their bound')
    tags = set(git(root, 'tag', '--list', 'v*').splitlines())
    tag = 'v' + current
    same_commit = tag in tags and git(root, 'rev-parse', tag + '^{commit}') == git(root, 'rev-parse', 'HEAD')
    version = current
    if tag in tags and not same_commit:
        prefix, number = current.rsplit('.', 1)
        version = prefix + '.' + str(int(number) + 1)
    candidate = identity(version)
    known = [identity(t[1:]) for t in tags if re.fullmatch('v' + VERSION, t)]
    require(not known or candidate >= max(known), 'main is behind a newer preview; update main first')
    tag = 'v' + version
    require(tag not in tags or same_commit, 'Preview tag already exists; it will not be moved')
    note_path = 'docs/releases/' + tag + '.md'
    if same_commit:
        require(not notes.strip(), 'This preview already exists; leave Notes empty to retry its CI')
        require((root / note_path).is_file(), 'Existing preview has no release notes')
        return tag, []
    require(not (root / note_path).is_symlink(), 'Release notes must be a regular source file')
    changes = {}
    if version != current:
        policy['developmentLane']['observerSourceVersion'] = version
        changes['observer/VERSION'] = version + '\n'
        changes[POLICY] = json.dumps(policy, indent=2) + '\n'
    if (root / note_path).exists():
        require(not notes.strip(), 'Prepared notes already exist; leave Notes empty or edit them on main')
        require((root / note_path).read_text().startswith('# ' + tag + '\n'), 'Prepared notes have the wrong title')
    else:
        if not notes.strip():
            previous = max((t for t in tags if re.fullmatch('v' + VERSION, t)),
                           key=lambda t: identity(t[1:]), default=None)
            revision = previous + '..HEAD' if previous else 'HEAD'
            subjects = git(root, 'log', '--format=%s', '--max-count=30', revision).splitlines()
            notes = '\n'.join('- ' + subject for subject in subjects)
        require(len(notes.encode('utf-8')) <= 8192, 'Commit subjects are too large; supply shorter release notes')
        changes[note_path] = (
            '# ' + tag + '\n\nPreview candidate. Each platform is published after its source and exact archive\n'
            'pass qualification in CI.\n\n' + notes.strip() + '\n\n'
            f'Core is `{core}`. Observer, CLI, MCP, Monitor and Updater are `{version}`.\n')
    for name, value in changes.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(value)
    return tag, list(changes)


def main():
    require(os.environ.get('GITHUB_REF') == 'refs/heads/main' and
            os.environ.get('GITHUB_EVENT_NAME') == 'workflow_dispatch', 'Run Release preview from main')
    root = Path(__file__).resolve().parents[1]
    repo = os.environ['GITHUB_REPOSITORY']
    require(re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repo), 'Invalid repository')
    require(not git(root, 'status', '--porcelain'), 'Release checkout is not clean')
    git(root, 'fetch', 'origin', 'main', '--tags')
    require(git(root, 'rev-parse', 'HEAD') == os.environ['GITHUB_SHA'] ==
            git(root, 'rev-parse', 'origin/main'), 'main moved; start Release preview again')
    tag, changed = prepare(root, os.environ.get('RELEASE_NOTES', ''))
    if changed:
        git(root, 'add', '--', *changed)
        git(root, *BOT, 'commit', '-m', 'chore(release): prepare preview ' + tag[1:])
    if tag not in git(root, 'tag', '--list', tag).splitlines():
        git(root, *BOT, 'tag', '-a', tag, '-m', 'Shin preview ' + tag[1:])
    sha = git(root, 'rev-parse', 'HEAD')
    # Atomic, non-forced push: a concurrent main update cannot leave a stale tag.
    git(root, 'push', '--atomic', 'origin', 'HEAD:refs/heads/main', 'refs/tags/' + tag)
    # GITHUB_TOKEN pushes do not trigger push workflows. Explicit dispatch does.
    subprocess.run(['gh', 'workflow', 'run', 'ci.yml', '--repo', repo, '--ref', tag],
                   cwd=root, check=True, timeout=30)
    summary = (f'## Preview {tag}\n\n'
               f'Source: [{sha[:12]}](https://github.com/{repo}/commit/{sha}).\n\n'
               f'[Follow builds, tests and publication](https://github.com/{repo}/actions/workflows/ci.yml?query=branch%3A{tag}).\n\n'
               'This step starts the release pipeline. Downloads appear independently after each platform passes '
               'its Core, Observer and exact-package checks. No local assets are uploaded.\n')
    with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as report:
        report.write(summary)
    print('Started qualified release pipeline for ' + tag + ' at ' + sha)


if __name__ == '__main__':
    main()
