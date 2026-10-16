"""Exercise the MSBuild path guard without needing a Windows compiler."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

CMAKE, POLICY = sys.argv[1:]
SCRIPT = Path(__file__).with_name('windows_build_placement.cmake')
with tempfile.TemporaryDirectory(prefix='shin-build-placement-') as temporary:
    base = Path(temporary).resolve()
    for name in ('appdata', 'roaming', 'temp', 'source', 'build', 'appdata-sibling'):
        (base / name).mkdir()
    env = {**os.environ, 'LOCALAPPDATA': str(base / 'appdata'), 'APPDATA': str(base / 'roaming'),
           'TEMP': str(base / 'temp'), 'TMP': str(base / 'temp'), 'USERPROFILE': str(base)}
    for source, build, generator, refused in (
        ('source', 'build', 'Visual Studio 17 2022', False),
        ('appdata', 'build', 'Visual Studio 17 2022', True),
        ('source', 'appdata', 'Visual Studio 17 2022', True),
        ('roaming', 'build', 'Visual Studio 17 2022', True),
        ('temp', 'build', 'Visual Studio 17 2022', True),
        ('appdata-sibling', 'build', 'Visual Studio 17 2022', False),
        ('appdata', 'temp', 'Ninja', False),
    ):
        result = subprocess.run([CMAKE, f'-DTEST_SOURCE={base / source}', f'-DTEST_BUILD={base / build}',
            f'-DTEST_GENERATOR={generator}', f'-DTEST_POLICY={POLICY}', '-P', str(SCRIPT)],
            env=env, capture_output=True, text=True, timeout=15)
        assert bool(result.returncode) == refused, (source, build, generator, result.stdout, result.stderr)
        if refused:
            assert 'MSBuild cannot reliably track Shin' in result.stderr
print('MSBuild source/build placement guard: 7 cases passed')
