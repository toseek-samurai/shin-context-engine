"""Check the compiled bilingual catalog, format safety and literal UI coverage."""
from pathlib import Path
import json
import re

root = Path(__file__).resolve().parents[2]
string = r'"(?:\\.|[^"\\])*"'
rows = [tuple(map(json.loads, row)) for row in re.findall(r'\{(' + string + r'), (' + string + r')\}', (root / 'observer/ui/messages.inc').read_text(encoding='utf-8'))]
assert len(rows) > 500 and len(dict(rows)) == len(rows)
assert rows == sorted(rows), 'catalog lookup requires sorted keys'
formats = re.compile(r'%(?:%|(?:\d+\$)?[-+ #0]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|ll|[hljztL])?[@diuoxXfFeEgGaAcspn])')
for en, ru in rows:
    assert formats.findall(en) == formats.findall(ru), (en, ru, 'format placeholders changed')
    assert '%n' not in en and '%n' not in ru
    assert en and ru
catalog = dict(rows)
for folder in ('observer/ui', 'observer/tui', 'observer/tray'):
    for path in (root / folder).iterdir():
        if path.suffix not in ('.h', '.cpp', '.mm') or 'test' in path.name: continue
        text = path.read_text(encoding='utf-8')
        for literal in re.findall(r'(?:\btr|ui_text)\((' + string + r')\)', text):
            assert json.loads(literal) in catalog, (path, literal, 'missing translation')
        for literal in re.findall(r'live_(?:label|button)\((' + string + r')', text) + re.findall(r'live_text\([^;\n]*?,\s*@"[^"]+",\s*(' + string + r')\)', text):
            assert json.loads(literal) in catalog, (path, literal, 'missing live translation')
        for literal in re.findall(string, text):
            value = json.loads(literal)
            assert not re.search('[А-Яа-яЁё]', value) or value == 'Русский', (path, value, 'untranslated Russian literal')
print(f'{len(rows)} EN/RU messages; format parity and UI literal coverage passed')
