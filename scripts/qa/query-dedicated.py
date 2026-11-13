#!/usr/bin/env python3
"""Verify an AssaultCube server-browser reply from a private loopback server."""

import argparse
import json
import socket
import struct
import time


class Reply:
    def __init__(self, data):
        self.data = data
        self.offset = 0

    def integer(self):
        if self.offset >= len(self.data):
            raise ValueError("Truncated server reply")
        value = self.data[self.offset]
        self.offset += 1
        if value in (0x80, 0x81):
            code = "<h" if value == 0x80 else "<i"
            value, = struct.unpack_from(code, self.data, self.offset)
            self.offset += struct.calcsize(code)
            return value
        return value if value < 128 else value - 256

    def string(self):
        chars = bytearray()
        while value := self.integer():
            if not -128 <= value <= 255:
                raise ValueError("Invalid character in server reply")
            chars.append(value & 0xff)
        return chars.decode("ascii", errors="replace")


def probe(port, name, clients, timeout):
    address = ("127.0.0.1", port)
    # Desktop browser ping ID followed by EXTPING_NOP.
    request = bytes([77, 0])
    deadline = time.monotonic() + timeout
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", 0))
        while (remaining := deadline - time.monotonic()) > 0:
            sock.settimeout(min(remaining, 0.5))
            sock.sendto(request, address)
            try:
                data, sender = sock.recvfrom(5000)
            except TimeoutError:
                continue
            if sender != address or not data.startswith(request):
                continue
            reply = Reply(data[len(request):])
            result = {
                "address": f"{sender[0]}:{sender[1]}",
                "request_hex": request.hex(),
                "reply_hex": data.hex(),
                "protocol": reply.integer(),
                "mode": reply.integer(),
                "players": reply.integer(),
                "minutes_remaining": reply.integer(),
                "map": reply.string(),
                "description": reply.string(),
                "max_clients": reply.integer(),
                "flags": reply.integer(),
            }
            if reply.integer() != 0 or reply.offset != len(reply.data):
                raise ValueError("Unexpected server-browser reply fields")
            if (result["protocol"], result["description"], result["max_clients"]) != (1300, name, clients):
                raise ValueError(f"Unexpected server identity: {result}")
            return result
    raise TimeoutError(f"No AssaultCube server-browser reply from {address[0]}:{address[1]}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=28786, help="Server info port (game port plus one)")
    parser.add_argument("--description", default="AssaultCube 3DS QA dedicated")
    parser.add_argument("--clients", type=int, default=4)
    parser.add_argument("--timeout", type=float, default=30)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535 or not 1 <= args.timeout <= 120:
        parser.error("Port must be 1..65535 and timeout 1..120 seconds")
    try:
        result = probe(args.port, args.description, args.clients, args.timeout)
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, f"{error}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
