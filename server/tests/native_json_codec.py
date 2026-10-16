"""Bounded test-only access to the protocol's native JSON serialization."""

from __future__ import annotations

import atexit
import json
import os
from pathlib import Path
import selectors
import subprocess
import threading
import time
from typing import Any


class NativeJsonCodec:
    def __init__(self, binary: Path) -> None:
        self.binary = binary
        self.process: subprocess.Popen[bytes] | None = None
        self.lock = threading.Lock()
        atexit.register(self.close)

    def _close(self, *, abort: bool = False) -> None:
        process, self.process = self.process, None
        if process is None:
            return
        assert process.stdin is not None and process.stdout is not None
        if abort:
            process.kill()
        process.stdin.close()
        try:
            status = process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
            raise RuntimeError("native JSON codec did not stop") from None
        finally:
            process.stdout.close()
        if status and not abort:
            raise RuntimeError("native JSON codec exited unsuccessfully")

    def close(self) -> None:
        with self.lock:
            self._close()

    def encode(self, value: Any) -> bytes:
        wire = json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode("utf-8") + b"\n"
        if len(wire) > 1024 * 1024:
            raise ValueError("native JSON fixture exceeds its input bound")
        with self.lock:
            if self.process is None:
                self.process = subprocess.Popen([str(self.binary)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
                assert self.process.stdin is not None and self.process.stdout is not None
                os.set_blocking(self.process.stdin.fileno(), False)
                os.set_blocking(self.process.stdout.fileno(), False)
            process = self.process
            assert process.stdin is not None and process.stdout is not None
            offset = 0
            output = bytearray()
            deadline = time.monotonic() + 10
            try:
                with selectors.DefaultSelector() as selector:
                    selector.register(process.stdin, selectors.EVENT_WRITE)
                    selector.register(process.stdout, selectors.EVENT_READ)
                    while not output.endswith(b"\n"):
                        remaining = deadline - time.monotonic()
                        if remaining <= 0:
                            raise TimeoutError("native JSON codec response timed out")
                        for key, event in selector.select(remaining):
                            if event & selectors.EVENT_WRITE:
                                offset += os.write(key.fd, wire[offset:])
                                if offset == len(wire):
                                    selector.unregister(process.stdin)
                            else:
                                chunk = os.read(key.fd, 65536)
                                if not chunk:
                                    raise RuntimeError("native JSON codec closed before responding")
                                output.extend(chunk)
                                if len(output) > 2 * 1024 * 1024:
                                    raise RuntimeError("native JSON codec exceeded its output bound")
                if offset != len(wire) or output.count(b"\n") != 1 or json.loads(output) != value:
                    raise RuntimeError("native JSON codec changed fixture semantics or framing")
                return bytes(output[:-1])
            except BaseException:
                self._close(abort=True)
                raise
