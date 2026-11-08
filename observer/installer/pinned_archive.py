"""Fetch an immutable test kit; transport fallback never substitutes its identity."""
import hashlib
from pathlib import Path
import re
import subprocess
from urllib.parse import unquote, urlsplit


def download_pinned_archive(pin, destination, run=subprocess.run):
    destination = Path(destination)
    url = urlsplit(pin['url'])
    parts = [unquote(part) for part in url.path.strip('/').split('/')]
    if (url.scheme != 'https' or url.netloc != 'github.com' or url.query or url.fragment or
            len(parts) != 6 or parts[:4] != ['toseek-samurai', 'shin-context-engine', 'releases', 'download'] or
            parts[4:] != [pin['tag'], pin['archive']] or
            Path(pin['archive']).name != pin['archive'] or destination.name != pin['archive'] or
            not re.fullmatch(r'v[0-9]+\.[0-9]+\.[0-9]+(?:-(?:dev|rc)\.[0-9]+)?', pin['tag']) or
            not re.fullmatch(r'[a-f0-9]{64}', pin['sha256']) or
            type(pin['bytes']) is not int or not 0 < pin['bytes'] <= 512 * 1024 * 1024):
        raise ValueError('Invalid pinned test archive identity')
    if destination.exists() or destination.is_symlink():
        raise ValueError('Pinned archive destination already exists')
    try:
        try:
            run(['/usr/bin/curl', '--fail', '--location', '--proto', '=https', '--proto-redir', '=https',
                 '--retry', '2', '--retry-all-errors', '--retry-max-time', '60',
                 '--connect-timeout', '15', '--max-time', '120', '--max-filesize', str(pin['bytes']),
                 '--output', str(destination), pin['url']], check=True, timeout=300)
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired, OSError):
            # Resolve a fresh release-asset URL through the GitHub API when the
            # public download route/CDN fails. GH_TOKEN stays in the environment.
            destination.unlink(missing_ok=True)
            run(['gh', 'release', 'download', pin['tag'], '--repo', 'toseek-samurai/shin-context-engine',
                 '--pattern', pin['archive'], '--dir', str(destination.parent)], check=True, timeout=120)
        if destination.is_symlink() or destination.stat().st_size != pin['bytes']:
            raise ValueError('Pinned test archive size mismatch')
        with destination.open('rb') as stream:
            if hashlib.file_digest(stream, 'sha256').hexdigest() != pin['sha256']:
                raise ValueError('Pinned test archive checksum mismatch')
    except BaseException:
        destination.unlink(missing_ok=True)
        raise
    return destination
