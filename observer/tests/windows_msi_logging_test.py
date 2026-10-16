"""Exercise WiX's native output capture with a harmless, deliberately refused command."""
import ctypes
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import uuid
import winreg
import xml.etree.ElementTree as ET


def main():
    wix, ce = (Path(value).resolve() for value in sys.argv[1:3])
    base = Path(tempfile.mkdtemp(prefix='shin-msi-log-proof-'))
    product = '{' + str(uuid.uuid4()).upper() + '}'
    key = 'Software\\Shin\\InstallerTests\\' + str(uuid.uuid4())
    ns = 'http://wixtoolset.org/schemas/v4/wxs'
    ET.register_namespace('', ns)

    def node(parent, name, **attributes):
        return ET.SubElement(parent, '{' + ns + '}' + name, attributes)

    root = ET.Element('{' + ns + '}Wix')
    package = node(root, 'Package', Name='Shin Installer Log Fixture', Manufacturer='Shin',
                   Version='1.0.0', Scope='perUser', ProductCode=product, UpgradeCode=str(uuid.uuid4()))
    node(package, 'MediaTemplate', EmbedCab='yes')
    node(package, 'Property', Id='MsiLogging', Value='voicewarmup')
    directory = node(node(package, 'StandardDirectory', Id='LocalAppDataFolder'), 'Directory', Id='FixtureRoot', Name='ShinLogFixture')
    component = node(directory, 'Component', Id='FixtureMarker', Guid='*')
    node(component, 'RegistryValue', Root='HKCU', Key=key, Name='marker', Type='integer', Value='1', KeyPath='yes')
    node(node(package, 'Feature', Id='Main'), 'ComponentRef', Id='FixtureMarker')
    node(package, 'SetProperty', Id='RefusedNativeCommand', Value=f'"{ce}" _scm-install',
         Before='RefusedNativeCommand', Sequence='execute')
    node(package, 'CustomAction', Id='RefusedNativeCommand', BinaryRef='Wix4UtilCA_X64',
         DllEntry='WixQuietExec', Execute='deferred', Impersonate='yes', Return='check')
    node(node(package, 'InstallExecuteSequence'), 'Custom', Action='RefusedNativeCommand', After='InstallFiles', Condition='NOT Installed')
    source, msi, log = base / 'fixture.wxs', base / 'fixture.msi', base / 'fixture.log'
    ET.ElementTree(root).write(source, encoding='utf-8', xml_declaration=True)
    api = ctypes.WinDLL('msi')
    api.MsiQueryProductStateW.argtypes = [ctypes.c_wchar_p]
    api.MsiQueryProductStateW.restype = ctypes.c_int
    try:
        build = subprocess.run([str(wix), 'build', '-arch', 'x64', '-ext', 'WixToolset.Util.wixext',
                                str(source), '-o', str(msi)], capture_output=True, text=True, timeout=60)
        assert build.returncode == 0, (build.stdout, build.stderr)
        result = subprocess.run(['msiexec.exe', '/i', str(msi), '/qn', '/norestart', '/L*V', str(log)],
                                capture_output=True, timeout=90)
        assert result.returncode == 1603, (result.returncode, str(log))
        raw = log.read_bytes()
        text = raw.decode('utf-16' if raw.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf-8', errors='replace')
        assert 'WixQuietExec' in text and 'unsafe-service-executable' in text, 'Native JSON diagnostic was lost'
        assert api.MsiQueryProductStateW(product) != 5, 'Failed fixture remained installed'
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key):
                raise AssertionError('Fixture marker survived rollback')
        except FileNotFoundError:
            pass
        print('Real MSI failure captured the native error code and rolled back its isolated marker; no service was created')
    except BaseException:
        print(f'Failure evidence retained: {base}', flush=True)
        raise
    finally:
        if api.MsiQueryProductStateW(product) == 5:
            result = subprocess.run(['msiexec.exe', '/x', product, '/qn', '/norestart'], capture_output=True, timeout=90)
            assert result.returncode == 0, 'Fixture removal failed'
    shutil.rmtree(base)


if __name__ == '__main__':
    main()
