"""Keep platform IO out of the shared scheduler/history and product source list."""
from pathlib import Path
import re
import sys
import unittest

ROOT = Path(sys.argv.pop(1)).resolve()


class PlatformBoundaryTests(unittest.TestCase):
    def test_shared_policy_has_no_os_headers(self):
        for name in ('local_service.cpp', 'local_history.cpp', 'local_mcp_scope.cpp'):
            text = (ROOT / 'src' / name).read_text(encoding='utf-8')
            self.assertNotRegex(text, r'#\s*include\s*[<"](?:windows\.h|unistd\.h|poll\.h|spawn\.h|sys/)')
            self.assertNotRegex(text, r'::(?:recv|send|socket|waitpid|posix_spawn|lstat|getpid)\s*\(')

    def test_one_product_source_list(self):
        native = (ROOT / 'cmake/Native.cmake').read_text(encoding='utf-8')
        platform = (ROOT / 'platform/windows/CMakeLists.txt').read_text(encoding='utf-8')
        standalone = (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')
        embedded = (ROOT.parent / 'CMakeLists.txt').read_text(encoding='utf-8')
        self.assertEqual(len(re.findall(r'add_library\(shin-observer-native\b', native)), 1)
        self.assertNotIn('add_library(', platform)
        self.assertNotIn('add_library(shin-observer-native', standalone)
        self.assertIn('/cmake/Native.cmake', standalone)
        self.assertIn('/observer/cmake/Native.cmake', embedded)
        for target in ('shin-ce', 'shin-mcp', 'shin-observer'):
            self.assertIn(f'add_executable({target} ', native)

    def test_one_cold_worker_parent(self):
        for name in ('local_service_posix.cpp', 'local_worker_windows.cpp'):
            text = (ROOT / 'src' / name).read_text(encoding='utf-8')
            self.assertNotIn('Json qualify_local_project(', text)
            self.assertNotIn('Json request_local_worker(', text)
        common = (ROOT / 'src/local_worker.cpp').read_text(encoding='utf-8')
        self.assertIn('LocalWorkerProcess child(', common)

    def test_core_does_not_depend_on_companions(self):
        for file in (ROOT.parent / 'src').rglob('*'):
            if file.suffix not in ('.cpp', '.h', '.hpp'):
                continue
            includes = re.findall(r'^\s*#\s*include\s*[<"]([^>\"]+)', file.read_text(encoding='utf-8'), re.MULTILINE)
            self.assertFalse(any(re.search(r'(^|/)(observer|server|tray)/', name) for name in includes), str(file))

    def test_shared_monitor_views(self):
        common = (ROOT / 'tray/monitor_view.h').read_text(encoding='utf-8')
        self.assertNotRegex(common, r'NS(?:App|Image|Window|Workspace|UserDefaults)|#import|MTLTexture')
        for shell in ('main.mm', 'windows_main.cpp'):
            text = (ROOT / 'tray' / shell).read_text(encoding='utf-8')
            self.assertIn('#include "monitor_view.h"', text)
            self.assertNotRegex(text, r'void render_(?:models|projects|overview|project_diagnostics)\(')


unittest.main()
