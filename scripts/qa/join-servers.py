#!/usr/bin/env python3
"""Provide deterministic server-browser replies for the loopback JOIN fixture."""

import argparse
from contextlib import ExitStack
import selectors
import socket
import struct
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout", type=int, default=90)
    args = parser.parse_args()
    if not 1 <= args.timeout <= 120:
        parser.error("Timeout must be 1..120 seconds")

    servers = ((29692, b"Alpha Lobby", b"MatchQAAlpha"), (29695, b"Beta Lobby", b"MatchQABeta"))
    with ExitStack() as stack:
        selector = stack.enter_context(selectors.DefaultSelector())
        for port, description, player in servers:
            server = stack.enter_context(socket.socket(socket.AF_INET, socket.SOCK_DGRAM))
            server.bind(("127.0.0.1", port))
            # Protocol 1300, team deathmatch, one player, nine minutes left.
            # The last two integers advertise four slots and a password.
            status = struct.pack("<BhBBB", 0x80, 1300, 0, 1, 9)
            status += b"ac_desert\0" + description + b"\0\x04\x01"
            selector.register(server, selectors.EVENT_READ, (status, player))

        print("AC3DS_JOIN_READY", flush=True)
        deadline = time.monotonic() + args.timeout
        while (remaining := deadline - time.monotonic()) > 0:
            for key, _ in selector.select(remaining):
                request, sender = key.fileobj.recvfrom(128)
                if len(request) != 2 or not 1 <= request[0] <= 100 or request[1] > 4:
                    continue
                status, player = key.data
                names = b"\x01" + player + b"\0" if request[1] == 1 else b""
                key.fileobj.sendto(request + status + names, sender)


if __name__ == "__main__":
    main()
