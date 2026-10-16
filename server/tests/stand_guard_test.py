"""Distinguish live listeners from TIME_WAIT under both socket reuse policies."""

import errno
import socket
import unittest
from unittest.mock import patch

from stand_guard import require_http_port_available


class HttpPortGuardTests(unittest.TestCase):
    def test_live_listener_is_not_reusable(self):
        for option in (socket.SO_REUSEADDR, socket.SO_REUSEPORT):
            with self.subTest(option=option), socket.socket() as listener:
                listener.setsockopt(socket.SOL_SOCKET, option, 1)
                listener.bind(("127.0.0.1", 0))
                listener.listen()
                with self.assertRaises(OSError):
                    require_http_port_available("127.0.0.1", listener.getsockname()[1])

    def test_completed_connection_does_not_block_next_harness(self):
        for option in (socket.SO_REUSEADDR, socket.SO_REUSEPORT):
            with self.subTest(option=option):
                with socket.socket() as listener:
                    listener.setsockopt(socket.SOL_SOCKET, option, 1)
                    listener.bind(("127.0.0.1", 0))
                    listener.listen()
                    listener.settimeout(2)
                    port = listener.getsockname()[1]
                    with socket.create_connection(("127.0.0.1", port), timeout=2) as client:
                        connection, _ = listener.accept()
                        with connection:
                            connection.settimeout(2)
                            connection.shutdown(socket.SHUT_WR)
                            self.assertEqual(client.recv(1), b"")
                            client.shutdown(socket.SHUT_WR)
                            self.assertEqual(connection.recv(1), b"")
                require_http_port_available("127.0.0.1", port)
                with socket.socket() as native_policy:
                    native_policy.setsockopt(socket.SOL_SOCKET, option, 1)
                    native_policy.bind(("127.0.0.1", port))
                    native_policy.listen()

    def test_only_connection_refusal_allows_time_wait_fallback(self):
        for code in (0, errno.ETIMEDOUT, errno.EACCES, errno.ENETUNREACH):
            with self.subTest(code=code), patch("stand_guard.socket.socket") as factory:
                probe = factory.return_value.__enter__.return_value
                probe.bind.side_effect = OSError(errno.EADDRINUSE, "occupied")
                probe.connect_ex.return_value = code
                with self.assertRaises(OSError):
                    require_http_port_available("127.0.0.1", 19125)

    def test_bind_permission_error_is_not_waived(self):
        with patch("stand_guard.socket.socket") as factory:
            probe = factory.return_value.__enter__.return_value
            probe.bind.side_effect = PermissionError(errno.EACCES, "denied")
            with self.assertRaises(PermissionError):
                require_http_port_available("127.0.0.1", 19125)
            probe.connect_ex.assert_not_called()


if __name__ == "__main__":
    unittest.main()
