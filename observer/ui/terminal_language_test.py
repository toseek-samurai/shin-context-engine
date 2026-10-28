"""Exercise the native locale resolver with isolated child environments."""
import os
from pathlib import Path
import subprocess
import sys
import unittest

TOOL = str(Path(sys.argv.pop(1)).resolve())
LOCALE_KEYS = {'LC_ALL', 'LC_MESSAGES', 'LANG'}


class TerminalLanguageTests(unittest.TestCase):
    def check_locale(self, values, expected, preference='auto'):
        env = {key: value for key, value in os.environ.items() if key.upper() not in LOCALE_KEYS}
        env.update(values)
        result = subprocess.run([TOOL, preference, expected], env=env,
                                capture_output=True, encoding='utf-8', timeout=10)
        self.assertEqual(result.returncode, 0, (values, preference, expected, result.stdout, result.stderr))

    def test_environment_precedence(self):
        for values, expected in [
            ({'LC_ALL': 'en_US.UTF-8', 'LC_MESSAGES': 'ru_RU.UTF-8', 'LANG': 'ru'}, 'en'),
            ({'LC_ALL': 'ru_RU.UTF-8', 'LC_MESSAGES': 'en', 'LANG': 'en'}, 'ru'),
            ({'LC_MESSAGES': 'en_US.UTF-8', 'LANG': 'ru'}, 'en'),
            ({'LC_MESSAGES': 'ru_RU.UTF-8', 'LANG': 'en'}, 'ru'),
        ]:
            with self.subTest(values=values):
                self.check_locale(values, expected)

    def test_empty_and_missing_values(self):
        for values in ({'LANG': 'ru'}, {'LC_ALL': '', 'LC_MESSAGES': 'ru', 'LANG': 'en'},
                       {'LC_ALL': '', 'LC_MESSAGES': '', 'LANG': 'ru'}):
            with self.subTest(values=values):
                self.check_locale(values, 'ru')
        self.check_locale({'LANG': 'ru'}, 'ru', preference='')

    def test_fallback(self):
        for values in ({}, {'LC_ALL': '', 'LC_MESSAGES': '', 'LANG': ''},
                       {'LC_ALL': 'de_DE.UTF-8', 'LANG': 'ru'}, {'LANG': 'rubbish'}):
            with self.subTest(values=values):
                self.check_locale(values, 'en')

    def test_explicit_preference(self):
        for preference in ('en', 'ru'):
            for values in ({}, {'LC_ALL': 'en'}, {'LC_ALL': 'ru'}):
                with self.subTest(preference=preference, values=values):
                    self.check_locale(values, preference, preference)


if __name__ == '__main__':
    unittest.main()
