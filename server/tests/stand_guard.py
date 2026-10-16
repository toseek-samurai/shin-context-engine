"""Process-lifetime exclusion for the one shared development PostgreSQL stand."""

import errno
import fcntl
import os
from pathlib import Path
import stat
import socket


def require_http_port_available(host: str, port: int) -> None:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            probe.bind((host, port))
            return
        except OSError as error:
            if error.errno != errno.EADDRINUSE:
                raise
            # A closed SO_REUSEPORT-only listener can retain TIME_WAIT on Linux.
            # Do not enable REUSEPORT on the guard: that would admit a live listener.
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as connection:
                connection.settimeout(0.25)
                if connection.connect_ex((host, port)) != errno.ECONNREFUSED:
                    raise error


class StandGuard:
    def __init__(self) -> None:
        self._fd = -1
        path = Path(__file__).resolve().parents[2] / "build" / "server-integration.lock"
        descriptor = os.open(path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
        try:
            info = os.fstat(descriptor)
            if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or info.st_nlink != 1 or stat.S_IMODE(info.st_mode) != 0o600:
                raise RuntimeError("development test stand lock is not a private regular file")
            try:
                fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as error:
                raise RuntimeError("another integration harness owns the development PostgreSQL stand") from error
            self._fd = descriptor
        except BaseException:
            os.close(descriptor)
            raise

    def __del__(self) -> None:
        if self._fd >= 0:
            os.close(self._fd)
