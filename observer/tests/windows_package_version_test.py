"""Exercise the MSI identity policy without a Windows compiler or installation."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import uuid

CMAKE = sys.argv.pop(1)
MODULE = Path(__file__).resolve().parents[1] / 'cmake/WindowsVersion.cmake'
NAMESPACE = uuid.UUID('4D5D4627-1B39-4F4A-ABEE-17CE51649F28')


class WindowsPackageVersionTests(unittest.TestCase):
    def identity(self, version, valid=True):
        with tempfile.TemporaryDirectory(prefix='shin-msi-version-') as directory:
            root = Path(directory)
            script = root / 'identity.cmake'
            script.write_text(
                f'include("{MODULE.as_posix()}")\n'
                'shin_windows_package_identity("${INPUT}" version product)\n'
                f'file(WRITE "{(root / "result").as_posix()}" "${{version}}\\n${{product}}")\n',
                encoding='utf-8')
            result = subprocess.run([CMAKE, f'-DINPUT={version}', '-P', str(script)],
                                    capture_output=True, text=True, timeout=10)
            if not valid:
                self.assertNotEqual(result.returncode, 0, version)
                self.assertFalse((root / 'result').exists())
                return
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return (root / 'result').read_text().splitlines()

    def test_versions_preserve_semantic_order(self):
        cases = [('2.0.5-dev.0', '2.0.5000'), ('2.0.5-dev.53', '2.0.5053'),
                 ('2.0.5-dev.54', '2.0.5054'), ('2.0.5-dev.899', '2.0.5899'),
                 ('2.0.5-rc.0', '2.0.5900'), ('2.0.5-rc.98', '2.0.5998'),
                 ('2.0.5', '2.0.5999'), ('2.0.6-dev.0', '2.0.6000'),
                 ('2.1.0-dev.0', '2.1.0'), ('3.0.0-dev.0', '3.0.0')]
        previous = (2, 0, 5)  # Previously unpacked numeric preview prefix.
        products = set()
        for source, expected in cases:
            with self.subTest(version=source):
                version, product = self.identity(source)
                self.assertEqual(version, expected)
                current = tuple(map(int, version.split('.')))
                self.assertGreater(current, previous)
                previous = current
                self.assertEqual(product, str(uuid.uuid5(NAMESPACE, f'windows-x64:{source}')).upper())
                self.assertNotIn(product, products)
                products.add(product)
        self.assertEqual(self.identity('2.0.5-dev.53'), self.identity('2.0.5-dev.53'))

    def test_boundary_and_invalid_versions(self):
        self.assertEqual(self.identity('255.255.64')[0], '255.255.64999')
        for version in ('256.0.0', '0.256.0', '0.0.65', '2.0.5-dev.900',
                        '2.0.5-rc.99', '2.0.5-dev.99999999999999999999',
                        '02.0.5', '2.00.5', '2.0.05', '2.0.5-dev.01',
                        '2.0.5-beta.1', '2.0.5.1', '2.0.5+build', '', '2.0.5\n'):
            with self.subTest(version=version):
                self.identity(version, valid=False)

    def test_packaging_refuses_same_version_replacement_and_unknown_template(self):
        template = '<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs"><MajorUpgrade AllowSameVersionUpgrades="yes"/></Wix>'
        for original, accepted, tool_version in (
                (template, True, '4.3'), (template, False, '4.2'),
                (template.replace('schemas/v4', 'schemas/v3'), False, '4.3'),
                (template.replace('AllowSameVersionUpgrades="yes"', ''), False, '4.3'),
                (template + template, False, '4.3')):
            with self.subTest(template=original, version=tool_version), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                installed = root / 'Modules/Internal/CPack/WIX.template.in'
                installed.parent.mkdir(parents=True)
                installed.write_text(original)
                output = root / 'ShinWIX.template.in'
                script = root / 'package.cmake'
                script.write_text(f'set(CMAKE_ROOT "{root.as_posix()}")\n'
                                  f'set(CMAKE_VERSION "{tool_version}")\nset(CPACK_GENERATOR WIX)\n'
                                  f'set(CPACK_SHIN_WIX_TEMPLATE "{output.as_posix()}")\n'
                                  f'include("{(MODULE.parent / "WindowsPackage.cmake").as_posix()}")\n')
                result = subprocess.run([CMAKE, '-P', str(script)], capture_output=True, text=True, timeout=10)
                if accepted:
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(output.read_text(), original.replace('Upgrades="yes"', 'Upgrades="no"'))
                else:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
