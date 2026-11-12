#!/usr/bin/env python3
"""Host test for webapp/tools/sidekick.py's http(), and install_firmware.py's status() over it, against
local sockets standing for a sidekick that is restarting: no sidekick needed.

  python3 tools/test_sidekick_http.py
"""
import os
import socket
import sys
import threading
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "webapp", "tools"))
import install_firmware  # noqa: E402
import sidekick  # noqa: E402


class FakeSidekick:
    """Accepts one connection at a time on a loopback port and answers each as `behave` says."""

    def __init__(self, behave):
        self.behave = behave
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(4)
        self.base = "http://127.0.0.1:%d" % self.listener.getsockname()[1]
        self.done = threading.Event()
        self.held = []
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        while not self.done.is_set():
            try:
                conn, _ = self.listener.accept()
            except OSError:
                return
            conn.recv(4096)
            if self.behave == "silent":
                self.held.append(conn)  # never answered, never closed
                continue
            if self.behave == "half":
                conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 400\r\n\r\n{\"state\":")
                self.held.append(conn)  # the rest never comes
                continue
            if self.behave == "cut":
                conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 400\r\n\r\n{\"state\":")
            elif self.behave == "busy":
                body = b'{"ok":false,"why":"busy"}'
                conn.sendall(b"HTTP/1.1 409 Conflict\r\nContent-Length: %d\r\n\r\n%s" % (len(body), body))
            elif self.behave == "idle":
                body = b'{"state":"idle"}'
                conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n%s" % (len(body), body))
            conn.close()  # "drop" closes with nothing sent

    def close(self):
        self.done.set()
        self.listener.close()
        for c in self.held:
            c.close()


class SidekickHttp(unittest.TestCase):
    def fake(self, behave):
        s = FakeSidekick(behave)
        self.addCleanup(s.close)
        return s.base

    def test_no_answer_is_a_sidekick_error(self):
        # silent: the read times out, 2026-11-11's crash. half: headers, then the body stops.
        # drop and cut: the connection closes before, or partway through, the reply.
        for behave in ("silent", "half", "drop", "cut"):
            with self.subTest(behave):
                base = self.fake(behave)
                with self.assertRaises(sidekick.SidekickError):
                    sidekick.http("GET", sidekick.url(base, "/api/fw/status"), timeout=0.3)

    def test_status_reads_no_answer_as_none(self):
        real = install_firmware.http
        install_firmware.http = lambda *a, **kw: real(*a, **{**kw, "timeout": 0.3})
        self.addCleanup(setattr, install_firmware, "http", real)
        for behave in ("silent", "half", "drop", "cut"):
            with self.subTest(behave):
                self.assertIsNone(install_firmware.status(self.fake(behave)))
        self.assertEqual(install_firmware.status(self.fake("idle")), {"state": "idle"})

    def test_an_http_error_is_still_its_status(self):
        code, body = sidekick.http("GET", sidekick.url(self.fake("busy"), "/api/fw/start"), timeout=2.0)
        self.assertEqual(code, 409)
        self.assertIn(b"busy", body)

    def test_nothing_listening_is_a_sidekick_error(self):
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()
        with self.assertRaises(sidekick.SidekickError):
            sidekick.http("GET", "http://127.0.0.1:%d/api/config" % port, timeout=1.0)


if __name__ == "__main__":
    unittest.main()
