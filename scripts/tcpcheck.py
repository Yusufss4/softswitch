#!/usr/bin/env python3
"""TCP echo check for the integration tests; runs inside a host network namespace.

  tcpcheck.py server BIND_IP PORT          echo one connection, then exit
  tcpcheck.py client SERVER_IP PORT BYTES  send BYTES, print how many came back intact
"""
import socket
import sys


def server(ip: str, port: int) -> int:
    with socket.socket() as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((ip, port))
        s.listen(1)
        s.settimeout(10)
        conn, _ = s.accept()
        with conn:
            while data := conn.recv(65536):
                conn.sendall(data)
    return 0


def client(ip: str, port: int, size: int) -> int:
    payload = bytes(i % 251 for i in range(size))
    with socket.create_connection((ip, port), timeout=5) as s:
        s.sendall(payload)
        s.shutdown(socket.SHUT_WR)
        echoed = bytearray()
        while data := s.recv(65536):
            echoed += data
    print(len(echoed) if echoed == payload else -1)
    return 0


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "server":
        sys.exit(server(sys.argv[2], int(sys.argv[3])))
    if len(sys.argv) == 5 and sys.argv[1] == "client":
        sys.exit(client(sys.argv[2], int(sys.argv[3]), int(sys.argv[4])))
    print(__doc__, file=sys.stderr)
    sys.exit(2)
