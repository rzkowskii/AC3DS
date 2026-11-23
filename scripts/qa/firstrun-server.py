#!/usr/bin/env python3
"""Local HTTPS server for testing the first-run download in Azahar.

Only a development build (-DAC3DS_PRODUCTION=OFF -DAC3DS_FIRSTRUN_TEST=ON)
can be pointed at it: such a build reads firstrun-test.cfg and
firstrun-test-root.pem from sdmc:/3ds/assaultcube/. This script makes a
throwaway certificate authority and a certificate for "localhost" with the
openssl command, writes both files for each scenario under OUT/<scenario>/,
and serves:

  /ok         the archive
  /redirect   302 to /ok
  /away       302 to another host (https://example.com/), which must be refused unvisited
  /downgrade  302 to plain http, which must be refused
  /404        404 Not Found
  /truncated  the right Content-Length, then half the bytes and a closed connection
  /stall      half the bytes, then silence
  /short      a Content-Length one byte short of the archive

Scenario "wrong-hash" is /ok with another SHA-256 in the configuration,
"untrusted" is /ok without the test root on the SD card, and "wrong-name" is
/ok asked for as https://127.0.0.1, a name the certificate does not carry.
Without --archive a small stand-in archive is made; it has the files the
installer requires but is not a playable game.
"""
import argparse
import bz2
import hashlib
import http.server
import io
import os
import pathlib
import ssl
import subprocess
import tarfile
import threading
import time

SCENARIOS = {"ok": "/ok", "redirect": "/redirect", "away": "/away", "downgrade": "/downgrade", "wrong-hash": "/ok",
             "404": "/404", "truncated": "/truncated", "stall": "/stall", "short": "/short",
             "untrusted": "/ok", "wrong-name": "/ok"}
REQUIRED = ["config/font.cfg", "config/configtemplates.zip", "config/mapmodelattributes.cfg",
            "packages/misc/notexture.jpg", "packages/misc/icon.png", "docs/license.html"]


def stand_in():
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.GNU_FORMAT) as archive:
        for index, name in enumerate(REQUIRED + [f"packages/maps/filler{n}.bin" for n in range(40)]):
            data = os.urandom(50000) if "filler" in name else f"stand-in {index}\n".encode()
            info = tarfile.TarInfo("./" + name)
            info.size = len(data)
            archive.addfile(info, io.BytesIO(data))
    return bz2.compress(raw.getvalue())


def make_certificates(out):
    def openssl(*arguments):
        subprocess.run(["openssl", *arguments], check=True, capture_output=True, cwd=out)
    openssl("req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes", "-days", "30",
            "-subj", "/CN=AC3DS first-run test root", "-keyout", "ca.key", "-out", "ca.pem",
            "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign")
    openssl("req", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes", "-subj", "/CN=localhost",
            "-keyout", "server.key", "-out", "server.csr")
    (out / "server.ext").write_text("subjectAltName=DNS:localhost\nbasicConstraints=CA:FALSE\n")
    openssl("x509", "-req", "-in", "server.csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial", "-days", "30",
            "-extfile", "server.ext", "-out", "server.pem")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("out", type=pathlib.Path, help="folder for the certificates and the per-scenario SD card files")
    parser.add_argument("--archive", type=pathlib.Path, help="archive to serve instead of the stand-in")
    parser.add_argument("--port", type=int, default=8443)
    parser.add_argument("--auto", choices=("accept", "quit"), default="accept", help="answer the offer without a button press")
    parser.add_argument("--cancel-at", type=int, help="add cancel_at=<bytes> to every configuration")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    body = args.archive.read_bytes() if args.archive else stand_in()
    digest = hashlib.sha256(body).hexdigest()
    make_certificates(args.out)
    for name, path in SCENARIOS.items():
        folder = args.out / name
        folder.mkdir(exist_ok=True)
        host = "127.0.0.1" if name == "wrong-name" else "localhost"
        lines = [f"url=https://{host}:{args.port}{path}", f"size={len(body)}",
                 f"sha256={'0' * 64 if name == 'wrong-hash' else digest}", f"auto={args.auto}", "shots=1"]
        if args.cancel_at is not None:
            lines.append(f"cancel_at={args.cancel_at}")
        (folder / "firstrun-test.cfg").write_text("\n".join(lines) + "\n")
        if name != "untrusted":
            (folder / "firstrun-test-root.pem").write_bytes((args.out / "ca.pem").read_bytes())

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self):
            print(f"{time.strftime('%H:%M:%S')} GET {self.path} from {self.client_address[0]}, "
                  f"User-Agent {self.headers.get('User-Agent')!r}", flush=True)
            if self.path in ("/redirect", "/away", "/downgrade"):
                self.send_response(302)
                self.send_header("Location", {"/redirect": f"https://localhost:{args.port}/ok", "/away": "https://example.com/",
                                              "/downgrade": f"http://localhost:{args.port}/ok"}[self.path])
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            if self.path not in ("/ok", "/truncated", "/stall", "/short"):
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(body) - (self.path == "/short")))
            self.end_headers()
            if self.path in ("/truncated", "/stall"):
                self.wfile.write(body[:len(body) // 2])
                self.wfile.flush()
                if self.path == "/stall":
                    time.sleep(120)
                self.close_connection = True
                return
            self.wfile.write(body[:len(body) - (self.path == "/short")])

        def log_message(self, *_):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    server.daemon_threads = True
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(args.out / "server.pem", args.out / "server.key")
    server.socket = context.wrap_socket(server.socket, server_side=True)
    print(f"serving {len(body)} bytes, SHA-256 {digest}, on https://localhost:{args.port}; SD card files under {args.out}", flush=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
