"""Independent lexical fixture decoder and legacy fixture transcode, never product code."""
import json
import struct
import sys
from pathlib import Path

def term_postings(path):
    """Independent fixture decoder; production never depends on Python."""
    data = path.read_bytes()
    assert data[:8] == b"SHINLEX1"
    count = struct.unpack_from("<I", data, 8)[0]
    offsets = struct.unpack_from(f"<{count + 1}Q", data, 32)
    for start, end in zip(offsets, offsets[1:]):
        position = start

        def integer():
            nonlocal position
            value, shift = 0, 0
            while True:
                byte = data[position]
                position += 1
                value |= (byte & 127) << shift
                if byte < 128:
                    return value
                shift += 7

        length = integer()
        row = {"term": data[position:position + length].decode()}
        position += length
        for kind in ("sections", "facts", "decisions", "contracts", "runbooks"):
            values, previous = [], -1
            for _ in range(integer()):
                previous += integer()
                values.append(previous)
            row[kind] = values
        assert position == end
        yield row


def fnv(data):
    value = 1469598103934665603  # Existing Shin wire offset; preserve compatibility.
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return f"fnv1a64:{value:016x}"


def legacy_fixture(root):
    records, offsets, offset = [], [], 0
    for row in term_postings(root / "terms.lex"):
        record = (json.dumps(row, ensure_ascii=False, separators=(",", ":")) + "\n").encode()
        records.append(record)
        offsets.append(f"{row['term']}\t{offset}\t{len(record)}\n".encode())
        offset += len(record)
    contents = {"terms.index.tsv": b"".join(offsets), "terms.jsonl": b"".join(records)}
    manifest_path = root / "manifest.json"
    text = manifest_path.read_text()
    manifest = json.loads(text)
    count = manifest["row_counts"]["terms"]
    text = text.replace("shin-generator-v22-compact-lexical", "shin-generator-v21-cpp-syntax")
    text = text.replace('  "term_index_format": "shin.lexical.v1",\n', "")
    new_output = f'    {{"name":"terms","type":"shin.lexical.v1","path":"docs/kb/generated/terms.lex","rows":{count}}},\n'
    old_output = (f'    {{"name":"terms","type":"jsonl","path":"docs/kb/generated/terms.jsonl","rows":{count}}},\n'
                  f'    {{"name":"terms-index","type":"tsv","path":"docs/kb/generated/terms.index.tsv","rows":{count}}},\n')
    assert new_output in text
    text = text.replace(new_output, old_output)
    integrity_line = next(line for line in text.splitlines(True) if line.startswith('    {"path":"docs/kb/generated/terms.lex"'))
    replacement = ""
    for name, data in contents.items():
        (root / name).write_bytes(data)
        replacement += f'    {{"path":"docs/kb/generated/{name}","bytes":{len(data)},"content_hash":"{fnv(data)}"}},\n'
    text = text.replace(integrity_line, replacement)
    manifest_path.write_text(text)
    (root / "terms.lex").unlink()


if __name__ == "__main__":
    legacy_fixture(Path(sys.argv[1]))
