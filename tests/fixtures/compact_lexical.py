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
