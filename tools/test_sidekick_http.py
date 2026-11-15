#!/usr/bin/env python3
"""Host test for webapp/tools/sidekick.py's http(), and install_firmware.py's status() over it, against
local sockets standing for a sidekick that is restarting; and for install_app.py's bench push against
a local server standing for a sidekick that takes one or refuses it. No sidekick needed.

  python3 tools/test_sidekick_http.py
"""
import contextlib
import http.server
import io
import json
import os
import re
import socket
import sys
import threading
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "webapp", "tools"))
import app_pack  # noqa: E402
import install_app  # noqa: E402
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


class PushTarget:
    """A sidekick as install_app.py sees it: POST /api/app/push answered as `answer` says of the
    body it was sent (empty for the question asked first), then GET /app.html. Records the pack."""

    def __init__(self, answer):
        target = self
        self.pushed = None
        self.token = None
        self.paths = []

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def reply(self, code, body):
                self.send_response(code)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_POST(self):
                target.paths.append("POST " + self.path)
                target.token = self.headers.get(sidekick.HEADER)
                sent = self.rfile.read(int(self.headers.get("Content-Length") or 0))
                if sent:
                    target.pushed = sent
                code, body = answer(sent)
                self.reply(code, body)

            def do_GET(self):
                target.paths.append("GET " + self.path)
                self.reply(200, b"<!doctype html>")

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.base = "http://127.0.0.1:%d" % self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()


class BenchPush(unittest.TestCase):
    """install_app.py's one request (docs/architecture/sidekick.md, The bench push). The sidekick's
    side of it, which image a push is served on and who refuses one, is host_test_app_push's."""

    PACK = app_pack.pack([("app.html", b"\x1f\x8b" + b"page" * 50), ("bands.js", b"var BANDS=1;\n")], "d18f9b2-dirty 2026-11-15")

    def target(self, answer):
        t = PushTarget(answer)
        self.addCleanup(t.close)
        return t

    @staticmethod
    def takes(sent):
        # As the sidekick answers: an empty body is no pack (`size`), a pack is served.
        if not sent:
            return 409, b'{"ok":false,"why":"size"}'
        build, _ = app_pack.unpack(sent)
        return 200, json.dumps({"ok": True, "build": build}).encode()

    def refused(self, code, body, of_the_pack=False):
        """The tool's words when the sidekick answers (code, body): to the question asked first,
        or with `of_the_pack` only once the pack itself arrives."""
        t = self.target(lambda sent: self.takes(sent) if of_the_pack and not sent else (code, body))
        err = io.StringIO()
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(err):
            install_app.push(t.base, "ab" * 16, self.PACK)
        self.assertEqual(t.paths, ["POST /api/app/push"] * (2 if of_the_pack else 1),
                         "a refused push is not followed by a look at the app")
        if not of_the_pack:
            self.assertIsNone(t.pushed, "and the pack is not sent to a sidekick that said no")
        return err.getvalue()

    def test_the_pack_goes_in_one_request_with_the_token(self):
        t = self.target(self.takes)
        self.assertEqual(install_app.push(t.base, "ab" * 16, self.PACK), "d18f9b2-dirty 2026-11-15")
        self.assertEqual(t.pushed, self.PACK, "the pack's bytes, as built")
        self.assertEqual(t.token, "ab" * 16)
        # Asked first with no body, then the pack, then a look at the app.
        self.assertEqual(t.paths, ["POST /api/app/push", "POST /api/app/push", "GET /app.html"])

    def test_a_release_refuses_and_the_tool_says_what_to_do(self):
        said = self.refused(409, b'{"ok":false,"why":"release"}')
        self.assertIn("runs a release", said)
        self.assertIn("flash a dev or bench build", said)

    def test_each_refusal_has_its_own_words(self):
        seen = set()
        for why in ("release", "busy", "confirming"):
            said = self.refused(409, json.dumps({"ok": False, "why": why}).encode())
            self.assertNotIn("push \u2192", said, why)  # not the raw reply
            seen.add(said)
        # What only the pack itself can be refused for.
        for code, why in ((409, "size"), (400, "pack"), (500, "flash")):
            said = self.refused(code, json.dumps({"ok": False, "why": why}).encode(), of_the_pack=True)
            self.assertNotIn("push \u2192", said, why)
            seen.add(said)
        self.assertEqual(len(seen), 6)

    def test_every_code_the_sidekick_can_refuse_with_has_words(self):
        main = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "sidekick", "main")
        rules = open(os.path.join(main, "app_push.c")).read()
        rules = rules[rules.index("const char *app_push_refusal("):]
        handler = open(os.path.join(main, "app_serve.c")).read()
        codes = set(re.findall(r'return "(\w+)";', rules))
        codes |= set(re.findall(r'push_refuse\(req, "[^"]+", "(\w+)"\)', handler))
        self.assertTrue({"release", "busy", "confirming", "size", "pack", "flash"} <= codes, codes)
        self.assertEqual(codes - set(install_app.REFUSALS), set(), "a refusal install_app.py has no words for")

    def test_firmware_from_before_the_push_is_told_to_flash(self):
        # Such firmware matches the path only with GET /*, and answers a POST to it with 405
        # (esp_http_server, httpd_find_uri_handler()). Asked with no body, so the answer arrives.
        self.assertIn("idf.py build flash", self.refused(405, b"Specified method is invalid for this resource"))
        self.assertIn("idf.py build flash", self.refused(404, b"Not found"))

    def test_a_refusal_it_has_no_words_for_is_shown_as_it_came(self):
        self.assertIn("teapot", self.refused(418, b'{"ok":false,"why":"teapot"}'))

    def test_a_rejected_token_is_an_auth_error_so_the_button_is_asked_for(self):
        t = self.target(lambda sent: (401, b""))
        with self.assertRaises(sidekick.AuthError):
            install_app.push(t.base, "ab" * 16, self.PACK)
        self.assertIsNone(t.pushed)


if __name__ == "__main__":
    unittest.main()
