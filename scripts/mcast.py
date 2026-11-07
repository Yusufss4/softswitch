#!/usr/bin/env python3
"""Multicast helper for the integration tests; runs inside a host network namespace.

  mcast.py recv GROUP PORT SECONDS IFACE_IP        join, count datagrams, print the count
  mcast.py send GROUP PORT COUNT INTERVAL IFACE_IP send COUNT datagrams
"""
import socket
import struct
import sys
import time


def recv(group: str, port: int, seconds: float, iface_ip: str) -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", port))
    mreq = struct.pack("4s4s", socket.inet_aton(group), socket.inet_aton(iface_ip))
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)  # kernel sends the report
    sock.settimeout(0.1)
    count = 0
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            sock.recv(2048)
            count += 1
        except socket.timeout:
            pass
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_DROP_MEMBERSHIP, mreq)  # and the leave
    sock.close()
    return count


def send(group: str, port: int, count: int, interval: float, iface_ip: str) -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(iface_ip))
    for i in range(count):
        sock.sendto(f"softswitch {i}".encode(), (group, port))
        time.sleep(interval)
    return count


def main() -> int:
    if len(sys.argv) == 6 and sys.argv[1] == "recv":
        print(recv(sys.argv[2], int(sys.argv[3]), float(sys.argv[4]), sys.argv[5]))
        return 0
    if len(sys.argv) == 7 and sys.argv[1] == "send":
        print(send(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), float(sys.argv[5]), sys.argv[6]))
        return 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
